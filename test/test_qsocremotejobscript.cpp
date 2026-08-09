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
 * No sshd is needed. The script is POSIX shell and the guards compare files
 * the launcher wrote against the live host, so running it locally exercises
 * exactly the code path a remote host would run. A reboot is simulated by
 * rewriting boot_id with a well-formed value of the same scheme, which is
 * precisely what a reboot leaves behind.
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

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void aVerifiedJobIsSignalled();
    void aRebootedHostLeavesTheRecycledPidAlone();
    void aRecycledPidIsLeftAlone();
    void anUnverifiableIdentityLeavesTheProcessAlone();
    void anIdentityFromAnotherSchemeLeavesTheProcessAlone();
    void aMissingJobDirectorySignalsNothing();

private:
    /* Start a process that will outlive the case unless the script kills it. */
    bool startVictim()
    {
        m_victim.start(QStringLiteral("/bin/sh"), {QStringLiteral("-c"), QStringLiteral("sleep 30")});
        return m_victim.waitForStarted(5000);
    }

    /* Write the four files the launcher writes, with live identities so the
     * guards pass unless a case tampers with one. */
    bool seedJobDir()
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
        return writeFile(QStringLiteral("pid"), QString::number(pid))
               && writeFile(QStringLiteral("boot_id"), boot)
               && writeFile(QStringLiteral("pid_start"), start);
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

    /* Run the generated script and return the token it reported. */
    QSocRemoteJobToken runSignal(const QString &jobDir, const QString &signal) const
    {
        QProcess proc;
        proc.start(m_shell, {QStringLiteral("-c"), jobSignalScript(jobDir, signal)});
        if (!proc.waitForStarted(5000) || !proc.waitForFinished(15000)) {
            return QSocRemoteJobToken::Unrecognized;
        }
        return parseJobToken(QString::fromUtf8(proc.readAllStandardOutput()));
    }

    /* Whether the victim is genuinely still running.
     * waitForFinished reaps, which is what makes this authoritative:
     * kill(pid, 0) also succeeds on a killed child nobody has reaped yet, so
     * it cannot tell a survivor from a fresh corpse. */
    bool victimSurvives()
    {
        return !m_victim.waitForFinished(700) && m_victim.state() == QProcess::Running;
    }

    void reapVictim()
    {
        if (m_victim.state() != QProcess::NotRunning) {
            m_victim.kill();
            m_victim.waitForFinished(3000);
        }
    }

    QTemporaryDir m_dir;
    QProcess      m_victim;
    QString       m_jobDir;
    QString       m_shell;
    bool          m_ready = false;
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
    reapVictim();
    /* QSOC_TEST_MAIN calls _exit(), so QTemporaryDir's destructor never runs. */
    QVERIFY2(m_dir.remove(), qPrintable(m_dir.errorString()));
}

void Test::aVerifiedJobIsSignalled()
{
    if (!m_ready) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a POSIX shell"));
    }
    QVERIFY(startVictim());
    if (!seedJobDir()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a readable host boot or pid identity"));
    }
    QVERIFY(victimSurvives());

    QCOMPARE(runSignal(m_jobDir, QStringLiteral("-TERM")), QSocRemoteJobToken::Signalled);
    QVERIFY2(
        m_victim.waitForFinished(5000), "the identities matched and the process was not signalled");
    reapVictim();
}

void Test::aRebootedHostLeavesTheRecycledPidAlone()
{
    if (!m_ready) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a POSIX shell"));
    }
    QVERIFY(startVictim());
    if (!seedJobDir()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a readable host boot or pid identity"));
    }

    /* What a reboot leaves behind: a well-formed identity of the same scheme
     * that no longer matches the running host. */
    const QString recorded = probeOnce(m_shell, bootIdentityProbe());
    const QString scheme   = recorded.section(QLatin1Char(':'), 0, 0);
    QVERIFY(writeFile(
        QStringLiteral("boot_id"),
        scheme + QStringLiteral(":00000000-0000-4000-8000-000000000000")));

    const auto token = runSignal(m_jobDir, QStringLiteral("-KILL"));
    /* Asserted before the token: a signalled process cannot be un-signalled,
     * so the liveness check is the finding and the token is the explanation. */
    QVERIFY2(victimSurvives(), "a job recorded before a host restart was signalled anyway");
    QCOMPARE(token, QSocRemoteJobToken::BootMismatch);
    reapVictim();
}

void Test::aRecycledPidIsLeftAlone()
{
    if (!m_ready) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a POSIX shell"));
    }
    QVERIFY(startVictim());
    if (!seedJobDir()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a readable host boot or pid identity"));
    }

    /* Same host, same pid, different process: the start identity is what
     * separates them, and a boot identity alone cannot. */
    const QString recorded = probeOnce(m_shell, pidStartProbe(QStringLiteral("1")));
    const QString scheme   = recorded.section(QLatin1Char(':'), 0, 0);
    QVERIFY(writeFile(QStringLiteral("pid_start"), scheme + QStringLiteral(":1")));

    const auto token = runSignal(m_jobDir, QStringLiteral("-KILL"));
    QVERIFY2(victimSurvives(), "a pid that is not the job's process was signalled anyway");
    QCOMPARE(token, QSocRemoteJobToken::PidReused);
    reapVictim();
}

void Test::anUnverifiableIdentityLeavesTheProcessAlone()
{
    if (!m_ready) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a POSIX shell"));
    }
    QVERIFY(startVictim());
    if (!seedJobDir()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a readable host boot or pid identity"));
    }

    /* An identity the launcher never captured cannot be compared, and an
     * unanswerable question is not a match. */
    QVERIFY(writeFile(QStringLiteral("boot_id"), QString()));

    const auto token = runSignal(m_jobDir, QStringLiteral("-KILL"));
    QVERIFY2(victimSurvives(), "a job with no recorded identity was signalled anyway");
    QCOMPARE(token, QSocRemoteJobToken::Unverifiable);
    reapVictim();
}

void Test::anIdentityFromAnotherSchemeLeavesTheProcessAlone()
{
    if (!m_ready) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a POSIX shell"));
    }
    QVERIFY(startVictim());
    if (!seedJobDir()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a readable host boot or pid identity"));
    }

    /* A host that answered one probe scheme at launch and another now has said
     * nothing about whether it restarted, so the pair is not a mismatch and
     * still not a match. */
    const QString live     = probeOnce(m_shell, bootIdentityProbe());
    const QString recorded = live.startsWith(QStringLiteral("boot_id:"))
                                 ? QStringLiteral("kern_boottime:1")
                                 : QStringLiteral("boot_id:00000000-0000-4000-8000-000000000000");
    QVERIFY(writeFile(QStringLiteral("boot_id"), recorded));

    const auto token = runSignal(m_jobDir, QStringLiteral("-KILL"));
    QVERIFY2(victimSurvives(), "a job whose recorded identity is not comparable was signalled");
    QCOMPARE(token, QSocRemoteJobToken::Unverifiable);
    /* The generated comparison and its reference form answer the same pair the
     * same way; a shell twin that drifted from the rule fails here. */
    QCOMPARE(compareIdentity(recorded, live), QSocRemoteIdentityMatch::Unknown);
    reapVictim();
}

void Test::aMissingJobDirectorySignalsNothing()
{
    if (!m_ready) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("a POSIX shell"));
    }
    QVERIFY(startVictim());
    QCOMPARE(
        runSignal(m_dir.path() + QStringLiteral("/no-such-job"), QStringLiteral("-KILL")),
        QSocRemoteJobToken::NoJob);
    QVERIFY(victimSurvives());
    reapVictim();
}

QSOC_TEST_MAIN(Test)
#include "test_qsocremotejobscript.moc"
