// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsocsessiontranscript.h"

#include "tui/qtuiassistanttextblock.h"
#include "tui/qtuiscrollview.h"
#include "tui/qtuitoolblock.h"
#include "tui/qtuiuserblock.h"

#include <nlohmann/json.hpp>

#include <QHash>
#include <QSet>
#include <QStringList>

#include <memory>

namespace QSocSessionTranscript {

namespace {

using json = nlohmann::json;

const QString kSummaryPrefix = QStringLiteral("[Conversation Summary]\n");

struct PendingTool
{
    QString name;
    QString detail;
};

QString stringValue(const json &object, const char *key)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string()) {
        return {};
    }
    return QString::fromStdString(it->get<std::string>());
}

QString toolDetail(const json &function)
{
    const auto argumentsIt = function.find("arguments");
    if (argumentsIt == function.end() || !argumentsIt->is_string()) {
        return {};
    }

    const json arguments = json::parse(argumentsIt->get_ref<const std::string &>(), nullptr, false);
    if (!arguments.is_object()) {
        return {};
    }

    static const QStringList fields{
        QStringLiteral("command"),
        QStringLiteral("title"),
        QStringLiteral("file_path"),
        QStringLiteral("path"),
        QStringLiteral("name"),
        QStringLiteral("regex"),
        QStringLiteral("query"),
        QStringLiteral("url"),
    };
    for (const QString &field : fields) {
        const std::string key = field.toStdString();
        const auto        it  = arguments.find(key);
        if (it == arguments.end() || !it->is_string()) {
            continue;
        }
        const QString value = QString::fromStdString(it->get<std::string>());
        return field == QStringLiteral("title") ? QStringLiteral("\"%1\"").arg(value) : value;
    }

    const auto id = arguments.find("id");
    if (id != arguments.end() && id->is_number_unsigned()) {
        return QStringLiteral("#%1").arg(id->get<qulonglong>());
    }
    if (id != arguments.end() && id->is_number_integer()) {
        return QStringLiteral("#%1").arg(id->get<qlonglong>());
    }
    return {};
}

bool isSyntheticUserMessage(const QString &content)
{
    static const QStringList prefixes{
        QStringLiteral("<task-notification>"),
        QStringLiteral("<goal_context>"),
        QStringLiteral("You are in plan mode and ended your turn without calling a tool."),
        QStringLiteral("[System: Context compacted."),
        QStringLiteral("[Restored file after compaction:"),
        QStringLiteral("[Referenced file after compaction:"),
        QStringLiteral("[Skills restored after compaction]"),
        QStringLiteral("[Background agents still running after compaction]"),
    };
    for (const QString &prefix : prefixes) {
        if (content.startsWith(prefix)) {
            return true;
        }
    }
    return false;
}

void appendAssistant(const QString &content, QTuiScrollView &view, bool dim = false)
{
    if (content.isEmpty()) {
        return;
    }
    auto block = std::make_unique<QTuiAssistantTextBlock>(content);
    block->setDimAll(dim);
    view.appendBlock(std::move(block));
}

void startToolBatch(const json &calls, QHash<QString, PendingTool> &pending)
{
    pending.clear();
    if (!calls.is_array()) {
        return;
    }

    QSet<QString> seenIds;
    for (const json &call : calls) {
        if (!call.is_object()) {
            continue;
        }
        const QString id         = stringValue(call, "id");
        const auto    functionIt = call.find("function");
        if (id.isEmpty()) {
            continue;
        }
        if (seenIds.contains(id)) {
            pending.remove(id);
            continue;
        }
        seenIds.insert(id);
        if (functionIt == call.end() || !functionIt->is_object()) {
            continue;
        }
        const QString name = stringValue(*functionIt, "name");
        if (name.isEmpty()) {
            continue;
        }
        pending.insert(id, PendingTool{name, toolDetail(*functionIt)});
    }
}

} // namespace

void appendTo(const json &messages, QTuiScrollView &view)
{
    if (!messages.is_array()) {
        return;
    }

    QHash<QString, PendingTool> pending;

    for (const json &message : messages) {
        if (!message.is_object()) {
            pending.clear();
            continue;
        }

        const QString role = stringValue(message, "role");
        if (role == QStringLiteral("tool")) {
            const QString id = stringValue(message, "tool_call_id");
            if (id.isEmpty() || !pending.contains(id)) {
                continue;
            }
            const PendingTool tool    = pending.take(id);
            const auto        content = message.find("content");
            if (content == message.end() || !content->is_string()) {
                continue;
            }
            const QString body  = QString::fromStdString(content->get<std::string>());
            auto          block = std::make_unique<QTuiToolBlock>(tool.name, tool.detail);
            block->appendBody(body);
            block->finish(QTuiToolBlock::Status::Success, QString());
            view.appendBlock(std::move(block));
            continue;
        }

        if (role == QStringLiteral("assistant")) {
            appendAssistant(stringValue(message, "content"), view);
            const auto calls = message.find("tool_calls");
            if (calls != message.end() && calls->is_array()) {
                startToolBatch(*calls, pending);
            } else {
                pending.clear();
            }
            continue;
        }

        pending.clear();
        if (role != QStringLiteral("user")) {
            continue;
        }

        const auto content = message.find("content");
        if (content == message.end() || !content->is_string()) {
            continue;
        }
        const QString text = QString::fromStdString(content->get<std::string>());
        if (text.startsWith(kSummaryPrefix)) {
            appendAssistant(
                QStringLiteral("*Conversation summary*\n\n") + text.mid(kSummaryPrefix.size()),
                view,
                true);
        } else if (!text.isEmpty() && !isSyntheticUserMessage(text)) {
            view.appendBlock(std::make_unique<QTuiUserBlock>(text));
        }
    }
}

} // namespace QSocSessionTranscript
