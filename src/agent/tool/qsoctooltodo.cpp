// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctooltodo.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QTextStream>

namespace {

/* Helper: Parse markdown todo file into structured list */
QList<QSocTodoItem> parseTodoMarkdown(const QString &content)
{
    QList<QSocTodoItem> todos;
    QStringList         lines     = content.split('\n');
    QString             priority  = "medium";
    int                 idCounter = 0;

    /* Regex to match: - [x] or - [ ] followed by text */
    QRegularExpression todoRegex(R"(^-\s*\[([ xX])\]\s*(.+)$)");

    for (const QString &line : lines) {
        /* Check for priority section headers */
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

        /* Parse todo items */
        QRegularExpressionMatch match = todoRegex.match(line.trimmed());
        if (match.hasMatch()) {
            QSocTodoItem item;
            item.id       = ++idCounter;
            item.title    = match.captured(2).trimmed();
            item.priority = priority;

            QString checkbox = match.captured(1);
            if (checkbox.toLower() == "x") {
                item.status = "done";
            } else {
                item.status = "pending";
            }

            todos.append(item);
        }
    }

    return todos;
}

/* Helper: Generate markdown from todo list */
QString generateTodoMarkdown(const QList<QSocTodoItem> &todos)
{
    QString result = "# QSoC Todo List\n\n";

    /* Group by priority */
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

    /* Output high priority */
    if (!highPriority.isEmpty()) {
        result += "## High Priority\n\n";
        for (const QSocTodoItem &item : highPriority) {
            QString checkbox = (item.status == "done") ? "[x]" : "[ ]";
            result += QString("- %1 %2\n").arg(checkbox, item.title);
        }
        result += "\n";
    }

    /* Output medium priority */
    if (!mediumPriority.isEmpty()) {
        result += "## Medium Priority\n\n";
        for (const QSocTodoItem &item : mediumPriority) {
            QString checkbox = (item.status == "done") ? "[x]" : "[ ]";
            result += QString("- %1 %2\n").arg(checkbox, item.title);
        }
        result += "\n";
    }

    /* Output low priority */
    if (!lowPriority.isEmpty()) {
        result += "## Low Priority\n\n";
        for (const QSocTodoItem &item : lowPriority) {
            QString checkbox = (item.status == "done") ? "[x]" : "[ ]";
            result += QString("- %1 %2\n").arg(checkbox, item.title);
        }
        result += "\n";
    }

    return result;
}

} /* anonymous namespace */

/* QSocToolTodoList Implementation */

QSocToolTodoList::QSocToolTodoList(QObject *parent, QSocProjectManager *projectManager)
    : QSocTool(parent)
    , projectManager(projectManager)
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

QString QSocToolTodoList::todoFilePath() const
{
    if (!projectManager) {
        return {};
    }

    QString projectPath = projectManager->getProjectPath();
    if (projectPath.isEmpty()) {
        projectPath = QDir::currentPath();
    }

    return QDir(projectPath).filePath(".qsoc/todos.md");
}

QList<QSocTodoItem> QSocToolTodoList::loadTodos() const
{
    QString filePath = todoFilePath();
    if (filePath.isEmpty()) {
        return {};
    }

    QFile file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QTextStream stream(&file);
    QString     content = stream.readAll();
    file.close();

    return parseTodoMarkdown(content);
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

    QList<QSocTodoItem> todos = loadTodos();

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

void QSocToolTodoList::setProjectManager(QSocProjectManager *projectManager)
{
    projectManager = projectManager;
}

/* QSocToolTodoAdd Implementation */

QSocToolTodoAdd::QSocToolTodoAdd(QObject *parent, QSocProjectManager *projectManager)
    : QSocTool(parent)
    , projectManager(projectManager)
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

QString QSocToolTodoAdd::todoFilePath() const
{
    if (!projectManager) {
        return {};
    }

    QString projectPath = projectManager->getProjectPath();
    if (projectPath.isEmpty()) {
        projectPath = QDir::currentPath();
    }

    return QDir(projectPath).filePath(".qsoc/todos.md");
}

QList<QSocTodoItem> QSocToolTodoAdd::loadTodos() const
{
    QString filePath = todoFilePath();
    if (filePath.isEmpty()) {
        return {};
    }

    QFile file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QTextStream stream(&file);
    QString     content = stream.readAll();
    file.close();

    return parseTodoMarkdown(content);
}

bool QSocToolTodoAdd::saveTodos(const QList<QSocTodoItem> &todos) const
{
    QString filePath = todoFilePath();
    if (filePath.isEmpty()) {
        return false;
    }

    /* Create parent directories if needed */
    QFileInfo fileInfo(filePath);
    QDir      parentDir = fileInfo.absoluteDir();
    if (!parentDir.exists()) {
        if (!parentDir.mkpath(".")) {
            return false;
        }
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream stream(&file);
    stream << generateTodoMarkdown(todos);
    file.close();

    return true;
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

    QList<QSocTodoItem> todos = loadTodos();

    /* Create new item */
    QSocTodoItem newItem;
    newItem.id       = static_cast<int>(todos.size()) + 1;
    newItem.title    = title;
    newItem.priority = priority;
    newItem.status   = "pending";

    todos.append(newItem);

    if (!saveTodos(todos)) {
        return "Error: Failed to save todo list";
    }

    return QString("Added todo #%1: %2 (%3 priority)").arg(newItem.id).arg(title, priority);
}

void QSocToolTodoAdd::setProjectManager(QSocProjectManager *projectManager)
{
    projectManager = projectManager;
}

/* QSocToolTodoUpdate Implementation */

QSocToolTodoUpdate::QSocToolTodoUpdate(QObject *parent, QSocProjectManager *projectManager)
    : QSocTool(parent)
    , projectManager(projectManager)
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

QString QSocToolTodoUpdate::todoFilePath() const
{
    if (!projectManager) {
        return {};
    }

    QString projectPath = projectManager->getProjectPath();
    if (projectPath.isEmpty()) {
        projectPath = QDir::currentPath();
    }

    return QDir(projectPath).filePath(".qsoc/todos.md");
}

QList<QSocTodoItem> QSocToolTodoUpdate::loadTodos() const
{
    QString filePath = todoFilePath();
    if (filePath.isEmpty()) {
        return {};
    }

    QFile file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QTextStream stream(&file);
    QString     content = stream.readAll();
    file.close();

    return parseTodoMarkdown(content);
}

bool QSocToolTodoUpdate::saveTodos(const QList<QSocTodoItem> &todos) const
{
    QString filePath = todoFilePath();
    if (filePath.isEmpty()) {
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream stream(&file);
    stream << generateTodoMarkdown(todos);
    file.close();

    return true;
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

    QList<QSocTodoItem> todos = loadTodos();

    /* Find and update the item */
    bool found = false;
    for (QSocTodoItem &item : todos) {
        if (item.id == todoId) {
            item.status = status;
            found       = true;
            break;
        }
    }

    if (!found) {
        return QString("Error: Todo #%1 not found").arg(todoId);
    }

    if (!saveTodos(todos)) {
        return "Error: Failed to save todo list";
    }

    return QString("Updated todo #%1 status to: %2").arg(todoId).arg(status);
}

void QSocToolTodoUpdate::setProjectManager(QSocProjectManager *projectManager)
{
    projectManager = projectManager;
}

/* QSocToolTodoDelete Implementation */

QSocToolTodoDelete::QSocToolTodoDelete(QObject *parent, QSocProjectManager *projectManager)
    : QSocTool(parent)
    , projectManager(projectManager)
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

QString QSocToolTodoDelete::todoFilePath() const
{
    if (!projectManager) {
        return {};
    }

    QString projectPath = projectManager->getProjectPath();
    if (projectPath.isEmpty()) {
        projectPath = QDir::currentPath();
    }

    return QDir(projectPath).filePath(".qsoc/todos.md");
}

QList<QSocTodoItem> QSocToolTodoDelete::loadTodos() const
{
    QString filePath = todoFilePath();
    if (filePath.isEmpty()) {
        return {};
    }

    QFile file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QTextStream stream(&file);
    QString     content = stream.readAll();
    file.close();

    return parseTodoMarkdown(content);
}

bool QSocToolTodoDelete::saveTodos(const QList<QSocTodoItem> &todos) const
{
    QString filePath = todoFilePath();
    if (filePath.isEmpty()) {
        return false;
    }

    QFile file(filePath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return false;
    }

    QTextStream stream(&file);
    stream << generateTodoMarkdown(todos);
    file.close();

    return true;
}

QString QSocToolTodoDelete::execute(const json &arguments)
{
    if (!arguments.contains("id") || !arguments["id"].is_number_integer()) {
        return "Error: id is required (integer)";
    }

    int todoId = arguments["id"].get<int>();

    QList<QSocTodoItem> todos = loadTodos();

    /* Find and remove the item */
    bool found = false;
    for (int idx = 0; idx < todos.size(); ++idx) {
        if (todos[idx].id == todoId) {
            todos.removeAt(idx);
            found = true;
            break;
        }
    }

    if (!found) {
        return QString("Error: Todo #%1 not found").arg(todoId);
    }

    /* Renumber remaining items */
    for (int idx = 0; idx < todos.size(); ++idx) {
        todos[idx].id = idx + 1;
    }

    if (!saveTodos(todos)) {
        return "Error: Failed to save todo list";
    }

    return QString("Deleted todo #%1").arg(todoId);
}

void QSocToolTodoDelete::setProjectManager(QSocProjectManager *projectManager)
{
    projectManager = projectManager;
}
