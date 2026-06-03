// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsocpredictioncontroller.h"
#include "qsoc_test.h"

#include <QSignalSpy>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    /* The filter is the core correctness surface: it decides whether a raw
     * model line is shown to the user. shouldFilter() returns true to REJECT. */
    void shouldFilter_acceptsPlausibleInputs()
    {
        QVERIFY(!QSocPredictionController::shouldFilter("run the tests"));
        QVERIFY(!QSocPredictionController::shouldFilter("commit the changes"));
        QVERIFY(!QSocPredictionController::shouldFilter("add a unit test for this"));
        QVERIFY(!QSocPredictionController::shouldFilter("push it"));
    }

    void shouldFilter_acceptsAllowlistedSingleWords()
    {
        QVERIFY(!QSocPredictionController::shouldFilter("commit"));
        QVERIFY(!QSocPredictionController::shouldFilter("yes"));
        QVERIFY(!QSocPredictionController::shouldFilter("continue"));
        /* Slash commands are valid even as a single token. */
        QVERIFY(!QSocPredictionController::shouldFilter("/help"));
    }

    void shouldFilter_rejectsEmptyAndOversized()
    {
        QVERIFY(QSocPredictionController::shouldFilter(""));
        QVERIFY(QSocPredictionController::shouldFilter(QString(120, QLatin1Char('x'))));
        QVERIFY(
            QSocPredictionController::shouldFilter(
                "run the tests and then commit and then push and also deploy to staging now"));
    }

    void shouldFilter_rejectsBareSingleWord()
    {
        /* A single non-allowlisted word is too vague. */
        QVERIFY(QSocPredictionController::shouldFilter("foo"));
        QVERIFY(QSocPredictionController::shouldFilter("refactor"));
    }

    void shouldFilter_rejectsQuestionsAndMultiSentence()
    {
        QVERIFY(QSocPredictionController::shouldFilter("what about the edge cases?"));
        QVERIFY(QSocPredictionController::shouldFilter("Fix it. Then run tests."));
    }

    void shouldFilter_rejectsEvaluative()
    {
        QVERIFY(QSocPredictionController::shouldFilter("looks good to me"));
        QVERIFY(QSocPredictionController::shouldFilter("that works great"));
        QVERIFY(QSocPredictionController::shouldFilter("thanks for the help"));
    }

    void shouldFilter_rejectsAssistantVoice()
    {
        QVERIFY(QSocPredictionController::shouldFilter("Let me run the tests"));
        QVERIFY(QSocPredictionController::shouldFilter("I'll commit the changes"));
        QVERIFY(QSocPredictionController::shouldFilter("Here's what I found"));
    }

    void shouldFilter_rejectsFormatting()
    {
        QVERIFY(QSocPredictionController::shouldFilter("run **the** tests"));
    }

    /* With no LLM service, every request is a guarded no-op: no ghost, no
     * signal, no crash. Verifies the fail-closed default. */
    void requestPrediction_withoutServiceIsNoOp()
    {
        QSocPredictionController predictor(nullptr, nullptr);
        QSignalSpy               spy(&predictor, &QSocPredictionController::ghostReady);

        json messages = json::array();
        messages.push_back({{"role", "user"}, {"content", "hello"}});
        messages.push_back({{"role", "assistant"}, {"content", "hi"}});
        messages.push_back({{"role", "user"}, {"content", "do it"}});
        messages.push_back({{"role", "assistant"}, {"content", "done"}});

        predictor.requestPrediction(messages);

        QCOMPARE(spy.count(), 0);
        QVERIFY(!predictor.hasGhost());
    }

    void setEnabled_togglesState()
    {
        QSocPredictionController predictor(nullptr, nullptr);
        QVERIFY(predictor.isEnabled());
        predictor.setEnabled(false);
        QVERIFY(!predictor.isEnabled());
        predictor.setEnabled(true);
        QVERIFY(predictor.isEnabled());
    }

    /* cancel() on an idle controller emits nothing (no spurious clears). */
    void cancel_withoutGhostIsSilent()
    {
        QSocPredictionController predictor(nullptr, nullptr);
        QSignalSpy               spy(&predictor, &QSocPredictionController::ghostCleared);
        predictor.cancel();
        QCOMPARE(spy.count(), 0);
    }
};

QSOC_TEST_MAIN(Test)

#include "test_qsocpredictioncontroller.moc"
