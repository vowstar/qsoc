// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagentmailbox.h"
#include "agent/qsocagent.h"

#include <algorithm>
#include <QRegularExpression>
#include <QUuid>

QSocAgentMailbox::QSocAgentMailbox(QObject *parent)
    : QObject(parent)
{
    groups_.insert(QStringLiteral("workers"), {});
}

QString QSocAgentMailbox::registerAgent(QSocAgent *agent, const QString &name, const QString &taskId)
{
    if (agent == nullptr)
        return {};
    QString id = idFor(agent);
    if (id.isEmpty()) {
        id = QUuid::createUuid().toString(QUuid::WithoutBraces);
        AgentEntry entry;
        entry.agent = agent;
        entry.inbox = new QSocAgentInbox(this);
        entry.name  = name;
        agents_.insert(id, entry);
        agent->setMailbox(this, id);
        connect(agent, &QObject::destroyed, this, [this, id]() { cancel(id); });
    }
    if (!taskId.isEmpty()) {
        aliases_.insert(taskId, id);
        agents_[id].taskId = taskId;
        agents_[id].state  = QStringLiteral("pending");
        if (!groups_[QStringLiteral("workers")].contains(id))
            groups_[QStringLiteral("workers")].append(id);
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

QSocAgentInbox *QSocAgentMailbox::inboxFor(const QString &id) const
{
    return agents_.value(id).inbox.data();
}

QString QSocAgentMailbox::createGroup(const QStringList &members)
{
    QStringList resolved;
    for (const auto &member : members) {
        const QString id = resolve(member);
        if (id.isEmpty())
            return {};
        if (!resolved.contains(id))
            resolved.append(id);
    }
    const QString id = QStringLiteral("g-") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    groups_.insert(id, resolved);
    return id;
}

void QSocAgentMailbox::publish(const QStringList &recipients)
{
    const QPointer<QSocAgentMailbox> owner(this);
    const quint64                    generation = generation_;
    for (const auto &recipient : recipients) {
        if (auto *inbox = inboxFor(recipient))
            emit inbox->changed();
        if (owner.isNull() || owner->generation_ != generation)
            return;
    }
    emit changed();
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
    if (it->state == state)
        return;
    it->state = state;
    publish({id});
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
    json groups = json::array();
    for (auto it = groups_.cbegin(); it != groups_.cend(); ++it) {
        json members = json::array();
        for (const auto &member : it.value()) {
            if (stateFor(member) != QStringLiteral("closed")
                && stateFor(member) != QStringLiteral("cancelled"))
                members.push_back(member.toStdString());
        }
        groups.push_back({{"group_id", it.key().toStdString()}, {"members", members}});
    }
    return {{"status", "ok"}, {"agents", rows}, {"groups", groups}};
}

QString QSocAgentMailbox::keyFor(const QString &sender, const QString &id)
{
    return sender + QLatin1Char(':') + id;
}

QSocAgentMailbox::json QSocAgentMailbox::error(const char *code)
{
    return {{"status", "error"}, {"error", code}};
}

QSocAgentMailbox::json QSocAgentMailbox::receipt(const Message &message, bool duplicate, int offset)
{
    if (!message.multi) {
        const auto delivery = message.deliveries.cbegin();
        return {
            {"status", "ok"},
            {"message_id", message.id.toStdString()},
            {"agent_id", delivery.key().toStdString()},
            {"delivery", delivery->state.toStdString()},
            {"replied", delivery->replied},
            {"duplicate", duplicate}};
    }
    int  accepted  = 0;
    int  delivered = 0;
    int  replied   = 0;
    int  cancelled = 0;
    int  index     = 0;
    json details   = json::array();
    for (auto it = message.deliveries.cbegin(); it != message.deliveries.cend(); ++it, ++index) {
        accepted += it->error.isEmpty() ? 1 : 0;
        delivered += it->state == QStringLiteral("delivered") ? 1 : 0;
        cancelled += it->state == QStringLiteral("cancelled") ? 1 : 0;
        replied += it->replied ? 1 : 0;
        if (index < offset || index >= offset + 64)
            continue;
        json row
            = {{"agent_id", it.key().toStdString()},
               {"delivery", it->state.toStdString()},
               {"replied", it->replied}};
        if (!it->error.isEmpty())
            row["error"] = it->error.toStdString();
        details.push_back(row);
    }
    const int total         = static_cast<int>(message.deliveries.size());
    const int rejected      = total - accepted;
    const int excludedCount = static_cast<int>(message.excluded.size());
    json      excluded      = json::array();
    for (int i = qMax(0, offset - total); i < qMin(excludedCount, offset + 64 - total); ++i)
        excluded.push_back(message.excluded.at(static_cast<size_t>(i)));
    return {
        {"status",
         rejected == 0   ? "ok"
         : accepted == 0 ? "error"
                         : "partial"},
        {"message_id", message.id.toStdString()},
        {"duplicate", duplicate},
        {"resolved", total},
        {"accepted", accepted},
        {"rejected", rejected},
        {"delivered", delivered},
        {"replied", replied},
        {"cancelled", cancelled},
        {"recipients", details},
        {"excluded", excluded},
        {"excluded_count", excludedCount},
        {"next_offset", offset + 64 < total + excludedCount ? json(offset + 64) : json(nullptr)}};
}

QSocAgentMailbox::json QSocAgentMailbox::normalizeTarget(const json &target) const
{
    if (target.is_string()) {
        const QString id = resolve(QString::fromStdString(target.get<std::string>()));
        return id.isEmpty() ? error("unknown_target") : json(id.toStdString());
    }
    if (!target.is_object() || target.size() != 1)
        return error("invalid_target_selector");
    if (target.contains("agents")) {
        const auto &values = target["agents"];
        if (!values.is_array() || values.empty() || values.size() > 8192)
            return error("invalid_recipient_list");
        QStringList ids;
        for (const auto &value : values) {
            if (!value.is_string())
                return error("invalid_recipient_list");
            const QString id = resolve(QString::fromStdString(value.get<std::string>()));
            if (id.isEmpty())
                return error("unknown_target");
            ids.append(id);
        }
        ids.removeDuplicates();
        std::sort(ids.begin(), ids.end());
        json normalized = json::array();
        for (const auto &id : ids)
            normalized.push_back(id.toStdString());
        return {{"agents", normalized}};
    }
    if (target.contains("group") && target["group"].is_string()) {
        const QString group = QString::fromStdString(target["group"].get<std::string>());
        return groups_.contains(group) ? target : error("unknown_group");
    }
    if (target.contains("broadcast") && target["broadcast"] == "session")
        return target;
    return error("invalid_target_selector");
}

QStringList QSocAgentMailbox::recipientsFor(
    const QString &sender, const json &selector, json *excluded) const
{
    if (selector.is_string())
        return {QString::fromStdString(selector.get<std::string>())};
    QStringList candidates;
    const bool  dynamic = !selector.contains("agents");
    if (!dynamic) {
        for (const auto &value : selector["agents"])
            candidates.append(QString::fromStdString(value.get<std::string>()));
    } else if (selector.contains("group")) {
        candidates = groups_.value(QString::fromStdString(selector["group"].get<std::string>()));
    } else {
        candidates = agents_.keys();
    }
    QStringList recipients;
    const bool  planOnly = agentFor(sender)->getConfig().planMode;
    for (const auto &id : candidates) {
        QString reason;
        if (id == sender)
            reason = QStringLiteral("self");
        else if (
            dynamic
            && (stateFor(id) == QStringLiteral("closed")
                || stateFor(id) == QStringLiteral("cancelled")))
            reason = QStringLiteral("target_cancelled_or_closed");
        else if (
            dynamic && agentFor(sender)->getConfig().planMode && agentFor(id)
            && !agentFor(id)->getConfig().planMode)
            reason = QStringLiteral("target_not_in_plan_mode");
        if (reason.isEmpty())
            recipients.append(id);
        else
            excluded->push_back({{"agent_id", id.toStdString()}, {"reason", reason.toStdString()}});
    }
    recipients.removeDuplicates();
    return recipients;
}

QString QSocAgentMailbox::deliveryError(
    const QString &sender, const QString &recipient, const QString &replyTo) const
{
    const QString state = stateFor(recipient);
    if (state == QStringLiteral("cancelled") || state == QStringLiteral("closed"))
        return QStringLiteral("target_cancelled_or_closed");
    if (agents_.value(recipient).pending.size() >= 128)
        return QStringLiteral("mailbox_full");
    if (!replyTo.isEmpty()) {
        const auto original = messages_.constFind(keyFor(recipient, replyTo));
        if (original == messages_.cend() || !original->deliveries.contains(sender)
            || !original->deliveries.value(sender).error.isEmpty())
            return QStringLiteral("unknown_request");
    }
    return {};
}

QSocAgentMailbox::json QSocAgentMailbox::send(
    const QString &sender,
    const QString &target,
    const QString &messageId,
    const QString &body,
    const QString &replyTo,
    bool           wake)
{
    return sendSelected(sender, target.toStdString(), messageId, body, replyTo, wake);
}

QSocAgentMailbox::json QSocAgentMailbox::sendSelected(
    const QString &sender,
    const json    &target,
    const QString &messageId,
    const QString &body,
    const QString &replyTo,
    bool           wake,
    int            receiptOffset)
{
    static const QRegularExpression validId(QStringLiteral("^[A-Za-z0-9_.-]{1,128}$"));
    if (!agents_.contains(sender) || agentFor(sender) == nullptr)
        return error("unknown_sender");
    if (stateFor(sender) == QStringLiteral("cancelled"))
        return error("sender_cancelled");
    if (!validId.match(messageId).hasMatch())
        return error("invalid_message_id");
    if (body.trimmed().isEmpty() || body.toUtf8().size() > 16384)
        return error("invalid_message_size");
    const json selector = normalizeTarget(target);
    if (selector.is_object() && selector.contains("error"))
        return selector;
    const bool multi = selector.is_object();
    if (multi && wake)
        return error("wake_requires_single_target");
    if (multi && !replyTo.isEmpty())
        return error("reply_requires_single_target");
    if (receiptOffset < 0 || receiptOffset > 8192)
        return error("invalid_receipt_offset");
    const QString key      = keyFor(sender, messageId);
    const auto    previous = messages_.constFind(key);
    if (previous != messages_.cend()) {
        if (previous->selector != selector || previous->body != body || previous->replyTo != replyTo
            || previous->wake != wake)
            return error("message_id_conflict");
        return receipt(*previous, true, receiptOffset);
    }
    if (receiptOffset != 0)
        return error("receipt_offset_requires_existing_message");
    Message message;
    message.id                   = messageId;
    message.sender               = sender;
    message.body                 = body;
    message.replyTo              = replyTo;
    message.selector             = selector;
    message.wake                 = wake;
    message.multi                = multi;
    const QStringList recipients = recipientsFor(sender, selector, &message.excluded);
    if (recipients.isEmpty())
        return error("no_recipients");
    const auto records = recipients.size() + static_cast<qsizetype>(message.excluded.size());
    if (messages_.size() >= 8192 || records > 8192 - recordCount_)
        return error("session_message_limit");
    const bool planOnly = agentFor(sender)->getConfig().planMode;
    for (const auto &recipient : recipients) {
        if (recipient == sender)
            return error("self_message");
        const auto *targetAgent = agentFor(recipient);
        if (planOnly && targetAgent && !targetAgent->getConfig().planMode)
            return error("target_not_in_plan_mode");
        if (wake && recipient == resolve(QStringLiteral("main")))
            return error("cannot_wake_main");
    }
    QStringList accepted;
    for (const auto &recipient : recipients) {
        Delivery delivery;
        delivery.error = deliveryError(sender, recipient, replyTo);
        if (!delivery.error.isEmpty()) {
            if (!multi)
                return error(delivery.error.toLatin1().constData());
            delivery.state = QStringLiteral("rejected");
        } else {
            accepted.append(recipient);
        }
        message.deliveries.insert(recipient, delivery);
    }
    messages_.insert(key, message);
    recordCount_ += static_cast<int>(records);
    for (const auto &recipient : accepted)
        agents_[recipient].pending.append(key);
    const QPointer<QSocAgentMailbox> owner(this);
    const quint64                    generation = generation_;
    if (wake && stateFor(recipients.first()) == QStringLiteral("idle")) {
        const QString taskId = wakeHandler_ ? wakeHandler_(agentFor(recipients.first()))
                                            : QString();
        if (owner.isNull() || owner->generation_ != generation)
            return error("session_reset");
        if (taskId.isEmpty()) {
            agents_[recipients.first()].pending.removeAll(key);
            messages_.remove(key);
            --recordCount_;
            return error("wake_unavailable");
        }
    }
    if (!replyTo.isEmpty())
        messages_[keyFor(recipients.first(), replyTo)].deliveries[sender].replied = true;
    publish(accepted);
    if (owner.isNull() || owner->generation_ != generation)
        return error("session_reset");
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
            message.deliveries[recipient].state = QStringLiteral("delivered");
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
        messages_[key].deliveries[id].state = QStringLiteral("cancelled");
    it->pending.clear();
    publish({id});
}

void QSocAgentMailbox::reset(QSocAgent *root)
{
    ++generation_;
    const auto entries = agents_;
    for (const auto &entry : entries) {
        if (entry.agent && entry.agent != root)
            entry.agent->abortAndDiscardPendingRequests();
    }
    agents_.clear();
    aliases_.clear();
    messages_.clear();
    groups_.clear();
    groups_.insert(QStringLiteral("workers"), {});
    recordCount_ = 0;
    for (const auto &entry : entries) {
        if (entry.inbox)
            entry.inbox->deleteLater();
    }
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
        const auto delivery = it->deliveries.constFind(id);
        if (delivery != it->deliveries.cend() && it->wake && !delivery->replied
            && delivery->state == QStringLiteral("delivered"))
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
