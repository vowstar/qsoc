// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "qsoc_test_pty.h"

#include <QTemporaryDir>
#include <QtTest>

namespace {

using namespace QSocTestPty;

/* The summary request carries this system prompt; the mock stalls on it. */
constexpr auto kSummaryMarker = "precise conversation summarizer";

QByteArray readBytes(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void escCancelsCompactionAndRunsQueuedInput_data();
    void escCancelsCompactionAndRunsQueuedInput();

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

void Test::escCancelsCompactionAndRunsQueuedInput_data()
{
    QTest::addColumn<bool>("manual");
    QTest::newRow("manual") << true;
    QTest::newRow("idle") << false;
}

void Test::escCancelsCompactionAndRunsQueuedInput()
{
#ifdef Q_OS_UNIX
    QFETCH(bool, manual);
    QTemporaryDir fixture(QDir::tempPath() + QStringLiteral("/test_qsoc_compact_cancel_XXXXXX"));
    QVERIFY(fixture.isValid());
    const QString project = QDir(fixture.path()).filePath(QStringLiteral("project"));
    const QString config  = QDir(fixture.path()).filePath(QStringLiteral("config"));
    QVERIFY(QDir().mkpath(project));
    QVERIFY(QDir().mkpath(config));
    const int port = pickFreePort();
    QVERIFY(port > 0);

    /* A request timeout far above the test bound: only ESC can end the
     * stalled summary in time. A tiny threshold makes the turn end compact. */
    QFile configFile(QDir(config).filePath(QStringLiteral("qsoc.yml")));
    QVERIFY(configFile.open(QIODevice::WriteOnly));
    const QByteArray configBytes
        = QStringLiteral(
              "llm:\n"
              "  model: mock\n"
              "  models:\n"
              "    mock:\n"
              "      name: Mock\n"
              "      url: \"http://127.0.0.1:%1/v1/chat/completions\"\n"
              "      timeout: 60000\n"
              "      context: 131072\n"
              "      max_output_tokens: 1024\n"
              "      reasoning: false\n"
              "agent:\n"
              "  predict_input: false\n"
              "  memory_recall: false\n"
              "  memory_extract: false\n"
              "  memory_dream: false\n"
              "  session_title: false\n"
              "%2"
              "proxy:\n"
              "  type: none\n")
              .arg(port)
              .arg(manual ? QString() : QStringLiteral("  compact_threshold: 0.01\n"))
              .toUtf8();
    QCOMPARE(configFile.write(configBytes), qint64(configBytes.size()));
    configFile.close();

    const QString requestLog      = QDir(fixture.path()).filePath(QStringLiteral("requests.jsonl"));
    auto          environment     = isolatedEnvironment(fixture.path());
    auto          mockEnvironment = environment;
    mockEnvironment.insert(QStringLiteral("MOCK_TTL"), QStringLiteral("120"));
    mockEnvironment.insert(QStringLiteral("MOCK_REPLY"), QStringLiteral("MOCKDONE"));
    mockEnvironment.insert(QStringLiteral("MOCK_REQUEST_LOG"), requestLog);
    mockEnvironment.insert(QStringLiteral("MOCK_HOLD"), QString::fromLatin1(kSummaryMarker));
    mockEnvironment.insert(QStringLiteral("MOCK_HOLD_MAX"), QStringLiteral("1"));
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

    /* Two turns give the summary enough messages to work on. In the idle
     * row the second turn end starts the stalled summary by itself. */
    for (const QByteArray &prompt : {QByteArray("first prompt"), QByteArray("second prompt")}) {
        const qsizetype mark = agent.markOutput();
        QVERIFY(agent.submitLine(prompt));
        QVERIFY2(
            agent.waitForOutputAfter("MOCKDONE", mark, 30000),
            agent.output().right(8192).constData());
    }
    if (manual) {
        QVERIFY(agent.submitLine("/compact"));
    }
    QVERIFY2(
        agent.waitUntil([&]() { return readBytes(requestLog).contains(kSummaryMarker); }, 15000),
        agent.output().right(8192).constData());

    const QByteArray queued = manual ? QByteArray("queued after manual compaction")
                                     : QByteArray("queued after idle compaction");
    QVERIFY(agent.submitLine(queued));
    QVERIFY(!readBytes(requestLog).contains(queued));

    const qsizetype cancelMark = agent.markOutput();
    QElapsedTimer   clock;
    clock.start();
    QVERIFY(agent.writeInput("\x1b"));
    QVERIFY2(
        agent.waitForOutputAfter("Compaction cancelled. The history is unchanged.", cancelMark, 10000),
        agent.output().right(8192).constData());
    QVERIFY2(clock.elapsed() < 10000, "ESC did not cancel the stalled summary");

    /* The input typed during compaction runs as the next turn. */
    QVERIFY2(
        agent.waitUntil([&]() { return readBytes(requestLog).contains(queued); }, 15000),
        agent.output().right(8192).constData());
    QVERIFY2(
        agent.waitForOutputAfter("MOCKDONE", cancelMark, 30000),
        agent.output().right(8192).constData());

    /* A line submitted while the turn still runs goes to the model, so let
     * the endpoint fall quiet before quitting. */
    QElapsedTimer quiet;
    quiet.start();
    qsizetype logSize = readBytes(requestLog).size();
    (void) agent.waitUntil(
        [&]() {
            const qsizetype size = readBytes(requestLog).size();
            if (size != logSize) {
                logSize = size;
                quiet.restart();
            }
            return quiet.elapsed() >= 1500;
        },
        15000);
    QVERIFY(agent.submitLine("/quit"));
    QVERIFY2(agent.waitForExit(10000), agent.output().right(8192).constData());
    QCOMPARE(agent.exitStatus(), QProcess::NormalExit);
    QCOMPARE(agent.exitCode(), 0);
#endif
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocagentcompactcancel.moc"
