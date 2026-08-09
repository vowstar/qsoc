// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocremotejobs.h"

#include "agent/qsoctool.h"

namespace {

/* Wrap one value as a single shell word. */
QString shellQuote(const QString &value)
{
    QString result = QStringLiteral("'");
    for (const QChar ch : value) {
        if (ch == QLatin1Char('\'')) {
            result += QStringLiteral("'\\''");
        } else {
            result += ch;
        }
    }
    result += QLatin1Char('\'');
    return result;
}

const auto kUnknownScheme = QStringLiteral("unknown:");
const auto kLogFence      = QStringLiteral("log:");

/* The host-incarnation part of one token. The signal script prints the four
 * tokens past the `equal` branch only after its own boot comparison answered
 * `equal`, so this reads that answer off the script's ordering; it compares no
 * identities. Every token that establishes nothing stays Unknown. */
QSocRemoteIdentityMatch bootFromToken(QSocRemoteJobToken token)
{
    switch (token) {
    case QSocRemoteJobToken::BootMismatch:
        return QSocRemoteIdentityMatch::Differs;
    case QSocRemoteJobToken::ProcessGone:
    case QSocRemoteJobToken::PidReused:
    case QSocRemoteJobToken::Signalled:
    case QSocRemoteJobToken::SignalFailed:
        return QSocRemoteIdentityMatch::Equal;
    case QSocRemoteJobToken::Absent:
    case QSocRemoteJobToken::NoJob:
    case QSocRemoteJobToken::NoPid:
    case QSocRemoteJobToken::Unverifiable:
    case QSocRemoteJobToken::Unrecognized:
        break;
    }
    return QSocRemoteIdentityMatch::Unknown;
}

/* Whether the recorded pid is present at all. Existence is not identity, so
 * this only ever rules a pid out. */
QString pidPresenceGuard(const QString &pidRef, const QString &onAbsent)
{
    return QStringLiteral(
               "if [ -d /proc/1 ]; then [ -d \"/proc/\"%1 ] || { %2 }; "
               "elif ! LC_ALL=C ps -o pid= -p %1 >/dev/null 2>&1; then %2 fi\n")
        .arg(pidRef, onAbsent);
}

} // namespace

QSocRemoteIdentityMatch compareIdentity(const QString &recorded, const QString &live)
{
    if (recorded.isEmpty() || live.isEmpty()) {
        return QSocRemoteIdentityMatch::Unknown;
    }
    if (recorded.startsWith(kUnknownScheme) || live.startsWith(kUnknownScheme)) {
        return QSocRemoteIdentityMatch::Unknown;
    }
    const int recordedColon = recorded.indexOf(QLatin1Char(':'));
    const int liveColon     = live.indexOf(QLatin1Char(':'));
    if (recordedColon <= 0 || liveColon <= 0) {
        return QSocRemoteIdentityMatch::Unknown;
    }
    if (recorded.left(recordedColon) != live.left(liveColon)) {
        return QSocRemoteIdentityMatch::Unknown;
    }
    const QString recordedValue = recorded.mid(recordedColon + 1);
    const QString liveValue     = live.mid(liveColon + 1);
    if (recordedValue.isEmpty() || liveValue.isEmpty()) {
        return QSocRemoteIdentityMatch::Unknown;
    }
    return recordedValue == liveValue ? QSocRemoteIdentityMatch::Equal
                                      : QSocRemoteIdentityMatch::Differs;
}

QString jobTokenName(QSocRemoteJobToken token)
{
    switch (token) {
    case QSocRemoteJobToken::Absent:
        return {};
    case QSocRemoteJobToken::NoJob:
        return QStringLiteral("no_job");
    case QSocRemoteJobToken::NoPid:
        return QStringLiteral("no_pid");
    case QSocRemoteJobToken::Unverifiable:
        return QStringLiteral("unverifiable");
    case QSocRemoteJobToken::BootMismatch:
        return QStringLiteral("boot_mismatch");
    case QSocRemoteJobToken::ProcessGone:
        return QStringLiteral("process_gone");
    case QSocRemoteJobToken::PidReused:
        return QStringLiteral("pid_reused");
    case QSocRemoteJobToken::Signalled:
        return QStringLiteral("signalled");
    case QSocRemoteJobToken::SignalFailed:
        return QStringLiteral("signal_failed");
    case QSocRemoteJobToken::Unrecognized:
        break;
    }
    return QStringLiteral("unrecognized");
}

QString jobLogFence()
{
    return kLogFence;
}

QSocRemoteJobToken parseJobToken(const QString &scriptOutput)
{
    const QStringList lines = scriptOutput.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        /* The script prints the fence itself, so the first one is always its
         * own and everything past it is the job speaking. */
        if (trimmed == kLogFence) {
            break;
        }
        if (!trimmed.startsWith(QStringLiteral("token:"))) {
            continue;
        }
        const QString name = trimmed.mid(QStringLiteral("token:").size()).trimmed();
        for (const QSocRemoteJobToken token :
             {QSocRemoteJobToken::NoJob,
              QSocRemoteJobToken::NoPid,
              QSocRemoteJobToken::Unverifiable,
              QSocRemoteJobToken::BootMismatch,
              QSocRemoteJobToken::ProcessGone,
              QSocRemoteJobToken::PidReused,
              QSocRemoteJobToken::Signalled,
              QSocRemoteJobToken::SignalFailed}) {
            if (name == jobTokenName(token)) {
                return token;
            }
        }
        return QSocRemoteJobToken::Unrecognized;
    }
    return QSocRemoteJobToken::Absent;
}

QString jobScriptEvidence(const QString &scriptOutput)
{
    QStringList       kept;
    bool              inLog = false;
    const QStringList lines = scriptOutput.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (!inLog && trimmed == kLogFence) {
            inLog = true;
        } else if (
            !inLog
            && (trimmed.startsWith(QStringLiteral("token:"))
                || trimmed.startsWith(QStringLiteral("detail=")))) {
            continue;
        }
        kept.append(line);
    }
    return kept.join(QLatin1Char('\n'));
}

QString parseJobStatusExitCode(const QString &statusOutput)
{
    const QStringList lines = statusOutput.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        if (trimmed == kLogFence) {
            break;
        }
        if (trimmed.startsWith(QStringLiteral("exit_code="))) {
            return trimmed.mid(QStringLiteral("exit_code=").size()).trimmed();
        }
    }
    return {};
}

/* Ledger */

bool QSocRemoteJobLedger::note(const QSocRemoteJobRecord &record)
{
    if (record.jobId.isEmpty()) {
        return false;
    }
    if (has(record.jobId)) {
        return true;
    }
    if (m_records.size() >= kCapacity) {
        int victim = -1;
        for (int index = 0; index < m_records.size(); ++index) {
            if (m_records.at(index).settled) {
                victim = index;
                break;
            }
        }
        if (victim < 0) {
            return false;
        }
        m_records.removeAt(victim);
    }
    m_records.append(record);
    return true;
}

bool QSocRemoteJobLedger::has(const QString &jobId) const
{
    for (const QSocRemoteJobRecord &entry : m_records) {
        if (entry.jobId == jobId) {
            return true;
        }
    }
    return false;
}

QSocRemoteJobRecord QSocRemoteJobLedger::record(const QString &jobId) const
{
    for (const QSocRemoteJobRecord &entry : m_records) {
        if (entry.jobId == jobId) {
            return entry;
        }
    }
    return {};
}

bool QSocRemoteJobLedger::rebind(const QString &jobId, quint64 generation)
{
    for (QSocRemoteJobRecord &entry : m_records) {
        if (entry.jobId == jobId) {
            entry.generation = generation;
            return true;
        }
    }
    return false;
}

bool QSocRemoteJobLedger::markSettled(const QString &jobId)
{
    for (QSocRemoteJobRecord &entry : m_records) {
        if (entry.jobId == jobId) {
            entry.settled = true;
            return true;
        }
    }
    return false;
}

bool QSocRemoteJobLedger::forget(const QString &jobId)
{
    for (int index = 0; index < m_records.size(); ++index) {
        if (m_records.at(index).jobId == jobId) {
            m_records.removeAt(index);
            return true;
        }
    }
    return false;
}

QStringList QSocRemoteJobLedger::liveJobIds() const
{
    QStringList ids;
    for (const QSocRemoteJobRecord &entry : m_records) {
        if (!entry.settled) {
            ids.append(entry.jobId);
        }
    }
    return ids;
}

void QSocRemoteJobLedger::clear()
{
    m_records.clear();
}

bool QSocRemoteJobLedger::isFull() const
{
    if (m_records.size() < kCapacity) {
        return false;
    }
    for (const QSocRemoteJobRecord &entry : m_records) {
        if (entry.settled) {
            return false;
        }
    }
    return true;
}

/* Decision */

QString refuseUnrecordedJob(const QString &jobId, const QSocRemoteJobLedger &ledger)
{
    if (!jobId.isEmpty() && ledger.has(jobId)) {
        return {};
    }
    return composeJobUncertain(
        jobId,
        QStringLiteral("this job id was not recorded by this session, so no pid can be tied to it"),
        QStringLiteral(
            "nothing was signalled; re-run the work with bash(background=true) if it still "
            "needs doing"));
}

QSocRemoteJobJudgement judgeSignal(
    const QString &jobId, const QString &signalName, QSocRemoteJobToken token, const QString &detail)
{
    QSocRemoteJobJudgement judgement;
    judgement.signalled = token == QSocRemoteJobToken::Signalled;
    judgement.boot      = bootFromToken(token);
    judgement.text      = composeJobSignalResult(jobId, signalName, token, detail);
    return judgement;
}

/* Generated shell */

QString identityCompareShell()
{
    /* Shell twin of compareIdentity(). Same three answers, and the same four
     * ways to reach `unknown`: an empty string, an `unknown:` tag, a value with
     * no scheme or an empty scheme, and an empty payload. Two identities that
     * are both nothing but a scheme name have established nothing about each
     * other, so they must not compare equal. */
    return QStringLiteral(
        "__cmp() { "
        "case \"$1\" in \"\"|unknown:*|:*) echo unknown; return;; *:*) ;; "
        "*) echo unknown; return;; esac; "
        "case \"$2\" in \"\"|unknown:*|:*) echo unknown; return;; *:*) ;; "
        "*) echo unknown; return;; esac; "
        "[ -n \"${1#*:}\" ] && [ -n \"${2#*:}\" ] || { echo unknown; return; }; "
        "[ \"${1%%:*}\" = \"${2%%:*}\" ] || { echo unknown; return; }; "
        "if [ \"$1\" = \"$2\" ]; then echo equal; else echo differ; fi; }\n");
}

QString bootIdentityProbe()
{
    return QStringLiteral(
        "{ if [ -r /proc/sys/kernel/random/boot_id ]; then "
        "printf \"boot_id:%s\" \"$(cat /proc/sys/kernel/random/boot_id)\"; "
        "elif __b=$(LC_ALL=C ps -o lstart= -p 1 2>/dev/null) && [ -n \"$__b\" ]; then "
        "printf \"init_lstart:%s\" \"$__b\"; "
        "elif __b=$(sysctl -n kern.boottime 2>/dev/null) && [ -n \"$__b\" ]; then "
        "printf \"kern_boottime:%s\" \"$__b\"; "
        "else printf \"unknown:\"; fi; }");
}

QString pidStartProbe(const QString &pidRef)
{
    /* Field 22 of /proc/<pid>/stat, taken after the LAST ") " so a comm
     * containing parentheses cannot shift the field. The remainder starts at
     * field 3, so starttime is its field 20. */
    return QStringLiteral(
               "{ __st=$(cat \"/proc/\"%1\"/stat\" 2>/dev/null); "
               "if [ -n \"$__st\" ]; then __rest=${__st##*\") \"}; "
               "printf \"proc_starttime:%s\" \"$(printf \"%s\" \"$__rest\" | cut -d\" \" -f20)\"; "
               "elif __l=$(LC_ALL=C ps -o lstart= -p %1 2>/dev/null) && [ -n \"$__l\" ]; then "
               "printf \"pid_lstart:%s\" \"$__l\"; "
               "else printf \"unknown:\"; fi; }")
        .arg(pidRef);
}

QString jobLaunchScript(
    const QString &jobDir, const QString &cwd, const QString &jobId, const QString &command)
{
    /* The shell that waits for the payload is the one the link drop would
     * otherwise kill, so it ignores SIGHUP before it spawns anything. It also
     * keeps the exec channel's stdout only long enough to report the binding,
     * then releases it so the channel closes while the job runs on. */
    const QString wrapper = QStringLiteral(
                                "trap \"\" HUP\n"
                                "__d=%1\n"
                                "cd %2 || exit 1\n"
                                "printf \"%s\" %3 > \"$__d\"/command\n"
                                "date +%s > \"$__d\"/start_time\n"
                                "__boot=$(%5)\n"
                                "printf \"%s\" \"$__boot\" > \"$__d\"/boot_id\n"
                                "nohup /bin/bash -lc %3 > \"$__d\"/output.log 2>&1 &\n"
                                "__pid=$!\n"
                                "printf \"%s\" \"$__pid\" > \"$__d\"/pid\n"
                                "__start=$(%6)\n"
                                "printf \"%s\" \"$__start\" > \"$__d\"/pid_start\n"
                                "printf \"job_id=%s\\nboot_id=%s\\npid=%s\\npid_start=%s\\n\" "
                                "%4 \"$__boot\" \"$__pid\" \"$__start\"\n"
                                "exec >/dev/null 2>&1\n"
                                "wait \"$__pid\"\n"
                                "printf \"%d\" $? > \"$__d\"/exit_code\n")
                                .arg(
                                    shellQuote(jobDir),
                                    shellQuote(cwd),
                                    shellQuote(command),
                                    shellQuote(jobId),
                                    bootIdentityProbe(),
                                    pidStartProbe(QStringLiteral("\"$__pid\"")));
    return QStringLiteral("mkdir -p %1 || exit 1\nnohup /bin/bash -c %2 &\n")
        .arg(shellQuote(jobDir), shellQuote(wrapper));
}

QString jobStatusScript(const QString &jobDir, const QSocRemoteJobRecord &record)
{
    /* Every value read off disk is narrowed to digits before it is echoed. All
     * three are numeric by construction, and a state file carrying a newline
     * would otherwise print a `token:` line of its own ahead of the verdict. */
    return identityCompareShell()
           + QStringLiteral(
                 "cd %1 2>/dev/null || { echo \"token: no_job\"; exit 0; }\n"
                 "__pid=%2\n"
                 "__start=$(cat start_time 2>/dev/null | tr -dc \"0-9\")\n"
                 "__code=$(cat exit_code 2>/dev/null | tr -dc \"0-9\")\n"
                 "__bytes=$(wc -c < output.log 2>/dev/null | tr -dc \"0-9\")\n"
                 "[ -n \"$__bytes\" ] || __bytes=0\n"
                 "printf \"pid=%s\\nstart_time=%s\\nexit_code=%s\\noutput_bytes=%s\\n\" "
                 "\"$__pid\" \"$__start\" \"$__code\" \"$__bytes\"\n"
                 "case \"$__pid\" in \"\"|0|*[!0-9]*) echo \"token: no_pid\"; exit 0;; esac\n"
                 "__want_boot=%3\n"
                 "__live_boot=$(%4)\n"
                 "case \"$(__cmp \"$__want_boot\" \"$__live_boot\")\" in\n"
                 "unknown) echo \"token: unverifiable\"; exit 0;;\n"
                 "differ) echo \"token: boot_mismatch\"; exit 0;;\n"
                 "esac\n"
                 "[ -z \"$__code\" ] || { printf \"running=no\\n\"; exit 0; }\n")
                 .arg(
                     shellQuote(jobDir),
                     shellQuote(QString::number(record.pid)),
                     shellQuote(record.bootIdentity),
                     bootIdentityProbe())
           + pidPresenceGuard(
               QStringLiteral("\"$__pid\""), QStringLiteral("printf \"running=no\\n\"; exit 0;"))
           + QStringLiteral(
                 "__want_start=%1\n"
                 "__live_start=$(%2)\n"
                 "case \"$(__cmp \"$__want_start\" \"$__live_start\")\" in\n"
                 "unknown) echo \"token: unverifiable\"; exit 0;;\n"
                 "differ) echo \"token: pid_reused\"; exit 0;;\n"
                 "esac\n"
                 "printf \"running=yes\\n\"\n")
                 .arg(shellQuote(record.pidStart), pidStartProbe(QStringLiteral("\"$__pid\"")));
}

QString jobOutputScript(const QString &jobDir, int maxLines, const QSocRemoteJobRecord &record)
{
    /* The log on disk is evidence whatever the host did since, so a failed
     * identity check labels the answer instead of withholding it. The fence
     * goes out before the first byte of the log, so nothing the job wrote can
     * be read as the verdict. */
    return identityCompareShell()
           + QStringLiteral(
                 "cd %1 2>/dev/null || { echo \"token: no_job\"; exit 0; }\n"
                 "__want_boot=%2\n"
                 "__live_boot=$(%3)\n"
                 "case \"$(__cmp \"$__want_boot\" \"$__live_boot\")\" in\n"
                 "unknown) echo \"token: unverifiable\";;\n"
                 "differ) echo \"token: boot_mismatch\";;\n"
                 "esac\n"
                 "printf \"%4\\n\"\n"
                 "tail -n %5 output.log 2>&1 || true\n")
                 .arg(
                     shellQuote(jobDir),
                     shellQuote(record.bootIdentity),
                     bootIdentityProbe(),
                     kLogFence,
                     QString::number(maxLines));
}

QString jobSignalScript(const QSocRemoteJobRecord &record, const QString &signal)
{
    return identityCompareShell()
           + QStringLiteral(
                 "__pid=%1\n"
                 "case \"$__pid\" in \"\"|0|*[!0-9]*) echo \"token: no_pid\"; exit 0;; esac\n"
                 "__want_boot=%2\n"
                 "__live_boot=$(%3)\n"
                 "case \"$(__cmp \"$__want_boot\" \"$__live_boot\")\" in\n"
                 "unknown) echo \"token: unverifiable\"; exit 0;;\n"
                 "differ) echo \"token: boot_mismatch\"; exit 0;;\n"
                 "esac\n")
                 .arg(
                     shellQuote(QString::number(record.pid)),
                     shellQuote(record.bootIdentity),
                     bootIdentityProbe())
           + pidPresenceGuard(
               QStringLiteral("\"$__pid\""), QStringLiteral("echo \"token: process_gone\"; exit 0;"))
           + QStringLiteral(
                 "__want_start=%1\n"
                 "__live_start=$(%2)\n"
                 "case \"$(__cmp \"$__want_start\" \"$__live_start\")\" in\n"
                 "unknown) echo \"token: unverifiable\"; exit 0;;\n"
                 "differ) echo \"token: pid_reused\"; exit 0;;\n"
                 "esac\n"
                 "if __err=$(kill %3 \"$__pid\" 2>&1); then echo \"token: signalled\"; "
                 "else echo \"token: signal_failed\"; fi\n"
                 "[ -z \"$__err\" ] || printf \"detail=%s\\n\" \"$__err\"\n")
                 .arg(shellQuote(record.pidStart), pidStartProbe(QStringLiteral("\"$__pid\"")), signal);
}

/* Text */

QSocRemoteJobLaunchReport parseJobLaunchOutput(const QString &stdoutText)
{
    QSocRemoteJobLaunchReport report;
    const QStringList         lines = stdoutText.split(QLatin1Char('\n'));
    for (const QString &line : lines) {
        const QString trimmed = line.trimmed();
        const int     equals  = trimmed.indexOf(QLatin1Char('='));
        if (equals <= 0) {
            continue;
        }
        const QString key   = trimmed.left(equals);
        const QString value = trimmed.mid(equals + 1);
        if (key == QStringLiteral("job_id")) {
            report.jobId = value;
        } else if (key == QStringLiteral("boot_id")) {
            report.bootIdentity = value;
        } else if (key == QStringLiteral("pid_start")) {
            report.pidStart = value;
        } else if (key == QStringLiteral("pid")) {
            bool         ok  = false;
            const qint64 pid = value.toLongLong(&ok);
            report.pid       = ok && pid > 0 ? pid : 0;
        }
    }
    return report;
}

QString composeJobUncertain(const QString &jobId, const QString &reason, const QString &next)
{
    return QSocTool::statusLine(QSocTool::ResultStatus::Uncertain)
           + QStringLiteral(
                 "job_id: %1\njob_state: unknown\nsignal_sent: no\nreason: %2\nnext: %3\n")
                 .arg(jobId, reason, next);
}

QString composeJobRefusal(const QString &jobId, QSocRemoteJobToken token)
{
    QString reason;
    QString next;
    switch (token) {
    case QSocRemoteJobToken::NoJob:
        reason = QStringLiteral("no job of this id is on record");
        next   = QStringLiteral(
            "list .qsoc-agent/jobs under the workspace to find the live ids, or re-launch "
            "the work with bash(background=true)");
        break;
    case QSocRemoteJobToken::NoPid:
        reason = QStringLiteral("no pid is on record for this job");
        next   = QStringLiteral("read the log with bash_manage(action=output)");
        break;
    case QSocRemoteJobToken::Unverifiable:
        reason = QStringLiteral(
            "this host cannot identify its own incarnation or this pid, so the recorded pid "
            "cannot be shown to be the job");
        next = QStringLiteral(
            "read the log with bash_manage(action=output) to see whether the work is still "
            "progressing");
        break;
    case QSocRemoteJobToken::BootMismatch:
        reason = QStringLiteral(
            "the host restarted since the job was launched, so the recorded pid now belongs "
            "to some other process");
        next = QStringLiteral(
            "the job did not survive the restart; re-launch it if it is still needed");
        break;
    case QSocRemoteJobToken::ProcessGone:
        reason = QStringLiteral(
            "the host did not restart and the recorded pid is not present, so there is "
            "nothing to signal");
        next = QStringLiteral("read exit_code with bash_manage(action=status)");
        break;
    case QSocRemoteJobToken::PidReused:
        reason = QStringLiteral(
            "the recorded pid exists but started at a different time, so it is a different "
            "process now");
        next = QStringLiteral("re-check with bash_manage(action=status)");
        break;
    case QSocRemoteJobToken::Absent:
    case QSocRemoteJobToken::Signalled:
    case QSocRemoteJobToken::SignalFailed:
    case QSocRemoteJobToken::Unrecognized:
        reason = QStringLiteral("the host answered with no verdict this build can read");
        next   = QStringLiteral("re-run the action; the outcome of this one is unknown");
        break;
    }
    QString text = composeJobUncertain(jobId, reason, next);
    if (token != QSocRemoteJobToken::Absent) {
        text += QStringLiteral("token: %1\n").arg(jobTokenName(token));
    }
    return text;
}

QString composeJobSignalResult(
    const QString &jobId, const QString &signalName, QSocRemoteJobToken token, const QString &detail)
{
    QString text;
    if (token == QSocRemoteJobToken::Signalled) {
        text = QSocTool::statusLine(QSocTool::ResultStatus::Ok)
               + QStringLiteral(
                     "job_id: %1\njob_state: signalled\nsignal_sent: %2\n"
                     "verified: boot_identity, pid_start\n")
                     .arg(jobId, signalName);
    } else if (token == QSocRemoteJobToken::SignalFailed) {
        /* Not a refusal: the guards passed and the host declined to deliver.
         * Nothing changed and that is known, which is a failure, not a doubt. */
        text = QSocTool::statusLine(QSocTool::ResultStatus::Failed)
               + QStringLiteral(
                     "job_id: %1\njob_state: unchanged\nsignal_sent: no\n"
                     "reason: the host refused to deliver %2 to the verified pid\n")
                     .arg(jobId, signalName);
    } else {
        text = composeJobRefusal(jobId, token);
    }
    if (!detail.isEmpty()) {
        text += QStringLiteral("detail: %1\n").arg(detail.trimmed());
    }
    return text;
}

QString composeJobLaunchResult(const QSocRemoteJobRecord &record, const QString &jobDir)
{
    QString text = QSocTool::statusLine(QSocTool::ResultStatus::Ok);
    text += QStringLiteral("job_id: %1\njob_dir: %2\n").arg(record.jobId, jobDir);
    text += QStringLiteral("pid: %1\n").arg(record.pid);
    text += QStringLiteral("boot_id: %1\n").arg(record.bootIdentity);
    text += QStringLiteral("pid_start: %1\n").arg(record.pidStart);
    const bool bootUnknown  = record.bootIdentity.isEmpty()
                              || record.bootIdentity.startsWith(kUnknownScheme);
    const bool startUnknown = record.pidStart.isEmpty()
                              || record.pidStart.startsWith(kUnknownScheme);
    if (bootUnknown || startUnknown) {
        QString which = bootUnknown && startUnknown
                            ? QStringLiteral("boot identity or process start identity")
                            : (bootUnknown ? QStringLiteral("boot identity")
                                           : QStringLiteral("process start identity"));
        text += QStringLiteral(
                    "note: this host did not answer the %1 probe, so bash_manage will refuse to "
                    "signal this job once the link has been replaced\n")
                    .arg(which);
    }
    return text;
}

QString jobLedgerFullNote()
{
    return QStringLiteral(
        "note: the job ledger holds no settled record to evict, so this job was not "
        "recorded and bash_manage will refuse to signal it once the link has been "
        "replaced\n");
}
