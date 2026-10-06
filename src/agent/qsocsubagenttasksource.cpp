// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocsubagenttasksource.h"

#include "agent/qsocagent.h"
#include "agent/qsocagentmailbox.h"
#include "agent/qsocprivatefile.h"

#include "agent/qsocsession.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <QStringList>
#include <QUuid>

#include <algorithm>
#include <limits>
#include <utility>

#ifdef Q_OS_UNIX
#include <unistd.h>
#endif

namespace {

/* JSONL event kind for a terminal state. `final` and `error` are the
 * historical wire words for a clean finish and a reported error. */
QString terminalEventKind(QSocTask::Status state)
{
    if (state == QSocTask::Status::Completed) {
        return QStringLiteral("final");
    }
    if (state == QSocTask::Status::Aborted) {
        return QStringLiteral("aborted");
    }
    return QStringLiteral("error");
}

/* Run files are `a<N>.<kind>`; the serial seeds new ids. */
const QRegularExpression &runFileName()
{
    static const QRegularExpression pattern(QStringLiteral(R"(^a([0-9]{1,9})\.)"));
    return pattern;
}

/* Child histories above this size are not kept. */
constexpr qint64 historyBytesLimit = qint64{16} * 1024 * 1024;

qint64 serializedSize(const nlohmann::json &value)
{
    try {
        return static_cast<qint64>(value.dump().size());
    } catch (const nlohmann::json::exception &) {
        return std::numeric_limits<qint64>::max();
    }
}

/* A regular file this user owns; links are never followed. */
bool trustedFile(const QString &path)
{
    const QFileInfo info(path);
    if (!info.isFile() || info.isSymLink()) {
        return false;
    }
#ifdef Q_OS_UNIX
    return info.ownerId() == ::getuid();
#else
    return true;
#endif
}

/* Replace @p path atomically; a link at @p path is refused, not followed. */
bool writeJsonFile(const QString &path, const QJsonObject &object)
{
    if (QFileInfo(path).isSymLink()) {
        return false;
    }
    QSaveFile file(path);
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        return false;
    }
    QSocPrivateFile::restrict(file);
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Indented);
    if (file.write(payload) != payload.size()) {
        file.cancelWriting();
        return false;
    }
    return file.commit();
}

/* The overlay rendering of a stored event stream: chunks verbatim, every
 * other event under a `=== <kind> ===` separator. */
QString renderEventFile(const QString &path)
{
    QFile file(path);
    if (path.isEmpty() || !file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }
    QString rendered;
    while (!file.atEnd()) {
        const QByteArray    line = file.readLine().trimmed();
        const QJsonDocument doc  = QJsonDocument::fromJson(line);
        if (line.isEmpty() || !doc.isObject()) {
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
    return rendered;
}

bool readHistoricalRun(
    const QString                         &path,
    bool                                   legacy,
    int                                    staleAgeSec,
    qint64                                 nowMs,
    QSocSubAgentTaskSource::HistoricalRun *run)
{
    QFile file(path);
    if (!trustedFile(path) || !file.open(QIODevice::ReadOnly)) {
        return false;
    }
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    file.close();
    if (!doc.isObject()) {
        return false;
    }
    const QJsonObject obj = doc.object();
    run->id               = obj.value(QStringLiteral("task_id")).toString();
    run->label            = obj.value(QStringLiteral("label")).toString();
    run->subagentType     = obj.value(QStringLiteral("subagent_type")).toString();
    run->status           = obj.value(QStringLiteral("status")).toString();
    run->startedAtMs      = obj.value(QStringLiteral("started_at_ms")).toVariant().toLongLong();
    run->finishedAtMs     = obj.value(QStringLiteral("finished_at_ms")).toVariant().toLongLong();
    run->isolation        = obj.value(QStringLiteral("isolation")).toString();
    run->worktreePath     = obj.value(QStringLiteral("worktree")).toString();
    run->error            = obj.value(QStringLiteral("error")).toString();
    run->finalPreview     = obj.value(QStringLiteral("final_preview")).toString();
    run->host             = obj.value(QStringLiteral("host")).toString();
    run->endpoint         = obj.value(QStringLiteral("endpoint")).toString(run->host);
    run->workspace        = obj.value(QStringLiteral("workspace")).toString();
    run->definition       = obj.value(QStringLiteral("definition")).toString(run->subagentType);
    run->legacy           = legacy;
    const QString history = obj.value(QStringLiteral("history_file")).toString();
    if (!legacy && runFileName().match(history).hasMatch() && !history.contains(QLatin1Char('/'))
        && !history.contains(QLatin1Char('\\'))) {
        run->historyFile = QFileInfo(path).dir().filePath(history);
    }
    if (run->id.isEmpty()) {
        return false;
    }
    /* Resurrect-prevention: a running meta older than staleAgeSec is from
     * a dead process. It was cut off, so its effects are unknown, the same
     * fact an aborted run carries; rewriting it as failed would invite a
     * retry of a run that may have finished. Legacy metas are never written. */
    if (run->status == QStringLiteral("running") && run->startedAtMs > 0
        && (nowMs - run->startedAtMs) / 1000 > staleAgeSec) {
        run->status       = QStringLiteral("aborted");
        run->error        = QStringLiteral("process restart (sub-agent did not finish)");
        run->finishedAtMs = nowMs;
        if (!legacy) {
            QJsonObject patched       = obj;
            patched["status"]         = run->status;
            patched["error"]          = run->error;
            patched["finished_at_ms"] = run->finishedAtMs;
            writeJsonFile(path, patched);
        }
    }
    return true;
}

} /* namespace */

QSocSubAgentTaskSource::QSocSubAgentTaskSource(QObject *parent)
    : QSocTaskSource(parent)
{}

void QSocSubAgentTaskSource::enableMessaging(QSocAgent *root)
{
    if (mailbox_ != nullptr)
        return;
    mailbox_ = new QSocAgentMailbox(this);
    mailbox_->registerAgent(root, QStringLiteral("main"));
    connect(root, &QSocAgent::runComplete, mailbox_, [this, root](const QString &) {
        mailbox_->setState(root->agentIdentity(), QStringLiteral("idle"));
    });
    connect(root, &QSocAgent::runError, mailbox_, [this, root](const QString &) {
        mailbox_->setState(root->agentIdentity(), QStringLiteral("idle"));
    });
    connect(root, &QSocAgent::runAborted, mailbox_, [this, root](const QString &) {
        mailbox_->setState(root->agentIdentity(), QStringLiteral("idle"));
    });
    connect(mailbox_, &QSocAgentMailbox::changed, this, [this]() {
        QStringList cancelled;
        for (const auto &run : std::as_const(runs_)) {
            if (!QSocTask::isTerminal(run.status) && run.agent
                && mailbox_->stateFor(run.agent->agentIdentity()) == QStringLiteral("cancelled"))
                cancelled.append(run.id);
        }
        for (const auto &id : cancelled)
            killTask(id);
    });
    mailbox_->setWakeHandler([this](QSocAgent *agent) { return startFollowup(agent); });
}

QString QSocSubAgentTaskSource::startFollowup(QSocAgent *agent)
{
    if (agent == nullptr || mailbox_ == nullptr || agent->isRunning())
        return {};
    const QString identity = mailbox_->idFor(agent);
    if (mailbox_->stateFor(identity) != QStringLiteral("idle"))
        return {};
    QString  isolation;
    QString  worktreePath;
    Dispatch dispatch;
    QString  definition;
    for (const auto &run : std::as_const(runs_)) {
        if (run.agent == agent) {
            isolation    = run.isolation;
            worktreePath = run.worktreePath;
            dispatch     = run.dispatch;
            definition   = run.definition;
        }
    }
    const QString id
        = registerRun(QStringLiteral("Follow-up"), QStringLiteral("continuation"), agent);
    for (RunState &run : runs_) {
        if (run.id == id) {
            run.definition = definition;
        }
    }
    setIsolationMetadata(id, isolation, worktreePath);
    setDispatchMetadata(id, dispatch);
    const auto connections = std::make_shared<QList<QMetaObject::Connection>>();
    *connections << connect(agent, &QSocAgent::contentChunk, this, [this, id](const QString &text) {
        appendTranscript(id, text);
    });
    *connections << connect(
        agent, &QSocAgent::toolCalled, this, [this, id](const QString &name, const QString &args) {
            appendTranscript(id, QStringLiteral("\n[tool] %1 %2\n").arg(name, args.left(200)));
            setWaitingForPeer(id, name == QStringLiteral("wait_agent"));
        });
    *connections << connect(
        agent, &QSocAgent::toolResult, this, [this, id](const QString &name, const QString &result) {
            appendTranscript(id, QStringLiteral("[result %1] %2\n").arg(name, result.left(400)));
            setWaitingForPeer(id, false);
        });
    *connections << connect(
        agent,
        &QSocAgent::toolCallFinished,
        this,
        [this,
         id](const QString &, const QString &name, const QString &, QSocToolResultStatus status) {
            appendTranscript(
                id, QStringLiteral("[outcome %1] %2").arg(name, QSocTool::statusLine(status)));
            emit taskEvidenceChanged(id);
        });
    *connections << connect(agent, &QSocAgent::runComplete, this, [this, id](const QString &text) {
        markCompleted(id, text);
    });
    *connections << connect(
        agent, &QSocAgent::toolCallOutput, this, [this, id](const QString &, const QString &text) {
            appendTranscript(id, text);
        });
    *connections << connect(agent, &QSocAgent::runError, this, [this, id](const QString &text) {
        markFailed(id, text);
    });
    *connections << connect(agent, &QSocAgent::runAborted, this, [this, id](const QString &text) {
        markAborted(id, text);
    });
    *connections << connect(
        this,
        &QSocTaskSource::taskTerminal,
        agent,
        [id, connections](const QString &taskId, QSocTask::Status, const QString &) {
            if (id != taskId)
                return;
            for (const auto &connection : std::as_const(*connections))
                QObject::disconnect(connection);
            connections->clear();
        });
    start(id, [agent]() { agent->resumeStream(); });
    return id;
}

void QSocSubAgentTaskSource::setWaitingForPeer(const QString &id, bool waiting)
{
    for (auto &run : runs_) {
        if (run.id != id || run.status != QSocTask::Status::Running)
            continue;
        if (run.waitingForPeer != waiting) {
            run.waitingForPeer = waiting;
            emit tasksChanged();
        }
        return;
    }
}

QList<QSocTask::Row> QSocSubAgentTaskSource::listTasks() const
{
    QList<QSocTask::Row> rows;
    rows.reserve(runs_.size());
    for (const RunState &run : runs_) {
        QSocTask::Row row;
        row.id             = run.id;
        row.label          = run.label;
        row.objective      = run.objective;
        row.waitingForPeer = run.status == QSocTask::Status::Running && run.waitingForPeer;
        row.kind           = QSocTask::Kind::SubAgent;
        row.status         = run.status;
        row.startedAtMs    = run.startedAtMs;
        row.canKill
            = (run.status == QSocTask::Status::Running || run.status == QSocTask::Status::Pending);
        row.agentId     = run.agent ? run.agent->agentIdentity() : QString();
        row.host        = run.dispatch.host;
        row.workspace   = run.dispatch.workspace;
        row.live        = liveAgentFor(run.id) != nullptr;
        row.resumable   = row.live
                          || (!run.historyFile.isEmpty()
                              && run.isolation != QStringLiteral("worktree"));
        QString summary = run.subagentType;
        if (run.status == QSocTask::Status::Pending) {
            summary += QStringLiteral(" · queued");
        }
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
    QString out;
    bool    tracked = false;
    for (const RunState &run : runs_) {
        if (run.id == id) {
            out     = renderRun(run);
            tracked = true;
            break;
        }
    }
    /* Not in memory (evicted): the stored event stream serves the tail. */
    if (!tracked) {
        out = renderEventFile(transcriptPathFor(id));
    }
    if (maxBytes > 0 && out.size() > maxBytes) {
        out = QStringLiteral("[... truncated ...]\n") + out.right(maxBytes);
    }
    return out;
}

QString QSocSubAgentTaskSource::renderRun(const RunState &run)
{
    QString out = run.transcript;
    if (run.status == QSocTask::Status::Completed && !run.finalResult.isEmpty()) {
        out += QStringLiteral("\n=== final ===\n") + run.finalResult;
    } else if (
        (run.status == QSocTask::Status::Failed || run.status == QSocTask::Status::Aborted)
        && !run.errorText.isEmpty()) {
        out += QStringLiteral("\n=== ") + QSocTask::statusWord(run.status)
               + QStringLiteral(" ===\n") + run.errorText;
    }
    return out;
}

QString QSocSubAgentTaskSource::renderedTranscript(const QString &id) const
{
    const QString stored = renderEventFile(transcriptPathFor(id));
    if (!stored.isEmpty()) {
        return stored;
    }
    for (const RunState &run : runs_) {
        if (run.id == id) {
            return renderRun(run);
        }
    }
    return {};
}

QSocSubAgentTaskSource::TranscriptPage QSocSubAgentTaskSource::transcriptPage(
    const QString &id, qint64 offset, int maxBytes) const
{
    const QByteArray bytes = renderedTranscript(id).toUtf8();
    TranscriptPage   page;
    page.offset = qBound<qint64>(0, offset, bytes.size());
    qint64 end  = maxBytes > 0 ? qMin<qint64>(bytes.size(), page.offset + maxBytes) : bytes.size();
    const auto inside = [&bytes](qint64 at) {
        return at < bytes.size() && (static_cast<unsigned char>(bytes.at(at)) & 0xC0) == 0x80;
    };
    while (end > page.offset && inside(end)) {
        --end;
    }
    /* A page narrower than one character still moves past it. */
    if (end == page.offset && end < bytes.size()) {
        ++end;
        while (inside(end)) {
            ++end;
        }
    }
    page.text = bytes.mid(page.offset, end - page.offset);
    page.next = end;
    page.eof  = end >= bytes.size();
    return page;
}

QSocAgent *QSocSubAgentTaskSource::liveAgentFor(const QString &id) const
{
    if (mailbox_ == nullptr) {
        for (const RunState &run : runs_) {
            if (run.id == id && !QSocTask::isTerminal(run.status)) {
                return run.agent.data();
            }
        }
        return nullptr;
    }
    const QString identity = mailbox_->resolve(id);
    const QString state    = mailbox_->stateFor(identity);
    if (identity.isEmpty() || identity == mailbox_->resolve(QStringLiteral("main"))
        || state == QStringLiteral("closed") || state == QStringLiteral("cancelled")) {
        return nullptr;
    }
    return mailbox_->agentFor(identity);
}

QString QSocSubAgentTaskSource::latestRunFor(const QSocAgent *agent) const
{
    for (auto it = runs_.crbegin(); it != runs_.crend(); ++it) {
        if (it->agent == agent) {
            return it->id;
        }
    }
    return {};
}

QSocSubAgentTaskSource::HistoryPage QSocSubAgentTaskSource::historyPage(
    const QString &id, int from, int maxBytes) const
{
    HistoryPage    page;
    nlohmann::json messages;
    if (const QSocAgent *agent = liveAgentFor(id)) {
        messages = agent->getMessages();
    } else {
        const QString path = locate(id, QStringLiteral(".history.jsonl"));
        if (!path.isEmpty() && trustedFile(path)) {
            messages = QSocSession::loadMessages(path);
        }
    }
    if (!messages.is_array()) {
        return page;
    }
    page.found      = true;
    const int total = static_cast<int>(messages.size());
    page.offset     = from >= 0 && from <= total ? from : 0;
    qint64 bytes    = 0;
    int    index    = page.offset;
    for (; index < total; ++index) {
        const qint64 size = serializedSize(messages.at(static_cast<size_t>(index)));
        if (index > page.offset && maxBytes > 0 && bytes + size > maxBytes) {
            break;
        }
        bytes += size;
        page.messages.push_back(messages.at(static_cast<size_t>(index)));
    }
    page.next = index;
    page.eof  = index >= total;
    return page;
}

nlohmann::json QSocSubAgentTaskSource::sendFromUser(const QString &id, const QString &message)
{
    return sendTo(id, message, QSocAgentMailbox::userSender());
}

nlohmann::json QSocSubAgentTaskSource::sendTo(
    const QString &id, const QString &message, const QString &sender)
{
    QSocAgent *agent = liveAgentFor(id);
    if (agent == nullptr || mailbox_ == nullptr) {
        return {{"status", "error"}, {"error", "not_live"}};
    }
    const QString identity = mailbox_->idFor(agent);
    const bool    idle     = mailbox_->stateFor(identity) == QStringLiteral("idle");
    const QPointer<QSocSubAgentTaskSource> owner(this);
    nlohmann::json                         receipt = mailbox_->send(
        sender, identity, QUuid::createUuid().toString(QUuid::WithoutBraces), message, {}, true);
    if (owner.isNull() || receipt.value("status", std::string()) != "ok") {
        return receipt;
    }
    receipt["delivery"] = idle ? "woken" : "queued";
    receipt["task_id"]  = latestRunFor(agent).toStdString();
    return receipt;
}

bool QSocSubAgentTaskSource::killTask(const QString &id)
{
    QPointer<QSocAgent> runningAgent;
    bool                cutOffNow = false;
    for (RunState &run : runs_) {
        if (run.id != id) {
            continue;
        }
        const bool isRunning = (run.status == QSocTask::Status::Running);
        const bool isPending = (run.status == QSocTask::Status::Pending);
        if (!isRunning && !isPending) {
            return false;
        }
        if (isRunning && run.launcherStarted && !run.agent.isNull()) {
            runningAgent = run.agent;
            break;
        }

        /* Pending and admitted-but-not-started runs have no live work. */
        run.launcher = {};
        cutOffNow    = true;
        break;
    }

    if (cutOffNow) {
        markTerminal(id, QSocTask::Status::Aborted, QStringLiteral("aborted by user"));
        return true;
    }
    if (runningAgent.isNull()) {
        return false;
    }

    /* The child's terminal callback owns the status change and slot. */
    runningAgent->abortAndDiscardPendingRequests();
    return true;
}

QString QSocSubAgentTaskSource::registerRun(
    const QString &label, const QString &subagentType, QSocAgent *agent, const QString &objective)
{
    evictStaleCompleted();

    RunState run;
    run.objective      = objective;
    run.id             = QStringLiteral("a") + QString::number(nextSerial_++);
    run.label          = label;
    run.subagentType   = subagentType;
    run.agent          = agent;
    run.status         = QSocTask::Status::Pending;
    run.queuedAtMs     = QDateTime::currentMSecsSinceEpoch();
    run.lastActivityMs = run.queuedAtMs;
    run.directory      = transcriptDir_;
    run.definition     = subagentType;
    if (agent != nullptr) {
        agent->setParent(this);
        if (mailbox_ != nullptr)
            mailbox_->registerAgent(agent, label, run.id);
    }
    runs_.append(run);
    writeMeta(run);
    emit tasksChanged();
    return run.id;
}

void QSocSubAgentTaskSource::setMaxConcurrent(int maxConcurrent)
{
    /* 0 (or any non-positive) means unbounded: every queued run is
     * admitted at once and flow control is left to the provider's 429
     * backpressure plus the agent loop's backoff. Negatives collapse to
     * the same unbounded sentinel so downstream only checks `<= 0`. */
    maxConcurrent_ = maxConcurrent > 0 ? maxConcurrent : 0;
    pumpQueue();
}

void QSocSubAgentTaskSource::start(const QString &id, std::function<void()> launcher)
{
    for (RunState &run : runs_) {
        if (run.id != id) {
            continue;
        }
        if (run.status == QSocTask::Status::Pending && !run.launcher) {
            run.launcher = std::move(launcher);
        }
        break;
    }
    pumpQueue();
}

void QSocSubAgentTaskSource::pumpQueue()
{
    /* Listeners and launchers may re-enter and append runs. The outer
     * pump owns admission; nested calls leave work for its next pass. */
    if (pumping_) {
        return;
    }
    const QPointer<QSocSubAgentTaskSource> owner(this);
    pumping_        = true;
    const auto done = qScopeGuard([owner]() {
        if (!owner.isNull()) {
            owner->pumping_ = false;
        }
    });

    /* Re-select after every external call because signals and launchers
     * may append runs and invalidate QList references. */
    while (true) {
        if (maxConcurrent_ > 0 && countRunning() >= maxConcurrent_) {
            break;
        }

        qsizetype candidate = -1;
        for (qsizetype i = 0; i < runs_.size(); ++i) {
            if (runs_[i].status == QSocTask::Status::Pending && runs_[i].launcher) {
                candidate = i;
                break;
            }
        }
        if (candidate < 0) {
            break;
        }

        QString               runId;
        std::function<void()> launcher;
        {
            RunState &run      = runs_[candidate];
            runId              = run.id;
            launcher           = std::move(run.launcher);
            run.status         = QSocTask::Status::Running;
            run.startedAtMs    = QDateTime::currentMSecsSinceEpoch();
            run.lastActivityMs = run.startedAtMs;
            appendDiskEvent(
                run,
                QStringLiteral("start"),
                QStringLiteral("run %1 (%2): %3").arg(run.id, run.subagentType, run.label));
            writeMeta(run);
        }
        emit tasksChanged();
        if (owner.isNull()) {
            return;
        }

        bool stillRunning = false;
        for (RunState &run : owner->runs_) {
            if (run.id == runId) {
                stillRunning = (run.status == QSocTask::Status::Running);
                if (stillRunning) {
                    run.launcherStarted = true;
                }
                break;
            }
        }
        if (stillRunning) {
            launcher();
            if (owner.isNull()) {
                return;
            }
        }
    }
}

void QSocSubAgentTaskSource::appendTranscript(const QString &id, const QString &chunk)
{
    for (RunState &run : runs_) {
        if (run.id != id) {
            continue;
        }
        if (QSocTask::isTerminal(run.status))
            return;
        run.transcript += chunk;
        if (transcriptCap_ > 0 && run.transcript.size() > transcriptCap_) {
            run.transcript = run.transcript.right(transcriptCap_);
        }
        run.lastActivityMs = QDateTime::currentMSecsSinceEpoch();
        appendDiskEvent(run, QStringLiteral("chunk"), chunk);
        return;
    }
}

void QSocSubAgentTaskSource::markTerminal(
    const QString &id, QSocTask::Status state, const QString &text)
{
    const QPointer<QSocSubAgentTaskSource> owner(this);
    for (RunState &run : runs_) {
        if (run.id != id) {
            continue;
        }
        /* Single-shot: a panel kill can race the child's own finish, and both
         * call here. The first transition owns the terminal event; a second
         * must not re-persist or re-emit it under a different word. */
        if (QSocTask::isTerminal(run.status)) {
            return;
        }
        run.status = state;
        if (state == QSocTask::Status::Completed) {
            run.finalResult = text;
        } else {
            run.errorText = text;
        }
        run.lastActivityMs = QDateTime::currentMSecsSinceEpoch();
        appendDiskEvent(run, terminalEventKind(state), text);
        writeHistory(run);
        writeMeta(run);
        if (mailbox_ != nullptr && run.agent != nullptr) {
            const QString identity = mailbox_->idFor(run.agent);
            if (state == QSocTask::Status::Aborted)
                mailbox_->cancel(identity);
            else
                mailbox_->finish(
                    identity,
                    QString::fromStdString(
                        nlohmann::json{
                            {"task_id", id.toStdString()},
                            {"status", QSocTask::statusWord(state).toStdString()},
                            {"result", text.left(2500).toStdString()}}
                            .dump()));
        }
        if (owner.isNull())
            return;
        emit tasksChanged();
        /* Signal handlers may release the task source. */
        // cppcheck-suppress identicalConditionAfterEarlyExit
        if (owner.isNull())
            return;
        emit taskTerminal(id, state, text);
        // cppcheck-suppress identicalConditionAfterEarlyExit
        if (owner.isNull()) {
            return;
        }
        break;
    }
    /* A finished run frees a slot; admit the next queued one. */
    owner->pumpQueue();
}

void QSocSubAgentTaskSource::markCompleted(const QString &id, const QString &finalResult)
{
    markTerminal(id, QSocTask::Status::Completed, finalResult);
}

void QSocSubAgentTaskSource::markFailed(const QString &id, const QString &errorText)
{
    markTerminal(id, QSocTask::Status::Failed, errorText);
}

void QSocSubAgentTaskSource::markAborted(const QString &id, const QString &reason)
{
    markTerminal(id, QSocTask::Status::Aborted, reason);
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

void QSocSubAgentTaskSource::setDispatchMetadata(const QString &id, const Dispatch &dispatch)
{
    for (RunState &run : runs_) {
        if (run.id != id) {
            continue;
        }
        run.dispatch = dispatch;
        writeMeta(run);
        return;
    }
}

bool QSocSubAgentTaskSource::hasActiveRun() const
{
    return countRunning() > 0;
}

bool QSocSubAgentTaskSource::hasUnsettledRun() const
{
    return std::any_of(runs_.cbegin(), runs_.cend(), [](const RunState &run) {
        return run.status == QSocTask::Status::Pending || run.status == QSocTask::Status::Running;
    });
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
    QList<QPointer<QSocAgent>> runningAgents;
    QStringList                cutOff;
    for (RunState &run : runs_) {
        const bool neverStarted = run.status == QSocTask::Status::Pending
                                  || (run.status == QSocTask::Status::Running
                                      && !run.launcherStarted);
        if (neverStarted) {
            run.launcher = {};
            cutOff.append(run.id);
        } else if (run.status == QSocTask::Status::Running && !run.agent.isNull()) {
            runningAgents.append(run.agent);
        }
    }
    for (const QString &id : std::as_const(cutOff)) {
        markTerminal(id, QSocTask::Status::Aborted, QStringLiteral("aborted by user"));
    }
    for (const QPointer<QSocAgent> &agent : std::as_const(runningAgents)) {
        if (!agent.isNull()) {
            agent->abortAndDiscardPendingRequests();
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
        if (!run.agent->queueRequest(message)) {
            return false;
        }
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
            out->canKill
                = (run.status == QSocTask::Status::Running
                   || run.status == QSocTask::Status::Pending);
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

void QSocSubAgentTaskSource::setTranscriptDir(const QString &dir)
{
    const auto foreign = [&dir](const RunState &run) {
        return run.directory != dir && QSocTask::isTerminal(run.status);
    };
    if (std::any_of(runs_.cbegin(), runs_.cend(), foreign)) {
        for (const RunState &run : std::as_const(runs_)) {
            if (foreign(run) && run.agent != nullptr && mailbox_ == nullptr) {
                run.agent->deleteLater();
            }
        }
        runs_.removeIf(foreign);
        historical_.clear();
        emit tasksChanged();
    }
    transcriptDir_          = dir;
    const QStringList names = dir.isEmpty() ? QStringList() : QDir(dir).entryList(QDir::Files);
    for (const QString &name : names) {
        const auto match = runFileName().match(name);
        if (match.hasMatch()) {
            nextSerial_ = std::max(nextSerial_, match.captured(1).toInt() + 1);
        }
    }
}

void QSocSubAgentTaskSource::reserveIdsFrom(const nlohmann::json &messages)
{
    if (!messages.is_array()) {
        return;
    }
    static const QRegularExpression taskId(QStringLiteral(R"re("task_id"\s*:\s*"a([0-9]{1,9})")re"));
    for (const auto &message : messages) {
        if (!message.is_object() || message.value("role", std::string()) != "tool"
            || !message.contains("content") || !message["content"].is_string()) {
            continue;
        }
        const QString content = QString::fromStdString(message["content"].get<std::string>());
        auto          matches = taskId.globalMatch(content);
        while (matches.hasNext()) {
            nextSerial_ = std::max(nextSerial_, matches.next().captured(1).toInt() + 1);
        }
    }
}

QString QSocSubAgentTaskSource::legacyTranscriptDir()
{
    const QString base = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
    return base.isEmpty() ? QString() : base + QStringLiteral("/qsoc/agents");
}

bool QSocSubAgentTaskSource::copyRunDirectory(const QString &from, const QString &to)
{
    if (!QFileInfo(from).isDir()) {
        return true;
    }
    if (QFileInfo::exists(to) || QFileInfo(to).isSymLink() || !QSocPrivateFile::makeDir(to)) {
        return false;
    }
    QDir target(to);
    for (const QFileInfo &entry : QDir(from).entryInfoList(QDir::Files | QDir::Hidden)) {
        if (!trustedFile(entry.filePath())) {
            continue;
        }
        const QString copy = target.filePath(entry.fileName());
        if (!QFile::copy(entry.filePath(), copy)) {
            target.removeRecursively();
            return false;
        }
        QSocPrivateFile::restrict(copy);
    }
    return true;
}

QString QSocSubAgentTaskSource::locate(const QString &id, const QString &suffix) const
{
    for (const RunState &run : runs_) {
        if (run.id == id) {
            return run.directory.isEmpty() ? QString() : QDir(run.directory).filePath(id + suffix);
        }
    }
    for (const QString &dir : {transcriptDir_, legacyTranscriptDir()}) {
        const QString path = dir.isEmpty() ? QString() : QDir(dir).filePath(id + suffix);
        if (!path.isEmpty() && trustedFile(path)) {
            return path;
        }
    }
    return transcriptDir_.isEmpty() ? QString() : QDir(transcriptDir_).filePath(id + suffix);
}

QString QSocSubAgentTaskSource::transcriptPathFor(const QString &id) const
{
    return locate(id, QStringLiteral(".jsonl"));
}

QString QSocSubAgentTaskSource::metaPathFor(const QString &id) const
{
    return locate(id, QStringLiteral(".meta.json"));
}

void QSocSubAgentTaskSource::appendDiskEvent(
    const RunState &run, const QString &kind, const QString &data) const
{
    if (run.directory.isEmpty() || (data.isEmpty() && kind != QStringLiteral("start"))
        || !QSocPrivateFile::makeDir(run.directory)) {
        return;
    }
    const QString path = QDir(run.directory).filePath(run.id + QStringLiteral(".jsonl"));
    QFile         file(path);
    if (QFileInfo(path).isSymLink() || !file.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return;
    }
    QSocPrivateFile::restrict(file);
    QJsonObject obj;
    obj["ts"]                   = QDateTime::currentMSecsSinceEpoch();
    obj["kind"]                 = kind;
    obj["data"]                 = data;
    const QByteArray serialized = QJsonDocument(obj).toJson(QJsonDocument::Compact);
    file.write(serialized);
    file.write("\n");
    file.close();
}

void QSocSubAgentTaskSource::writeHistory(RunState &run) const
{
    if (run.directory.isEmpty() || run.agent.isNull()) {
        return;
    }
    const nlohmann::json messages = run.agent->getMessages();
    if (!messages.is_array() || messages.empty() || serializedSize(messages) > historyBytesLimit) {
        return;
    }
    const QString path = QDir(run.directory).filePath(run.id + QStringLiteral(".history.jsonl"));
    if (QFileInfo(path).isSymLink()) {
        return;
    }
    QSocSession history(
        run.id,
        path,
        QFileInfo::exists(path) ? QSocSession::StorageMode::Existing
                                : QSocSession::StorageMode::Fresh);
    if (history.rewriteMessages(messages)) {
        run.historyFile = QFileInfo(path).fileName();
    }
}

void QSocSubAgentTaskSource::writeMeta(const RunState &run) const
{
    if (run.directory.isEmpty() || !QSocPrivateFile::makeDir(run.directory)) {
        return;
    }
    QJsonObject meta;
    if (run.agent && run.agent->toolResultStore() && !run.agent->toolResultStore()->isTemporary()) {
        meta["artifact_directory"] = run.agent->toolResultStore()->directory();
        meta["artifact_owner"]     = run.agent->toolResultStore()->owner();
    }
    meta["task_id"]       = run.id;
    meta["label"]         = run.label;
    meta["subagent_type"] = run.subagentType;
    meta["started_at_ms"] = run.startedAtMs;
    meta["status"]        = QSocTask::statusWord(run.status);
    meta["isolation"]     = run.isolation.isEmpty() ? QStringLiteral("none") : run.isolation;
    if (!run.worktreePath.isEmpty()) {
        meta["worktree"] = run.worktreePath;
    }
    if (!run.dispatch.host.isEmpty()) {
        meta["host"] = run.dispatch.host;
    }
    if (!run.dispatch.endpoint.isEmpty()) {
        meta["endpoint"] = run.dispatch.endpoint;
    }
    if (!run.dispatch.workspace.isEmpty()) {
        meta["workspace"] = run.dispatch.workspace;
    }
    if (!run.historyFile.isEmpty()) {
        meta["history_file"] = run.historyFile;
    }
    if (!run.definition.isEmpty()) {
        meta["definition"] = run.definition;
    }
    if (isTerminal(run.status)) {
        meta["finished_at_ms"] = run.lastActivityMs;
        if (!run.finalResult.isEmpty()) {
            meta["final_preview"] = run.finalResult.left(256);
        }
        if (!run.errorText.isEmpty()) {
            meta["error"] = run.errorText;
        }
    }
    writeJsonFile(QDir(run.directory).filePath(run.id + QStringLiteral(".meta.json")), meta);
}

QList<QSocSubAgentTaskSource::HistoricalRun> QSocSubAgentTaskSource::loadHistoricalRuns(
    int staleAgeSec)
{
    historical_.clear();
    const QString                         legacyDir = legacyTranscriptDir();
    const qint64                          nowMs     = QDateTime::currentMSecsSinceEpoch();
    const QList<std::pair<QString, bool>> sources
        = {{transcriptDir_, false}, {legacyDir == transcriptDir_ ? QString() : legacyDir, true}};
    for (const auto &[dirPath, legacy] : sources) {
        const QDir dir(dirPath);
        if (dirPath.isEmpty() || !dir.exists()) {
            continue;
        }
        for (const QString &name : dir.entryList({QStringLiteral("*.meta.json")}, QDir::Files)) {
            HistoricalRun run;
            if (readHistoricalRun(dir.filePath(name), legacy, staleAgeSec, nowMs, &run)) {
                historical_.append(run);
            }
        }
    }
    /* Newest first. */
    std::stable_sort(
        historical_.begin(),
        historical_.end(),
        [](const HistoricalRun &lhs, const HistoricalRun &rhs) {
            return lhs.startedAtMs > rhs.startedAtMs;
        });
    return historical_;
}

bool QSocSubAgentTaskSource::findHistoricalRun(const QString &id, HistoricalRun *out)
{
    auto search = [&]() -> bool {
        const HistoricalRun *hit = nullptr;
        for (const HistoricalRun &run : historical_) {
            if (run.id == id && (hit == nullptr || hit->legacy)) {
                hit = &run;
            }
        }
        if (hit != nullptr && out != nullptr) {
            *out = *hit;
        }
        return hit != nullptr;
    };
    if (search()) {
        return true;
    }
    /* Cache miss: rescan disk in case the run was just produced. */
    loadHistoricalRuns();
    return search();
}

void QSocSubAgentTaskSource::evictStaleCompleted()
{
    const qint64 nowMs   = QDateTime::currentMSecsSinceEpoch();
    bool         changed = false;
    for (int i = static_cast<int>(runs_.size()) - 1; i >= 0; --i) {
        const RunState &run = runs_[i];
        if (!isTerminal(run.status)) {
            continue;
        }
        if (nowMs - run.lastActivityMs < completionTtlMs_) {
            continue;
        }
        if (run.agent != nullptr && mailbox_ == nullptr) {
            run.agent->deleteLater();
        }
        runs_.removeAt(i);
        changed = true;
    }
    if (changed) {
        emit tasksChanged();
    }
}

#include "moc_qsocsubagenttasksource.cpp"
