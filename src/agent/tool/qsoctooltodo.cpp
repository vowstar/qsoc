// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctooltodo.h"

#include "agent/qsocworkspacefs.h"

#include <QRegularExpression>

namespace {

const QString kTodoFile = QStringLiteral(".qsoc/todos.md");
const QString kMarkFile = QStringLiteral(".qsoc/todos.hwm");

/* Helper: Parse markdown todo file into structured list.
 * Format: - [x] #42 Task title
 * IDs are stable — never renumbered after deletion. */
QList<QSocTodoItem> parseTodoMarkdown(const QString &content)
{
    QList<QSocTodoItem> todos;
    QStringList         lines    = content.split('\n');
    QString             priority = "medium";

    /* Match: - [x] #42 Title  or legacy: - [x] Title (no ID) */
    QRegularExpression todoRegex(R"(^-\s*\[([ xX])\]\s*(?:#(\d+)\s+)?(.+)$)");

    int legacyId = 0;

    for (const QString &line : lines) {
        if (line.startsWith("## High Priority")) {
            priority = "high";
            continue;
        }
        if (line.startsWith("## Medium Priority")) {
            priority = "medium";
            continue;
        }
        if (line.startsWith("## Low Priority")) {
            priority = "low";
            continue;
        }

        QRegularExpressionMatch match = todoRegex.match(line.trimmed());
        if (match.hasMatch()) {
            QSocTodoItem item;
            QString      idStr = match.captured(2);
            item.id            = idStr.isEmpty() ? ++legacyId : idStr.toInt();
            item.title         = match.captured(3).trimmed();
            item.priority      = priority;
            item.status        = (match.captured(1).toLower() == "x") ? "done" : "pending";
            todos.append(item);
        }
    }

    return todos;
}

/* Helper: Generate markdown from todo list.
 * Embeds stable IDs: - [ ] #42 Task title */
QString generateTodoMarkdown(const QList<QSocTodoItem> &todos)
{
    QString result = "# QSoC Todo List\n\n";

    QList<QSocTodoItem> highPriority;
    QList<QSocTodoItem> mediumPriority;
    QList<QSocTodoItem> lowPriority;

    for (const QSocTodoItem &item : todos) {
        if (item.priority == "high") {
            highPriority.append(item);
        } else if (item.priority == "low") {
            lowPriority.append(item);
        } else {
            mediumPriority.append(item);
        }
    }

    auto writeGroup = [&result](const QString &header, const QList<QSocTodoItem> &group) {
        if (group.isEmpty()) {
            return;
        }
        result += header + "\n\n";
        for (const QSocTodoItem &item : group) {
            QString checkbox = (item.status == "done") ? "[x]" : "[ ]";
            result += QString("- %1 #%2 %3\n").arg(checkbox).arg(item.id).arg(item.title);
        }
        result += "\n";
    };

    writeGroup("## High Priority", highPriority);
    writeGroup("## Medium Priority", mediumPriority);
    writeGroup("## Low Priority", lowPriority);

    return result;
}

QString storeError(const QString &error)
{
    return QStringLiteral("Error: ") + error;
}

/* An absent file is an empty list; a failed read is an error, never an empty
 * list, so a later save cannot overwrite todos that were not read. */
bool loadTodos(QSocWorkspaceFs *fs, QList<QSocTodoItem> *todos, QString *error)
{
    if (fs == nullptr) {
        *error = QStringLiteral("no workspace is bound");
        return false;
    }
    QByteArray bytes;
    switch (fs->read(kTodoFile, &bytes, error)) {
    case QSocWorkspaceFs::Result::Absent:
        todos->clear();
        return true;
    case QSocWorkspaceFs::Result::Failed:
        return false;
    case QSocWorkspaceFs::Result::Ok:
        break;
    }
    *todos = parseTodoMarkdown(QString::fromUtf8(bytes));
    return true;
}

bool saveTodos(QSocWorkspaceFs *fs, const QList<QSocTodoItem> &todos, QString *error)
{
    return fs->write(kTodoFile, generateTodoMarkdown(todos).toUtf8(), error);
}

/* The highest id ever handed out, so deleted ids are never reused. */
int readHighWaterMark(QSocWorkspaceFs *fs)
{
    QByteArray bytes;
    QString    error;
    if (fs->read(kMarkFile, &bytes, &error) != QSocWorkspaceFs::Result::Ok) {
        return 0;
    }
    return QString::fromUtf8(bytes).trimmed().toInt();
}

} /* anonymous namespace */

/* QSocToolTodoList Implementation */

QSocToolTodoList::QSocToolTodoList(QObject *parent, QSocWorkspaceFs *fs)
    : QSocTool(parent)
    , fs(fs)
{}

QSocToolTodoList::~QSocToolTodoList() = default;

QString QSocToolTodoList::getName() const
{
    return "todo_list";
}

QString QSocToolTodoList::getDescription() const
{
    return "List all todo items for the current project. "
           "Shows task title, priority, and completion status.";
}

json QSocToolTodoList::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"filter",
           {{"type", "string"},
            {"enum", {"all", "pending", "done"}},
            {"description", "Filter by status: 'all', 'pending', or 'done' (default: all)"}}}}},
        {"required", json::array()}};
}

QString QSocToolTodoList::formatTodoList(const QList<QSocTodoItem> &todos) const
{
    if (todos.isEmpty()) {
        return "No todos found. Use todo_add to create new tasks.";
    }

    QString result = "Todo List:\n";
    for (const QSocTodoItem &item : todos) {
        QString checkbox = (item.status == "done") ? "[x]" : "[ ]";
        result
            += QString("%1 %2. %3 (%4)\n").arg(checkbox).arg(item.id).arg(item.title, item.priority);
    }

    return result;
}

QString QSocToolTodoList::execute(const json &arguments)
{
    QString filter = "all";
    if (arguments.contains("filter") && arguments["filter"].is_string()) {
        filter = QString::fromStdString(arguments["filter"].get<std::string>());
    }

    QList<QSocTodoItem> todos;
    QString             error;
    if (!loadTodos(fs, &todos, &error)) {
        return storeError(error);
    }

    /* Apply filter */
    if (filter == "pending") {
        QList<QSocTodoItem> filtered;
        for (const QSocTodoItem &item : todos) {
            if (item.status != "done") {
                filtered.append(item);
            }
        }
        todos = filtered;
    } else if (filter == "done") {
        QList<QSocTodoItem> filtered;
        for (const QSocTodoItem &item : todos) {
            if (item.status == "done") {
                filtered.append(item);
            }
        }
        todos = filtered;
    }

    return formatTodoList(todos);
}

/* QSocToolTodoAdd Implementation */

QSocToolTodoAdd::QSocToolTodoAdd(QObject *parent, QSocWorkspaceFs *fs)
    : QSocTool(parent)
    , fs(fs)
{}

QSocToolTodoAdd::~QSocToolTodoAdd() = default;

QString QSocToolTodoAdd::getName() const
{
    return "todo_add";
}

QString QSocToolTodoAdd::getDescription() const
{
    return "Add a new todo item to the project task list.";
}

json QSocToolTodoAdd::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"title", {{"type", "string"}, {"description", "Brief title for the todo item"}}},
          {"priority",
           {{"type", "string"},
            {"enum", {"high", "medium", "low"}},
            {"description", "Priority level (default: medium)"}}}}},
        {"required", json::array({"title"})}};
}

QString QSocToolTodoAdd::execute(const json &arguments)
{
    if (!arguments.contains("title") || !arguments["title"].is_string()) {
        return "Error: title is required";
    }

    QString title = QString::fromStdString(arguments["title"].get<std::string>());

    QString priority = "medium";
    if (arguments.contains("priority") && arguments["priority"].is_string()) {
        priority = QString::fromStdString(arguments["priority"].get<std::string>());
        if (priority != "high" && priority != "medium" && priority != "low") {
            priority = "medium";
        }
    }

    QList<QSocTodoItem> todos;
    QString             error;
    if (!loadTodos(fs, &todos, &error)) {
        return storeError(error);
    }

    /* Allocate next ID from high water mark */
    int hwm = readHighWaterMark(fs);

    /* Also check existing todos for max ID (in case hwm file is stale) */
    for (const auto &item : todos) {
        hwm = qMax(hwm, item.id);
    }

    QSocTodoItem newItem;
    newItem.id       = hwm + 1;
    newItem.title    = title;
    newItem.priority = priority;
    newItem.status   = "pending";

    todos.append(newItem);

    if (!fs->write(kMarkFile, QByteArray::number(newItem.id), &error)
        || !saveTodos(fs, todos, &error)) {
        return storeError(error);
    }

    return QString("Added todo #%1: %2 (%3 priority)").arg(newItem.id).arg(title, priority);
}

/* QSocToolTodoUpdate Implementation */

QSocToolTodoUpdate::QSocToolTodoUpdate(QObject *parent, QSocWorkspaceFs *fs)
    : QSocTool(parent)
    , fs(fs)
{}

QSocToolTodoUpdate::~QSocToolTodoUpdate() = default;

QString QSocToolTodoUpdate::getName() const
{
    return "todo_update";
}

QString QSocToolTodoUpdate::getDescription() const
{
    return "Update a todo item's status (mark as done, pending, or in_progress).";
}

json QSocToolTodoUpdate::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"id", {{"type", "integer"}, {"description", "The todo item ID to update"}}},
          {"status",
           {{"type", "string"},
            {"enum", {"done", "pending", "in_progress"}},
            {"description", "New status for the todo item"}}}}},
        {"required", json::array({"id", "status"})}};
}

QString QSocToolTodoUpdate::execute(const json &arguments)
{
    if (!arguments.contains("id") || !arguments["id"].is_number_integer()) {
        return "Error: id is required (integer)";
    }

    if (!arguments.contains("status") || !arguments["status"].is_string()) {
        return "Error: status is required";
    }

    int     todoId = arguments["id"].get<int>();
    QString status = QString::fromStdString(arguments["status"].get<std::string>());

    if (status != "done" && status != "pending" && status != "in_progress") {
        return "Error: status must be 'done', 'pending', or 'in_progress'";
    }

    QList<QSocTodoItem> todos;
    QString             error;
    if (!loadTodos(fs, &todos, &error)) {
        return storeError(error);
    }

    /* Find and update the item */
    bool    found = false;
    QString title;
    for (QSocTodoItem &item : todos) {
        if (item.id == todoId) {
            title       = item.title;
            item.status = status;
            found       = true;
            break;
        }
    }

    if (!found) {
        return QString("Error: Todo #%1 not found").arg(todoId);
    }

    if (!saveTodos(fs, todos, &error)) {
        return storeError(error);
    }

    return QString("Updated todo #%1: %2 (status: %3)").arg(todoId).arg(title, status);
}

/* QSocToolTodoDelete Implementation */

QSocToolTodoDelete::QSocToolTodoDelete(QObject *parent, QSocWorkspaceFs *fs)
    : QSocTool(parent)
    , fs(fs)
{}

QSocToolTodoDelete::~QSocToolTodoDelete() = default;

QString QSocToolTodoDelete::getName() const
{
    return "todo_delete";
}

QString QSocToolTodoDelete::getDescription() const
{
    return "Delete a todo item from the project task list.";
}

json QSocToolTodoDelete::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"id", {{"type", "integer"}, {"description", "The todo item ID to delete"}}}}},
        {"required", json::array({"id"})}};
}

QString QSocToolTodoDelete::execute(const json &arguments)
{
    if (!arguments.contains("id") || !arguments["id"].is_number_integer()) {
        return "Error: id is required (integer)";
    }

    int todoId = arguments["id"].get<int>();

    QList<QSocTodoItem> todos;
    QString             error;
    if (!loadTodos(fs, &todos, &error)) {
        return storeError(error);
    }

    /* Find and remove the item */
    bool    found = false;
    QString title;
    for (int idx = 0; idx < todos.size(); ++idx) {
        if (todos[idx].id == todoId) {
            title = todos[idx].title;
            todos.removeAt(idx);
            found = true;
            break;
        }
    }

    if (!found) {
        return QString("Error: Todo #%1 not found").arg(todoId);
    }

    /* IDs are stable — no renumbering after deletion */

    if (!saveTodos(fs, todos, &error)) {
        return storeError(error);
    }

    return QString("Deleted todo #%1: %2").arg(todoId).arg(title);
}

#include "moc_qsoctooltodo.cpp"
