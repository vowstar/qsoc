// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshsession.h"
#include "qsoc_test.h"

#include <QElapsedTimer>
#include <QtCore>
#include <QtTest>

#include <utility>

/*
 * What one turn may spend on reconnecting, and whether a user who asks to stop
 * is made to sit through the rest of it.
 *
 * Every attempt is a full connect: TCP, handshake, auth, then the SFTP
 * subsystem. It runs on the Qt event loop, so its duration is time the
 * interface is frozen. The rebuilder here stands in for that cost with a
 * measured sleep, which is what lets the assertions be about elapsed time
 * rather than about a configured cap.
 */

namespace {

/* One attempt against a host that has stopped answering, priced at what a
 * dead-link connect really costs. Small enough to keep the suite quick,
 * proportionate enough that a per-call bound and a per-turn bound are
 * distinguishable in the measurement. */
constexpr int kAttemptCostMs = 120;

class Test : public QObject
{
    Q_OBJECT

private:
    /* A transport the test owns, parented to `scratch` so live ones are
     * countable. */
    static AgentRemoteState fakeTransport(
        QObject *scratch, const QString &target, const QString &workspace)
    {
        AgentRemoteState state;
        state.session   = new QSocSshSession(scratch);
        state.sftp      = new QSocSftpClient(*state.session);
        state.targetKey = target;
        state.workspace = workspace;
        return state;
    }

    /* A rebuilder that costs what a dead-link attempt costs and never
     * succeeds, which is the case a turn has to stay bounded through. */
    static QSocRemoteConnection::Rebuilder failingRebuilder(int *attempts)
    {
        return [attempts](const QString &, const QString &, AgentRemoteState *, QString *why) {
            ++(*attempts);
            QElapsedTimer clock;
            clock.start();
            while (clock.elapsed() < kAttemptCostMs) {
                QTest::qSleep(10);
            }
            *why = QStringLiteral("the remote host stopped responding");
            return false;
        };
    }

    /* Bind a transport whose session was never connected: that reads as
     * unusable, which is the state a dead link leaves behind. Takes the
     * connection by reference because it is pinned in place by design. */
    static void bind(QSocRemoteConnection *conn, QObject *scratch)
    {
        conn->adopt(fakeTransport(scratch, QStringLiteral("u@h:22"), QStringLiteral("/w")));
    }

private slots:
    /*
     * The budget bounds the turn, not the call. Every tool call in a turn
     * consults the workspace, so without a turn-scoped budget a turn with
     * several failing calls pays a full connect sequence per call and the
     * event loop is held for the sum of them.
     */
    void oneTurnPaysForAtMostOneReconnect()
    {
        QObject              scratch;
        QSocRemoteConnection conn;
        bind(&conn, &scratch);
        int attempts = 0;
        conn.setRebuilder(failingRebuilder(&attempts));

        QElapsedTimer turn;
        turn.start();
        QString err;
        /* Six tool calls in one turn, each finding the link dead. */
        const int calls = 6;
        for (int call = 0; call < calls; ++call) {
            conn.reconnect(&err);
        }
        const qint64 turnMs = turn.elapsed();

        /* The harm first: this is time the interface is frozen and the user
         * cannot do anything, so it is what the bound is for. */
        QVERIFY2(
            turnMs < static_cast<qint64>(calls) * kAttemptCostMs,
            qPrintable(QStringLiteral(
                           "a single turn held the event loop for %1 ms reconnecting; "
                           "one attempt costs %2 ms and each of the %3 tool calls paid "
                           "for its own")
                           .arg(turnMs)
                           .arg(kAttemptCostMs)
                           .arg(calls)));
        /* One reconnect() may try twice; six of them must not try twelve. */
        QVERIFY2(
            attempts <= 2,
            qPrintable(QStringLiteral(
                           "a single turn spent %1 connect attempts across %2 tool "
                           "calls; each one is a full connect on the event loop")
                           .arg(attempts)
                           .arg(calls)));

        /* Spent means spent, and it must not read as a host that refused. */
        QCOMPARE(conn.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::BudgetSpent);
        QVERIFY(conn.reconnectBudgetSpent());
    }

    /*
     * A spent budget is not a permanent refusal. A new user request is a new
     * intent and gets the full budget back. Without this the bound would be a
     * quality reduction rather than a bound: a link that came back between two
     * requests would never be reconnected to again.
     */
    void aNewTurnGetsItsBudgetBack()
    {
        QObject              scratch;
        QSocRemoteConnection conn;
        bind(&conn, &scratch);
        int attempts = 0;
        conn.setRebuilder(failingRebuilder(&attempts));

        QString err;
        conn.reconnect(&err);
        const int afterFirstTurn = attempts;
        QVERIFY(afterFirstTurn > 0);
        QCOMPARE(conn.reconnect(&err), QSocRemoteConnection::ReconnectOutcome::BudgetSpent);

        conn.resetReconnectBudget();
        const auto outcome = conn.reconnect(&err);
        /* attempts is written through the pointer the rebuilder captured, from
         * inside reconnect(), which valueflow does not follow: it keeps the
         * value frozen at the assignment above and calls this always false.
         * Measured here it is 4 against 2. */
        // cppcheck-suppress knownConditionTrueFalse
        QVERIFY2(
            attempts > afterFirstTurn,
            "a new user request was refused a reconnect because an earlier turn's failures had "
            "used the budget up, so a link that came back would never be reconnected to");
        QCOMPARE(outcome, QSocRemoteConnection::ReconnectOutcome::Exhausted);
    }

    /*
     * Each attempt is a full connect, so a user who asked to stop must not be
     * made to sit through the next one.
     */
    void anAbortStopsTheAttemptsThatHaveNotStartedYet()
    {
        QObject              scratch;
        QSocRemoteConnection conn;
        bind(&conn, &scratch);
        int attempts = 0;
        conn.setRebuilder(failingRebuilder(&attempts));

        bool stopping = false;
        conn.setAbortProbe([&stopping] { return stopping; });

        /* Asked to stop before anything was tried: nothing may be tried. */
        stopping = true;
        QString    err;
        const auto outcome = conn.reconnect(&err);
        /* The harm first: an attempt the user asked not to happen is a full
         * connect they have to sit through. */
        QVERIFY2(
            attempts == 0,
            qPrintable(QStringLiteral(
                           "a user who asked to stop was made to wait for %1 connect "
                           "attempt(s) anyway")
                           .arg(attempts)));
        QCOMPARE(outcome, QSocRemoteConnection::ReconnectOutcome::Aborted);

        /* And a stop that arrives between attempts ends the sequence there. */
        stopping = false;
        conn.resetReconnectBudget();
        int seen = 0;
        conn.setAbortProbe([&seen] { return seen++ > 0; });
        const auto second = conn.reconnect(&err);
        QVERIFY2(
            attempts == 1,
            qPrintable(QStringLiteral("a stop between attempts left %1 attempt(s) still to pay for")
                           .arg(attempts)));
        QCOMPARE(second, QSocRemoteConnection::ReconnectOutcome::Aborted);
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocreconnectbudget.moc"
