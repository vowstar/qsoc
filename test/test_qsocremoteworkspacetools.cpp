// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsoctool.h"
#include "agent/runtime/qsocagentruntime.h"
#include "common/qsocimageattach.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <QBuffer>
#include <QColor>
#include <QDir>
#include <QFile>
#include <QImage>
#include <QTemporaryDir>
#include <QtTest>

using json = nlohmann::json;

/*
 * A runtime bound to a loopback sshd through `/ssh`: the workspace-facing
 * pieces (read_file, /skill placeholders, the prompt's skill listing) must
 * describe and reach the remote workspace, never the local project.
 *
 * A dependency the fixture cannot supply itself (sshd, ssh-keygen, a login
 * name) skips these cases, unless QSOC_TEST_DEPS_REQUIRED is set, which CI
 * does after installing the lot.
 */

namespace {

constexpr auto kAlias = "workspacehost";

QByteArray makePng(int width, int height)
{
    QImage image(width, height, QImage::Format_RGB32);
    image.fill(QColor(Qt::darkCyan));
    QByteArray bytes;
    QBuffer    buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return bytes;
}

bool writeFile(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly | QIODevice::Truncate)
           && file.write(bytes) == bytes.size();
}

bool writeSkill(
    const QString &project, const QString &name, bool userInvocable, const QByteArray &body)
{
    const QString dir = project + QStringLiteral("/.qsoc/skills/") + name;
    return QDir().mkpath(dir)
           && writeFile(
               dir + QStringLiteral("/SKILL.md"),
               "---\nname: " + name.toUtf8() + "\ndescription: " + name.toUtf8()
                   + " probe\nuser-invocable: " + (userInvocable ? "true" : "false") + "\n---\n"
                   + body);
}

/* The value after `key=` on the line that carries it. */
QString valueOf(const QString &text, const QString &key)
{
    for (const QString &line : text.split(QLatin1Char('\n'))) {
        if (line.startsWith(key + QLatin1Char('='))) {
            return line.mid(key.size() + 1).trimmed();
        }
    }
    return {};
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void init();
    void cleanup();
    void readFileAttachesARemoteImage();
    void readFilePagesALargeLogWithoutReadingItAll();
    void readFileBoundsOneOversizedLine();
    void skillPlaceholdersNameTheRemoteWorkspace();
    void skillListingFollowsTheBinding();

private:
    bool prepare();

    bool fail(const QString &detail)
    {
        m_failure = detail;
        return false;
    }

    QString readRemote(const json &args) const
    {
        QSocTool *tool = m_runtime->agent()->getToolRegistry()->getTool(QStringLiteral("read_file"));
        return tool == nullptr ? QStringLiteral("(no read_file tool)") : tool->execute(args);
    }

    QSocTestSshd                      m_fixture;
    QTemporaryDir                     m_dir;
    std::unique_ptr<QSocAgentRuntime> m_runtime;
    bool                              m_ready = false;
    QString                           m_failure;
    QString                           m_home;
    QString                           m_project;
    QString                           m_workspace;
    QByteArray                        m_oldHome;
    QByteArray                        m_oldQsocHome;
    QByteArray                        m_oldXdgHome;
    bool                              m_hadHome     = false;
    bool                              m_hadQsocHome = false;
    bool                              m_hadXdgHome  = false;
};

#define REQUIRE_WORKSPACE_FIXTURE() \
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
    m_home      = m_dir.path() + QStringLiteral("/home");
    m_project   = m_dir.path() + QStringLiteral("/project");
    m_workspace = m_fixture.workDir() + QStringLiteral("/ws");
    if (!QDir().mkpath(m_home + QStringLiteral("/.ssh")) || !QDir().mkpath(m_project)
        || !QDir().mkpath(m_workspace)) {
        return fail(QStringLiteral("could not lay out the fixture tree"));
    }
    const QString sshConfig = QStringLiteral(
                                  "Host %1\n"
                                  "  HostName 127.0.0.1\n"
                                  "  Port %2\n"
                                  "  User %3\n"
                                  "  IdentityFile %4\n"
                                  "  IdentitiesOnly yes\n"
                                  "  StrictHostKeyChecking no\n"
                                  "  UserKnownHostsFile /dev/null\n")
                                  .arg(QString::fromLatin1(kAlias))
                                  .arg(m_fixture.port())
                                  .arg(m_fixture.user(), m_fixture.keyPath());
    if (!writeFile(m_home + QStringLiteral("/.ssh/config"), sshConfig.toUtf8())) {
        return fail(QStringLiteral("could not write the client ssh config"));
    }
    QFile::setPermissions(
        m_home + QStringLiteral("/.ssh/config"), QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    /* A vision model, so an image read can take the attachment path. Nothing
     * here sends a request, so the endpoint is never dialed. */
    const QByteArray yaml = "llm:\n"
                            "  model: vision\n"
                            "  models:\n"
                            "    vision:\n"
                            "      url: http://127.0.0.1:9/v1/chat/completions\n"
                            "      modalities:\n"
                            "        image: true\n"
                            "        image_max_tokens: 10000\n"
                            "        image_max_dimension: 1568\n";
    if (!writeFile(m_home + QStringLiteral("/qsoc.yml"), yaml)) {
        return fail(QStringLiteral("could not write the model config"));
    }
    if (!writeSkill(m_project, QStringLiteral("probe"), true, "PROJECT=${PROJECT}\nCWD=${CWD}\n")
        || !writeSkill(m_project, QStringLiteral("modelonly"), false, "model only\n")) {
        return fail(QStringLiteral("could not write the project skills"));
    }
    m_hadHome     = qEnvironmentVariableIsSet("HOME");
    m_hadQsocHome = qEnvironmentVariableIsSet("QSOC_HOME");
    m_hadXdgHome  = qEnvironmentVariableIsSet("XDG_CONFIG_HOME");
    m_oldHome     = qgetenv("HOME");
    m_oldQsocHome = qgetenv("QSOC_HOME");
    m_oldXdgHome  = qgetenv("XDG_CONFIG_HOME");
    if (!qputenv("HOME", m_home.toUtf8()) || !qputenv("QSOC_HOME", m_home.toUtf8())
        || !qputenv("XDG_CONFIG_HOME", m_home.toUtf8())) {
        return fail(QStringLiteral("could not redirect the environment"));
    }
    return true;
}

void Test::cleanupTestCase()
{
    if (m_ready) {
        m_hadHome ? qputenv("HOME", m_oldHome) : qunsetenv("HOME");
        m_hadQsocHome ? qputenv("QSOC_HOME", m_oldQsocHome) : qunsetenv("QSOC_HOME");
        m_hadXdgHome ? qputenv("XDG_CONFIG_HOME", m_oldXdgHome) : qunsetenv("XDG_CONFIG_HOME");
    }
    m_fixture.stop();
    /* QSOC_TEST_MAIN calls _exit(), so QTemporaryDir's destructor never runs. */
    if (m_dir.isValid()) {
        QVERIFY2(m_dir.remove(), qPrintable(m_dir.errorString()));
    }
    QVERIFY2(m_fixture.removeRoot(), "the fixture root could not be removed");
}

/* Every case starts local and binds with `/ssh`, the way a user does. */
void Test::init()
{
    if (!m_ready) {
        return;
    }
    QSocAgentRuntimeOptions options;
    options.projectDirectory = m_project;
    options.workspace        = m_workspace;
    m_runtime                = std::make_unique<QSocAgentRuntime>(options);
    QVERIFY(m_runtime->openSession());
}

void Test::cleanup()
{
    if (m_runtime) {
        m_runtime->disconnectRemote();
        m_runtime.reset();
    }
}

/* Counterexample: the remote read_file returned the PNG bytes decoded as
 * UTF-8 text, so the model got mojibake instead of the picture. */
void Test::readFileAttachesARemoteImage()
{
    REQUIRE_WORKSPACE_FIXTURE();
    QString error;
    QVERIFY2(m_runtime->connectRemote(QString::fromLatin1(kAlias), &error), qPrintable(error));
    QVERIFY(writeFile(m_workspace + QStringLiteral("/shot.txt"), makePng(64, 48)));

    const QString result = readRemote({{"file_path", "shot.txt"}});
    QVERIFY2(result.startsWith(QStringLiteral("[image attached:")), qPrintable(result.left(200)));
    QVERIFY(result.contains(QString::fromLatin1(QSocImageAttach::attachmentMarkerOpen())));
    QVERIFY(!result.contains(QStringLiteral("PNG\r\n")));
}

/* A log far past one read's byte limit still pages by line: the file size is
 * never refused, and only the requested window comes back. */
void Test::readFilePagesALargeLogWithoutReadingItAll()
{
    REQUIRE_WORKSPACE_FIXTURE();
    QString error;
    QVERIFY2(m_runtime->connectRemote(QString::fromLatin1(kAlias), &error), qPrintable(error));
    QByteArray log;
    const int  lines = 300000;
    log.reserve(lines * 64);
    for (int i = 0; i < lines; ++i) {
        log += QByteArray("line ").append(QByteArray::number(i)).append(QByteArray(50, 'x')) + '\n';
    }
    QVERIFY(log.size() > 16 * 1024 * 1024);
    QVERIFY(writeFile(m_workspace + QStringLiteral("/big.log"), log));

    const QString tail = readRemote({{"file_path", "big.log"}, {"offset", lines - 2}});
    QVERIFY2(tail.startsWith(QStringLiteral("line 299998x")), qPrintable(tail.left(200)));
    QCOMPARE(tail.count(QLatin1Char('\n')), 2);

    const QString window = readRemote(
        {{"file_path", "big.log"}, {"offset", 1000}, {"max_lines", 2}});
    QVERIFY2(window.startsWith(QStringLiteral("line 1000x")), qPrintable(window.left(200)));
    QVERIFY(window.contains(QStringLiteral("line 1001x")));
    QVERIFY(!window.contains(QStringLiteral("line 1002x")));
    QVERIFY2(window.contains(QStringLiteral("offset=1002")), qPrintable(window));
}

/* Counterexample: one line with no newline came back whole, so a single read
 * could hand the model any number of megabytes. */
void Test::readFileBoundsOneOversizedLine()
{
    REQUIRE_WORKSPACE_FIXTURE();
    QString error;
    QVERIFY2(m_runtime->connectRemote(QString::fromLatin1(kAlias), &error), qPrintable(error));
    QVERIFY(writeFile(m_workspace + QStringLiteral("/one.line"), QByteArray(17 * 1024 * 1024, 'y')));

    const QString result = readRemote({{"file_path", "one.line"}});
    QVERIFY2(result.size() < 4096, qPrintable(QString::number(result.size())));
    QVERIFY2(result.startsWith(QStringLiteral("Error: line 0 ")), qPrintable(result));
}

/* Counterexample: ${PROJECT} named the local project while ${CWD} named the
 * remote directory, so one skill body pointed at two different trees. */
void Test::skillPlaceholdersNameTheRemoteWorkspace()
{
    REQUIRE_WORKSPACE_FIXTURE();
    QString error;
    QVERIFY2(m_runtime->connectRemote(QString::fromLatin1(kAlias), &error), qPrintable(error));

    QVERIFY(m_runtime->executeCommand(QStringLiteral("/probe")));
    const QStringList queued = m_runtime->takePendingAutoInputs();
    QCOMPARE(queued.size(), 1);
    const QString project = valueOf(queued.first(), QStringLiteral("PROJECT"));
    const QString cwd     = valueOf(queued.first(), QStringLiteral("CWD"));
    QVERIFY2(project.endsWith(QStringLiteral("/ws")), qPrintable(queued.first()));
    QCOMPARE(project, cwd);
    QVERIFY(!queued.first().contains(m_project));
}

/* Counterexample: the listing was built once at startup, so after `/ssh` the
 * prompt still told the model to load skills with a tool it no longer had. */
void Test::skillListingFollowsTheBinding()
{
    REQUIRE_WORKSPACE_FIXTURE();
    const auto listing = [this] { return m_runtime->agent()->getConfig().skillListing; };
    QVERIFY2(listing().contains(QStringLiteral("skill_find(action")), qPrintable(listing()));
    QVERIFY(listing().contains(QStringLiteral("**modelonly**")));

    QString error;
    QVERIFY2(m_runtime->connectRemote(QString::fromLatin1(kAlias), &error), qPrintable(error));
    QVERIFY(m_runtime->agent()->getToolRegistry()->getTool(QStringLiteral("skill_find")) == nullptr);
    QVERIFY2(listing().contains(QStringLiteral("not in the remote workspace")), qPrintable(listing()));
    QVERIFY(!listing().contains(QStringLiteral("skill_find(action")));
    QVERIFY(listing().contains(QStringLiteral("**probe**")));
    QVERIFY(!listing().contains(QStringLiteral("**modelonly**")));

    m_runtime->disconnectRemote();
    QVERIFY2(listing().contains(QStringLiteral("skill_find(action")), qPrintable(listing()));
    QVERIFY(listing().contains(QStringLiteral("**modelonly**")));
}

QSOC_TEST_MAIN(Test)
#include "test_qsocremoteworkspacetools.moc"
