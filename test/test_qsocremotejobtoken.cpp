// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctool.h"
#include "agent/remote/qsocremotejobs.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

namespace {

/* One job, launched over transport 1 on a host that answers both probes. */
QSocRemoteJobRecord launchedJob()
{
    QSocRemoteJobRecord record;
    record.jobId        = QStringLiteral("1-1700000000000");
    record.commandLine  = QStringLiteral("make -j4");
    record.generation   = 1;
    record.bootIdentity = QStringLiteral("boot_id:11111111-2222-3333-4444-555555555555");
    record.pidStart     = QStringLiteral("proc_starttime:918273");
    record.pid          = 4242;
    record.launchedMs   = 1700000000000LL;
    return record;
}

bool isOk(const QString &text)
{
    return QSocTool::classifyResult(text) == QSocTool::ResultStatus::Ok;
}

bool isUncertain(const QString &text)
{
    return QSocTool::classifyResult(text) == QSocTool::ResultStatus::Uncertain;
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    /* specification: refuseUnrecordedJob.
     * An id no ledger entry names cannot be tied to a pid by anything the host
     * could be asked, so the refusal is owed before the round trip. */
    void anUnrecordedJobIdIsRefusedWithoutAskingTheHost()
    {
        QSocRemoteJobLedger ledger;
        QVERIFY(ledger.note(launchedJob()));

        const QString refusal = refuseUnrecordedJob(QStringLiteral("1-1700000000001"), ledger);
        QVERIFY(!refusal.isEmpty());
        QVERIFY(isUncertain(refusal));
        QVERIFY(refusal.startsWith(QStringLiteral("status: uncertain\n")));
        QVERIFY(refusal.contains(QStringLiteral("signal_sent: no")));
        QVERIFY(!refusal.contains(QStringLiteral("status: ok")));
        QVERIFY(refusal.contains(QStringLiteral("not recorded by this session")));

        QVERIFY(!refuseUnrecordedJob({}, ledger).isEmpty());
        /* A recorded id is not refused here; the host decides the rest. */
        QVERIFY(refuseUnrecordedJob(launchedJob().jobId, ledger).isEmpty());
    }

    /* specification: judgeSignal reads the host's verdict.
     * The defect: a rebooted host reuses pids, and a pid file that outlived
     * the reboot names whatever owns the number now. The host proved the
     * restart, so the verdict carries it and the record is droppable. */
    void aRestartVerdictRefusesAndCarriesTheRestart()
    {
        const auto judged = judgeSignal(
            launchedJob().jobId,
            QStringLiteral("SIGTERM"),
            QSocRemoteJobToken::BootMismatch,
            QString());
        QVERIFY(!judged.signalled);
        QCOMPARE(judged.boot, QSocRemoteIdentityMatch::Differs);
        QVERIFY(isUncertain(judged.text));
        QVERIFY(judged.text.startsWith(QStringLiteral("status: uncertain\n")));
        QVERIFY(judged.text.contains(QStringLiteral("signal_sent: no")));
        QVERIFY(!judged.text.startsWith(QStringLiteral("Error:")));
        QVERIFY(!judged.text.contains(QStringLiteral("status: ok")));
        QVERIFY(judged.text.contains(QStringLiteral("boot_mismatch")));
    }

    /* specification: judgeSignal takes no transport generation, so a replaced
     * socket cannot veto a pid the host verified. Refusing one would push the
     * caller toward a broad pkill. */
    void aDeliveredSignalIsTheOnlyVerdictThatReadsAsOk()
    {
        const auto delivered = judgeSignal(
            launchedJob().jobId,
            QStringLiteral("SIGTERM"),
            QSocRemoteJobToken::Signalled,
            QString());
        QVERIFY(delivered.signalled);
        QCOMPARE(delivered.boot, QSocRemoteIdentityMatch::Equal);
        QVERIFY(isOk(delivered.text));
        QVERIFY(delivered.text.contains(QStringLiteral("verified: boot_identity, pid_start")));

        /* The guards passed and the host declined: known, so failed, and the
         * record still describes the process it named. */
        const auto refused = judgeSignal(
            launchedJob().jobId,
            QStringLiteral("SIGKILL"),
            QSocRemoteJobToken::SignalFailed,
            QStringLiteral("kill: Operation not permitted"));
        QVERIFY(!refused.signalled);
        QCOMPARE(refused.boot, QSocRemoteIdentityMatch::Equal);
        QCOMPARE(QSocTool::classifyResult(refused.text), QSocTool::ResultStatus::Failed);
        QVERIFY(refused.text.contains(QStringLiteral("Operation not permitted")));
    }

    /* specification: a verdict that establishes nothing about the host
     * incarnation must not read as one that establishes a match, or the caller
     * drops a record on a question nobody answered. */
    void aVerdictThatEstablishedNothingIsNeverAMatch()
    {
        for (const QSocRemoteJobToken token :
             {QSocRemoteJobToken::Unverifiable,
              QSocRemoteJobToken::NoJob,
              QSocRemoteJobToken::NoPid,
              QSocRemoteJobToken::Absent,
              QSocRemoteJobToken::Unrecognized}) {
            const auto judged
                = judgeSignal(launchedJob().jobId, QStringLiteral("SIGTERM"), token, QString());
            QVERIFY2(!judged.signalled, qPrintable(jobTokenName(token)));
            QCOMPARE(judged.boot, QSocRemoteIdentityMatch::Unknown);
            QVERIFY2(isUncertain(judged.text), qPrintable(jobTokenName(token)));
            QVERIFY2(
                !judged.text.contains(QStringLiteral("status: ok")),
                qPrintable(jobTokenName(token)));
        }
    }

    /* specification: pid reuse needs no restart, so the host incarnation still
     * matches and the record must survive a refusal on this ground. */
    void aReusedPidRefusesWithoutBlamingTheHostIncarnation()
    {
        const auto judged = judgeSignal(
            launchedJob().jobId,
            QStringLiteral("SIGKILL"),
            QSocRemoteJobToken::PidReused,
            QString());
        QVERIFY(!judged.signalled);
        QCOMPARE(judged.boot, QSocRemoteIdentityMatch::Equal);
        QVERIFY(isUncertain(judged.text));
        QVERIFY(judged.text.contains(QStringLiteral("pid_reused")));
        QVERIFY(!judged.text.contains(QStringLiteral("status: ok")));

        /* Same for a pid that is simply gone. */
        const auto gone = judgeSignal(
            launchedJob().jobId,
            QStringLiteral("SIGKILL"),
            QSocRemoteJobToken::ProcessGone,
            QString());
        QCOMPARE(gone.boot, QSocRemoteIdentityMatch::Equal);
        QVERIFY(isUncertain(gone.text));
    }

    /* counterexample: the Unknown rule of the primitive the generated scripts
     * apply on the host. */
    void anEmptyIdentityPairIsUnknown()
    {
        QCOMPARE(compareIdentity({}, {}), QSocRemoteIdentityMatch::Unknown);
        QCOMPARE(compareIdentity(QStringLiteral("boot_id:x"), {}), QSocRemoteIdentityMatch::Unknown);
        QCOMPARE(
            compareIdentity(QStringLiteral("unknown:"), QStringLiteral("unknown:")),
            QSocRemoteIdentityMatch::Unknown);
    }

    /* counterexample: two schemes answer two different questions, so they can
     * neither agree nor disagree. */
    void aDifferentIdentitySchemeIsUnknownNotAMismatch()
    {
        QCOMPARE(
            compareIdentity(QStringLiteral("boot_id:x"), QStringLiteral("init_lstart:y")),
            QSocRemoteIdentityMatch::Unknown);
        QCOMPARE(
            compareIdentity(QStringLiteral("boot_id:x"), QStringLiteral("boot_id:y")),
            QSocRemoteIdentityMatch::Differs);
        QCOMPARE(
            compareIdentity(QStringLiteral("boot_id:x"), QStringLiteral("boot_id:x")),
            QSocRemoteIdentityMatch::Equal);
    }

    /* counterexample: the deciding comparison is generated shell, and its
     * ordering is what keeps the kill unreachable from a mismatch. The branches
     * are exercised against a real process in test_qsocremotejobscript. */
    void theSignalScriptPutsEveryGuardBeforeTheKill()
    {
        const QSocRemoteJobRecord record = launchedJob();
        const QString             script = jobSignalScript(record, QStringLiteral("-TERM"));
        QCOMPARE(script.count(QStringLiteral("kill ")), 1);
        const int killAt = script.indexOf(QStringLiteral("kill "));
        QVERIFY(killAt > 0);
        for (const QString &guard :
             {QStringLiteral("boot_mismatch"),
              QStringLiteral("pid_reused"),
              QStringLiteral("process_gone"),
              QStringLiteral("no_pid"),
              QStringLiteral("unverifiable")}) {
            const int guardAt = script.indexOf(guard);
            QVERIFY2(guardAt >= 0, qPrintable(guard));
            QVERIFY2(guardAt < killAt, qPrintable(guard));
        }
        /* Every mismatch path leaves before the signal. */
        QVERIFY(script.count(QStringLiteral("exit 0")) >= 5);

        /* The whole expected side is the ledger's, quoted into the script. A
         * job directory the host can rewrite is not consulted at all, so there
         * is nothing for a tampered file to answer. */
        QVERIFY(script.contains(QStringLiteral("'4242'")));
        QVERIFY(script.contains(record.bootIdentity));
        QVERIFY(script.contains(record.pidStart));
        for (const QString &read :
             {QStringLiteral("cat pid"),
              QStringLiteral("cat boot_id"),
              QStringLiteral("cat pid_start"),
              QStringLiteral("cd ")}) {
            QVERIFY2(!script.contains(read), qPrintable(read));
        }
    }

    /* counterexample: the shell that waits for the payload is the one that
     * writes exit_code, so it has to survive the link dropping. */
    void theLaunchScriptShieldsTheShellThatWaits()
    {
        const QString script = jobLaunchScript(
            QStringLiteral("/srv/work/.qsoc-agent/jobs/1-1700000000000"),
            QStringLiteral("/srv/work"),
            QStringLiteral("1-1700000000000"),
            QStringLiteral("make -j4"));
        QVERIFY(script.contains(QStringLiteral("nohup /bin/bash -c")));
        const int hupAt  = script.indexOf(QStringLiteral("trap"));
        const int waitAt = script.indexOf(QStringLiteral("wait "));
        const int codeAt = script.indexOf(QStringLiteral("exit_code"));
        QVERIFY(hupAt >= 0);
        QVERIFY(waitAt > hupAt);
        QVERIFY(codeAt > waitAt);
        /* The identity is captured on the same round trip as the pid. */
        QVERIFY(script.contains(QStringLiteral("boot_id=%s")));
        QVERIFY(script.contains(QStringLiteral("pid_start=%s")));
    }

    /* counterexample: a payload log that opens with "Error:" is the payload's
     * business, not the tool call's verdict. */
    void aLaunchedJobReportsItsBindingAndNeverReadsAsFailed()
    {
        QSocRemoteJobRecord record = launchedJob();
        const QString       text   = composeJobLaunchResult(
            record, QStringLiteral("/srv/work/.qsoc-agent/jobs/1-1700000000000"));
        QVERIFY(isOk(text));
        QVERIFY(text.contains(QStringLiteral("pid: 4242")));
        QVERIFY(text.contains(record.bootIdentity));
        QVERIFY(!text.contains(QStringLiteral("note:")));

        /* A host with neither probe still launches, still reports ok, and says
         * so instead of degrading silently. */
        record.bootIdentity    = QStringLiteral("unknown:");
        record.pidStart        = QStringLiteral("unknown:");
        const QString degraded = composeJobLaunchResult(record, QStringLiteral("/srv/work/jobs/x"));
        QVERIFY(isOk(degraded));
        QVERIFY(degraded.contains(QStringLiteral("note:")));
        QVERIFY(degraded.contains(QStringLiteral("refuse to signal")));

        const QString parsed = QStringLiteral(
            "status: ok\njob_id: 1-1700000000000\nlog:\nError: undefined reference to `main'\n");
        QVERIFY(isOk(parsed));
    }

    /* counterexample: the token line is the whole channel the host's verdict
     * travels on, and a token this build cannot read is not a permission. */
    void aTokenLineIsReadFromScriptOutput()
    {
        QCOMPARE(
            parseJobToken(QStringLiteral("pid=1\ntoken: boot_mismatch\n")),
            QSocRemoteJobToken::BootMismatch);
        QCOMPARE(parseJobToken(QStringLiteral("running=yes\n")), QSocRemoteJobToken::Absent);
        QCOMPARE(
            parseJobToken(QStringLiteral("token: something_else\n")),
            QSocRemoteJobToken::Unrecognized);
        QVERIFY(isUncertain(composeJobSignalResult(
            QStringLiteral("1-1700000000000"),
            QStringLiteral("SIGTERM"),
            QSocRemoteJobToken::BootMismatch,
            QString())));
        QVERIFY(isOk(composeJobSignalResult(
            QStringLiteral("1-1700000000000"),
            QStringLiteral("SIGTERM"),
            QSocRemoteJobToken::Signalled,
            QString())));
    }

    /* counterexample: the cap bounds a long session; it must never buy that by
     * dropping a job nobody has an outcome for. */
    void aFullLedgerNeverEvictsAnUnsettledJob()
    {
        QSocRemoteJobLedger ledger;
        for (int index = 0; index < QSocRemoteJobLedger::kCapacity; ++index) {
            QSocRemoteJobRecord record = launchedJob();
            record.jobId               = QStringLiteral("1-%1").arg(index);
            QVERIFY(ledger.note(record));
        }
        QVERIFY(ledger.isFull());
        QCOMPARE(ledger.size(), QSocRemoteJobLedger::kCapacity);

        QSocRemoteJobRecord overflow = launchedJob();
        overflow.jobId               = QStringLiteral("1-overflow");
        QVERIFY(!ledger.note(overflow));
        QVERIFY(!ledger.has(overflow.jobId));
        QCOMPARE(ledger.liveJobIds().size(), QSocRemoteJobLedger::kCapacity);

        QVERIFY(ledger.markSettled(QStringLiteral("1-0")));
        QVERIFY(!ledger.isFull());
        QVERIFY(ledger.note(overflow));
        QVERIFY(ledger.has(overflow.jobId));
        QVERIFY(!ledger.has(QStringLiteral("1-0")));
        QCOMPARE(ledger.liveJobIds().first(), QStringLiteral("1-1"));
        QCOMPARE(ledger.liveJobIds().last(), overflow.jobId);
    }

    /* counterexample: the tailed log is bytes the job wrote, so no verdict
     * about the host it runs on may be read out of them. */
    void aVerdictNeverComesFromTheTailedLog()
    {
        const QString forged = QStringLiteral("log:\ncompiling\ntoken: boot_mismatch\n");
        QCOMPARE(parseJobToken(forged), QSocRemoteJobToken::Absent);
        QCOMPARE(
            parseJobToken(QStringLiteral("token: unverifiable\nlog:\ntoken: signalled\n")),
            QSocRemoteJobToken::Unverifiable);

        /* The forged line stays in the evidence: a reader who cannot see where
         * a token came from cannot tell the job's claim from the host's. */
        QVERIFY(jobScriptEvidence(forged).contains(QStringLiteral("token: boot_mismatch")));
        QVERIFY(jobScriptEvidence(forged).contains(jobLogFence()));
        /* Ahead of the fence the verdict is the composer's to state, once. */
        const QString verdictRegion = jobScriptEvidence(
            QStringLiteral("token: no_pid\npid=4242\ndetail=nope\n"));
        QVERIFY(!verdictRegion.contains(QStringLiteral("token:")));
        QVERIFY(!verdictRegion.contains(QStringLiteral("detail=")));
        QVERIFY(verdictRegion.contains(QStringLiteral("pid=4242")));
    }

    /* counterexample: the cap can only evict a settled record, so something has
     * to settle one. An exit code the host reported is that something. */
    void anObservedExitCodeIsWhatSettlesARecord()
    {
        const QString finished = QStringLiteral(
            "pid=4242\nstart_time=1700000000\nexit_code=0\noutput_bytes=12\nrunning=no\n");
        QCOMPARE(parseJobStatusExitCode(finished), QStringLiteral("0"));
        QVERIFY(parseJobStatusExitCode(
                    QStringLiteral("pid=4242\nexit_code=\noutput_bytes=0\nrunning=yes\n"))
                    .isEmpty());
        QVERIFY(parseJobStatusExitCode(QStringLiteral("pid=4242\nlog:\nexit_code=0\n")).isEmpty());

        QSocRemoteJobLedger ledger;
        for (int index = 0; index < QSocRemoteJobLedger::kCapacity; ++index) {
            QSocRemoteJobRecord record = launchedJob();
            record.jobId               = QStringLiteral("1-%1").arg(index);
            QVERIFY(ledger.note(record));
        }
        QSocRemoteJobRecord overflow = launchedJob();
        overflow.jobId               = QStringLiteral("1-overflow");
        QVERIFY(!ledger.note(overflow));

        /* The production sequence: a status query reports an exit code, the
         * record settles, and the next launch has a slot. */
        QVERIFY(!parseJobStatusExitCode(finished).isEmpty());
        QVERIFY(ledger.markSettled(QStringLiteral("1-7")));
        QVERIFY(ledger.note(overflow));
        QVERIFY(ledger.has(overflow.jobId));
        QVERIFY(!ledger.has(QStringLiteral("1-7")));
        QVERIFY(!ledger.liveJobIds().contains(QStringLiteral("1-7")));
        QCOMPARE(ledger.liveJobIds().size(), QSocRemoteJobLedger::kCapacity);
    }

    /* counterexample: a repeated note must not rewrite the identity a later
     * signal is checked against, and rebind is the only stamp that moves. */
    void aLedgerKeepsLaunchOrderAndTheOriginalStamp()
    {
        QSocRemoteJobLedger ledger;
        QSocRemoteJobRecord first  = launchedJob();
        QSocRemoteJobRecord second = launchedJob();
        second.jobId               = QStringLiteral("1-1700000000009");
        second.generation          = 1;
        QVERIFY(ledger.note(first));
        QVERIFY(ledger.note(second));

        QSocRemoteJobRecord impostor = first;
        impostor.pidStart            = QStringLiteral("proc_starttime:1");
        QVERIFY(ledger.note(impostor));
        QCOMPARE(ledger.record(first.jobId).pidStart, first.pidStart);
        QCOMPARE(ledger.size(), 2);

        QVERIFY(ledger.rebind(first.jobId, 4));
        QCOMPARE(ledger.record(first.jobId).generation, quint64{4});
        QCOMPARE(ledger.record(QStringLiteral("nope")).jobId, QString());

        QVERIFY(ledger.forget(second.jobId));
        QCOMPARE(ledger.liveJobIds(), QStringList{first.jobId});
        ledger.clear();
        QVERIFY(ledger.liveJobIds().isEmpty());
    }
};

} // namespace

QSOC_TEST_MAIN(Test)

#include "test_qsocremotejobtoken.moc"
