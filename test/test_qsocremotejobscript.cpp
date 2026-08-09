// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocremotejobs.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

/*
 * The signal script is the one place a recorded identity is compared against a
 * live one to decide whether to signal, and it is generated shell, which no
 * compiler and no static analyser reads. Every case here is a counterexample:
 * it runs the real script against a real process and asserts on the process,
 * not on the text. After every branch that refuses, the target must still be
 * alive; remove a guard from the script and the matching case fails.
 *
 * No sshd is needed. The script is POSIX shell, and the expected side of every
 * comparison is a literal the client baked in, so running it locally exercises
 * exactly the code path a remote host would run. A reboot is simulated by
 * recording a well-formed value of the same scheme, which is precisely what a
 * reboot leaves behind. The job directory is written too, so a case can rewrite
 * it and prove the signal does not follow it.
 */

namespace {

/* One captured identity, as the launcher records it. */
QString probeOnce(const QString &shell, const QString &script)
{
    QProcess proc;
    proc.start(shell, {QStringLiteral("-c"), script});
    if (!proc.waitForStarted(5000) || !proc.waitForFinished(10000)) {
        return {};
    }
    return QString::fromUtf8(proc.readAllStandardOutput()).trimmed();
}

/* Wrap one value as a single shell word, as the generator does. */
QString quoteWord(const QString &value)
{
    QString result = QStringLiteral("'");
    for (const QChar ch : value) {
        if (ch == QLatin1Char('\'')) {
            result += QStringLiteral("'\\''");
        } else {
            result += ch;
        }
    }
    return result + QLatin1Char('\'');
}

QString matchName(QSocRemoteIdentityMatch match)
{
    switch (match) {
    case QSocRemoteIdentityMatch::Equal:
        return QStringLiteral("equal");
    case QSocRemoteIdentityMatch::Differs:
        return QStringLiteral("differ");
    case QSocRemoteIdentityMatch::Unknown:
        break;
    }
    return QStringLiteral("unknown");
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    /* Reaping after every case, failed or not: a case that aborts mid-way must
     * not leave a live process behind for the next one to mistake for its own,
     * or one broken guard reads as seven. */
    void cleanup() { reap(); }
    void aVerifiedJobIsSignalled();
    void aTamperedJobDirectoryCannotRedirectTheSignal();
    void aRecordThatCannotBeVerifiedNeverReportsVerified();
    void aHostileRecordedIdentityIsAShellLiteral();
    void aRebootedHostLeavesTheRecycledPidAlone();
    void aRecycledPidIsLeftAlone();
    void anUnverifiableIdentityLeavesTheProcessAlone();
    void anIdentityFromAnotherSchemeLeavesTheProcessAlone();
    void aRecordWithNoPidSignalsNothing();
    void aTailedLogCannotForgeTheVerdict();
    void aStateFileCannotForgeTheVerdict();
    void theShellComparisonAnswersEveryPairLikeItsReference();

private:
    /* Start a process that will outlive the case unless the script kills it. */
    bool startVictim()
    {
        m_victim.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("sleep 30")});
        return m_victim.waitForStarted(5000);
    }

    /* A second process no job ever named. Nothing in any case may signal it. */
    bool startBystander()
    {
        m_bystander
            .start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("sleep 30")});
        return m_bystander.waitForStarted(5000);
    }

    /* Record the launch, and write the job directory the launcher writes. The
     * two agree here; a case that makes them disagree is testing which one the
     * script obeys. */
    bool seedJob()
    {
        m_jobDir = m_dir.path() + QStringLiteral("/job");
        if (!QDir().mkpath(m_jobDir)) {
            return false;
        }
        const qint64  pid   = m_victim.processId();
        const QString boot  = probeOnce(m_shell, bootIdentityProbe());
        const QString start = probeOnce(m_shell, pidStartProbe(QStringLiteral("%1").arg(pid)));
        if (boot.isEmpty() || start.isEmpty()) {
            return false;
        }
        m_record              = {};
        m_record.jobId        = QStringLiteral("1-1700000000000");
        m_record.commandLine  = QStringLiteral("sleep 30");
        m_record.generation   = 1;
        m_record.bootIdentity = boot;
        m_record.pidStart     = start;
        m_record.pid          = pid;
        return writeFile(QStringLiteral("pid"), QString::number(pid))
               && writeFile(QStringLiteral("boot_id"), boot)
               && writeFile(QStringLiteral("pid_start"), start)
               && writeFile(QStringLiteral("output.log"), QStringLiteral("started\n"));
    }

    bool writeFile(const QString &name, const QString &content) const
    {
        QFile file(m_jobDir + QLatin1Char('/') + name);
        if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return false;
        }
        file.write(content.toUtf8());
        file.close();
        return true;
    }

    /* Run one generated script and return its stdout verbatim. */
    QString runScript(const QString &script) const
    {
        QProcess proc;
        proc.start(m_shell, {QStringLiteral("-c"), script});
        if (!proc.waitForStarted(5000) || !proc.waitForFinished(15000)) {
            return {};
        }
        return QString::fromUtf8(proc.readAllStandardOutput());
    }

    /* Run the generated signal script and return the token it reported. */
    QSocRemoteJobToken runSignal(const QSocRemoteJobRecord &record, const QString &signal) const
    {
        return parseJobToken(runScript(jobSignalScript(record, signal)));
    }

    /* Whether a process is genuinely still running.
     * waitForFinished reaps, which is what makes this authoritative:
     * kill(pid, 0) also succeeds on a killed child nobody has reaped yet, so
     * it cannot tell a survivor from a fresh corpse. */
    static bool survives(QProcess &proc)
    {
        return !proc.waitForFinished(700) && proc.state() == QProcess::Running;
    }

    bool victimSurvives() { return survives(m_victim); }

    void reap()
    {
        for (QProcess *proc : {&m_victim, &m_bystander}) {
            if (proc->state() != QProcess::NotRunning) {
                proc->kill();
                proc->waitForFinished(3000);
            }
        }
    }

    QTemporaryDir       m_dir;
    QProcess            m_victim;
    QProcess            m_bystander;
    QSocRemoteJobRecord m_record;
    QString             m_jobDir;
    QString             m_shell;
    bool                m_ready = false;
};

void Test::initTestCase()
{
    m_shell = QStandardPaths::findExecutable(QStringLiteral("sh"));
    if (m_shell.isEmpty()) {
        m_shell = QStringLiteral("/bin/sh");
    }
    m_ready = QFile::exists(m_shell) && m_dir.isValid();
}

void Test::cleanupTestCase()
{
    reap();
    /* QSOC_TEST_MAIN calls _exit(), so QTemporaryDir's destructor never runs. */
    QVERIFY2(m_dir.remove(), qPrintable(m_dir.errorString()));
}

void Test::aVerifiedJobIsSignalled()
{
    if (!m_ready) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a POSIX shell"));
    }
    QVERIFY(startVictim());
    if (!seedJob()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a readable host boot or pid identity"));
    }
    QVERIFY(victimSurvives());

    QCOMPARE(runSignal(m_record, QStringLiteral("-TERM")), QSocRemoteJobToken::Signalled);
    QVERIFY2(
        m_victim.waitForFinished(5000), "the identities matched and the process was not signalled");
}

void Test::aTamperedJobDirectoryCannotRedirectTheSignal()
{
    if (!m_ready) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a POSIX shell"));
    }
    QVERIFY(startVictim());
    if (!seedJob()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a readable host boot or pid identity"));
    }
    QVERIFY(startBystander());

    /* The job directory sits in a writable remote directory, so every file in
     * it is an expectation the attacker supplies. Here all three describe an
     * unrelated live process, consistently and with live values. */
    const qint64 stranger = m_bystander.processId();
    QVERIFY(writeFile(QStringLiteral("pid"), QString::number(stranger)));
    QVERIFY(writeFile(QStringLiteral("boot_id"), probeOnce(m_shell, bootIdentityProbe())));
    QVERIFY(writeFile(
        QStringLiteral("pid_start"),
        probeOnce(m_shell, pidStartProbe(QStringLiteral("%1").arg(stranger)))));

    const auto token = runSignal(m_record, QStringLiteral("-KILL"));
    /* Asserted first: a signalled process cannot be un-signalled, so the
     * bystander's fate is the finding and everything else is explanation. */
    QVERIFY2(
        survives(m_bystander),
        "the job directory redirected the signal to a process no job ever named");
    QCOMPARE(token, QSocRemoteJobToken::Signalled);
    QVERIFY2(m_victim.waitForFinished(5000), "the recorded pid was not the one signalled");
}

void Test::aRecordThatCannotBeVerifiedNeverReportsVerified()
{
    if (!m_ready) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a POSIX shell"));
    }
    QVERIFY(startVictim());
    if (!seedJob()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a readable host boot or pid identity"));
    }

    /* The host answered no boot probe at launch, and the job directory says
     * otherwise. An answer nobody gave cannot be supplied by the host later. */
    QSocRemoteJobRecord record = m_record;
    record.bootIdentity        = QStringLiteral("unknown:");

    const auto token = runSignal(record, QStringLiteral("-KILL"));
    QVERIFY2(victimSurvives(), "a job with no recorded host identity was signalled anyway");
    QCOMPARE(token, QSocRemoteJobToken::Unverifiable);
    const QString text
        = composeJobSignalResult(record.jobId, QStringLiteral("SIGKILL"), token, QString());
    /* The attestation names what was compared, so it may only appear when
     * something was. */
    QVERIFY2(!text.contains(QStringLiteral("verified")), qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("status: uncertain")));
}

void Test::aHostileRecordedIdentityIsAShellLiteral()
{
    if (!m_ready) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a POSIX shell"));
    }
    QVERIFY(startVictim());
    if (!seedJob()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a readable host boot or pid identity"));
    }
    QVERIFY(startBystander());

    /* The recorded identity was read off the host's stdout at launch, so its
     * bytes are the host's to choose. Interpolated into the script rather than
     * quoted into it, this one runs, and what it runs is a kill of its own. */
    QSocRemoteJobRecord record = m_record;
    record.bootIdentity        = QStringLiteral("boot_id:$(kill -KILL ")
                                 + QString::number(m_bystander.processId()) + QStringLiteral(")");

    const auto token = runSignal(record, QStringLiteral("-KILL"));
    QVERIFY2(survives(m_bystander), "a recorded identity ran as shell and killed a process");
    QCOMPARE(token, QSocRemoteJobToken::BootMismatch);
    QVERIFY(victimSurvives());
}

void Test::aRebootedHostLeavesTheRecycledPidAlone()
{
    if (!m_ready) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a POSIX shell"));
    }
    QVERIFY(startVictim());
    if (!seedJob()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a readable host boot or pid identity"));
    }

    /* What a reboot leaves behind: a well-formed identity of the same scheme
     * that no longer matches the running host. */
    QSocRemoteJobRecord record = m_record;
    record.bootIdentity        = record.bootIdentity.section(QLatin1Char(':'), 0, 0)
                                 + QStringLiteral(":00000000-0000-4000-8000-000000000000");

    const auto token = runSignal(record, QStringLiteral("-KILL"));
    /* Asserted before the token: a signalled process cannot be un-signalled,
     * so the liveness check is the finding and the token is the explanation. */
    QVERIFY2(victimSurvives(), "a job recorded before a host restart was signalled anyway");
    QCOMPARE(token, QSocRemoteJobToken::BootMismatch);
}

void Test::aRecycledPidIsLeftAlone()
{
    if (!m_ready) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a POSIX shell"));
    }
    QVERIFY(startVictim());
    if (!seedJob()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a readable host boot or pid identity"));
    }

    /* Same host, same pid, different process: the start identity is what
     * separates them, and a boot identity alone cannot. */
    QSocRemoteJobRecord record = m_record;
    record.pidStart = record.pidStart.section(QLatin1Char(':'), 0, 0) + QStringLiteral(":1");

    const auto token = runSignal(record, QStringLiteral("-KILL"));
    QVERIFY2(victimSurvives(), "a pid that is not the job's process was signalled anyway");
    QCOMPARE(token, QSocRemoteJobToken::PidReused);
}

void Test::anUnverifiableIdentityLeavesTheProcessAlone()
{
    if (!m_ready) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a POSIX shell"));
    }
    QVERIFY(startVictim());
    if (!seedJob()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a readable host boot or pid identity"));
    }

    /* An identity the launcher never captured cannot be compared, and an
     * unanswerable question is not a match. */
    QSocRemoteJobRecord record = m_record;
    record.pidStart            = QString();

    const auto token = runSignal(record, QStringLiteral("-KILL"));
    QVERIFY2(victimSurvives(), "a job with no recorded start identity was signalled anyway");
    QCOMPARE(token, QSocRemoteJobToken::Unverifiable);
}

void Test::anIdentityFromAnotherSchemeLeavesTheProcessAlone()
{
    if (!m_ready) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a POSIX shell"));
    }
    QVERIFY(startVictim());
    if (!seedJob()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a readable host boot or pid identity"));
    }

    /* A host that answered one probe scheme at launch and another now has said
     * nothing about whether it restarted, so the pair is not a mismatch and
     * still not a match. */
    const QString       live   = m_record.bootIdentity;
    QSocRemoteJobRecord record = m_record;
    record.bootIdentity = live.startsWith(QStringLiteral("boot_id:"))
                              ? QStringLiteral("kern_boottime:1")
                              : QStringLiteral("boot_id:00000000-0000-4000-8000-000000000000");

    const auto token = runSignal(record, QStringLiteral("-KILL"));
    QVERIFY2(victimSurvives(), "a job whose recorded identity is not comparable was signalled");
    QCOMPARE(token, QSocRemoteJobToken::Unverifiable);
    /* The generated comparison and its reference form answer the same pair the
     * same way; a shell twin that drifted from the rule fails here. */
    QCOMPARE(compareIdentity(record.bootIdentity, live), QSocRemoteIdentityMatch::Unknown);
}

void Test::aRecordWithNoPidSignalsNothing()
{
    if (!m_ready) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a POSIX shell"));
    }
    QVERIFY(startVictim());
    if (!seedJob()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a readable host boot or pid identity"));
    }

    /* No pid on record and a job directory naming a live one. The directory is
     * not consulted, so there is nothing to signal. */
    QSocRemoteJobRecord record = m_record;
    record.pid                 = 0;
    QCOMPARE(runSignal(record, QStringLiteral("-KILL")), QSocRemoteJobToken::NoPid);
    QVERIFY(victimSurvives());
}

void Test::aTailedLogCannotForgeTheVerdict()
{
    if (!m_ready) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a POSIX shell"));
    }
    QVERIFY(startVictim());
    if (!seedJob()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a readable host boot or pid identity"));
    }

    /* The payload writes its own log, so it can print anything the client
     * reads. A claim that the host restarted would have the client re-launch
     * work that is still running. */
    QVERIFY(writeFile(
        QStringLiteral("output.log"),
        QStringLiteral("compiling\ntoken: boot_mismatch\nstill going\n")));

    const QString observed = runScript(jobOutputScript(m_jobDir, 50, m_record));
    QCOMPARE(parseJobToken(observed), QSocRemoteJobToken::Absent);
    /* The forged line stays visible: the reader has to be able to see that the
     * job wrote it, which is what stripping it took away. */
    QVERIFY(jobScriptEvidence(observed).contains(QStringLiteral("token: boot_mismatch")));
    const int fenceAt = observed.indexOf(jobLogFence());
    QVERIFY(fenceAt >= 0);
    QVERIFY(observed.indexOf(QStringLiteral("token: boot_mismatch")) > fenceAt);
    QVERIFY(victimSurvives());
}

void Test::aStateFileCannotForgeTheVerdict()
{
    if (!m_ready) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a POSIX shell"));
    }
    QVERIFY(startVictim());
    if (!seedJob()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a readable host boot or pid identity"));
    }

    /* The status script echoes what the job directory holds, so a state file
     * with a second line in it would print a verdict of its own. */
    QVERIFY(writeFile(QStringLiteral("exit_code"), QStringLiteral("0\ntoken: boot_mismatch\n")));

    const QString status = runScript(jobStatusScript(m_jobDir, m_record));
    QCOMPARE(parseJobToken(status), QSocRemoteJobToken::Absent);
    QCOMPARE(parseJobStatusExitCode(status), QStringLiteral("0"));
    QVERIFY(status.contains(QStringLiteral("running=no")));
    QVERIFY(victimSurvives());
}

void Test::theShellComparisonAnswersEveryPairLikeItsReference()
{
    if (!m_ready) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a POSIX shell"));
    }
    struct Pair
    {
        const char *recorded;
        const char *live;
    };
    /* Every shape the two forms have to agree on, including the pair the shell
     * twin used to call equal: two identities that are nothing but a scheme. */
    static const Pair pairs[] = {
        {"boot_id:abc", "boot_id:abc"},
        {"boot_id:abc", "boot_id:def"},
        {"boot_id:abc", "init_lstart:abc"},
        {"boot_id:", "boot_id:"},
        {"boot_id:", "boot_id:abc"},
        {"boot_id:abc", "boot_id:"},
        {"", ""},
        {"boot_id:abc", ""},
        {"unknown:", "boot_id:abc"},
        {"boot_id:abc", "unknown:"},
        {"boot_id", "boot_id"},
        {":abc", ":abc"},
        {"boot_id:a:b", "boot_id:a:b"},
        {"boot_id:a:", "boot_id:a:"},
        {"boot_id:a'b", "boot_id:a'b"},
        {"pid_lstart:Mon Jan  1 00:00:00 2035", "pid_lstart:Mon Jan  1 00:00:00 2035"},
    };

    QString     script = identityCompareShell();
    QStringList expected;
    for (const Pair &pair : pairs) {
        const QString recorded = QString::fromUtf8(pair.recorded);
        const QString live     = QString::fromUtf8(pair.live);
        script += QStringLiteral("__cmp %1 %2\n").arg(quoteWord(recorded), quoteWord(live));
        expected.append(matchName(compareIdentity(recorded, live)));
    }
    const QStringList answered = runScript(script).split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    QCOMPARE(answered, expected);
}

QSOC_TEST_MAIN(Test)
#include "test_qsocremotejobscript.moc"
