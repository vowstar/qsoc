// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctooltodo.h"
#include "agent/qsocagent.h"
#include "agent/qsocworkspacefs.h"

QSocToolTodo::QSocToolTodo(QObject *parent, QSocWorkspaceFs *fs, const QString &operation)
    : QSocTool(parent)
    , fs_(fs)
    , operation_(operation)
{}

QString QSocToolTodo::getDescription() const
{
    if (operation_ == "todo_list")
        return QStringLiteral(
            "List workspace todos, their state and dependencies. plan_id=current selects this "
            "agent's current plan.");
    if (operation_ == "todo_add")
        return QStringLiteral(
            "Add a todo to this agent's current plan. Use short, verifiable outcomes for complex "
            "work. Optional plan_id starts a named plan. Dependencies must be existing todos in "
            "that plan.");
    if (operation_ == "todo_update")
        return QStringLiteral(
            "Update a todo's state, completion criteria or dependencies. Mark done after checking "
            "its result. A dependency cycle is rejected; remove dependency edges to repair a "
            "cyclic imported plan.");
    return QStringLiteral(
        "Delete a todo without renumbering IDs. Remove incoming dependencies before deleting a "
        "referenced todo.");
}

json QSocToolTodo::getParametersSchema() const
{
    json properties = json::object();
    json required   = json::array();
    if (operation_ == "todo_list") {
        properties
            = {{"filter", {{"type", "string"}, {"enum", {"all", "pending", "done"}}}},
               {"plan_id", {{"type", "string"}, {"description", "A plan ID, or current"}}}};
    } else if (operation_ == "todo_add") {
        properties
            = {{"title", {{"type", "string"}}},
               {"priority", {{"type", "string"}, {"enum", {"high", "medium", "low"}}}},
               {"plan_id",
                {{"type", "string"},
                 {"description", "Optional name for a new plan within this session or goal"}}}};
        required = {"title"};
    } else {
        properties = {{"id", {{"type", "integer"}, {"minimum", 1}}}};
        required   = {"id"};
    }
    if (operation_ == "todo_update")
        properties["status"] = {{"type", "string"}, {"enum", {"done", "pending", "in_progress"}}};
    if (operation_ == "todo_add" || operation_ == "todo_update") {
        properties["description"]
            = {{"type", "string"}, {"description", "Completion criteria, at most 4096 characters"}};
        properties["depends_on"]
            = {{"type", "array"},
               {"items", {{"type", "integer"}, {"minimum", 1}}},
               {"uniqueItems", true}};
    }
    return {
        {"type", "object"},
        {"properties", properties},
        {"required", required},
        {"additionalProperties", false}};
}

QSocTodoModel *QSocToolTodo::model() const
{
    return fs_ ? fs_->todos() : nullptr;
}

QString QSocToolTodo::execute(const json &arguments)
{
    if (!fs_)
        return QStringLiteral("Error: no workspace is bound");
    const auto context = currentCallContext();
    const auto caller  = context ? qobject_cast<QSocAgent *>(context->executionScope()) : nullptr;
    if (context && context->isCancellationRequested())
        return QStringLiteral("Error: cancelled");
    return fs_->todos()->execute(
        operation_,
        arguments,
        caller ? caller->taskPlanId() : QString(),
        caller && caller->getConfig().isSubAgent);
}

#include "moc_qsoctooltodo.cpp"
