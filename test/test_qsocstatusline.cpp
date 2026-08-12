// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsocstatusline.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

    static nlohmann::json samplePayload()
    {
        return {{"model", {{"id", "test-model"}}}, {"marker", "one"}};
    }

    /* Wait for exactly one textReady and return its text. */
    static QString waitText(QSocStatusLine &line, int timeoutMs = 8000)
    {
        QSignalSpy spy(&line, &QSocStatusLine::textReady);
        if (!spy.wait(timeoutMs)) {
            return QStringLiteral("<timeout>");
        }
        return spy.takeFirst().at(0).toString();
    }

private slots:
    void initTestCase()
    {
#ifdef Q_OS_WIN
        QSKIP("POSIX shell fixtures");
#endif
    }

    void publishesStdout()
    {
        QSocStatusLine line;
        line.setDebounceMs(0);
        line.setCommand(QStringLiteral("printf 'hello status'"));
        line.requestRefresh(samplePayload());
        QCOMPARE(waitText(line), QStringLiteral("hello status"));
    }

    void firstNonEmptyLineOnly()
    {
        QSocStatusLine line;
        line.setDebounceMs(0);
        line.setCommand(QStringLiteral("printf '\\n\\nfirst\\nsecond\\n'"));
        line.requestRefresh(samplePayload());
        QCOMPARE(waitText(line), QStringLiteral("first"));
    }

    void stdinCarriesPayload()
    {
        QSocStatusLine line;
        line.setDebounceMs(0);
        /* The child sees the payload JSON on stdin; grep a field out. */
        line.setCommand(QStringLiteral("grep -o 'test-model'"));
        line.requestRefresh(samplePayload());
        QCOMPARE(waitText(line), QStringLiteral("test-model"));
    }

    void nonZeroExitClears()
    {
        QSocStatusLine line;
        line.setDebounceMs(0);
        line.setCommand(QStringLiteral("printf 'junk'; exit 3"));
        line.requestRefresh(samplePayload());
        QCOMPARE(waitText(line), QString());
    }

    void timeoutKillsAndClears()
    {
        QSocStatusLine line;
        line.setDebounceMs(0);
        line.setTimeoutMs(200);
        /* Compound command: the shell forks the sleep, so only a
         * process-group kill reaps it. The odd duration is a unique
         * tag for the orphan probe below. */
        line.setCommand(QStringLiteral("sleep 31234; printf 'NEVER'"));
        QElapsedTimer timer;
        timer.start();
        line.requestRefresh(samplePayload());
        QCOMPARE(waitText(line), QString());
        QVERIFY(timer.elapsed() < 5000);
        QTest::qWait(100);
        QCOMPARE(
            QProcess::execute(
                QStringLiteral("pgrep"), {QStringLiteral("-f"), QStringLiteral("sleep 31234")}),
            1);
    }

    void debounceCoalescesToNewestPayload()
    {
        QSocStatusLine line;
        line.setDebounceMs(150);
        line.setCommand(QStringLiteral("grep -o 'marker-[a-z]*'"));
        nlohmann::json first  = {{"marker", "marker-old"}};
        nlohmann::json second = {{"marker", "marker-new"}};
        line.requestRefresh(first);
        line.requestRefresh(second);
        QCOMPARE(waitText(line), QStringLiteral("marker-new"));
    }

    void disableClears()
    {
        QSocStatusLine line;
        QSignalSpy     spy(&line, &QSocStatusLine::textReady);
        line.setCommand(QString());
        QCOMPARE(spy.count(), 1);
        QCOMPARE(spy.takeFirst().at(0).toString(), QString());
        /* Disabled line ignores refresh requests */
        line.requestRefresh(samplePayload());
        QVERIFY(!spy.wait(500));
    }

    void ansiPreserved()
    {
        QSocStatusLine line;
        line.setDebounceMs(0);
        line.setCommand(QStringLiteral(R"(printf '\033[31mred\033[0m')"));
        line.requestRefresh(samplePayload());
        QCOMPARE(waitText(line), QStringLiteral("\033[31mred\033[0m"));
    }

    void supersededProcessSilent()
    {
        /* A slow in-flight run must be killed by a newer request and
         * never publish. */
        QSocStatusLine line;
        line.setDebounceMs(0);
        line.setCommand(QStringLiteral("sleep 2; printf 'stale'"));
        line.requestRefresh(samplePayload());
        QTest::qWait(100); /* let the first process start */
        line.setCommand(QStringLiteral("printf 'fresh'"));
        line.requestRefresh(samplePayload());
        QCOMPARE(waitText(line), QStringLiteral("fresh"));
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocstatusline.moc"
