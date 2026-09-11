// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolagentmessage.h"
#include "agent/qsocagent.h"
#include "agent/qsocagentmailbox.h"

#include <QEventLoop>
#include <QTimer>

namespace {
QString encoded(const json &value)
{
    return QString::fromStdString(value.dump());
}
QString failure(const char *reason)
{
    return encoded({{"status", "error"}, {"error", reason}});
}
QString field(const json &args, const char *key)
{
    const auto it = args.find(key);
    return it != args.end() && it->is_string() ? QString::fromStdString(it->get<std::string>())
                                               : QString();
}
} // namespace

QSocToolAgentMessage::QSocToolAgentMessage(
    QObject *parent, QSocAgentMailbox *mailbox, const QString &operation)
    : QSocTool(parent)
    , mailbox_(mailbox)
    , operation_(operation)
{}

QString QSocToolAgentMessage::getName() const
{
    return operation_;
}

QString QSocToolAgentMessage::getDescription() const
{
    if (operation_ == QStringLiteral("interrupt_agent"))
        return QStringLiteral(
            "Cancel a child run and discard pending peer messages. Later messages cannot revive "
            "that agent. Only the main agent can cancel peers.");
    if (operation_ == QStringLiteral("agent_list"))
        return QStringLiteral(
            "List session peers, stable agent_id addresses, task aliases, runtime groups and "
            "state. "
            "Names are display labels.");
    if (operation_ == QStringLiteral("agent_inbox"))
        return QStringLiteral(
            "Read pending peer messages. peek=true leaves messages available for later delivery. "
            "Filters use stable sender IDs and reply_to request IDs.");
    if (operation_ == QStringLiteral("wait_agent"))
        return QStringLiteral(
            "Wait for peer messages without blocking other agents. Use reply_to to await a "
            "specific request. Timeout does not resend or cancel that request. Unmatched messages "
            "remain queued.");
    return QStringLiteral(
               "Send an agent-authored message to a stable agent_id or task alias from agent_list. "
               "Reuse message_id only for retries of the identical request. reply_to answers a "
               "request from the target. Delivery receipts do not prove execution. ")
           + (operation_ == QStringLiteral("followup_task")
                  ? QStringLiteral(
                        "Wake an idle child for a new task using its existing context and model. "
                        "Its final output is returned as a correlated reply. Cannot wake main or a "
                        "cancelled agent.")
                  : QStringLiteral(
                        "Queue information without waking idle agents. target accepts one address, "
                        "{agents:[addresses]}, {group:group_id}, or {broadcast:session}. Use the "
                        "smallest audience. Group retries keep the original recipients, including "
                        "rejections. Read per-recipient results. Retry rejected recipients with a "
                        "new message_id. receipt_offset reads the next receipt page on an "
                        "identical "
                        "retry. Reply to the sender, not the group. Informational messages need no "
                        "acknowledgement. Use followup_task when an idle child must work."));
}

json QSocToolAgentMessage::getParametersSchema() const
{
    json properties = json::object();
    json required   = json::array();
    if (operation_ == QStringLiteral("send_message")
        || operation_ == QStringLiteral("followup_task")) {
        properties
            = {{"target", {{"type", "string"}}},
               {"message", {{"type", "string"}}},
               {"message_id",
                {{"type", "string"},
                 {"description",
                  "Unique per sender, 1-128 letters, digits, dot, hyphen or underscore. Reuse on "
                  "retry."}}},
               {"reply_to", {{"type", "string"}}}};
        required = {"target", "message", "message_id"};
        if (operation_ == QStringLiteral("send_message")) {
            const auto selector = [](const char *name, const json &value) {
                return json{
                    {"type", "object"},
                    {"properties", {{name, value}}},
                    {"required", {name}},
                    {"additionalProperties", false}};
            };
            properties["target"] = {
                {"oneOf",
                 json::array(
                     {{{"type", "string"}},
                      selector(
                          "agents",
                          {{"type", "array"},
                           {"items", {{"type", "string"}}},
                           {"minItems", 1},
                           {"maxItems", 8192}}),
                      selector("group", {{"type", "string"}}),
                      selector("broadcast", {{"type", "string"}, {"enum", {"session"}}})})}};
            properties["receipt_offset"] = {{"type", "integer"}, {"minimum", 0}, {"maximum", 8192}};
        }

    } else if (operation_ == QStringLiteral("interrupt_agent")) {
        properties = {{"target", {{"type", "string"}}}};
        required   = {"target"};
    } else if (operation_ != QStringLiteral("agent_list")) {
        properties = {{"from", {{"type", "string"}}}, {"reply_to", {{"type", "string"}}}};
        if (operation_ == QStringLiteral("wait_agent"))
            properties["timeout_ms"] = {{"type", "integer"}, {"minimum", 0}, {"maximum", 60000}};
        else
            properties["peek"] = {{"type", "boolean"}};
    }
    return {
        {"type", "object"},
        {"properties", properties},
        {"required", required},
        {"additionalProperties", false}};
}

QString QSocToolAgentMessage::execute(const json &arguments)
{
    const QPointer<QSocToolCallContext> context(currentCallContext());
    const QPointer<QSocAgentMailbox>    mailbox(mailbox_);
    auto *caller = context ? qobject_cast<QSocAgent *>(context->executionScope()) : nullptr;
    if (!mailbox || caller == nullptr)
        return failure("caller_not_registered");
    const QString sender = mailbox->idFor(caller);
    if (sender.isEmpty())
        return failure("caller_not_registered");
    if (context->isCancellationRequested())
        return failure("cancelled");
    const json schema = QSocToolAgentMessage::getParametersSchema();
    if (!arguments.is_object())
        return failure("invalid_arguments");
    for (const auto &key : schema["required"]) {
        if (!arguments.contains(key.get<std::string>()))
            return failure("missing_required_argument");
    }
    for (auto it = arguments.begin(); it != arguments.end(); ++it) {
        if (!schema["properties"].contains(it.key()))
            return failure("unknown_argument");
        if (it.key() == "target" && operation_ == QStringLiteral("send_message")) {
            if (!it->is_string() && !it->is_object())
                return failure("invalid_argument_type");
            continue;
        }
        const std::string type = schema["properties"][it.key()]["type"];
        if ((type == "string" && !it->is_string()) || (type == "boolean" && !it->is_boolean())
            || (type == "integer" && !it->is_number_integer()))
            return failure("invalid_argument_type");
    }
    if (operation_ == QStringLiteral("agent_list"))
        return encoded(mailbox->list(sender));
    if (operation_ == QStringLiteral("interrupt_agent")) {
        const QString target = mailbox->resolve(field(arguments, "target"));
        if (sender != mailbox->resolve(QStringLiteral("main")))
            return failure("only_main_can_cancel");
        auto *agent = mailbox->agentFor(target);
        if (agent == nullptr || target == sender)
            return failure("invalid_cancel_target");
        agent->abortAndDiscardPendingRequests();
        return encoded(
            {{"status", "ok"}, {"agent_id", target.toStdString()}, {"state", "cancelled"}});
    }
    if (operation_ == QStringLiteral("send_message")
        || operation_ == QStringLiteral("followup_task")) {
        const bool wake   = operation_ == QStringLiteral("followup_task");
        const auto offset = arguments.value("receipt_offset", json(0));
        if (offset < 0 || offset > 8192)
            return failure("invalid_receipt_offset");
        return encoded(mailbox->sendSelected(
            sender,
            arguments["target"],
            field(arguments, "message_id"),
            field(arguments, "message"),
            field(arguments, "reply_to"),
            wake,
            offset.get<int>()));
    }
    const QString fromArg = field(arguments, "from");
    const QString from    = mailbox->resolve(fromArg);
    if (!fromArg.isEmpty() && from.isEmpty())
        return failure("unknown_sender_filter");
    const QString replyTo = field(arguments, "reply_to");
    const bool    wait    = operation_ == QStringLiteral("wait_agent");
    const bool    peek    = arguments.value("peek", false);
    const auto    read    = [mailbox, context, sender, from, replyTo, peek, wait]() {
        if (!mailbox || !context || context->isCancellationRequested()
            || mailbox->stateFor(sender) == QStringLiteral("cancelled")
            || mailbox->stateFor(sender) == QStringLiteral("closed"))
            return failure("cancelled");
        const json messages = mailbox->take(sender, from, replyTo, peek);
        if (messages.empty() && !from.isEmpty()
            && (mailbox->stateFor(from) == QStringLiteral("cancelled")
                || mailbox->stateFor(from) == QStringLiteral("closed")))
            return failure("target_cancelled_or_closed");
        return encoded(
            {{"status", "ok"}, {"messages", messages}, {"timed_out", wait && messages.empty()}});
    };
    if (wait) {
        const auto timeoutValue = arguments.value("timeout_ms", json(30000));
        if (timeoutValue < 0 || timeoutValue > 60000)
            return failure("invalid_timeout");
        QEventLoop loop;
        QTimer     timer;
        timer.setSingleShot(true);
        const auto ready = [mailbox, context, sender, from, replyTo]() {
            return !mailbox || !context || context->isCancellationRequested()
                   || mailbox->stateFor(sender) == QStringLiteral("cancelled")
                   || mailbox->stateFor(sender) == QStringLiteral("closed")
                   || !mailbox->take(sender, from, replyTo, true).empty()
                   || (!from.isEmpty()
                       && (mailbox->stateFor(from) == QStringLiteral("cancelled")
                           || mailbox->stateFor(from) == QStringLiteral("closed")));
        };
        const int timeoutMs = timeoutValue.get<int>();
        if (context->canDefer() && timeoutMs > 0 && !ready()) {
            context->defer();
            auto *deadline = new QTimer(context);
            deadline->setSingleShot(true);
            const auto complete = [context, read]() {
                if (context && context->isDeferredPending())
                    context->completeDeferred(read());
            };
            for (const auto &id : QStringList{sender, from}) {
                if (auto *inbox = mailbox->inboxFor(id)) {
                    QObject::connect(inbox, &QSocAgentInbox::changed, context, [ready, complete]() {
                        if (ready())
                            complete();
                    });
                    QObject::connect(inbox, &QObject::destroyed, context, complete);
                }
            }
            QObject::connect(mailbox, &QObject::destroyed, context, complete);
            QObject::connect(context, &QSocToolCallContext::cancellationRequested, context, complete);
            QObject::connect(deadline, &QTimer::timeout, context, complete);
            deadline->start(timeoutMs);
            return {};
        }
        for (const auto &id : QStringList{sender, from}) {
            if (auto *inbox = mailbox->inboxFor(id)) {
                QObject::connect(inbox, &QSocAgentInbox::changed, &loop, [&]() {
                    if (ready())
                        loop.quit();
                });
                QObject::connect(inbox, &QObject::destroyed, &loop, &QEventLoop::quit);
            }
        }
        QObject::connect(mailbox, &QObject::destroyed, &loop, &QEventLoop::quit);
        QObject::connect(
            context, &QSocToolCallContext::cancellationRequested, &loop, &QEventLoop::quit);
        QObject::connect(&timer, &QTimer::timeout, &loop, &QEventLoop::quit);
        if (!ready() && timeoutMs > 0) {
            timer.start(timeoutMs);
            loop.exec();
        }
    }
    return read();
}
