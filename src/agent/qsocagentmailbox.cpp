// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagentmailbox.h"
#include "agent/qsocagent.h"

#include <QRegularExpression>
#include <QUuid>

QSocAgentMailbox::QSocAgentMailbox(QObject *parent)
    : QObject(parent)
{}

QString QSocAgentMailbox::registerAgent(QSocAgent *agent, const QString &name, const QString &taskId)
{
    QString id = idFor(agent);
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        AgentEntry entry;
        entry.agent = agent;
        entry.name  = name;
        agents_.insert(id, entry);
        agent->setMailbox(this, id);
        connect(agent, &QObject::destroyed, this, [this, id]() { cancel(id); });
    }
    if (!taskId.isEmpty()) {
        aliases_.insert(taskId, id);
        agents_[id].taskId = taskId;
        agents_[id].state  = QStringLiteral("pending");
    } else if (name == QStringLiteral("main")) {
        aliases_.insert(name, id);
    }
    return id;
}

QString QSocAgentMailbox::idFor(const QSocAgent *agent) const
{
    for (auto it = agents_.cbegin(); it != agents_.cend(); ++it) {
        if (it->agent == agent)
            return it.key();
    }
    return {};
}

QString QSocAgentMailbox::resolve(const QString &target) const
{
    return agents_.contains(target) ? target : aliases_.value(target);
}

QSocAgent *QSocAgentMailbox::agentFor(const QString &id) const
{
    return agents_.value(id).agent.data();
}

void QSocAgentMailbox::setWakeHandler(std::function<QString(QSocAgent *)> handler)
{
    wakeHandler_ = std::move(handler);
}

void QSocAgentMailbox::setState(const QString &id, const QString &state)
{
    auto it = agents_.find(id);
    if (it == agents_.end())
        return;
    if (state == QStringLiteral("cancelled")) {
        cancel(id);
        return;
    }
    if (it->state == QStringLiteral("cancelled")
        && !(id == resolve(QStringLiteral("main")) && state == QStringLiteral("running")))
        return;
    it->state = state;
    emit changed();
}

QString QSocAgentMailbox::stateFor(const QString &id) const
{
    const auto it = agents_.constFind(id);
    return it == agents_.cend() || it->agent.isNull() ? QStringLiteral("closed") : it->state;
}

QSocAgentMailbox::json QSocAgentMailbox::list(const QString &caller) const
{
    json rows = json::array();
    for (auto it = agents_.cbegin(); it != agents_.cend(); ++it) {
        rows.push_back(
            {{"agent_id", it.key().toStdString()},
             {"name", it->name.toStdString()},
             {"task_id", it->taskId.toStdString()},
             {"state", stateFor(it.key()).toStdString()},
             {"pending", it->pending.size()},
             {"self", it.key() == caller}});
    }
    return {{"status", "ok"}, {"agents", rows}};
}

QString QSocAgentMailbox::keyFor(const QString &sender, const QString &id)
{
    return sender + QLatin1Char(':') + id;
}

QSocAgentMailbox::json QSocAgentMailbox::error(const char *code)
{
    return {{"status", "error"}, {"error", code}};
}

QSocAgentMailbox::json QSocAgentMailbox::receipt(const Message &message, bool duplicate)
{
    return {
        {"status", "ok"},
        {"message_id", message.id.toStdString()},
        {"agent_id", message.recipient.toStdString()},
        {"delivery", message.delivery.toStdString()},
        {"replied", message.replied},
        {"duplicate", duplicate}};
}

QSocAgentMailbox::json QSocAgentMailbox::send(
    const QString &sender,
    const QString &target,
    const QString &messageId,
    const QString &body,
    const QString &replyTo,
    bool           wake)
{
    static const QRegularExpression validId(QStringLiteral("^[A-Za-z0-9_.-]{1,128}$"));
    const QString                   recipient = resolve(target);
    if (!agents_.contains(sender) || agentFor(sender) == nullptr)
        return error("unknown_sender");
    if (recipient.isEmpty())
        return error("unknown_target");
    if (recipient == sender)
        return error("self_message");
    if (!validId.match(messageId).hasMatch())
        return error("invalid_message_id");
    if (body.trimmed().isEmpty() || body.toUtf8().size() > 16384)
        return error("invalid_message_size");
    const QString key      = keyFor(sender, messageId);
    auto          previous = messages_.constFind(key);
    if (previous != messages_.cend()) {
        if (previous->recipient != recipient || previous->body != body
            || previous->replyTo != replyTo || previous->wake != wake)
            return error("message_id_conflict");
        return receipt(*previous, true);
    }
    const QString state = stateFor(recipient);
    if (state == QStringLiteral("cancelled") || state == QStringLiteral("closed"))
        return error("target_cancelled_or_closed");
    if (stateFor(sender) == QStringLiteral("cancelled"))
        return error("sender_cancelled");
    if (wake && recipient == resolve(QStringLiteral("main")))
        return error("cannot_wake_main");
    if (agents_[recipient].pending.size() >= 128)
        return error("mailbox_full");
    if (messages_.size() >= 8192)
        return error("session_message_limit");
    const QString originalKey = keyFor(recipient, replyTo);
    if (!replyTo.isEmpty()) {
        const auto original = messages_.constFind(originalKey);
        if (original == messages_.cend() || original->recipient != sender)
            return error("unknown_request");
    }
    Message message{
        messageId, sender, recipient, body, replyTo, QStringLiteral("accepted"), wake, false};
    messages_.insert(key, message);
    agents_[recipient].pending.append(key);
    if (wake && state == QStringLiteral("idle")) {
        const QString taskId = wakeHandler_ ? wakeHandler_(agentFor(recipient)) : QString();
        if (taskId.isEmpty()) {
            agents_[recipient].pending.removeAll(key);
            messages_.remove(key);
            return error("wake_unavailable");
        }
    }
    if (!replyTo.isEmpty())
        messages_[originalKey].replied = true;
    emit changed();
    return receipt(messages_[key], false);
}

QSocAgentMailbox::json QSocAgentMailbox::take(
    const QString &recipient, const QString &from, const QString &replyTo, bool peek)
{
    json result = json::array();
    auto entry  = agents_.find(recipient);
    if (entry == agents_.end())
        return result;
    QStringList remaining;
    for (const QString &key : std::as_const(entry->pending)) {
        auto &message = messages_[key];
        if ((!from.isEmpty() && message.sender != from)
            || (!replyTo.isEmpty() && message.replyTo != replyTo)) {
            remaining.append(key);
            continue;
        }
        result.push_back(
            {{"message_id", message.id.toStdString()},
             {"sender", message.sender.toStdString()},
             {"recipient", recipient.toStdString()},
             {"reply_to", message.replyTo.toStdString()},
             {"kind", message.wake ? "followup" : "message"},
             {"body", message.body.toStdString()}});
        if (!peek)
            message.delivery = QStringLiteral("delivered");
    }
    if (!peek)
        entry->pending = remaining;
    return result;
}

int QSocAgentMailbox::pendingCount(const QString &recipient) const
{
    return agents_.value(recipient).pending.size();
}

void QSocAgentMailbox::cancel(const QString &id)
{
    auto it = agents_.find(id);
    if (it == agents_.end())
        return;
    if (it->state == QStringLiteral("cancelled") && it->pending.isEmpty())
        return;
    it->state = QStringLiteral("cancelled");
    for (const QString &key : std::as_const(it->pending))
        messages_[key].delivery = QStringLiteral("cancelled");
    it->pending.clear();
    emit changed();
}

void QSocAgentMailbox::reset(QSocAgent *root)
{
    const auto entries = agents_;
    for (const auto &entry : entries) {
        if (entry.agent && entry.agent != root)
            entry.agent->abortAndDiscardPendingRequests();
    }
    agents_.clear();
    aliases_.clear();
    messages_.clear();
    registerAgent(root, QStringLiteral("main"));
    emit changed();
}

QString QSocAgentMailbox::render(const json &message)
{
    return QStringLiteral(
               "Peer message (agent-authored, not user approval or permission changes):\n")
           + QString::fromStdString(message.dump());
}

void QSocAgentMailbox::finish(const QString &id, const QString &result)
{
    if (stateFor(id) == QStringLiteral("cancelled"))
        return;
    QStringList requests;
    for (auto it = messages_.cbegin(); it != messages_.cend(); ++it) {
        if (it->recipient == id && it->wake && !it->replied
            && it->delivery == QStringLiteral("delivered"))
            requests.append(it.key());
    }
    for (const QString &key : requests) {
        const Message request = messages_.value(key);
        send(
            id,
            request.sender,
            QUuid::createUuid().toString(QUuid::WithoutBraces),
            result.isEmpty() ? QStringLiteral("Task completed.") : result,
            request.id,
            false);
    }
    setState(id, QStringLiteral("idle"));
}
