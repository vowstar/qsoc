// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocsubagenttasksource.h"

#include "agent/qsocagent.h"

#include <QDateTime>

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
    return {};
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
        emit tasksChanged();
        return;
    }
}

bool QSocSubAgentTaskSource::hasActiveRun() const
{
    for (const RunState &run : runs_) {
        if (run.status == QSocTask::Status::Running) {
            return true;
        }
    }
    return false;
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
