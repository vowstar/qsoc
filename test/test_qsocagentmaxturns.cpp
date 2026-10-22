// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "qsoc_test_llm.h"

#include <QtCore>
#include <QtTest>

/* `qsoc agent -q` against the mock endpoint: no turn cap by default, and
 * `--max-turns` as the only way to set one. */

namespace {

struct Run
{
    int        exitCode = -1;
    QByteArray output;
};

class Fixture
{
public:
    explicit Fixture(int toolCalls)
    {
        project_ = QDir(mock_.dir.path()).filePath(QStringLiteral("project"));
        QDir().mkpath(project_);
        QDir().mkpath(QDir(mock_.dir.path()).filePath(QStringLiteral("config")));
        ready_ = mock_.start(
            {{QStringLiteral("MOCK_REPLY"), QStringLiteral("MOCKDONE")},
             {QStringLiteral("MOCK_TOOL_NAME"), QStringLiteral("bash")},
             {QStringLiteral("MOCK_TOOL_ARGS"), QStringLiteral("{\"command\":\"true\"}")},
             {QStringLiteral("MOCK_TOOL_MAX"), QString::number(toolCalls)}},
            QStringLiteral("none"));
        QFile config(QDir(mock_.dir.path()).filePath(QStringLiteral("config/qsoc.yml")));
        ready_ = ready_ && config.open(QIODevice::WriteOnly)
                 && config.write(QStringLiteral(
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
                                     "  away_summary: false\n"
                                     "proxy:\n"
                                     "  type: none\n")
                                     .arg(mock_.port)
                                     .toUtf8())
                        > 0;
    }

    bool ready() const { return ready_; }

    int requests() const { return static_cast<int>(mock_.requests().size()); }

    Run agent(const QStringList &extra) const
    {
        QSocTestPty::BoundedProcess process;
        process.setProcessEnvironment(QSocTestPty::isolatedEnvironment(mock_.dir.path()));
        process.setWorkingDirectory(project_);
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(
            QSocTestPty::builtQsoc(),
            QStringList{QStringLiteral("agent"), QStringLiteral("-d"), project_} + extra);
        Run run;
        if (!process.waitForStarted(10000) || !process.waitForFinished(120000)) {
            run.output = process.readAll();
            return run;
        }
        run.exitCode = process.exitStatus() == QProcess::NormalExit ? process.exitCode() : -1;
        run.output   = process.readAll();
        return run;
    }

private:
    QSocTestLlmMock mock_;
    QString         project_;
    bool            ready_ = false;
};

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
#ifndef Q_OS_UNIX
        QSKIP("Agent process coverage requires Unix");
#else
        QVERIFY2(!QSocTestPty::builtQsoc().isEmpty(), "the built qsoc executable was not found");
#endif
    }

    void aQueryIsNotCappedByDefault_data()
    {
        QTest::addColumn<bool>("streaming");
        QTest::newRow("stream") << true;
        QTest::newRow("no-stream") << false;
    }

    void aQueryIsNotCappedByDefault()
    {
        QFETCH(bool, streaming);
        Fixture fixture(120);
        QVERIFY(fixture.ready());

        QStringList args{QStringLiteral("-q"), QStringLiteral("work")};
        if (!streaming)
            args << QStringLiteral("--no-stream");
        const Run run = fixture.agent(args);
        QVERIFY2(run.exitCode == 0, run.output.right(4096).constData());
        QVERIFY2(run.output.contains("MOCKDONE"), run.output.right(4096).constData());
        QCOMPARE(fixture.requests(), 121);
    }

    void maxTurnsStopsAQuery_data()
    {
        QTest::addColumn<bool>("streaming");
        QTest::newRow("stream") << true;
        QTest::newRow("no-stream") << false;
    }

    void maxTurnsStopsAQuery()
    {
        QFETCH(bool, streaming);
        Fixture fixture(0);
        QVERIFY(fixture.ready());

        QStringList
            args{QStringLiteral("-q"), QStringLiteral("work"), QStringLiteral("--max-turns"), "3"};
        if (!streaming)
            args << QStringLiteral("--no-stream");
        const Run run = fixture.agent(args);
        QVERIFY2(run.exitCode == 1, run.output.right(4096).constData());
        QVERIFY2(run.output.contains("Reached max turns (3)"), run.output.right(4096).constData());
        QCOMPARE(fixture.requests(), 3);
    }

    void maxTurnsIsRejectedOutsideAQuery_data()
    {
        QTest::addColumn<QStringList>("args");
        QTest::newRow("no query") << QStringList{QStringLiteral("--max-turns"), "3"};
        QTest::newRow("zero") << QStringList{
            QStringLiteral("-q"), QStringLiteral("work"), QStringLiteral("--max-turns"), "0"};
        QTest::newRow("text") << QStringList{
            QStringLiteral("-q"), QStringLiteral("work"), QStringLiteral("--max-turns"), "many"};
    }

    void maxTurnsIsRejectedOutsideAQuery()
    {
        QFETCH(QStringList, args);
        Fixture fixture(0);
        QVERIFY(fixture.ready());

        const Run run = fixture.agent(args);
        QVERIFY2(run.exitCode == 2, run.output.right(4096).constData());
        QVERIFY2(run.output.contains("--max-turns"), run.output.right(4096).constData());
        QCOMPARE(fixture.requests(), 0);
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocagentmaxturns.moc"
