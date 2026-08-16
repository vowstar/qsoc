// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"

#include <QDirIterator>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>
#endif

namespace {

QString builtQsoc()
{
    const QDir buildDir(QStringLiteral(QT_TESTCASE_BUILDDIR));
    for (const QString &candidate :
         {QStringLiteral("../qsoc"), QStringLiteral("../qsoc.app/Contents/MacOS/qsoc")}) {
        const QString path = buildDir.absoluteFilePath(candidate);
        if (QFile::exists(path)) {
            return path;
        }
    }
    return {};
}

int pickFreePort()
{
    QTcpServer probe;
    if (!probe.listen(QHostAddress::LocalHost, 0)) {
        return 0;
    }
    const int port = probe.serverPort();
    probe.close();
    return port;
}

bool waitForPort(int port, int timeoutMs)
{
    QElapsedTimer clock;
    clock.start();
    while (clock.elapsed() < timeoutMs) {
        QTcpSocket probe;
        probe.connectToHost(QHostAddress::LocalHost, static_cast<quint16>(port));
        if (probe.waitForConnected(100)) {
            return true;
        }
        QTest::qWait(20);
    }
    return false;
}

QProcessEnvironment isolatedEnvironment(const QString &root)
{
    const QString home    = QDir(root).filePath(QStringLiteral("home"));
    const QString xdg     = QDir(root).filePath(QStringLiteral("xdg"));
    const QString config  = QDir(root).filePath(QStringLiteral("config"));
    const QString runtime = QDir(root).filePath(QStringLiteral("runtime"));
    const QString temp    = QDir(root).filePath(QStringLiteral("tmp"));
    QDir().mkpath(home);
    QDir().mkpath(xdg);
    QDir().mkpath(runtime);
    QDir().mkpath(temp);
    QFile::setPermissions(
        runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);

    QProcessEnvironment environment;
    environment.insert(QStringLiteral("HOME"), home);
    environment.insert(QStringLiteral("XDG_CONFIG_HOME"), xdg);
    environment.insert(QStringLiteral("XDG_RUNTIME_DIR"), runtime);
    environment.insert(QStringLiteral("QSOC_HOME"), config);
    environment.insert(QStringLiteral("TEMP"), temp);
    environment.insert(QStringLiteral("TMP"), temp);
    environment.insert(QStringLiteral("TMPDIR"), temp);
    environment.insert(QStringLiteral("PATH"), qEnvironmentVariable("PATH"));
    environment.insert(QStringLiteral("LANG"), QStringLiteral("C.UTF-8"));
    environment.insert(QStringLiteral("TERM"), QStringLiteral("xterm-256color"));
    environment.insert(QStringLiteral("NO_PROXY"), QStringLiteral("*"));
    environment.insert(QStringLiteral("no_proxy"), QStringLiteral("*"));
    return environment;
}

bool projectStorageExists(const QString &projectPath)
{
    const QFileInfo info(QDir(projectPath).filePath(QStringLiteral(".qsoc")));
    return info.exists() || info.isSymLink();
}

class BoundedProcess final : public QProcess
{
public:
    ~BoundedProcess() override { stop(); }

    void stop()
    {
        if (state() == QProcess::NotRunning) {
            return;
        }
        terminate();
        if (!waitForFinished(2000)) {
            kill();
            waitForFinished(2000);
        }
    }
};

#ifdef Q_OS_UNIX

[[noreturn]] void failPtyChild()
{
    ::_exit(127);
}

void installPty(int slaveFd)
{
    if (::setsid() < 0) {
        failPtyChild();
    }
#ifdef TIOCSCTTY
    if (::ioctl(slaveFd, TIOCSCTTY, 0) < 0) {
        failPtyChild();
    }
#endif
    for (int fd : {STDIN_FILENO, STDOUT_FILENO, STDERR_FILENO}) {
        if (::dup2(slaveFd, fd) != fd) {
            failPtyChild();
        }
    }
    if (slaveFd > STDERR_FILENO) {
        ::close(slaveFd);
    }
}

class PtyProcess final : public QProcess
{
public:
    ~PtyProcess() override
    {
        stop();
        closeDescriptors();
    }

    bool startInPty(
        const QString             &program,
        const QStringList         &arguments,
        const QString             &workingDirectory,
        const QProcessEnvironment &environment)
    {
        closeDescriptors();

        masterFd_ = ::posix_openpt(O_RDWR | O_NOCTTY);
        if (masterFd_ < 0 || ::grantpt(masterFd_) != 0 || ::unlockpt(masterFd_) != 0) {
            closeDescriptors();
            return false;
        }
        const char *slaveName = ::ptsname(masterFd_);
        if (slaveName == nullptr) {
            closeDescriptors();
            return false;
        }
        slaveFd_ = ::open(slaveName, O_RDWR | O_NOCTTY);
        if (slaveFd_ < 0) {
            closeDescriptors();
            return false;
        }

        struct winsize size = {};
        size.ws_col         = 120;
        size.ws_row         = 30;
        if (::ioctl(slaveFd_, TIOCSWINSZ, &size) != 0) {
            closeDescriptors();
            return false;
        }
        const int flags = ::fcntl(masterFd_, F_GETFL, 0);
        if (flags < 0 || ::fcntl(masterFd_, F_SETFL, flags | O_NONBLOCK) != 0) {
            closeDescriptors();
            return false;
        }
        (void) ::fcntl(masterFd_, F_SETFD, FD_CLOEXEC);
        (void) ::fcntl(slaveFd_, F_SETFD, FD_CLOEXEC);

        setWorkingDirectory(workingDirectory);
        setProcessEnvironment(environment);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
        const int childSlave = slaveFd_;
        setChildProcessModifier([childSlave]() { installPty(childSlave); });
#endif
        start(program, arguments);
        const bool started = waitForStarted(5000);
        if (started) {
            ::close(slaveFd_);
            slaveFd_ = -1;
        }
        return started;
    }

    bool waitForOutput(const QByteArray &needle, int timeoutMs)
    {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < timeoutMs) {
            drainOutput();
            if (output_.contains(needle)) {
                return true;
            }
            if (state() == QProcess::NotRunning) {
                break;
            }
            QTest::qWait(20);
        }
        drainOutput();
        return output_.contains(needle);
    }

    qsizetype markOutput()
    {
        drainOutput();
        return output_.size();
    }

    bool waitForOutputAfter(const QByteArray &needle, qsizetype offset, int timeoutMs)
    {
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < timeoutMs) {
            drainOutput();
            if (output_.indexOf(needle, offset) >= 0) {
                return true;
            }
            if (state() == QProcess::NotRunning) {
                break;
            }
            QTest::qWait(20);
        }
        drainOutput();
        return output_.indexOf(needle, offset) >= 0;
    }

    bool writeInput(const QByteArray &input, int timeoutMs = 2000)
    {
        QElapsedTimer clock;
        qsizetype     written = 0;
        clock.start();
        while (written < input.size() && clock.elapsed() < timeoutMs) {
            const ssize_t result = ::write(
                masterFd_, input.constData() + written, static_cast<size_t>(input.size() - written));
            if (result > 0) {
                written += result;
                continue;
            }
            if (result < 0 && errno == EINTR) {
                continue;
            }
            if (result < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                QTest::qWait(10);
                continue;
            }
            return false;
        }
        return written == input.size();
    }

    bool submitLine(const QByteArray &line, int timeoutMs = 2000)
    {
        drainOutput();
        const qsizetype priorOutput = output_.size();
        if (!writeInput(line, timeoutMs)) {
            return false;
        }
        QElapsedTimer clock;
        clock.start();
        while (clock.elapsed() < timeoutMs) {
            drainOutput();
            if (output_.indexOf(line, priorOutput) >= 0) {
                return writeInput("\r", timeoutMs);
            }
            if (state() == QProcess::NotRunning) {
                return false;
            }
            QTest::qWait(10);
        }
        return false;
    }

    bool waitForExit(int timeoutMs)
    {
        QElapsedTimer clock;
        clock.start();
        while (state() != QProcess::NotRunning && clock.elapsed() < timeoutMs) {
            drainOutput();
            QTest::qWait(20);
        }
        drainOutput();
        return state() == QProcess::NotRunning;
    }

    const QByteArray &output() const { return output_; }

    void stop()
    {
        if (state() == QProcess::NotRunning) {
            drainOutput();
            return;
        }
        terminate();
        if (!waitForExit(2000)) {
            kill();
            (void) waitForExit(2000);
        }
    }

protected:
#if QT_VERSION < QT_VERSION_CHECK(6, 0, 0)
    void setupChildProcess() override { installPty(slaveFd_); }
#endif

private:
    void drainOutput()
    {
        if (masterFd_ < 0) {
            return;
        }
        char buffer[8192];
        for (;;) {
            const ssize_t size = ::read(masterFd_, buffer, sizeof(buffer));
            if (size > 0) {
                output_.append(buffer, size);
                constexpr qsizetype maxCapture = 4 * 1024 * 1024;
                if (output_.size() > maxCapture) {
                    output_.remove(0, output_.size() - maxCapture);
                }
                continue;
            }
            if (size < 0 && errno == EINTR) {
                continue;
            }
            break;
        }
    }

    void closeDescriptors()
    {
        if (slaveFd_ >= 0) {
            ::close(slaveFd_);
            slaveFd_ = -1;
        }
        if (masterFd_ >= 0) {
            ::close(masterFd_);
            masterFd_ = -1;
        }
    }

    QByteArray output_;
    int        masterFd_ = -1;
    int        slaveFd_  = -1;
};

#endif

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void pristineCurrentDirectoryStaysClean();
    void explicitProjectDirectoryStaysClean();
    void nonDurableCommandsStayClean_data();
    void nonDurableCommandsStayClean();
    void pristineProjectSwitchStaysClean();
    void unsafeProjectSwitchKeepsCurrentSession();
    void firstPromptPersistsAndCanContinue();

private:
    QString m_qsoc;
};

void Test::initTestCase()
{
#ifndef Q_OS_UNIX
    QSKIP("Interactive pseudo-terminal coverage requires Unix");
#else
    m_qsoc = builtQsoc();
    QVERIFY2(!m_qsoc.isEmpty(), "the built qsoc executable was not found");
#endif
}

void Test::pristineCurrentDirectoryStaysClean()
{
#ifdef Q_OS_UNIX
    QTemporaryDir fixture(QDir::tempPath() + QStringLiteral("/test_qsoc_agent_lazy_XXXXXX"));
    QVERIFY(fixture.isValid());
    const QString workspace = QDir(fixture.path()).filePath(QStringLiteral("work"));
    QVERIFY(QDir().mkpath(workspace));

    PtyProcess agent;
    QVERIFY(agent.startInPty(
        m_qsoc, {QStringLiteral("agent")}, workspace, isolatedEnvironment(fixture.path())));
    const bool       ready = agent.waitForOutput("Type 'exit' to exit", 15000);
    const QByteArray log   = agent.output().right(8192);
    QVERIFY2(ready, log.constData());
    QVERIFY2(!projectStorageExists(workspace), "idle startup created project storage");
    QVERIFY(agent.submitLine("/quit"));
    QVERIFY2(agent.waitForExit(10000), agent.output().right(8192).constData());
    QCOMPARE(agent.exitStatus(), QProcess::NormalExit);
    QCOMPARE(agent.exitCode(), 0);
    QVERIFY2(!projectStorageExists(workspace), "quitting an idle agent created project storage");
#endif
}

void Test::explicitProjectDirectoryStaysClean()
{
#ifdef Q_OS_UNIX
    QTemporaryDir fixture(QDir::tempPath() + QStringLiteral("/test_qsoc_agent_lazy_XXXXXX"));
    QVERIFY(fixture.isValid());
    const QString launcher = QDir(fixture.path()).filePath(QStringLiteral("launcher"));
    const QString project  = QDir(fixture.path()).filePath(QStringLiteral("project"));
    QVERIFY(QDir().mkpath(launcher));
    QVERIFY(QDir().mkpath(project));

    PtyProcess agent;
    QVERIFY(agent.startInPty(
        m_qsoc,
        {QStringLiteral("agent"), QStringLiteral("-d"), project},
        launcher,
        isolatedEnvironment(fixture.path())));
    const bool       ready = agent.waitForOutput("Type 'exit' to exit", 15000);
    const QByteArray log   = agent.output().right(8192);
    QVERIFY2(ready, log.constData());
    QVERIFY2(!projectStorageExists(project), "idle -d startup created project storage");
    QVERIFY2(!projectStorageExists(launcher), "idle -d startup polluted its launch directory");
    QVERIFY(agent.submitLine("/quit"));
    QVERIFY2(agent.waitForExit(10000), agent.output().right(8192).constData());
    QCOMPARE(agent.exitStatus(), QProcess::NormalExit);
    QCOMPARE(agent.exitCode(), 0);
    QVERIFY2(!projectStorageExists(project), "idle -d shutdown created project storage");
    QVERIFY2(!projectStorageExists(launcher), "idle -d shutdown polluted its launch directory");
#endif
}

void Test::nonDurableCommandsStayClean_data()
{
    QTest::addColumn<QByteArray>("command");
    QTest::addColumn<QByteArray>("marker");
    QTest::newRow("help") << QByteArray("/help") << QByteArray("Keyboard shortcuts:");
    QTest::newRow("status") << QByteArray("/status") << QByteArray("Remote:   (local mode)");
    QTest::newRow("loop-list") << QByteArray("/loop list") << QByteArray("(no /loop jobs)");
    QTest::newRow("clear") << QByteArray("/clear") << QByteArray("History cleared.");
}

void Test::nonDurableCommandsStayClean()
{
#ifdef Q_OS_UNIX
    QFETCH(QByteArray, command);
    QFETCH(QByteArray, marker);

    QTemporaryDir fixture(QDir::tempPath() + QStringLiteral("/test_qsoc_agent_lazy_XXXXXX"));
    QVERIFY(fixture.isValid());
    const QString workspace = QDir(fixture.path()).filePath(QStringLiteral("work"));
    QVERIFY(QDir().mkpath(workspace));

    PtyProcess agent;
    QVERIFY(agent.startInPty(
        m_qsoc, {QStringLiteral("agent")}, workspace, isolatedEnvironment(fixture.path())));
    QVERIFY2(
        agent.waitForOutput("Type 'exit' to exit", 15000), agent.output().right(8192).constData());
    QVERIFY(agent.submitLine(command));
    const bool       completed = agent.waitForOutput(marker, 10000);
    const QByteArray log       = agent.output().right(8192);
    QVERIFY2(completed, log.constData());
    QVERIFY2(!projectStorageExists(workspace), "a non-durable command created project storage");
    QVERIFY(agent.submitLine("/quit"));
    QVERIFY2(agent.waitForExit(10000), agent.output().right(8192).constData());
    QCOMPARE(agent.exitStatus(), QProcess::NormalExit);
    QCOMPARE(agent.exitCode(), 0);
    QVERIFY2(!projectStorageExists(workspace), "non-durable shutdown created project storage");
#endif
}

void Test::pristineProjectSwitchStaysClean()
{
#ifdef Q_OS_UNIX
    QTemporaryDir fixture(QDir::tempPath() + QStringLiteral("/test_qsoc_agent_lazy_XXXXXX"));
    QVERIFY(fixture.isValid());
    const QString projectA = QDir(fixture.path()).filePath(QStringLiteral("project-a"));
    const QString projectB = QDir(fixture.path()).filePath(QStringLiteral("project-b"));
    QVERIFY(QDir().mkpath(projectA));
    QVERIFY(QDir().mkpath(projectB));

    PtyProcess agent;
    QVERIFY(agent.startInPty(
        m_qsoc,
        {QStringLiteral("agent"), QStringLiteral("-d"), projectA},
        fixture.path(),
        isolatedEnvironment(fixture.path())));
    QVERIFY2(
        agent.waitForOutput("Type 'exit' to exit", 15000), agent.output().right(8192).constData());
    QVERIFY2(!projectStorageExists(projectA), "idle startup created storage in project A");
    QVERIFY2(!projectStorageExists(projectB), "idle startup created storage in project B");

    const QByteArray switchCommand = QByteArray("/project ") + projectB.toUtf8();
    QVERIFY(agent.submitLine(switchCommand));
    const QByteArray switchMarker = QByteArray("Project: ") + projectB.toUtf8()
                                    + QByteArray(" (new session ");
    const bool       switched     = agent.waitForOutput(switchMarker, 15000);
    const QByteArray log          = agent.output().right(8192);
    QVERIFY2(switched, log.constData());
    QVERIFY2(!projectStorageExists(projectA), "project switch persisted the pristine project A");
    QVERIFY2(!projectStorageExists(projectB), "project switch persisted the pristine project B");

    QVERIFY(agent.submitLine("/quit"));
    QVERIFY2(agent.waitForExit(10000), agent.output().right(8192).constData());
    QCOMPARE(agent.exitStatus(), QProcess::NormalExit);
    QCOMPARE(agent.exitCode(), 0);
    QVERIFY2(!projectStorageExists(projectA), "project switch shutdown persisted project A");
    QVERIFY2(!projectStorageExists(projectB), "project switch shutdown persisted project B");
#endif
}

void Test::unsafeProjectSwitchKeepsCurrentSession()
{
#ifdef Q_OS_UNIX
    QTemporaryDir fixture(QDir::tempPath() + QStringLiteral("/test_qsoc_agent_lazy_XXXXXX"));
    QVERIFY(fixture.isValid());
    const QString projectA = QDir(fixture.path()).filePath(QStringLiteral("project-a"));
    const QString projectB = QDir(fixture.path()).filePath(QStringLiteral("project-b"));
    QVERIFY(QDir().mkpath(projectA));
    QVERIFY(QDir().mkpath(projectB));

    QFile metadata(QDir(projectB).filePath(QStringLiteral(".qsoc")));
    QVERIFY(metadata.open(QIODevice::WriteOnly));
    QCOMPARE(metadata.write("sentinel"), qint64(8));
    metadata.close();

    PtyProcess agent;
    QVERIFY(agent.startInPty(
        m_qsoc,
        {QStringLiteral("agent"), QStringLiteral("-d"), projectA},
        fixture.path(),
        isolatedEnvironment(fixture.path())));
    QVERIFY2(
        agent.waitForOutput("Type 'exit' to exit", 15000), agent.output().right(8192).constData());
    const QRegularExpression sessionPattern(QStringLiteral(R"(\(New session ([0-9A-Fa-f]{8})\))"));
    const QRegularExpressionMatch sessionMatch = sessionPattern.match(
        QString::fromUtf8(agent.output()));
    QVERIFY2(sessionMatch.hasMatch(), agent.output().right(8192).constData());
    const QByteArray sessionId = sessionMatch.captured(1).toUtf8();

    QVERIFY(agent.submitLine(QByteArray("/project ") + projectB.toUtf8()));
    QVERIFY2(
        agent.waitForOutput("Could not prepare a session in the new project.", 10000),
        agent.output().right(8192).constData());
    QVERIFY2(!projectStorageExists(projectA), "rejected project switch persisted project A");
    QVERIFY(metadata.open(QIODevice::ReadOnly));
    QCOMPARE(metadata.readAll(), QByteArray("sentinel"));
    metadata.close();

    const qsizetype statusOffset = agent.markOutput();
    QVERIFY(agent.submitLine("/status"));
    QVERIFY2(
        agent.waitForOutputAfter(QByteArray("Session:  ") + sessionId, statusOffset, 10000),
        agent.output().right(8192).constData());

    QVERIFY(agent.submitLine("/project"));
    QVERIFY2(
        agent.waitForOutput(QByteArray("Project: ") + projectA.toUtf8(), 10000),
        agent.output().right(8192).constData());
    QVERIFY(agent.submitLine("/quit"));
    QVERIFY2(agent.waitForExit(10000), agent.output().right(8192).constData());
    QCOMPARE(agent.exitStatus(), QProcess::NormalExit);
    QCOMPARE(agent.exitCode(), 0);
    QVERIFY2(!projectStorageExists(projectA), "rejected project switch shutdown persisted project A");
    QVERIFY(metadata.open(QIODevice::ReadOnly));
    QCOMPARE(metadata.readAll(), QByteArray("sentinel"));
#endif
}

void Test::firstPromptPersistsAndCanContinue()
{
#ifdef Q_OS_UNIX
    const QString python = QStandardPaths::findExecutable(QStringLiteral("python3"));
    if (python.isEmpty()) {
        QSKIP("python3 is required for the local mock endpoint");
    }
    const QString mockScript
        = QDir(QStringLiteral(QT_TESTCASE_SOURCEDIR)).absoluteFilePath("qsoc_mock_llm.py");
    QVERIFY2(QFile::exists(mockScript), "qsoc_mock_llm.py was not found");

    QTemporaryDir fixture(QDir::tempPath() + QStringLiteral("/test_qsoc_agent_lazy_XXXXXX"));
    QVERIFY(fixture.isValid());
    const QString project = QDir(fixture.path()).filePath(QStringLiteral("project"));
    const QString config  = QDir(fixture.path()).filePath(QStringLiteral("config"));
    QVERIFY(QDir().mkpath(project));
    QVERIFY(QDir().mkpath(config));

    const int port = pickFreePort();
    QVERIFY2(port > 0, "no free loopback port was available");

    QFile configFile(QDir(config).filePath(QStringLiteral("qsoc.yml")));
    QVERIFY(configFile.open(QIODevice::WriteOnly | QIODevice::Text));
    configFile.write(QStringLiteral(
                         "llm:\n"
                         "  model: mock\n"
                         "  models:\n"
                         "    mock:\n"
                         "      name: Mock\n"
                         "      url: \"http://127.0.0.1:%1/v1/chat/completions\"\n"
                         "      timeout: 10000\n"
                         "      context: 131072\n"
                         "      max_output_tokens: 1024\n"
                         "      reasoning: false\n"
                         "agent:\n"
                         "  predict_input: false\n"
                         "  memory_recall: false\n"
                         "  memory_extract: false\n"
                         "  memory_dream: false\n"
                         "  session_title: false\n"
                         "proxy:\n"
                         "  type: none\n")
                         .arg(port)
                         .toUtf8());
    configFile.close();

    QProcessEnvironment environment = isolatedEnvironment(fixture.path());
    QProcessEnvironment mockEnvironment(environment);
    mockEnvironment.insert(QStringLiteral("MOCK_TTL"), QStringLiteral("60"));
    mockEnvironment.insert(QStringLiteral("MOCK_REPLY"), QStringLiteral("MOCKDONE"));

    BoundedProcess mock;
    mock.setProcessEnvironment(mockEnvironment);
    mock.setWorkingDirectory(fixture.path());
    mock.setStandardOutputFile(QDir(fixture.path()).filePath(QStringLiteral("mock.out")));
    mock.setStandardErrorFile(QDir(fixture.path()).filePath(QStringLiteral("mock.err")));
    mock.start(python, {mockScript, QString::number(port), QStringLiteral("none")});
    QVERIFY(mock.waitForStarted(5000));
    QVERIFY2(waitForPort(port, 5000), "the local mock endpoint did not start");

    QString sessionPath;
    {
        PtyProcess agent;
        QVERIFY(agent.startInPty(
            m_qsoc,
            {QStringLiteral("agent"), QStringLiteral("-d"), project},
            fixture.path(),
            environment));
        QVERIFY2(
            agent.waitForOutput("Type 'exit' to exit", 15000),
            agent.output().right(8192).constData());
        QVERIFY2(!projectStorageExists(project), "storage existed before the first prompt");

        QVERIFY(agent.submitLine("hello"));
        QVERIFY2(agent.waitForOutput("MOCKDONE", 30000), agent.output().right(8192).constData());

        const QDir    sessions(QDir(project).filePath(QStringLiteral(".qsoc/sessions")));
        QElapsedTimer persistClock;
        persistClock.start();
        QStringList files;
        while (persistClock.elapsed() < 5000) {
            files = sessions.entryList({QStringLiteral("*.jsonl")}, QDir::Files, QDir::Name);
            if (files.size() == 1) {
                break;
            }
            QTest::qWait(20);
        }
        QCOMPARE(files.size(), 1);
        sessionPath = sessions.filePath(files.constFirst());

        QFile transcript(sessionPath);
        QVERIFY(transcript.open(QIODevice::ReadOnly | QIODevice::Text));
        const QByteArray records = transcript.readAll();
        QVERIFY(records.contains("\"type\":\"run\""));
        QVERIFY(records.contains("\"input\":\"hello\""));
        QVERIFY(records.contains("\"type\":\"message\""));

        QVERIFY(agent.submitLine("/quit"));
        QVERIFY2(agent.waitForExit(10000), agent.output().right(8192).constData());
        QCOMPARE(agent.exitStatus(), QProcess::NormalExit);
        QCOMPARE(agent.exitCode(), 0);
    }

    QDirIterator locks(
        QDir(project).filePath(QStringLiteral(".qsoc")),
        {QStringLiteral("*.lock")},
        QDir::Files | QDir::Hidden,
        QDirIterator::Subdirectories);
    QVERIFY2(!locks.hasNext(), "the completed agent left a project lock behind");

    {
        PtyProcess resumed;
        QVERIFY(resumed.startInPty(
            m_qsoc,
            {QStringLiteral("agent"), QStringLiteral("-d"), project, QStringLiteral("--continue")},
            fixture.path(),
            environment));
        const bool       loaded = resumed.waitForOutput("(Resumed session ", 15000);
        const QByteArray log    = resumed.output().right(8192);
        QVERIFY2(loaded, log.constData());
        QVERIFY(resumed.output().contains(QFileInfo(sessionPath).baseName().left(8).toUtf8()));
        QVERIFY(resumed.submitLine("/quit"));
        QVERIFY2(resumed.waitForExit(10000), resumed.output().right(8192).constData());
        QCOMPARE(resumed.exitStatus(), QProcess::NormalExit);
        QCOMPARE(resumed.exitCode(), 0);
    }

    QDirIterator resumedLocks(
        QDir(project).filePath(QStringLiteral(".qsoc")),
        {QStringLiteral("*.lock")},
        QDir::Files | QDir::Hidden,
        QDirIterator::Subdirectories);
    QVERIFY2(!resumedLocks.hasNext(), "the resumed agent left a project lock behind");
#endif
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocagentlazystorage.moc"
