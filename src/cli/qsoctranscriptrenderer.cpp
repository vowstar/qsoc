// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsoctranscriptrenderer.h"
#include "cli/qsocsessionreplay.h"
#include "cli/qsocterminaltext.h"
#include "common/qsoclinediff.h"
#include "tui/qtuiassistanttextblock.h"
#include "tui/qtuicompositor.h"
#include "tui/qtuidiffblock.h"
#include "tui/qtuiimagepreviewblock.h"
#include "tui/qtuitodoblock.h"

#include <QRegularExpression>
#include <QStringList>

#include <memory>

namespace {

using Kind = QSocAgentRuntimeEvent::Kind;

QList<QTuiTodoList::TodoItem> parseTodoListResult(const QString &result)
{
    QList<QTuiTodoList::TodoItem> items;

    /* Match pattern: [x] or [ ] followed by ID. Title (priority) */
    QRegularExpression regex(R"(\[([ x])\]\s*(\d+)\.\s*(.+?)\s*\((\w+)\))");

    const QStringList lines = result.split('\n');
    for (const QString &line : lines) {
        QRegularExpressionMatch match = regex.match(line);
        if (match.hasMatch()) {
            QTuiTodoList::TodoItem item;
            item.status   = (match.captured(1) == "x") ? "done" : "pending";
            item.id       = match.captured(2).toInt();
            item.title    = match.captured(3).trimmed();
            item.priority = match.captured(4);
            items.append(item);
        }
    }

    return items;
}

/**
 * @brief Parse todo_add result into a single TodoItem
 * @param result The result string from todo_add tool
 *        Format: "Added todo #37: Title here (priority)"
 * @return TodoItem if parsed successfully, empty item if not
 */
QTuiTodoList::TodoItem parseTodoAddResult(const QString &result)
{
    QTuiTodoList::TodoItem item;
    item.id = -1; /* Invalid by default */

    /* Match: "Added todo #ID: Title (priority)" */
    QRegularExpression      regex(R"(Added todo #(\d+):\s*(.+?)\s*\((\w+)(?:\s+priority)?\))");
    QRegularExpressionMatch match = regex.match(result);

    if (match.hasMatch()) {
        item.id       = match.captured(1).toInt();
        item.title    = match.captured(2).trimmed();
        item.priority = match.captured(3);
        item.status   = "pending";
    }

    return item;
}

/**
 * @brief Parse todo_update result to extract ID and new status
 * @param result The result string from todo_update tool
 *        Format: "Updated todo #37 status to: done"
 * @return Pair of (todoId, newStatus), todoId=-1 if parse failed
 */
QPair<int, QString> parseTodoUpdateResult(const QString &result)
{
    /* Match: "Updated todo #ID: Title (status: STATUS)" */
    QRegularExpression      regex(R"(Updated todo #(\d+):\s*.+?\(status:\s*(\w+)\))");
    QRegularExpressionMatch match = regex.match(result);

    if (match.hasMatch()) {
        return qMakePair(match.captured(1).toInt(), match.captured(2));
    }

    return qMakePair(-1, QString());
}

QTuiScrollView::LineStyle styleFor(QSocAgentRuntimeStyle style)
{
    switch (style) {
    case QSocAgentRuntimeStyle::Dim:
        return QTuiScrollView::Dim;
    case QSocAgentRuntimeStyle::Bold:
        return QTuiScrollView::Bold;
    case QSocAgentRuntimeStyle::Warning:
        return QTuiScrollView::DiffHunk;
    case QSocAgentRuntimeStyle::Normal:
        break;
    }
    return QTuiScrollView::Normal;
}

QTuiDiffBlock::Kind diffKind(QSocLineDiff::Kind kind)
{
    switch (kind) {
    case QSocLineDiff::Kind::Add:
        return QTuiDiffBlock::Kind::Add;
    case QSocLineDiff::Kind::Del:
        return QTuiDiffBlock::Kind::Del;
    case QSocLineDiff::Kind::Hunk:
        return QTuiDiffBlock::Kind::Hunk;
    case QSocLineDiff::Kind::Context:
        break;
    }
    return QTuiDiffBlock::Kind::Context;
}

QString jsonString(const nlohmann::json &object, const char *key)
{
    if (!object.is_object())
        return {};
    const auto it = object.find(key);
    if (it == object.end() || !it->is_string())
        return {};
    return QString::fromStdString(it->get<std::string>());
}

/* The argument a tool header shows: the first well-known field, or the id. */
QString toolDetail(const nlohmann::json &arguments)
{
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
        const QString value = jsonString(arguments, field.toUtf8().constData());
        if (!value.isEmpty())
            return field == QStringLiteral("title") ? QStringLiteral("\"%1\"").arg(value) : value;
    }
    const auto id = arguments.is_object() ? arguments.find("id") : arguments.end();
    if (id != arguments.end() && id->is_number_unsigned())
        return QStringLiteral("#%1").arg(id->get<qulonglong>());
    if (id != arguments.end() && id->is_number_integer())
        return QStringLiteral("#%1").arg(id->get<qlonglong>());
    return {};
}

QTuiToolBlock::Status toolStatus(const QSocAgentRuntimeEvent &event)
{
    using Status = QTuiToolBlock::Status;
    static const QHash<QString, Status> statuses{
        {QStringLiteral("ok"), Status::Success},
        {QStringLiteral("failed"), Status::Failure},
        {QStringLiteral("uncertain"), Status::Uncertain},
        {QStringLiteral("dispatched"), Status::Background},
        {QStringLiteral("skipped"), Status::Skipped},
    };
    return statuses
        .value(jsonString(event.json, "status"), event.ok ? Status::Success : Status::Failure);
}

/* Events whose text came from the model, a tool, a file, a provider or a shell. */
bool isExternal(const QSocAgentRuntimeEvent &event)
{
    switch (event.kind) {
    case Kind::Output:
        return jsonString(event.json, "origin") == QStringLiteral("shell");
    case Kind::ContentChunk:
    case Kind::ReasoningChunk:
    case Kind::ToolStarted:
    case Kind::ToolOutput:
    case Kind::ToolFinished:
    case Kind::RunError:
    case Kind::TaskNotification:
    case Kind::UserMessage:
    case Kind::Diff:
    case Kind::Compacted:
    case Kind::ImagePreview:
        return true;
    default:
        return false;
    }
}

QSocAgentRuntimeEvent plainEvent(QSocAgentRuntimeEvent event)
{
    event.text      = QSocTerminalText::plain(event.text);
    event.secondary = QSocTerminalText::plain(event.secondary);
    if (event.kind != Kind::ImagePreview)
        event.json = QSocTerminalText::plain(std::move(event.json));
    return event;
}

} // namespace

QSocTranscriptRenderer::QSocTranscriptRenderer(QTuiCompositor &compositor)
    : compositor(compositor)
{}

void QSocTranscriptRenderer::apply(const QSocAgentRuntimeEvent &event)
{
    if (isExternal(event))
        render(plainEvent(event));
    else
        render(event);
}

void QSocTranscriptRenderer::render(const QSocAgentRuntimeEvent &event)
{
    switch (event.kind) {
    case Kind::ContentChunk:
        streamedContent = true;
        answerOpen      = true;
        compositor.appendAssistantChunk(event.text);
        break;
    case Kind::ReasoningChunk:
        answerOpen = true;
        compositor.appendReasoningChunk(event.text);
        break;
    case Kind::ToolStarted:
        answerOpen = true;
        startTool(event);
        break;
    case Kind::ToolOutput:
        compositor.appendToolUseBody(event.text, event.callId);
        break;
    case Kind::ToolFinished:
        answerOpen = true;
        finishTool(event);
        break;
    case Kind::RunComplete:
        if (!streamedContent && !event.text.isEmpty())
            compositor.appendAssistantChunk(event.text);
        closeAnswer();
        resetExecution();
        break;
    case Kind::RunError:
        streamedContent = false;
        answerOpen      = false;
        compositor.finishStream();
        resetExecution();
        compositor.printContent(QStringLiteral("\nError: %1\n").arg(event.text));
        break;
    case Kind::RunAborted:
        answerOpen = false;
        resetExecution();
        compositor.printContent(QStringLiteral("\n%1\n").arg(
            event.text.isEmpty() ? QStringLiteral("(interrupted)") : event.text));
        break;
    case Kind::ImagePreview:
        appendImage(event);
        break;
    case Kind::Diff:
        appendDiff(event.text, jsonString(event.json, "before"), jsonString(event.json, "after"));
        compositor.invalidate();
        break;
    case Kind::Output:
        compositor.printContent(event.text, styleFor(event.style));
        break;
    case Kind::SessionResumed:
        if (event.json.is_object() && event.json.contains("messages"))
            replaceHistory(event.json["messages"]);
        [[fallthrough]];
    case Kind::SessionStarted:
    case Kind::SessionCleared:
        compositor.printContent(event.text);
        break;
    case Kind::TaskNotification:
        compositor.printContent(
            QStringLiteral("(task: %1)\n").arg(event.text.section(QLatin1Char('\n'), 0, 0).left(160)),
            QTuiScrollView::Dim);
        break;
    case Kind::UserMessage:
        if (answerOpen)
            closeAnswer();
        compositor.appendUserMessage(event.text);
        break;
    case Kind::Compacted:
        appendSummary(event);
        break;
    default:
        break;
    }
}

void QSocTranscriptRenderer::replaceHistory(const nlohmann::json &messages)
{
    compositor.clearTranscript();
    compositor.todoList().setItems({});
    pendingArgs.clear();
    streamedContent = false;
    answerOpen      = false;
    replaying       = true;
    todoPaneKnown   = false;
    for (const auto &event : QSocSessionReplay::events(messages))
        apply(event);
    replaying     = false;
    todoPaneKnown = true;
}

void QSocTranscriptRenderer::startTool(const QSocAgentRuntimeEvent &event)
{
    auto arguments = QSocTerminalText::plain(
        nlohmann::json::parse(event.text.toStdString(), nullptr, false));
    compositor.beginToolUse(event.secondary, toolDetail(arguments), event.callId);
    pendingArgs.insert(event.callId, std::move(arguments));
}

void QSocTranscriptRenderer::finishTool(const QSocAgentRuntimeEvent &event)
{
    const auto    args   = pendingArgs.take(event.callId);
    const QString name   = event.secondary;
    const auto    status = toolStatus(event);
    updateTodos(name, event.text);
    if (status == QTuiToolBlock::Status::Success && (name == "edit_file" || name == "write_file"))
        appendDiff(
            jsonString(args, "file_path"),
            jsonString(args, "old_string"),
            jsonString(args, name == "edit_file" ? "new_string" : "content"));
    compositor.replaceToolUseBody(event.text, event.callId);
    compositor.finishToolUse(status, {}, event.callId);
}

void QSocTranscriptRenderer::closeAnswer()
{
    streamedContent = false;
    answerOpen      = false;
    compositor.finishStream();
    compositor.printContent(QStringLiteral("\n"));
}

void QSocTranscriptRenderer::resetExecution()
{
    if (!replaying)
        compositor.resetExecution();
}

void QSocTranscriptRenderer::updateTodos(const QString &name, const QString &result)
{
    auto &todos = compositor.todoList();
    if (name == "todo_list") {
        const auto items = parseTodoListResult(result);
        todos.setItems(items);
        todoPaneKnown = true;
        compositor.contentView().appendBlock(std::make_unique<QTuiTodoBlock>(items));
    } else if (!todoPaneKnown) {
        return;
    } else if (name == "todo_add") {
        const auto item = parseTodoAddResult(result);
        if (item.id >= 0)
            todos.addItem(item);
    } else if (name == "todo_update" || name == "todo_delete") {
        const auto [id, status] = parseTodoUpdateResult(result);
        if (id < 0)
            return;
        if (name == "todo_delete")
            todos.removeItem(id);
        else
            todos.updateStatus(id, status);
    }
}

void QSocTranscriptRenderer::appendDiff(
    const QString &path, const QString &before, const QString &after)
{
    auto block = std::make_unique<QTuiDiffBlock>("--- a/" + path, "+++ b/" + path);
    for (const auto &line : QSocLineDiff::computeLineDiff(before, after))
        block->addRow(diffKind(line.kind), line.text);
    compositor.contentView().appendBlock(std::move(block));
}

void QSocTranscriptRenderer::appendImage(const QSocAgentRuntimeEvent &event)
{
    if (!event.json.is_object())
        return;
    compositor.contentView().appendBlock(
        std::make_unique<QTuiImagePreviewBlock>(
            event.text,
            jsonString(event.json, "mime"),
            event.json.value("width", 0),
            event.json.value("height", 0),
            QByteArray::fromBase64(
                QByteArray::fromStdString(event.json.value("data", std::string())))));
    compositor.invalidate();
}

void QSocTranscriptRenderer::appendSummary(const QSocAgentRuntimeEvent &event)
{
    const QString summary = jsonString(event.json, "summary");
    if (summary.isEmpty())
        return;
    auto block = std::make_unique<QTuiAssistantTextBlock>(
        QStringLiteral("*Conversation summary*\n\n") + summary);
    block->setDimAll(true);
    compositor.contentView().appendBlock(std::move(block));
}
