// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocremotejobwatcher.h"

#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocremotehost.h"
#include "agent/remote/qsocsshexec.h"
#include "common/qsocmachine.h"

#include <QDateTime>

namespace {

const QByteArray kJob  = "@@job ";
const QByteArray kLog  = "@@log ";
const QByteArray kErr  = "@@err ";
const QByteArray kTail = "@@tail ";
const QByteArray kEnd  = "@@end";

/* Every payload is length-prefixed and followed by one newline, so nothing a
 * job writes can be read as a section marker. */
QString streamSection(const QString &marker, const QString &file, qint64 offset)
{
    return QStringLiteral(
               "if [ -f %2 ] && [ ! -L %2 ]; then\n"
               "__n=$(tail -c +%1 %2 2>/dev/null | head -c %3 | wc -c | tr -dc 0-9)\n"
               "printf '%4 %s\\n' \"${__n:-0}\"\n"
               "[ \"${__n:-0}\" -gt 0 ] && tail -c +%1 %2 | head -c \"$__n\"\n"
               "printf '\\n'\n"
               "fi\n")
        .arg(
            QString::number(offset + 1),
            file,
            QString::number(QSocRemoteJobWatcher::kLogChunk),
            marker);
}

QString tailSection(const QString &log)
{
    return QStringLiteral(
               "if [ -f %1 ] && [ ! -L %1 ]; then\n"
               "__t=$(tail -n %2 %1 2>/dev/null | tail -c %3)\n"
               "__n=$(printf %s \"$__t\" | wc -c | tr -dc 0-9)\n"
               "printf '@@tail %s\\n' \"${__n:-0}\"\n"
               "printf '%s\\n' \"$__t\"\n"
               "fi\n")
        .arg(
            log,
            QString::number(QSocRemoteJobWatcher::kTailLines),
            QString::number(QSocRemoteJobWatcher::kTailBytes));
}

/* The payload after a `@@log N` or `@@tail N` line ending at @p eol. */
QByteArray payloadAt(const QByteArray &output, qsizetype eol, const QByteArray &line, qsizetype *pos)
{
    const qsizetype size = line.mid(line.indexOf(' ') + 1).toLongLong();
    const qsizetype from = eol + 1;
    const qsizetype take = qBound<qsizetype>(0, size, output.size() - from);
    *pos                 = from + take;
    if (*pos < output.size() && output.at(*pos) == '\n') {
        ++*pos;
    }
    return output.mid(from, take);
}

} // namespace

QString remoteJobDir(const QSocRemoteConnection *conn, const QString &jobId)
{
    QString root = conn->canonicalWorkspace();
    if (!root.endsWith(QLatin1Char('/'))) {
        root += QLatin1Char('/');
    }
    return machineShellPath(conn->host(), root + QStringLiteral(".qsoc-agent/jobs/") + jobId);
}

QSocRemoteJobWatcher::QSocRemoteJobWatcher(QSocRemoteConnection *conn, QObject *parent)
    : QObject(parent)
    , m_conn(conn)
{
    connect(&m_timer, &QTimer::timeout, this, [this]() { pollOnce(); });
}

void QSocRemoteJobWatcher::setEnabled(bool enabled)
{
    m_enabled = enabled;
    rearm();
}

void QSocRemoteJobWatcher::follow(const QString &jobId)
{
    m_offsets.insert(jobId, 0);
    m_errOffsets.insert(jobId, 0);
    rearm();
}

void QSocRemoteJobWatcher::forget(const QString &jobId)
{
    m_offsets.remove(jobId);
    m_errOffsets.remove(jobId);
    m_quiet.remove(jobId);
    m_done.insert(jobId);
    rearm();
}

void QSocRemoteJobWatcher::kick()
{
    m_failures = 0;
    rearm();
}

void QSocRemoteJobWatcher::requestStop(const QString &jobId)
{
    queueSignal(jobId, QStringLiteral("-TERM"), true);
    m_escalate.insert(jobId, QDateTime::currentMSecsSinceEpoch() + kStopGraceMs);
}

void QSocRemoteJobWatcher::queueSignal(const QString &jobId, const QString &signal, bool byOwner)
{
    m_signals.insert(jobId, {signal, byOwner});
    m_failures = 0;
    QTimer::singleShot(0, this, [this]() { pollOnce(); });
}

QString QSocRemoteJobWatcher::signalScript() const
{
    QString script;
    for (auto it = m_signals.cbegin(); it != m_signals.cend(); ++it) {
        script += QStringLiteral("printf '@@sig %s\\n' %1\n(\n%2)\nprintf '@@end\\n'\n")
                      .arg(
                          remoteShellQuote(it.key()),
                          jobSignalScript(m_conn->jobs()->record(it.key()), it->signal));
    }
    return script;
}

void QSocRemoteJobWatcher::applySignals(const QByteArray &output)
{
    for (auto it = m_signals.cbegin(); it != m_signals.cend(); ++it) {
        const QByteArray head  = "@@sig " + it.key().toUtf8() + '\n';
        const qsizetype  from  = output.indexOf(head);
        const qsizetype  until = from < 0 ? -1 : output.indexOf(kEnd, from);
        if (until < 0) {
            continue;
        }
        const QString section = QString::fromUtf8(output.mid(from + head.size(), until - from));
        if (it->byOwner && parseJobToken(section) == QSocRemoteJobToken::Signalled) {
            m_conn->jobs()->markStopped(it.key());
        }
    }
    m_signals.clear();
}

void QSocRemoteJobWatcher::enforce(const Report &report)
{
    const QSocRemoteJobRecord record = m_conn->jobs()->record(report.jobId);
    if (!report.status.contains(QStringLiteral("running=yes"))) {
        m_escalate.remove(report.jobId);
        return;
    }
    const auto due = m_escalate.constFind(report.jobId);
    if (due != m_escalate.cend() && QDateTime::currentMSecsSinceEpoch() >= *due) {
        m_escalate.remove(report.jobId);
        queueSignal(report.jobId, QStringLiteral("-KILL"), true);
        return;
    }
    const qint64 bytes
        = parseJobStatusField(report.status, QStringLiteral("output_bytes")).toLongLong();
    if (record.maxOutputBytes > 0 && bytes > record.maxOutputBytes
        && !m_killReasons.contains(report.jobId)) {
        m_killReasons.insert(
            report.jobId, QStringLiteral("output exceeded %1 bytes").arg(record.maxOutputBytes));
        queueSignal(report.jobId, QStringLiteral("-KILL"), false);
    }
}

QList<QSocRemoteJobWatcher::Probe> QSocRemoteJobWatcher::probes() const
{
    QList<Probe> out;
    if (!m_enabled || m_conn == nullptr || !machineOffersExecTools(m_conn->host())) {
        return out;
    }
    for (const QString &id : m_conn->jobs()->liveJobIds()) {
        if (m_done.contains(id)) {
            continue;
        }
        out.append(
            {remoteJobDir(m_conn, id),
             m_conn->jobs()->record(id),
             m_offsets.value(id, -1),
             m_errOffsets.value(id, -1)});
    }
    return out;
}

QSocRemoteJobWatcher::Poll QSocRemoteJobWatcher::pollOnce()
{
    const QList<Probe> asked = probes();
    if (asked.isEmpty()) {
        rearm();
        return Poll::Idle;
    }
    if (m_conn->operationInFlight() || !m_conn->isUsable()) {
        ++m_skipped;
        rearm();
        return Poll::Skipped;
    }
    const QSocRemoteExec request
        = remoteScriptExec(m_conn->host(), signalScript() + pollScript(asked), false);
    QSocSshExec exec(*m_conn->session());
    const auto  result = exec.run(request.command, kExecTimeoutMs, request.input);
    if (result.exitCode != 0 || result.timedOut || result.aborted || result.transportDead
        || !result.errorText.isEmpty()) {
        ++m_failures;
        rearm();
        return Poll::Failed;
    }
    m_failures = 0;
    applySignals(result.stdoutBytes);
    for (const Report &report : parsePoll(result.stdoutBytes)) {
        apply(report);
        if (!m_done.contains(report.jobId)) {
            enforce(report);
        }
    }
    rearm();
    return Poll::Polled;
}

void QSocRemoteJobWatcher::apply(const Report &report)
{
    if (!m_conn->jobs()->has(report.jobId) || m_done.contains(report.jobId)) {
        return;
    }
    const bool followed = m_offsets.contains(report.jobId);
    if (followed) {
        m_offsets[report.jobId] += report.log.size();
        m_errOffsets[report.jobId] += report.err.size();
        if (!report.log.isEmpty()) {
            emit jobOutput(report.jobId, report.log, false);
        }
        if (!report.err.isEmpty()) {
            emit jobOutput(report.jobId, report.err, true);
        }
        if (report.log.size() >= kLogChunk || report.err.size() >= kLogChunk) {
            return; /* settle once both logs are drained */
        }
    } else {
        m_tails.insert(report.jobId, report.tail);
    }
    const auto    token = parseJobToken(report.status);
    const QString code  = parseJobStatusExitCode(report.status);
    if (!code.isEmpty()
        && (token == QSocRemoteJobToken::Absent || token == QSocRemoteJobToken::Unverifiable)) {
        if (token == QSocRemoteJobToken::Absent) {
            m_conn->jobs()->markSettled(report.jobId, code.toInt());
        }
        settle(report.jobId, code.toInt(), report.tail);
        return;
    }
    /* A wrapper writes exit_code just after its payload ends, so one "not
     * running" reading without a code is a race; two are a job the host lost. */
    if (report.status.contains(QStringLiteral("running=no"))) {
        ++m_quiet[report.jobId];
    }
    if (token == QSocRemoteJobToken::NoJob || token == QSocRemoteJobToken::BootMismatch
        || m_quiet.value(report.jobId) >= 2) {
        settle(report.jobId, -1, report.tail);
    }
}

void QSocRemoteJobWatcher::settle(const QString &jobId, int exitCode, const QByteArray &tail)
{
    m_done.insert(jobId);
    m_offsets.remove(jobId);
    m_errOffsets.remove(jobId);
    m_quiet.remove(jobId);
    m_escalate.remove(jobId);
    emit jobSettled(jobId, exitCode, tail);
}

void QSocRemoteJobWatcher::rearm()
{
    /* Ids the ledger no longer holds belong to a binding that is gone. */
    for (auto it = m_done.begin(); it != m_done.end();) {
        it = m_conn->jobs()->has(*it) ? std::next(it) : m_done.erase(it);
    }
    for (auto it = m_tails.begin(); it != m_tails.end();) {
        it = m_conn->jobs()->has(it.key()) ? std::next(it) : m_tails.erase(it);
    }
    for (auto it = m_killReasons.begin(); it != m_killReasons.end();) {
        it = m_conn->jobs()->has(it.key()) ? std::next(it) : m_killReasons.erase(it);
    }
    const QList<Probe> live = probes();
    if (live.isEmpty() || m_conn->session() == nullptr) {
        m_timer.stop();
        return;
    }
    bool following = false;
    for (const Probe &probe : live) {
        following = following || probe.offset >= 0;
    }
    const int interval = m_failures >= 2 ? kBackoffMs : following ? kFollowIntervalMs : kIntervalMs;
    if (!m_timer.isActive() || m_timer.interval() != interval) {
        m_timer.start(interval);
    }
}

QString QSocRemoteJobWatcher::pollScript(const QList<Probe> &probes)
{
    QString script;
    for (const Probe &probe : probes) {
        const QString out = remoteShellQuote(probe.jobDir + QStringLiteral("/output.log"));
        script += QStringLiteral("printf '@@job %s\\n' %1\n(\n%2)\n")
                      .arg(
                          remoteShellQuote(probe.record.jobId),
                          jobStatusScript(probe.jobDir, probe.record));
        if (probe.offset >= 0) {
            const QString err = remoteShellQuote(probe.jobDir + QStringLiteral("/stderr.log"));
            script += streamSection(QStringLiteral("@@log"), out, probe.offset);
            script += streamSection(QStringLiteral("@@err"), err, probe.errOffset);
        } else {
            script += tailSection(out);
        }
        script += QStringLiteral("printf '@@end\\n'\n");
    }
    return script;
}

QList<QSocRemoteJobWatcher::Report> QSocRemoteJobWatcher::parsePoll(const QByteArray &output)
{
    QList<Report> reports;
    qsizetype     pos = 0;
    while ((pos = output.indexOf(kJob, pos)) >= 0) {
        qsizetype eol = output.indexOf('\n', pos);
        if (eol < 0) {
            break;
        }
        Report report;
        report.jobId = QString::fromUtf8(output.mid(pos + kJob.size(), eol - pos - kJob.size()));
        pos          = eol + 1;
        bool ended   = false;
        while (!ended && (eol = output.indexOf('\n', pos)) >= 0) {
            const QByteArray line = output.mid(pos, eol - pos);
            pos                   = eol + 1;
            if (line == kEnd) {
                ended = true;
            } else if (line.startsWith(kLog)) {
                report.log = payloadAt(output, eol, line, &pos);
            } else if (line.startsWith(kErr)) {
                report.err = payloadAt(output, eol, line, &pos);
            } else if (line.startsWith(kTail)) {
                report.tail = payloadAt(output, eol, line, &pos);
            } else {
                report.status += QString::fromUtf8(line) + QLatin1Char('\n');
            }
        }
        if (!ended) {
            break; /* a cut-off section says nothing reliable */
        }
        reports.append(report);
    }
    return reports;
}

#include "moc_qsocremotejobwatcher.cpp"
