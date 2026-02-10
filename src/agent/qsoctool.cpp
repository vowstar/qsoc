// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctool.h"

/* QSocTool Implementation */

QSocTool::QSocTool(QObject *parent)
    : QObject(parent)
{}

QSocTool::~QSocTool() = default;

void QSocTool::abort() {}

json QSocTool::getDefinition() const
{
    return {
        {"type", "function"},
        {"function",
         {{"name", getName().toStdString()},
          {"description", getDescription().toStdString()},
          {"parameters", getParametersSchema()}}}};
}

/* QSocToolRegistry Implementation */

QSocToolRegistry::QSocToolRegistry(QObject *parent)
    : QObject(parent)
{}

QSocToolRegistry::~QSocToolRegistry() = default;

void QSocToolRegistry::registerTool(QSocTool *tool)
{
    if (tool) {
        tools_[tool->getName()] = tool;
    }
}

QSocTool *QSocToolRegistry::getTool(const QString &name) const
{
    return tools_.value(name, nullptr);
}

bool QSocToolRegistry::hasTool(const QString &name) const
{
    return tools_.contains(name);
}

json QSocToolRegistry::getToolDefinitions() const
{
    json definitions = json::array();
    for (auto it = tools_.constBegin(); it != tools_.constEnd(); ++it) {
        definitions.push_back(it.value()->getDefinition());
    }
    return definitions;
}

QString QSocToolRegistry::executeTool(const QString &name, const json &arguments)
{
    QSocTool *tool = getTool(name);
    if (!tool) {
        return QString("Error: Tool '%1' not found").arg(name);
    }
    return tool->execute(arguments);
}

int QSocToolRegistry::count() const
{
    return static_cast<int>(tools_.size());
}

QStringList QSocToolRegistry::toolNames() const
{
    return tools_.keys();
}

void QSocToolRegistry::abortAll()
{
    for (auto *tool : tools_) {
        tool->abort();
    }
}
