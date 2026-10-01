// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "qsoc_test_pty.h"

#include <QDirIterator>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QUuid>
#include <QtTest>

namespace {

using namespace QSocTestPty;

QByteArray mockConfiguration(int port, bool captureStatus = false)
{
    QByteArray config = QStringLiteral(
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
                            .toUtf8();
    if (captureStatus) {
        config.replace("agent:\n", "agent:\n  status_line: 'cat > \"$QSOC_TEST_STATUS_CAPTURE\"'\n");
    }
    return config;
}

bool projectStorageExists(const QString &projectPath)
{
    const QFileInfo info(QDir(projectPath).filePath(QStringLiteral(".qsoc")));
    return info.exists() || info.isSymLink();
}

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
    void changedArtifactBindingRefusesRequest_data();
    void changedArtifactBindingRefusesRequest();

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
    QVERIFY2(agent.waitForOutput("(New session ", 15000), agent.output().right(8192).constData());
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
    const QString mockBinary = QString::fromUtf8(QSOC_MOCK_LLM_PATH);
    QVERIFY2(QFile::exists(mockBinary), "qsoc_mock_llm was not built");

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
    configFile.write(mockConfiguration(port));
    configFile.close();

    QProcessEnvironment environment = isolatedEnvironment(fixture.path());
    QProcessEnvironment mockEnvironment(environment);
    mockEnvironment.insert(QStringLiteral("MOCK_TTL"), QStringLiteral("60"));
    mockEnvironment.insert(QStringLiteral("MOCK_REPLY"), QStringLiteral("MOCKDONE"));

    BoundedProcess mock;
    mock.setProcessEnvironment(mockEnvironment);
    mock.setWorkingDirectory(fixture.path());
    const QString mockOut = QDir(fixture.path()).filePath(QStringLiteral("mock.out"));
    const QString mockErr = QDir(fixture.path()).filePath(QStringLiteral("mock.err"));
    mock.setStandardOutputFile(mockOut);
    mock.setStandardErrorFile(mockErr);
    mock.start(mockBinary, {QString::number(port), QStringLiteral("none")});
    QVERIFY(mock.waitForStarted(5000));
    const bool mockReady = waitForMockReady(mock, port, 45000);
    QByteArray mockErrLog;
    QFile      errFile(mockErr);
    if (errFile.open(QIODevice::ReadOnly)) {
        mockErrLog = errFile.readAll().right(2048);
    }
    QByteArray mockOutLog;
    QFile      outFile(mockOut);
    if (outFile.open(QIODevice::ReadOnly)) {
        mockOutLog = outFile.readAll().right(2048);
    }
    const QString mockState = [&mock]() {
        switch (mock.state()) {
        case QProcess::NotRunning:
            return QStringLiteral("NotRunning");
        case QProcess::Starting:
            return QStringLiteral("Starting");
        case QProcess::Running:
            return QStringLiteral("Running");
        }
        return QStringLiteral("unknown");
    }();
    QVERIFY2(
        mockReady,
        qPrintable(QStringLiteral(
                       "the local mock endpoint did not start"
                       " (state=%1 exitCode=%2 exitStatus=%3)\nstderr: %4\nstdout: %5")
                       .arg(mockState)
                       .arg(mock.exitCode())
                       .arg(
                           mock.exitStatus() == QProcess::NormalExit ? QStringLiteral("NormalExit")
                                                                     : QStringLiteral("CrashExit"))
                       .arg(QString::fromUtf8(mockErrLog))
                       .arg(QString::fromUtf8(mockOutLog))));

    QString    sessionPath;
    QByteArray artifactBinding;
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

        const QDir  sessions(QDir(project).filePath(QStringLiteral(".qsoc/sessions")));
        QStringList files;
        (void) agent.waitUntil(
            [&]() {
                files = sessions.entryList({QStringLiteral("*.jsonl")}, QDir::Files, QDir::Name);
                return files.size() == 1;
            },
            5000);
        QCOMPARE(files.size(), 1);
        sessionPath = sessions.filePath(files.constFirst());

        QFile scope(sessionPath + QStringLiteral(".artifacts/.scope"));
        QVERIFY(scope.open(QIODevice::ReadOnly));
        artifactBinding = scope.readAll();
        QVERIFY(!artifactBinding.isEmpty());

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
        QFile scope(sessionPath + QStringLiteral(".artifacts/.scope"));
        QVERIFY(scope.open(QIODevice::ReadOnly));
        QCOMPARE(scope.readAll(), artifactBinding);
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

void Test::changedArtifactBindingRefusesRequest_data()
{
    QTest::addColumn<bool>("boundScope");
    QTest::newRow("pending-artifact-symlink") << false;
    QTest::newRow("bound-scope-replaced") << true;
}

void Test::changedArtifactBindingRefusesRequest()
{
#ifdef Q_OS_UNIX
    QFETCH(bool, boundScope);
    QTemporaryDir fixture(QDir::tempPath() + QStringLiteral("/test_qsoc_agent_lazy_XXXXXX"));
    QVERIFY(fixture.isValid());
    const QString project  = QDir(fixture.path()).filePath(QStringLiteral("project"));
    const QString config   = QDir(fixture.path()).filePath(QStringLiteral("config"));
    const QString external = QDir(fixture.path()).filePath(QStringLiteral("external"));
    QVERIFY(QDir().mkpath(project));
    QVERIFY(QDir().mkpath(config));
    QVERIFY(QDir().mkpath(external));
    const int port = pickFreePort();
    QVERIFY(port > 0);
    QFile configFile(QDir(config).filePath(QStringLiteral("qsoc.yml")));
    QVERIFY(configFile.open(QIODevice::WriteOnly));
    const QByteArray configBytes = mockConfiguration(port, true);
    QCOMPARE(configFile.write(configBytes), qint64(configBytes.size()));
    configFile.close();

    const auto readBytes = [](const QString &path) {
        QFile file(path);
        return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
    };
    const QString requestLog = QDir(fixture.path()).filePath(QStringLiteral("requests.jsonl"));
    QFile         requests(requestLog);
    QVERIFY(requests.open(QIODevice::WriteOnly));
    requests.close();
    const QString statusCapture = QDir(fixture.path()).filePath(QStringLiteral("status.json"));
    auto          environment   = isolatedEnvironment(fixture.path());
    environment.insert(QStringLiteral("QSOC_TEST_STATUS_CAPTURE"), statusCapture);
    auto mockEnvironment = environment;
    mockEnvironment.insert(QStringLiteral("MOCK_TTL"), QStringLiteral("120"));
    mockEnvironment.insert(QStringLiteral("MOCK_REPLY"), QStringLiteral("MOCKDONE"));
    mockEnvironment.insert(QStringLiteral("MOCK_REQUEST_LOG"), requestLog);
    BoundedProcess mock;
    mock.setProcessEnvironment(mockEnvironment);
    mock.setWorkingDirectory(fixture.path());
    mock.setStandardOutputFile(QDir(fixture.path()).filePath(QStringLiteral("mock.out")));
    mock.setStandardErrorFile(QDir(fixture.path()).filePath(QStringLiteral("mock.err")));
    mock.start(QString::fromUtf8(QSOC_MOCK_LLM_PATH), {QString::number(port), QStringLiteral("none")});
    QVERIFY(mock.waitForStarted(5000));
    QVERIFY(waitForMockReady(mock, port, 45000));

    PtyProcess agent;
    QVERIFY(agent.startInPty(
        m_qsoc,
        {QStringLiteral("agent"), QStringLiteral("-d"), project},
        fixture.path(),
        environment));
    QVERIFY2(
        agent.waitForOutput("Type 'exit' to exit", 15000), agent.output().right(8192).constData());
    QVERIFY(!projectStorageExists(project));
    QVERIFY(agent.submitLine("/effort off"));
    const auto capturedSession = [&]() {
        return QJsonDocument::fromJson(readBytes(statusCapture))
            .object()
            .value(QStringLiteral("session"))
            .toObject()
            .value(QStringLiteral("id"))
            .toString();
    };
    QString sessionId;
    QVERIFY2(
        agent.waitUntil([&]() { return !(sessionId = capturedSession()).isEmpty(); }, 5000),
        qPrintable(QStringLiteral(
                       "Agent state: %1, exit code: %2, capture exists: "
                       "%3\nCapture: %4\nOutput: %5")
                       .arg(agent.state())
                       .arg(agent.exitCode())
                       .arg(QFileInfo::exists(statusCapture))
                       .arg(QString::fromUtf8(readBytes(statusCapture).right(4096)))
                       .arg(QString::fromUtf8(agent.output().right(8192)))));
    QVERIFY(!QUuid(sessionId).isNull());
    QVERIFY(!projectStorageExists(project));
    QCOMPARE(readBytes(requestLog).count('\n'), 0);
    const QString sessionPath = QDir(project).filePath(
        QStringLiteral(".qsoc/sessions/") + sessionId + ".jsonl");
    const QString artifactPath = sessionPath + QStringLiteral(".artifacts");
    const QString scopePath    = artifactPath + QStringLiteral("/.scope");
    QByteArray    expectedScope;
    QByteArray    expectedTranscript;
    if (boundScope) {
        QVERIFY(agent.submitLine("first prompt"));
        QVERIFY2(agent.waitForOutput("MOCKDONE", 30000), agent.output().right(8192).constData());
        QVERIFY(agent.waitUntil(
            [&]() { return readBytes(sessionPath).contains("\"event\":\"completed\""); }, 5000));
        QCOMPARE(readBytes(requestLog).count('\n'), 1);
        auto scope = QJsonDocument::fromJson(readBytes(scopePath)).object();
        QCOMPARE(scope.value(QStringLiteral("owner")).toString(), sessionId);
        const QString nonce = QUuid::createUuid().toString(QUuid::Id128);
        QVERIFY(nonce != scope.value(QStringLiteral("nonce")).toString());
        scope.insert(QStringLiteral("nonce"), nonce);
        expectedScope = QJsonDocument(scope).toJson(QJsonDocument::Compact);
        QFile marker(scopePath);
        QVERIFY(marker.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QCOMPARE(marker.write(expectedScope), qint64(expectedScope.size()));
        marker.close();
        expectedTranscript = readBytes(sessionPath);
    } else {
        QVERIFY(QDir().mkpath(QFileInfo(sessionPath).absolutePath()));
        QFile sentinel(QDir(external).filePath(QStringLiteral("sentinel")));
        QVERIFY(sentinel.open(QIODevice::WriteOnly));
        QCOMPARE(sentinel.write("unchanged"), qint64(9));
        sentinel.close();
        QVERIFY(QFile::link(external, artifactPath));
        QVERIFY(QFileInfo(artifactPath).isSymLink());
    }
    const qsizetype outputMark = agent.markOutput();
    QVERIFY(agent.submitLine("blocked prompt"));
    QVERIFY2(
        agent.waitForOutputAfter(
            "session persistence failed; request not started.", outputMark, 10000),
        agent.output().right(8192).constData());
    QCOMPARE(readBytes(requestLog).count('\n'), boundScope ? 1 : 0);
    QVERIFY(agent.submitLine("/quit"));
    QVERIFY2(agent.waitForExit(10000), agent.output().right(8192).constData());
    QCOMPARE(agent.exitStatus(), QProcess::NormalExit);
    QCOMPARE(agent.exitCode(), 0);
    QCOMPARE(readBytes(requestLog).count('\n'), boundScope ? 1 : 0);
    QCOMPARE(readBytes(sessionPath), expectedTranscript);
    if (boundScope) {
        QCOMPARE(readBytes(scopePath), expectedScope);
    } else {
        QVERIFY(!QFileInfo::exists(sessionPath));
        QVERIFY(QFileInfo(artifactPath).isSymLink());
        QCOMPARE(
            QDir(external).entryList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot),
            QStringList{QStringLiteral("sentinel")});
        QCOMPARE(
            readBytes(QDir(external).filePath(QStringLiteral("sentinel"))), QByteArray("unchanged"));
    }
#endif
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocagentlazystorage.moc"
