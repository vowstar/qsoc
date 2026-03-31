// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolmemory.h"

/* QSocToolMemoryRead Implementation */

QSocToolMemoryRead::QSocToolMemoryRead(QObject *parent, QSocMemoryManager *memoryManager)
    : QSocTool(parent)
    , memoryManager(memoryManager)
{}

QSocToolMemoryRead::~QSocToolMemoryRead() = default;

QString QSocToolMemoryRead::getName() const
{
    return "memory_read";
}

QString QSocToolMemoryRead::getDescription() const
{
    return "Read persistent memory. Memory is organized as topic files with YAML frontmatter "
           "in two scopes: 'user' (global preferences) and 'project' (project-specific context). "
           "The MEMORY.md index is auto-loaded into the system prompt; use this tool to read "
           "individual topic file content.";
}

json QSocToolMemoryRead::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"scope",
           {{"type", "string"},
            {"enum", {"user", "project", "all"}},
            {"description", "Which memory scope to read (default: all)"}}},
          {"name",
           {{"type", "string"},
            {"description",
             "Read a specific topic file by name (e.g., 'user-preferences'). "
             "If omitted, lists all topics with their metadata."}}},
          {"type",
           {{"type", "string"},
            {"enum", {"user", "feedback", "project", "reference"}},
            {"description", "Filter by memory type"}}}}},
        {"required", json::array()}};
}

QString QSocToolMemoryRead::execute(const json &arguments)
{
    if (!memoryManager) {
        return "Error: Memory manager not configured";
    }

    QString scope = "all";
    if (arguments.contains("scope") && arguments["scope"].is_string()) {
        scope = QString::fromStdString(arguments["scope"].get<std::string>());
    }

    /* Read a specific topic file */
    if (arguments.contains("name") && arguments["name"].is_string()) {
        QString name = QString::fromStdString(arguments["name"].get<std::string>());

        /* Try user scope first, then project */
        if (scope == "user" || scope == "all") {
            QString content = memoryManager->readTopicFile("user", name);
            if (!content.isEmpty()) {
                return QString("## %1 (user scope)\n\n%2").arg(name, content);
            }
        }
        if (scope == "project" || scope == "all") {
            QString content = memoryManager->readTopicFile("project", name);
            if (!content.isEmpty()) {
                return QString("## %1 (project scope)\n\n%2").arg(name, content);
            }
        }
        return QString("No topic file found: %1").arg(name);
    }

    /* List all topics with metadata */
    QString typeFilter;
    if (arguments.contains("type") && arguments["type"].is_string()) {
        typeFilter = QString::fromStdString(arguments["type"].get<std::string>());
    }

    auto entries = memoryManager->scanMemories(scope);

    if (entries.isEmpty()) {
        return "No memory found. Use memory_write to save preferences and context.";
    }

    QString result;
    for (const auto &entry : entries) {
        if (!typeFilter.isEmpty() && entry.type != typeFilter) {
            continue;
        }

        QString age;
        if (entry.ageDays > 1) {
            age = QString(" (%1 days ago)").arg(entry.ageDays);
        }

        result += QString("- **%1** [%2] — %3%4\n")
                      .arg(
                          entry.name,
                          entry.type.isEmpty() ? "untyped" : entry.type,
                          entry.description.isEmpty() ? "(no description)" : entry.description,
                          age);
    }

    if (result.isEmpty()) {
        return QString("No memory found matching type '%1'.").arg(typeFilter);
    }

    return result;
}

void QSocToolMemoryRead::setMemoryManager(QSocMemoryManager *memoryManager)
{
    this->memoryManager = memoryManager;
}

/* QSocToolMemoryWrite Implementation */

QSocToolMemoryWrite::QSocToolMemoryWrite(QObject *parent, QSocMemoryManager *memoryManager)
    : QSocTool(parent)
    , memoryManager(memoryManager)
{}

QSocToolMemoryWrite::~QSocToolMemoryWrite() = default;

QString QSocToolMemoryWrite::getName() const
{
    return "memory_write";
}

QString QSocToolMemoryWrite::getDescription() const
{
    return "Write persistent memory as a topic file with YAML frontmatter. "
           "The MEMORY.md index is automatically rebuilt after each write. "
           "Memory types: 'user' (role/preferences), 'feedback' (approach corrections), "
           "'project' (decisions/context), 'reference' (external resources).";
}

json QSocToolMemoryWrite::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"name",
           {{"type", "string"},
            {"description",
             "Topic filename (e.g., 'user-preferences', 'project-architecture'). "
             "Lowercase, hyphens allowed."}}},
          {"content",
           {{"type", "string"}, {"description", "The memory content to write (Markdown format)"}}},
          {"scope",
           {{"type", "string"},
            {"enum", {"user", "project"}},
            {"description",
             "Where to save: 'user' for global preferences, 'project' for project context"}}},
          {"type",
           {{"type", "string"},
            {"enum", {"user", "feedback", "project", "reference"}},
            {"description", "Memory type for categorization"}}},
          {"description",
           {{"type", "string"},
            {"description",
             "One-line description for the MEMORY.md index (be specific for relevance)"}}}}},
        {"required", json::array({"name", "content", "scope"})}};
}

QString QSocToolMemoryWrite::execute(const json &arguments)
{
    if (!memoryManager) {
        return "Error: Memory manager not configured";
    }

    /* Extract required parameters */
    if (!arguments.contains("name") || !arguments["name"].is_string()) {
        return "Error: 'name' is required (topic filename)";
    }
    if (!arguments.contains("content") || !arguments["content"].is_string()) {
        return "Error: 'content' is required";
    }
    if (!arguments.contains("scope") || !arguments["scope"].is_string()) {
        return "Error: 'scope' is required ('user' or 'project')";
    }

    QString name    = QString::fromStdString(arguments["name"].get<std::string>());
    QString content = QString::fromStdString(arguments["content"].get<std::string>());
    QString scope   = QString::fromStdString(arguments["scope"].get<std::string>());

    if (scope != "user" && scope != "project") {
        return "Error: scope must be 'user' or 'project'";
    }

    /* Extract optional parameters */
    QString type;
    if (arguments.contains("type") && arguments["type"].is_string()) {
        type = QString::fromStdString(arguments["type"].get<std::string>());
    }

    QString description;
    if (arguments.contains("description") && arguments["description"].is_string()) {
        description = QString::fromStdString(arguments["description"].get<std::string>());
    }

    /* Write topic file (auto-rebuilds index) */
    if (!memoryManager->writeTopicFile(scope, name, type, description, content)) {
        return QString("Error: Failed to write memory topic '%1'").arg(name);
    }

    QString scopeDir = (scope == "user") ? memoryManager->userMemoryDir()
                                         : memoryManager->projectMemoryDir();

    return QString("Saved memory '%1' to %2 scope (%3)").arg(name, scope, scopeDir);
}

void QSocToolMemoryWrite::setMemoryManager(QSocMemoryManager *memoryManager)
{
    this->memoryManager = memoryManager;
}
