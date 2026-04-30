// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocsubagenttasksource.h"

#include "agent/qsocagent.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

QSocSubAgentTaskSource::QSocSubAgentTaskSource(QObject *parent)
    : QSocTaskSource(parent)
{}

QList<QSocTask::Row> QSocSubAgentTaskSource::listTasks() const
{
    QList<QSocTask::Row> rows;
    rows.reserve(runs_.size());
    for (const RunState &run : runs_) {
        QSocTask::Row row;
        row.id          = run.id;
        row.label       = run.label;
        row.kind        = QSocTask::Kind::SubAgent;
        row.status      = run.status;
        row.startedAtMs = run.startedAtMs;
        row.canKill     = (run.status == QSocTask::Status::Running);
        QString summary = run.subagentType;
        if (run.startedAtMs > 0) {
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            const qint64 elapsedSec
                = (nowMs - run.startedAtMs > 0 ? (nowMs - run.startedAtMs) / 1000 : 0);
            summary += QStringLiteral(" · ") + QString::number(elapsedSec) + QStringLiteral("s");
        }
        row.summary = summary;
        rows.append(row);
    }
    return rows;
}

QString QSocSubAgentTaskSource::tailFor(const QString &id, int maxBytes) const
{
    for (const RunState &run : runs_) {
        if (run.id != id) {
            continue;
        }
        QString out = run.transcript;
        if (run.status == QSocTask::Status::Completed && !run.finalResult.isEmpty()) {
            out += QStringLiteral("\n=== final ===\n") + run.finalResult;
        } else if (run.status == QSocTask::Status::Failed && !run.errorText.isEmpty()) {
            out += QStringLiteral("\n=== failed ===\n") + run.errorText;
        }
        if (maxBytes > 0 && out.size() > maxBytes) {
            out = QStringLiteral("[... truncated ...]\n") + out.right(maxBytes);
        }
        return out;
    }
    /* Not in memory (evicted). Fall back to the on-disk JSONL
     * transcript so agent_status / overlay tail can still serve
     * historical runs. We render each event back to text by
     * concatenating data fields (with `=== <kind> ===` separators
     * for non-chunk events). */
    const QString path = transcriptPathFor(id);
    QFile         file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    QString rendered;
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.isEmpty()) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(line);
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject obj  = doc.object();
        const QString     kind = obj.value(QStringLiteral("kind")).toString();
        const QString     data = obj.value(QStringLiteral("data")).toString();
        if (kind == QStringLiteral("chunk") || kind == QStringLiteral("start")) {
            rendered += data;
        } else {
            rendered += QStringLiteral("\n=== ") + kind + QStringLiteral(" ===\n") + data
                        + QLatin1Char('\n');
        }
    }
    file.close();
    if (maxBytes > 0 && rendered.size() > maxBytes) {
        return QStringLiteral("[... truncated ...]\n") + rendered.right(maxBytes);
    }
    return rendered;
}

bool QSocSubAgentTaskSource::killTask(const QString &id)
{
    for (RunState &run : runs_) {
        if (run.id != id) {
            continue;
        }
        if (run.status != QSocTask::Status::Running) {
            return false;
        }
        if (run.agent != nullptr) {
            run.agent->abort();
        }
        run.status         = QSocTask::Status::Failed;
        run.errorText      = QStringLiteral("aborted by user");
        run.lastActivityMs = QDateTime::currentMSecsSinceEpoch();
        emit tasksChanged();
        return true;
    }
    return false;
}

QString QSocSubAgentTaskSource::registerRun(
    const QString &label, const QString &subagentType, QSocAgent *agent)
{
    evictStaleCompleted();

    RunState run;
    run.id             = QStringLiteral("a") + QString::number(nextSerial_++);
    run.label          = label;
    run.subagentType   = subagentType;
    run.agent          = agent;
    run.status         = QSocTask::Status::Running;
    run.startedAtMs    = QDateTime::currentMSecsSinceEpoch();
    run.lastActivityMs = run.startedAtMs;
    if (agent != nullptr) {
        agent->setParent(this);
    }
    runs_.append(run);
    appendDiskEvent(
        run.id,
        QStringLiteral("start"),
        QStringLiteral("run %1 (%2): %3").arg(run.id, run.subagentType, run.label));
    writeMeta(run);
    emit tasksChanged();
    return run.id;
}

void QSocSubAgentTaskSource::appendTranscript(const QString &id, const QString &chunk)
{
    for (RunState &run : runs_) {
        if (run.id != id) {
            continue;
        }
        run.transcript += chunk;
        if (transcriptCap_ > 0 && run.transcript.size() > transcriptCap_) {
            run.transcript = run.transcript.right(transcriptCap_);
        }
        run.lastActivityMs = QDateTime::currentMSecsSinceEpoch();
        appendDiskEvent(id, QStringLiteral("chunk"), chunk);
        return;
    }
}

void QSocSubAgentTaskSource::markCompleted(const QString &id, const QString &finalResult)
{
    for (RunState &run : runs_) {
        if (run.id != id) {
            continue;
        }
        run.status         = QSocTask::Status::Completed;
        run.finalResult    = finalResult;
        run.lastActivityMs = QDateTime::currentMSecsSinceEpoch();
        appendDiskEvent(id, QStringLiteral("final"), finalResult);
        writeMeta(run);
        emit tasksChanged();
        return;
    }
}

void QSocSubAgentTaskSource::markFailed(const QString &id, const QString &errorText)
{
    for (RunState &run : runs_) {
        if (run.id != id) {
            continue;
        }
        run.status         = QSocTask::Status::Failed;
        run.errorText      = errorText;
        run.lastActivityMs = QDateTime::currentMSecsSinceEpoch();
        appendDiskEvent(id, QStringLiteral("error"), errorText);
        writeMeta(run);
        emit tasksChanged();
        return;
    }
}

void QSocSubAgentTaskSource::setIsolationMetadata(
    const QString &id, const QString &isolation, const QString &worktreePath)
{
    for (RunState &run : runs_) {
        if (run.id != id) {
            continue;
        }
        run.isolation    = isolation;
        run.worktreePath = worktreePath;
        writeMeta(run);
        return;
    }
}

bool QSocSubAgentTaskSource::hasActiveRun() const
{
    return countRunning() > 0;
}

int QSocSubAgentTaskSource::countRunning() const
{
    int count = 0;
    for (const RunState &run : runs_) {
        if (run.status == QSocTask::Status::Running) {
            ++count;
        }
    }
    return count;
}

int QSocSubAgentTaskSource::runCount() const
{
    return static_cast<int>(runs_.size());
}

void QSocSubAgentTaskSource::abortAll()
{
    for (RunState &run : runs_) {
        if (run.status == QSocTask::Status::Running && run.agent != nullptr) {
            run.agent->abort();
        }
    }
}

bool QSocSubAgentTaskSource::queueRequestFor(const QString &id, const QString &message)
{
    for (RunState &run : runs_) {
        if (run.id != id) {
            continue;
        }
        if (run.status != QSocTask::Status::Running || run.agent == nullptr) {
            return false;
        }
        run.agent->queueRequest(message);
        run.lastActivityMs = QDateTime::currentMSecsSinceEpoch();
        return true;
    }
    return false;
}

bool QSocSubAgentTaskSource::findRow(const QString &id, QSocTask::Row *out) const
{
    for (const RunState &run : runs_) {
        if (run.id != id) {
            continue;
        }
        if (out != nullptr) {
            out->id          = run.id;
            out->label       = run.label;
            out->summary     = run.subagentType;
            out->kind        = QSocTask::Kind::SubAgent;
            out->status      = run.status;
            out->startedAtMs = run.startedAtMs;
            out->canKill     = (run.status == QSocTask::Status::Running);
        }
        return true;
    }
    return false;
}

qint64 QSocSubAgentTaskSource::elapsedSecondsFor(const QString &id) const
{
    for (const RunState &run : runs_) {
        if (run.id != id) {
            continue;
        }
        if (run.startedAtMs <= 0) {
            return 0;
        }
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const qint64 delta = nowMs - run.startedAtMs;
        return delta > 0 ? delta / 1000 : 0;
    }
    return 0;
}

QString QSocSubAgentTaskSource::subagentTypeFor(const QString &id) const
{
    for (const RunState &run : runs_) {
        if (run.id == id) {
            return run.subagentType;
        }
    }
    return {};
}

QString QSocSubAgentTaskSource::transcriptDir() const
{
    if (!transcriptDir_.isEmpty()) {
        return transcriptDir_;
    }
    QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    if (base.isEmpty()) {
        base = QDir::tempPath() + QStringLiteral("/qsoc-agents");
    } else {
        base += QStringLiteral("/qsoc/agents");
    }
    return base;
}

QString QSocSubAgentTaskSource::transcriptPathFor(const QString &id) const
{
    return QDir(transcriptDir()).filePath(id + QStringLiteral(".jsonl"));
}

QString QSocSubAgentTaskSource::metaPathFor(const QString &id) const
{
    return QDir(transcriptDir()).filePath(id + QStringLiteral(".meta.json"));
}

void QSocSubAgentTaskSource::appendDiskEvent(
    const QString &id, const QString &kind, const QString &data) const
{
    if (data.isEmpty() && kind != QStringLiteral("start")) {
        return;
    }
    const QString dir = transcriptDir();
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(id + QStringLiteral(".jsonl"));
    QFile         file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return;
    }
    QJsonObject obj;
    obj["ts"]                   = QDateTime::currentMSecsSinceEpoch();
    obj["kind"]                 = kind;
    obj["data"]                 = data;
    const QByteArray serialized = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    file.write(serialized);
    file.write("\n");
    file.close();
}

void QSocSubAgentTaskSource::writeMeta(const RunState &run) const
{
    const QString dir = transcriptDir();
    QDir().mkpath(dir);
    const QString path = QDir(dir).filePath(run.id + QStringLiteral(".meta.json"));
    QJsonObject   meta;
    meta["task_id"]       = run.id;
    meta["label"]         = run.label;
    meta["subagent_type"] = run.subagentType;
    meta["started_at_ms"] = run.startedAtMs;
    const char *statusStr = "running";
    switch (run.status) {
    case QSocTask::Status::Running:
        statusStr = "running";
        break;
    case QSocTask::Status::Completed:
        statusStr = "completed";
        break;
    case QSocTask::Status::Failed:
        statusStr = "failed";
        break;
    case QSocTask::Status::Pending:
        statusStr = "pending";
        break;
    case QSocTask::Status::Idle:
        statusStr = "idle";
        break;
    case QSocTask::Status::Stuck:
        statusStr = "stuck";
        break;
    }
    meta["status"]    = QString::fromLatin1(statusStr);
    meta["isolation"] = run.isolation.isEmpty() ? QStringLiteral("none") : run.isolation;
    if (!run.worktreePath.isEmpty()) {
        meta["worktree"] = run.worktreePath;
    }
    if (run.status == QSocTask::Status::Completed || run.status == QSocTask::Status::Failed) {
        meta["finished_at_ms"] = run.lastActivityMs;
        if (!run.finalResult.isEmpty()) {
            meta["final_preview"] = run.finalResult.left(256);
        }
        if (!run.errorText.isEmpty()) {
            meta["error"] = run.errorText;
        }
    }
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return;
    }
    file.write(QJsonDocument(meta).toJson(QJsonDocument::Indented));
    file.close();
}

QList<QSocSubAgentTaskSource::HistoricalRun> QSocSubAgentTaskSource::loadHistoricalRuns(
    int staleAgeSec)
{
    historical_.clear();
    const QString dirPath = transcriptDir();
    QDir          dir(dirPath);
    if (!dir.exists()) {
        return {};
    }
    const QStringList entries
        = dir.entryList({QStringLiteral("*.meta.json")}, QDir::Files | QDir::Readable);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    for (const QString &name : entries) {
        const QString path = dir.filePath(name);
        QFile         file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            continue;
        }
        const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
        file.close();
        if (!doc.isObject()) {
            continue;
        }
        const QJsonObject obj = doc.object();
        HistoricalRun     run;
        run.id           = obj.value(QStringLiteral("task_id")).toString();
        run.label        = obj.value(QStringLiteral("label")).toString();
        run.subagentType = obj.value(QStringLiteral("subagent_type")).toString();
        run.status       = obj.value(QStringLiteral("status")).toString();
        run.startedAtMs  = obj.value(QStringLiteral("started_at_ms")).toVariant().toLongLong();
        run.finishedAtMs = obj.value(QStringLiteral("finished_at_ms")).toVariant().toLongLong();
        run.isolation    = obj.value(QStringLiteral("isolation")).toString();
        run.worktreePath = obj.value(QStringLiteral("worktree")).toString();
        run.error        = obj.value(QStringLiteral("error")).toString();
        run.finalPreview = obj.value(QStringLiteral("final_preview")).toString();
        if (run.id.isEmpty()) {
            continue;
        }
        /* Resurrect-prevention: a running meta older than staleAgeSec
         * is necessarily from a dead process. Rewrite it as failed
         * so it never appears as live again. */
        if (run.status == QStringLiteral("running") && run.startedAtMs > 0
            && (nowMs - run.startedAtMs) / 1000 > staleAgeSec) {
            run.status          = QStringLiteral("failed");
            run.error           = QStringLiteral("process restart (sub-agent did not finish)");
            run.finishedAtMs    = nowMs;
            QJsonObject patched = obj;
            patched["status"]   = run.status;
            patched["error"]    = run.error;
            patched["finished_at_ms"] = run.finishedAtMs;
            QFile out(path);
            if (out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
                out.write(QJsonDocument(patched).toJson(QJsonDocument::Indented));
                out.close();
            }
        }
        historical_.append(run);
    }
    /* Newest first. */
    std::sort(
        historical_.begin(),
        historical_.end(),
        [](const HistoricalRun &lhs, const HistoricalRun &rhs) {
            return lhs.startedAtMs > rhs.startedAtMs;
        });
    return historical_;
}

void QSocSubAgentTaskSource::evictStaleCompleted()
{
    const qint64 nowMs   = QDateTime::currentMSecsSinceEpoch();
    bool         changed = false;
    for (int i = static_cast<int>(runs_.size()) - 1; i >= 0; --i) {
        const RunState &run = runs_[i];
        const bool      finished
            = (run.status == QSocTask::Status::Completed || run.status == QSocTask::Status::Failed);
        if (!finished) {
            continue;
        }
        if (nowMs - run.lastActivityMs < completionTtlMs_) {
            continue;
        }
        if (run.agent != nullptr) {
            run.agent->deleteLater();
        }
        runs_.removeAt(i);
        changed = true;
    }
    if (changed) {
        emit tasksChanged();
    }
}
