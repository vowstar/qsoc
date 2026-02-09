// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolskill.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTextStream>

/* QSocToolSkillFind Implementation */

QSocToolSkillFind::QSocToolSkillFind(QObject *parent, QSocProjectManager *projectManager)
    : QSocTool(parent)
    , projectManager(projectManager)
{}

QSocToolSkillFind::~QSocToolSkillFind() = default;

QString QSocToolSkillFind::getName() const
{
    return "skill_find";
}

QString QSocToolSkillFind::getDescription() const
{
    return "Discover, search, and read user-defined skills (SKILL.md prompt templates). "
           "Skills extend agent capabilities without code changes.";
}

json QSocToolSkillFind::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"action",
           {{"type", "string"},
            {"enum", {"list", "search", "read"}},
            {"description", "Action: 'list' all skills, 'search' by keyword, 'read' full content"}}},
          {"query",
           {{"type", "string"},
            {"description",
             "For 'search': keyword to match in name/description. "
             "For 'read': exact skill name to retrieve."}}},
          {"scope",
           {{"type", "string"},
            {"enum", {"user", "project", "all"}},
            {"description", "Which scope to search: 'user', 'project', or 'all' (default: all)"}}}}},
        {"required", json::array({"action"})}};
}

QString QSocToolSkillFind::userSkillsPath() const
{
    QString configPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return QDir(configPath).filePath("qsoc/skills");
}

QString QSocToolSkillFind::projectSkillsPath() const
{
    if (!projectManager) {
        return {};
    }

    QString projectPath = projectManager->getProjectPath();
    if (projectPath.isEmpty()) {
        projectPath = QDir::currentPath();
    }

    return QDir(projectPath).filePath(".qsoc/skills");
}

QSocToolSkillFind::SkillInfo QSocToolSkillFind::parseSkillFile(
    const QString &filePath, const QString &scope) const
{
    SkillInfo info;
    info.path  = filePath;
    info.scope = scope;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return info;
    }

    QTextStream stream(&file);
    QString     content = stream.readAll();
    file.close();

    /* Parse YAML frontmatter between --- delimiters */
    if (!content.startsWith("---")) {
        return info;
    }

    qsizetype endMarker = content.indexOf("\n---", 3);
    if (endMarker < 0) {
        return info;
    }

    QString           frontmatter = content.mid(4, endMarker - 4);
    const QStringList lines       = frontmatter.split('\n');

    for (const QString &line : lines) {
        qsizetype colonPos = line.indexOf(':');
        if (colonPos < 0) {
            continue;
        }

        QString key   = line.left(colonPos).trimmed();
        QString value = line.mid(colonPos + 1).trimmed();

        if (key == "name") {
            info.name = value;
        } else if (key == "description") {
            info.description = value;
        }
    }

    return info;
}

QList<QSocToolSkillFind::SkillInfo> QSocToolSkillFind::scanSkillsDir(
    const QString &dirPath, const QString &scope) const
{
    QList<SkillInfo> skills;

    QDir dir(dirPath);
    if (!dir.exists()) {
        return skills;
    }

    const QStringList entries = dir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString &entry : entries) {
        QString skillFile = dir.filePath(entry + "/SKILL.md");
        if (QFile::exists(skillFile)) {
            SkillInfo info = parseSkillFile(skillFile, scope);
            if (!info.name.isEmpty()) {
                skills.append(info);
            }
        }
    }

    return skills;
}

QString QSocToolSkillFind::readSkillContent(const QString &filePath) const
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QTextStream stream(&file);
    QString     content = stream.readAll();
    file.close();

    return content;
}

QString QSocToolSkillFind::execute(const json &arguments)
{
    if (!arguments.contains("action") || !arguments["action"].is_string()) {
        return "Error: action is required (must be 'list', 'search', or 'read')";
    }

    QString action = QString::fromStdString(arguments["action"].get<std::string>());

    QString scope = "all";
    if (arguments.contains("scope") && arguments["scope"].is_string()) {
        scope = QString::fromStdString(arguments["scope"].get<std::string>());
    }

    /* Collect skills from requested scopes */
    QList<SkillInfo> allSkills;

    if (scope == "project" || scope == "all") {
        QString projPath = projectSkillsPath();
        if (!projPath.isEmpty()) {
            allSkills.append(scanSkillsDir(projPath, "project"));
        }
    }

    if (scope == "user" || scope == "all") {
        allSkills.append(scanSkillsDir(userSkillsPath(), "user"));
    }

    if (action == "list") {
        if (allSkills.isEmpty()) {
            return "No skills found. Use skill_create to create one.";
        }

        QString result = "Found " + QString::number(allSkills.size()) + " skill(s):\n\n";
        for (const SkillInfo &skill : allSkills) {
            result += "- " + skill.name + " [" + skill.scope + "]: " + skill.description + "\n";
        }
        return result;
    }

    if (action == "search") {
        if (!arguments.contains("query") || !arguments["query"].is_string()) {
            return "Error: query is required for search action";
        }

        QString query = QString::fromStdString(arguments["query"].get<std::string>());

        QList<SkillInfo> matches;
        for (const SkillInfo &skill : allSkills) {
            if (skill.name.contains(query, Qt::CaseInsensitive)
                || skill.description.contains(query, Qt::CaseInsensitive)) {
                matches.append(skill);
            }
        }

        if (matches.isEmpty()) {
            return "No matching skills found for: " + query;
        }

        QString result = "Found " + QString::number(matches.size()) + " matching skill(s) for '"
                         + query + "':\n\n";
        for (const SkillInfo &skill : matches) {
            result += "- " + skill.name + " [" + skill.scope + "]: " + skill.description + "\n";
        }
        return result;
    }

    if (action == "read") {
        if (!arguments.contains("query") || !arguments["query"].is_string()) {
            return "Error: query is required for read action (the skill name)";
        }

        QString name = QString::fromStdString(arguments["query"].get<std::string>());

        /* Search project first, then user (project takes priority) */
        for (const SkillInfo &skill : allSkills) {
            if (skill.name == name) {
                QString content = readSkillContent(skill.path);
                if (content.isEmpty()) {
                    return "Error: Failed to read skill file: " + skill.path;
                }
                return "Skill: " + skill.name + " [" + skill.scope + "]\nPath: " + skill.path
                       + "\n\n" + content;
            }
        }

        return "Error: Skill not found: " + name;
    }

    return "Error: Unknown action '" + action + "'. Use 'list', 'search', or 'read'.";
}

void QSocToolSkillFind::setProjectManager(QSocProjectManager *projectManager)
{
    this->projectManager = projectManager;
}

/* QSocToolSkillCreate Implementation */

QSocToolSkillCreate::QSocToolSkillCreate(QObject *parent, QSocProjectManager *projectManager)
    : QSocTool(parent)
    , projectManager(projectManager)
{}

QSocToolSkillCreate::~QSocToolSkillCreate() = default;

QString QSocToolSkillCreate::getName() const
{
    return "skill_create";
}

QString QSocToolSkillCreate::getDescription() const
{
    return "Create a new skill as a SKILL.md prompt template file. "
           "Skills are stored in project or user directories.";
}

json QSocToolSkillCreate::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"name",
           {{"type", "string"},
            {"description", "Skill name: lowercase letters, digits, and hyphens only (1-64 chars)"}}},
          {"description", {{"type", "string"}, {"description", "Short description of the skill"}}},
          {"instructions",
           {{"type", "string"}, {"description", "Detailed instructions (the SKILL.md body)"}}},
          {"scope",
           {{"type", "string"},
            {"enum", {"user", "project"}},
            {"description",
             "Where to create: 'user' (~/.config/qsoc/skills/) or "
             "'project' (<project>/.qsoc/skills/)"}}}}},
        {"required", json::array({"name", "description", "instructions", "scope"})}};
}

QString QSocToolSkillCreate::userSkillsPath() const
{
    QString configPath = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
    return QDir(configPath).filePath("qsoc/skills");
}

QString QSocToolSkillCreate::projectSkillsPath() const
{
    if (!projectManager) {
        return {};
    }

    QString projectPath = projectManager->getProjectPath();
    if (projectPath.isEmpty()) {
        projectPath = QDir::currentPath();
    }

    return QDir(projectPath).filePath(".qsoc/skills");
}

bool QSocToolSkillCreate::isValidSkillName(const QString &name) const
{
    if (name.isEmpty() || name.length() > 64) {
        return false;
    }

    /* Must match: lowercase letters, digits, hyphens; no leading/trailing/consecutive hyphens */
    static const QRegularExpression regex("^[a-z0-9]([a-z0-9-]*[a-z0-9])?$");
    if (!regex.match(name).hasMatch()) {
        return false;
    }

    /* No consecutive hyphens */
    if (name.contains("--")) {
        return false;
    }

    return true;
}

QString QSocToolSkillCreate::execute(const json &arguments)
{
    /* Validate required parameters */
    if (!arguments.contains("name") || !arguments["name"].is_string()) {
        return "Error: name is required";
    }
    if (!arguments.contains("description") || !arguments["description"].is_string()) {
        return "Error: description is required";
    }
    if (!arguments.contains("instructions") || !arguments["instructions"].is_string()) {
        return "Error: instructions is required";
    }
    if (!arguments.contains("scope") || !arguments["scope"].is_string()) {
        return "Error: scope is required (must be 'user' or 'project')";
    }

    QString name         = QString::fromStdString(arguments["name"].get<std::string>());
    QString description  = QString::fromStdString(arguments["description"].get<std::string>());
    QString instructions = QString::fromStdString(arguments["instructions"].get<std::string>());
    QString scope        = QString::fromStdString(arguments["scope"].get<std::string>());

    /* Validate name */
    if (!isValidSkillName(name)) {
        return "Error: Invalid skill name '" + name
               + "'. Must be 1-64 chars, lowercase letters/digits/hyphens, "
                 "no leading/trailing/consecutive hyphens.";
    }

    /* Determine base path */
    QString basePath;
    if (scope == "user") {
        basePath = userSkillsPath();
    } else if (scope == "project") {
        basePath = projectSkillsPath();
        if (basePath.isEmpty()) {
            return "Error: No project directory available for project-scoped skill";
        }
    } else {
        return "Error: scope must be 'user' or 'project'";
    }

    /* Build file path */
    QString skillDir  = QDir(basePath).filePath(name);
    QString skillFile = QDir(skillDir).filePath("SKILL.md");

    /* Check if already exists */
    if (QFile::exists(skillFile)) {
        return "Error: Skill '" + name + "' already exists at: " + skillFile;
    }

    /* Create directory */
    QDir dir(skillDir);
    if (!dir.exists() && !dir.mkpath(".")) {
        return "Error: Failed to create directory: " + skillDir;
    }

    /* Write SKILL.md */
    QFile file(skillFile);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        return "Error: Failed to create file: " + skillFile;
    }

    QTextStream out(&file);
    out << "---\n";
    out << "name: " << name << "\n";
    out << "description: " << description << "\n";
    out << "---\n\n";
    out << instructions;

    /* Ensure trailing newline */
    if (!instructions.endsWith('\n')) {
        out << "\n";
    }

    file.close();

    return "Successfully created skill '" + name + "' at: " + skillFile;
}

void QSocToolSkillCreate::setProjectManager(QSocProjectManager *projectManager)
{
    this->projectManager = projectManager;
}
