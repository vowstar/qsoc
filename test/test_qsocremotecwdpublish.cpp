// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <functional>
#include <utility>

/*
 * The remote working directory has two readers: the tools resolve every
 * relative path against it, and the agent config carries a copy of it into the
 * system prompt and the hook envelope. A reconnect moves it, keeping the
 * directory the host confirms and rewinding to the workspace root otherwise,
 * and the move used to write only the first reader. The model was then told one
 * directory while its writes landed in another.
 *
 * Every case asserts the copy equals what the tools resolve against before it
 * asserts which directory that is: the disagreement is the defect, and a case
 * that checked only the directory would pass with both readers wrong together.
 *
 * The in-memory cases drive the connection directly. The last case is the same
 * property through the production mutator against a loopback sshd, which is the
 * only way to exercise it: moving the working directory canonicalizes the
 * request on the host, so it needs a host.
 */

namespace {

/* The alias the fixture's ~/.ssh/config defines, so connectAgentSshSession
 * resolves host, port, user and key exactly as it does for a real target. */
constexpr auto kAlias = "cwdhost";

class Test : public QObject
{
    Q_OBJECT

private:
    /* A transport the test owns, carrying no path: the connection seeds that
     * from the workspace it adopted. */
    static AgentRemoteState fakeTransport(
        QObject *scratch, const QString &target, const QString &workspace)
    {
        AgentRemoteState state;
        state.session            = new QSocSshSession(scratch);
        state.sftp               = new QSocSftpClient(*state.session);
        state.targetKey          = target;
        state.endpointIdentity   = target;
        state.workspace          = workspace;
        state.canonicalWorkspace = workspace;
        state.workspaceTreeId    = workspace.isEmpty() ? QString() : QStringLiteral("tree-id");
        return state;
    }

    /* A rebuilder that always produces a real transport, like the one the CLI
     * installs minus the connect sequence. */
    static QSocRemoteConnection::Rebuilder rebuilderOn(QObject *scratch)
    {
        return
            [scratch](
                const QString &target, const QString &workspace, AgentRemoteState *out, QString *) {
                *out = fakeTransport(scratch, target, workspace);
                return true;
            };
    }

    /* The rebuilder the CLI installs, minus the interactive secret prompt. */
    static QSocRemoteConnection::Rebuilder sshRebuilder(QObject *parent)
    {
        return [parent](
                   const QString    &target,
                   const QString    &workspace,
                   AgentRemoteState *out,
                   QString          *errorMessage) {
            AgentRemoteState fresh;
            if (!connectAgentSshSession(target, parent, &fresh, errorMessage)) {
                return false;
            }
            if (!prepareAgentRemoteWorkspace(workspace, &fresh, errorMessage)) {
                discardAgentRemoteState(&fresh);
                return false;
            }
            *out = fresh;
            return true;
        };
    }

    /* Stands in for the copy the config carries into the system prompt, written
     * by the same observer the CLI installs. */
    static std::function<void(const QString &)> recordInto(QString *told)
    {
        return [told](const QString &cwd) { *told = cwd; };
    }

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

private slots:
    void initTestCase();
    void cleanupTestCase();

    /*
     * COUNTEREXAMPLE. A reconnect that cannot confirm the working directory
     * rewinds to the workspace root. The rewind wrote the directory the tools
     * read and left the copy the model is shown naming the subdirectory that
     * is gone, so a relative write went to the root while the prompt said
     * otherwise.
     */
    void aRewindingReconnectPublishesTheDirectoryItLandsOn()
    {
        QObject              scratch;
        QSocRemoteConnection conn;
        QString              told;
        conn.setWorkingDirectoryObserver(recordInto(&told));
        /* The host confirms nothing, so the reconnect below rewinds. */
        conn.setDirectoryProbe([](QSocSftpClient *, const QString &) { return false; });

        QVERIFY(conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));
        QCOMPARE(told, conn.path()->cwd());
        QCOMPARE(told, QStringLiteral("/w"));

        /* Where the session is when the link breaks. */
        conn.path()->setCwd(QStringLiteral("/w/sub"));
        QCOMPARE(told, conn.path()->cwd());
        QCOMPARE(told, QStringLiteral("/w/sub"));

        conn.setRebuilder(rebuilderOn(&scratch));
        QString err;
        QCOMPARE(conn.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::Reconnected);

        /* The property: one directory, not two. */
        QCOMPARE(told, conn.path()->cwd());
        /* And it is the root, because nothing confirmed the subdirectory. */
        QCOMPARE(told, QStringLiteral("/w"));
        QVERIFY(!conn.lastReconnectKeptCwd());
    }

    /* A reconnect that keeps the working directory must leave the two readers
     * agreeing too, and must still keep it: publishing is not licence to move
     * a directory the host confirmed. */
    void aKeepingReconnectLeavesTheCopyInStep()
    {
        QObject              scratch;
        QSocRemoteConnection conn;
        QString              told;
        conn.setWorkingDirectoryObserver(recordInto(&told));
        conn.setDirectoryProbe([](QSocSftpClient *, const QString &) { return true; });

        QVERIFY(conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));
        conn.path()->setCwd(QStringLiteral("/w/sub"));

        conn.setRebuilder(rebuilderOn(&scratch));
        QString err;
        QCOMPARE(conn.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::Reconnected);

        QCOMPARE(told, conn.path()->cwd());
        QCOMPARE(told, QStringLiteral("/w/sub"));
        QVERIFY(conn.lastReconnectKeptCwd());
    }

    /* Binding a different workspace seeds the working directory from it, which
     * is the other branch of the same seeding step. A swap that did not publish
     * would leave the copy naming a directory on the workspace just left. */
    void aSwapToADifferentWorkspacePublishesTheNewDirectory()
    {
        QObject              scratch;
        QSocRemoteConnection conn;
        QString              told;
        conn.setWorkingDirectoryObserver(recordInto(&told));

        QVERIFY(conn.adopt(fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/w"))));
        conn.path()->setCwd(QStringLiteral("/w/sub"));
        QCOMPARE(told, QStringLiteral("/w/sub"));

        /* The order the /ssh handler uses: the previous transport is freed and
         * the path context reset before the new one is bound. */
        conn.teardown();
        QVERIFY(conn.adopt(
            fakeTransport(&scratch, QStringLiteral("u@h:22"), QStringLiteral("/other"))));

        QCOMPARE(told, conn.path()->cwd());
        QCOMPARE(told, QStringLiteral("/other"));
    }

    /* The same property through the production mutator and a real reconnect:
     * `path_context(action=cwd)` moves the directory by canonicalizing the
     * request on the host, and the reconnect re-verifies it there. Both halves
     * run against the loopback sshd, so the remote filesystem is this one and
     * a directory can be taken away between them. */
    void theCopyFollowsARealReconnect()
    {
        QSOC_REQUIRE_SSHD(m_fixture);
        if (!m_ready) {
            QSOC_TEST_FIXTURE_FAILED(m_failure);
        }

        const QString vanishing = m_workspace + QStringLiteral("/vanishing");
        QVERIFY(QDir().mkpath(vanishing));

        QObject              owner;
        QSocRemoteConnection conn;
        QString              told;
        conn.setWorkingDirectoryObserver(recordInto(&told));

        AgentRemoteState staged;
        QString          why;
        QVERIFY2(
            connectAgentSshSession(QString::fromLatin1(kAlias), &owner, &staged, &why),
            qPrintable(why));
        QVERIFY2(prepareAgentRemoteWorkspace(m_workspace, &staged, &why), qPrintable(why));
        QVERIFY(conn.adopt(std::move(staged)));
        conn.setRebuilder(sshRebuilder(&owner));

        /* Exactly what the path tool does. */
        QCOMPARE(
            conn.setWorkingDirectory(QStringLiteral("vanishing"), &why),
            QSocRemoteConnection::CwdChange::Changed);
        QCOMPARE(told, conn.path()->cwd());
        QCOMPARE(told, vanishing);

        /* The host loses the directory while the link is down. */
        QVERIFY(QDir().rmdir(vanishing));
        conn.session()->disconnectFromHost();
        QVERIFY(!conn.isUsable());
        QCOMPARE(conn.reconnect(&why), QSocRemoteConnection::ReconnectOutcome::Reconnected);

        QCOMPARE(told, conn.path()->cwd());
        QCOMPARE(told, m_workspace);
        QVERIFY(!conn.lastReconnectKeptCwd());

        /* And a directory the host still has is kept, with the copy on it. */
        const QString surviving = m_workspace + QStringLiteral("/surviving");
        QVERIFY(QDir().mkpath(surviving));
        QCOMPARE(
            conn.setWorkingDirectory(QStringLiteral("surviving"), &why),
            QSocRemoteConnection::CwdChange::Changed);
        conn.session()->disconnectFromHost();
        /* A later turn, so a fresh reconnect budget. */
        conn.resetReconnectBudget();
        QCOMPARE(conn.reconnect(&why), QSocRemoteConnection::ReconnectOutcome::Reconnected);

        QCOMPARE(told, conn.path()->cwd());
        QCOMPARE(told, surviving);
        QVERIFY(conn.lastReconnectKeptCwd());
    }
};

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

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocremotecwdpublish.moc"
