// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctool.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>
#include <utility>

/*
 * The read-before-overwrite guard, exercised through the real `--ssh` wiring
 * against a loopback sshd. The wiring is what is under test: the guard keys off
 * the read state inside the connection's path context, and a reconnect only
 * clears the container the tools actually consult when there is exactly one of
 * them. When the launch path kept a second copy, the guard read as satisfied
 * after a reconnect and a write landed on a baseline nobody had re-read.
 *
 * The decisive assertion is on the file's bytes, not on a tool's wording: a
 * refusal that still wrote is the failure this exists to catch.
 *
 * A dependency the fixture cannot supply itself (sshd, ssh-keygen, a login
 * name) skips these cases, unless QSOC_TEST_DEPS_REQUIRED is set, which CI does
 * after installing the lot. Anything else, a fixture that has its dependencies
 * and still will not come up, always fails: QtTest exits 0 on a skip, so a
 * silent skip is indistinguishable from coverage.
 */

namespace {

/* The alias the fixture's ~/.ssh/config defines, so connectAgentSshSession
 * resolves host, port, user and key exactly as it does for a real target. */
constexpr auto kAlias = "guardhost";

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void aWriteIsRefusedAfterAReconnectUntilTheFileIsReadAgain();
    void aReconnectDoesNotRestoreAWorkingDirectoryThatIsGone();
    void theCwdCommandIsVisibleToTheTools();

private:
    /* One connection wired the way the `--ssh` launch path wires it, plus the
     * registry its tools live in. The connection is heap-allocated because the
     * tools hold its address for their whole life. */
    struct Wiring
    {
        QObject                               owner;
        std::unique_ptr<QSocRemoteConnection> conn     = std::make_unique<QSocRemoteConnection>();
        QSocToolRegistry                     *registry = nullptr;

        QSocTool *tool(const QString &name) const { return registry->getTool(name); }
    };

    /* Build the real wiring against the fixture's sshd. Returns false with a
     * reason on any connect failure. */
    bool wireUp(Wiring *wiring, const QString &workspace, QString *why) const
    {
        AgentRemoteState staged;
        if (!connectAgentSshSession(QString::fromLatin1(kAlias), &wiring->owner, &staged, why)) {
            return false;
        }
        if (!prepareAgentRemoteWorkspace(workspace, &staged, why)) {
            discardAgentRemoteState(&staged);
            return false;
        }
        if (!wiring->conn->adopt(std::move(staged))) {
            /* A refused adopt consumes nothing, so the transport is still ours
             * to free. */
            // cppcheck-suppress accessMoved
            discardAgentRemoteState(&staged);
            *why = QStringLiteral("adopt refused a complete transport");
            return false;
        }
        wiring->registry = buildAgentRemoteRegistry(&wiring->owner, wiring->conn.get(), nullptr);
        return true;
    }

    /* The rebuilder the CLI installs, minus the interactive secret prompt. */
    static QSocRemoteConnection::Rebuilder rebuilder(QObject *parent)
    {
        return [parent](
                   const QString    &target,
                   const QString    &workspace,
                   AgentRemoteState *out,
                   QString          *errorMessage,
                   QDeadlineTimer    deadline) {
            AgentRemoteState fresh;
            if (!connectAgentSshSession(target, parent, &fresh, errorMessage, {}, {}, deadline)) {
                return false;
            }
            if (!prepareAgentRemoteWorkspace(workspace, &fresh, errorMessage, deadline)) {
                discardAgentRemoteState(&fresh);
                return false;
            }
            *out = fresh;
            return true;
        };
    }

    static QByteArray fileBytes(const QString &path)
    {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    }

    static bool writeFileBytes(const QString &path, const QByteArray &bytes)
    {
        QFile file(path);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        return file.write(bytes) == bytes.size();
    }

    /* Everything after the dependency check. False leaves m_failure set, so
     * every early return here is already a failure and none has to be
     * remembered. HOME is redirected last, so m_ready holds exactly when it
     * has to be restored. */
    bool prepare();

    bool fail(const QString &detail)
    {
        m_failure = detail;
        return false;
    }

    QSocTestSshd  m_fixture;
    QTemporaryDir m_dir;
    bool          m_ready = false;
    QString       m_failure;
    QString       m_home;
    QString       m_workspace;
    QByteArray    m_oldHome;
    bool          m_hadHome = false;
};

/* First line of every case: the sshd fixture's own three-state policy, then
 * the wiring this test builds on top of it. */
#define REQUIRE_GUARD_FIXTURE() \
    do { \
        QSOC_REQUIRE_SSHD(m_fixture); \
        if (!m_ready) { \
            QSOC_TEST_FIXTURE_FAILED(m_failure); \
        } \
    } while (false)

void Test::initTestCase()
{
    m_fixture.start();
    if (m_fixture.state() != QSocTestSshd::State::Ready) {
        return; /* the fixture's own state decides skip versus fail */
    }
    m_ready = prepare();
}

bool Test::prepare()
{
    if (!m_dir.isValid()) {
        return fail(QStringLiteral("temporary directory: %1").arg(m_dir.errorString()));
    }
    const QString root = m_dir.path();
    m_home             = root + QStringLiteral("/home");
    m_workspace        = root + QStringLiteral("/work");
    QDir().mkpath(m_home + QStringLiteral("/.ssh"));
    QDir().mkpath(m_workspace);

    QFile sshCfg(m_home + QStringLiteral("/.ssh/config"));
    if (!sshCfg.open(QIODevice::WriteOnly)) {
        return fail(QStringLiteral("could not write the client ssh config"));
    }
    sshCfg.write(QStringLiteral(
                     "Host %1\n"
                     "  HostName 127.0.0.1\n"
                     "  Port %2\n"
                     "  User %3\n"
                     "  IdentityFile %4\n"
                     "  IdentitiesOnly yes\n")
                     .arg(QString::fromLatin1(kAlias))
                     .arg(m_fixture.port())
                     .arg(m_fixture.user(), m_fixture.keyPath())
                     .toUtf8());
    sshCfg.close();
    QFile::setPermissions(
        m_home + QStringLiteral("/.ssh/config"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);

    /* HOME is redirected for the whole run so no developer ssh config leaks
     * in. cleanupTestCase puts it back. */
    m_hadHome = qEnvironmentVariableIsSet("HOME");
    m_oldHome = qgetenv("HOME");
    if (!qputenv("HOME", m_home.toUtf8())) {
        return fail(QStringLiteral("could not redirect HOME"));
    }
    return true;
}

void Test::cleanupTestCase()
{
    if (m_ready) {
        if (m_hadHome) {
            qputenv("HOME", m_oldHome);
        } else {
            qunsetenv("HOME");
        }
    }
    m_fixture.stop();
    /* QSOC_TEST_MAIN calls _exit(), so QTemporaryDir's destructor never runs
     * and the keys plus the work tree would survive every run. */
    if (m_dir.isValid()) {
        QVERIFY2(m_dir.remove(), qPrintable(m_dir.errorString()));
    }
    QVERIFY2(m_fixture.removeRoot(), "the fixture root could not be removed");
}

void Test::aWriteIsRefusedAfterAReconnectUntilTheFileIsReadAgain()
{
    REQUIRE_GUARD_FIXTURE();

    const QString guardPath = m_workspace + QStringLiteral("/guard.sv");
    QVERIFY(writeFileBytes(guardPath, QByteArray("v1")));

    Wiring  wiring;
    QString why;
    QVERIFY2(wireUp(&wiring, m_workspace, &why), qPrintable(why));
    wiring.conn->setRebuilder(rebuilder(&wiring.owner));
    QCOMPARE(wiring.conn->generation(), QSocRemoteConnection::Generation{1});

    /* The read that satisfies the guard. */
    const QString read = wiring.tool(QStringLiteral("read_file"))
                             ->execute({{"file_path", guardPath.toStdString()}});
    QCOMPARE(read.trimmed(), QStringLiteral("v1"));

    /* Kill the link and let the connection rebuild it. */
    wiring.conn->session()->disconnectFromHost();
    QVERIFY(!wiring.conn->isUsable());
    QString reconnectErr;
    QCOMPARE(
        wiring.conn->reconnect(&reconnectErr), QSocRemoteConnection::ReconnectOutcome::Reconnected);
    QCOMPARE(wiring.conn->generation(), QSocRemoteConnection::Generation{2});
    QVERIFY(wiring.conn->isUsable());

    /* Nothing on the host has been observed since it broke, so the overwrite
     * must be refused. */
    const QString refused = wiring.tool(QStringLiteral("write_file"))
                                ->execute(
                                    {{"file_path", guardPath.toStdString()}, {"content", "v2"}});
    QVERIFY2(
        refused.contains(QStringLiteral("File not read yet")),
        qPrintable(QStringLiteral("write_file said: %1").arg(refused)));
    /* The assertion that matters: real bytes, not the wording of a refusal. */
    QCOMPARE(fileBytes(guardPath), QByteArray("v1"));

    /* Re-observe, then the same write goes through. */
    const QString reread = wiring.tool(QStringLiteral("read_file"))
                               ->execute({{"file_path", guardPath.toStdString()}});
    QCOMPARE(reread.trimmed(), QStringLiteral("v1"));
    const QString wrote = wiring.tool(QStringLiteral("write_file"))
                              ->execute({{"file_path", guardPath.toStdString()}, {"content", "v2"}});
    QVERIFY2(!wrote.contains(QStringLiteral("Error")), qPrintable(wrote));
    QCOMPARE(fileBytes(guardPath), QByteArray("v2"));
}

void Test::aReconnectDoesNotRestoreAWorkingDirectoryThatIsGone()
{
    REQUIRE_GUARD_FIXTURE();

    const QString gone = m_workspace + QStringLiteral("/vanishing");
    QVERIFY(QDir().mkpath(gone));

    Wiring  wiring;
    QString why;
    QVERIFY2(wireUp(&wiring, m_workspace, &why), qPrintable(why));
    wiring.conn->setRebuilder(rebuilder(&wiring.owner));

    wiring.conn->path()->setCwd(wiring.conn->path()->resolveCwdRequest(QStringLiteral("vanishing")));
    QCOMPARE(wiring.conn->path()->cwd(), gone);

    /* The host loses the directory while the link is down. */
    QVERIFY(QDir().rmdir(gone));
    wiring.conn->session()->disconnectFromHost();
    QString reconnectErr;
    QCOMPARE(
        wiring.conn->reconnect(&reconnectErr), QSocRemoteConnection::ReconnectOutcome::Reconnected);

    QCOMPARE(wiring.conn->path()->cwd(), m_workspace);
    QVERIFY(!wiring.conn->lastReconnectKeptCwd());

    /* And a directory that is still there is kept. */
    const QString kept = m_workspace + QStringLiteral("/surviving");
    QVERIFY(QDir().mkpath(kept));
    wiring.conn->path()->setCwd(wiring.conn->path()->resolveCwdRequest(QStringLiteral("surviving")));
    wiring.conn->session()->disconnectFromHost();
    /* A later turn, so a fresh reconnect budget. */
    wiring.conn->resetReconnectBudget();
    QCOMPARE(
        wiring.conn->reconnect(&reconnectErr), QSocRemoteConnection::ReconnectOutcome::Reconnected);
    QCOMPARE(wiring.conn->path()->cwd(), kept);
    QVERIFY(wiring.conn->lastReconnectKeptCwd());
}

void Test::theCwdCommandIsVisibleToTheTools()
{
    REQUIRE_GUARD_FIXTURE();

    const QString sub = m_workspace + QStringLiteral("/nested");
    QVERIFY(QDir().mkpath(sub));
    QVERIFY(writeFileBytes(sub + QStringLiteral("/inside.txt"), QByteArray("under nested")));

    Wiring  wiring;
    QString why;
    QVERIFY2(wireUp(&wiring, m_workspace, &why), qPrintable(why));

    /* Exactly what /cwd does. With two path containers the tools kept
     * resolving against the workspace root and never saw this. */
    wiring.conn->path()->setCwd(wiring.conn->path()->resolveCwdRequest(QStringLiteral("nested")));
    QCOMPARE(wiring.conn->path()->cwd(), sub);

    const QString read
        = wiring.tool(QStringLiteral("read_file"))->execute({{"file_path", "inside.txt"}});
    QCOMPARE(read.trimmed(), QStringLiteral("under nested"));

    /* And the relative read is what registered in the guard's read state, so
     * the tools and the command agree on which file that was. */
    QVERIFY(wiring.conn->path()->readState().wasRead(sub + QStringLiteral("/inside.txt")));
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocremoteguardafterreconnect.moc"
