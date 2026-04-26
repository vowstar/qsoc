// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolskill.h"

#include "common/qsocpaths.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
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
            {"enum", {"user", "project", "system", "all"}},
            {"description",
             "Which scope to search: 'user', 'project', 'system', or 'all' (default: all)"}}}}},
        {"required", json::array({"action"})}};
}

QStringList QSocToolSkillFind::allSkillsDirs() const
{
    QString projectPath;
    if (projectManager) {
        projectPath = projectManager->getProjectPath();
    }
    if (projectPath.isEmpty()) {
        projectPath = QDir::currentPath();
    }
    return QSocPaths::resourceDirs(QStringLiteral("skills"), projectPath);
}

QSocToolSkillFind::SkillInfo QSocToolSkillFind::parseSkillFile(
    const QString &filePath, const QString &scope) const
{
    SkillInfo info;
    info.path  = filePath;
    info.scope = scope;

    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        info.parseError = QStringLiteral("cannot open file");
        return info;
    }

    QTextStream stream(&file);
    QString     content = stream.readAll();
    file.close();

    /* Parse YAML frontmatter between --- delimiters */
    if (!content.startsWith("---")) {
        info.parseError = QStringLiteral("missing frontmatter ('---' on first line)");
        return info;
    }

    qsizetype endMarker = content.indexOf("\n---", 3);
    if (endMarker < 0) {
        info.parseError = QStringLiteral("frontmatter has no closing '---' line");
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

        if (key == QLatin1String("name")) {
            info.name = value;
        } else if (key == QLatin1String("description")) {
            info.description = value;
        } else if (key == QLatin1String("argument-hint")) {
            info.argumentHint = value;
        } else if (key == QLatin1String("when_to_use") || key == QLatin1String("when-to-use")) {
            info.whenToUse = value;
        } else if (key == QLatin1String("user-invocable")) {
            info.userInvocable = (value.toLower() != QLatin1String("false"));
        } else if (key == QLatin1String("disable-model-invocation")) {
            info.disableModelInvocation = (value.toLower() == QLatin1String("true"));
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
            /* Broken SKILL.md files keep info.name empty but carry parseError;
             * scanAllSkillFiles() needs them, scanAllSkills() filters them out. */
            skills.append(info);
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

QList<QSocToolSkillFind::SkillInfo> QSocToolSkillFind::scanAllSkills() const
{
    /* Classify each candidate dir by which layer root it originates from.
     * Same iteration order as resourceDirs(): env → project → user → system. */
    QString projectPath;
    if (projectManager) {
        projectPath = projectManager->getProjectPath();
    }
    if (projectPath.isEmpty()) {
        projectPath = QDir::currentPath();
    }

    const QString envPrefix     = QSocPaths::envRoot();
    const QString projectPrefix = QSocPaths::projectRoot(projectPath);
    const QString userPrefix    = QSocPaths::userRoot();
    const QString systemPrefix  = QSocPaths::systemRoot();

    auto classify = [&](const QString &dir) -> QString {
        if (!projectPrefix.isEmpty() && dir.startsWith(projectPrefix)) {
            return QStringLiteral("project");
        }
        if (!envPrefix.isEmpty() && dir.startsWith(envPrefix)) {
            return QStringLiteral("user");
        }
        if (dir.startsWith(userPrefix)) {
            return QStringLiteral("user");
        }
        if (dir.startsWith(systemPrefix)) {
            return QStringLiteral("system");
        }
        return QStringLiteral("user");
    };

    const QStringList dirs = allSkillsDirs();
    QList<SkillInfo>  result;
    QSet<QString>     seen; /* dedup by name, first-found wins (higher layer shadows lower) */
    for (const QString &dir : dirs) {
        const QString scope = classify(dir);
        for (const SkillInfo &skill : scanSkillsDir(dir, scope)) {
            if (skill.name.isEmpty()) {
                continue;
            }
            if (!seen.contains(skill.name)) {
                seen.insert(skill.name);
                result.append(skill);
            }
        }
    }
    return result;
}

QString QSocToolSkillFind::formatPromptListing(const QList<SkillInfo> &skills)
{
    if (skills.isEmpty()) {
        return {};
    }

    /* Cap each description so a single noisy SKILL.md cannot blow past the
     * prompt cache prefix budget. 200 chars is enough for one full line. */
    constexpr int kDescriptionCap = 200;

    QString body;
    for (const auto &skill : skills) {
        if (skill.disableModelInvocation) {
            continue;
        }
        QString desc = skill.description;
        if (desc.size() > kDescriptionCap) {
            desc = desc.left(kDescriptionCap - 3) + QStringLiteral("...");
        }
        body += QStringLiteral("- **") + skill.name + QStringLiteral("** [") + skill.scope
                + QStringLiteral("]: ") + desc;
        if (skill.userInvocable) {
            body += QStringLiteral(" (user-invocable: /") + skill.name + QStringLiteral(")");
        }
        body += QStringLiteral("\n");
    }
    if (body.isEmpty()) {
        return {};
    }
    return QStringLiteral(
               "The following skills are installed. Use skill_find(action:\"read\", "
               "query:\"<name>\") to load the full prompt, or the user can invoke "
               "user-invocable ones directly as /<name> slash commands.\n\n")
           + body;
}

QString QSocToolSkillFind::substitutePlaceholders(
    const QString &body,
    const QString &args,
    const QString &cwd,
    const QString &projectPath,
    bool          *argsConsumed)
{
    if (argsConsumed != nullptr) {
        *argsConsumed = body.contains(QStringLiteral("${ARGS}"));
    }
    QString out = body;
    out.replace(QStringLiteral("${ARGS}"), args);
    out.replace(QStringLiteral("${CWD}"), cwd);
    out.replace(QStringLiteral("${PROJECT}"), projectPath);
    return out;
}

QList<QSocToolSkillFind::SkillInfo> QSocToolSkillFind::scanAllSkillFiles() const
{
    QString projectPath;
    if (projectManager) {
        projectPath = projectManager->getProjectPath();
    }
    if (projectPath.isEmpty()) {
        projectPath = QDir::currentPath();
    }

    const QString envPrefix     = QSocPaths::envRoot();
    const QString projectPrefix = QSocPaths::projectRoot(projectPath);
    const QString userPrefix    = QSocPaths::userRoot();
    const QString systemPrefix  = QSocPaths::systemRoot();

    auto classify = [&](const QString &dir) -> QString {
        if (!projectPrefix.isEmpty() && dir.startsWith(projectPrefix)) {
            return QStringLiteral("project");
        }
        if (!envPrefix.isEmpty() && dir.startsWith(envPrefix)) {
            return QStringLiteral("user");
        }
        if (dir.startsWith(userPrefix)) {
            return QStringLiteral("user");
        }
        if (dir.startsWith(systemPrefix)) {
            return QStringLiteral("system");
        }
        return QStringLiteral("user");
    };

    const QStringList dirs = allSkillsDirs();
    QList<SkillInfo>  result;
    for (const QString &dir : dirs) {
        const QString scope = classify(dir);
        result.append(scanSkillsDir(dir, scope));
    }
    return result;
}

QString QSocToolSkillFind::execute(const json &arguments)
{
    if (!arguments.contains("action") || !arguments["action"].is_string()) {
        return "Error: action is required (must be 'list', 'search', or 'read')";
    }

    QString action = QString::fromStdString(arguments["action"].get<std::string>());

    /* Collect skills — scope filtering is done after scanAllSkills for simplicity. */
    QString scope = "all";
    if (arguments.contains("scope") && arguments["scope"].is_string()) {
        scope = QString::fromStdString(arguments["scope"].get<std::string>());
    }

    QList<SkillInfo> allSkills = scanAllSkills();
    if (scope != "all") {
        QList<SkillInfo> filtered;
        for (const SkillInfo &skill : allSkills) {
            if (skill.scope == scope) {
                filtered.append(skill);
            }
        }
        allSkills = filtered;
    }

    /* The model invokes skill_find. Skills marked disable-model-invocation
     * are intentionally hidden from list/search so the model cannot pull
     * them in autonomously; the user's /name slash dispatch still works. */
    if (action != "read") {
        QList<SkillInfo> visible;
        for (const SkillInfo &skill : allSkills) {
            if (!skill.disableModelInvocation) {
                visible.append(skill);
            }
        }
        allSkills = visible;
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
             "'project' (<project>/.qsoc/skills/). "
             "System-level skills are installed via packaging, not this tool."}}}}},
        {"required", json::array({"name", "description", "instructions", "scope"})}};
}

QString QSocToolSkillCreate::userSkillsPath() const
{
    return QDir(QSocPaths::userRoot()).filePath(QStringLiteral("skills"));
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

    const QString root = QSocPaths::projectRoot(projectPath);
    return root.isEmpty() ? QString() : QDir(root).filePath(QStringLiteral("skills"));
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
    out << "user-invocable: true\n";
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
