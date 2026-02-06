// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolproject.h"

#include "common/qstaticdatasedes.h"

#include <QRegularExpression>

/* QSocToolProjectList Implementation */

QSocToolProjectList::QSocToolProjectList(QObject *parent, QSocProjectManager *projectManager)
    : QSocTool(parent)
    , projectManager(projectManager)
{}

QSocToolProjectList::~QSocToolProjectList() = default;

QString QSocToolProjectList::getName() const
{
    return "project_list";
}

QString QSocToolProjectList::getDescription() const
{
    return "List all projects in the project directory. "
           "Returns a list of project names that match the optional regex pattern.";
}

json QSocToolProjectList::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"regex",
           {{"type", "string"},
            {"description",
             "Regular expression pattern to filter project names (default: '.*' matches all)"}}},
          {"directory",
           {{"type", "string"},
            {"description",
             "Project directory path (optional, uses current directory if not specified)"}}}}},
        {"required", json::array()}};
}

QString QSocToolProjectList::execute(const json &arguments)
{
    if (!projectManager) {
        return "Error: Project manager not configured";
    }

    /* Set directory if provided */
    if (arguments.contains("directory") && arguments["directory"].is_string()) {
        projectManager->setProjectPath(
            QString::fromStdString(arguments["directory"].get<std::string>()));
    }

    /* Get regex pattern */
    QString regexStr = ".*";
    if (arguments.contains("regex") && arguments["regex"].is_string()) {
        regexStr = QString::fromStdString(arguments["regex"].get<std::string>());
    }

    QRegularExpression regex(regexStr);
    if (!regex.isValid()) {
        return QString("Error: Invalid regex pattern: %1").arg(regex.errorString());
    }

    QStringList projects = projectManager->list(regex);

    if (projects.isEmpty()) {
        return "No projects found.";
    }

    return QString("Found %1 project(s):\n%2").arg(projects.size()).arg(projects.join("\n"));
}

void QSocToolProjectList::setProjectManager(QSocProjectManager *projectManager)
{
    projectManager = projectManager;
}

/* QSocToolProjectShow Implementation */

QSocToolProjectShow::QSocToolProjectShow(QObject *parent, QSocProjectManager *projectManager)
    : QSocTool(parent)
    , projectManager(projectManager)
{}

QSocToolProjectShow::~QSocToolProjectShow() = default;

QString QSocToolProjectShow::getName() const
{
    return "project_show";
}

QString QSocToolProjectShow::getDescription() const
{
    return "Show detailed information about a specific project. "
           "Returns the project configuration including paths for bus, module, schematic, and "
           "output directories.";
}

json QSocToolProjectShow::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"name", {{"type", "string"}, {"description", "Name of the project to show"}}},
          {"directory",
           {{"type", "string"},
            {"description",
             "Project directory path (optional, uses current directory if not specified)"}}}}},
        {"required", json::array({"name"})}};
}

QString QSocToolProjectShow::execute(const json &arguments)
{
    if (!projectManager) {
        return "Error: Project manager not configured";
    }

    if (!arguments.contains("name") || !arguments["name"].is_string()) {
        return "Error: Project name is required";
    }

    QString projectName = QString::fromStdString(arguments["name"].get<std::string>());

    /* Set directory if provided */
    if (arguments.contains("directory") && arguments["directory"].is_string()) {
        projectManager->setProjectPath(
            QString::fromStdString(arguments["directory"].get<std::string>()));
    }

    /* Load project */
    if (!projectManager->load(projectName)) {
        return QString("Error: Failed to load project '%1'").arg(projectName);
    }

    /* Format project info */
    QString result = QString("Project: %1\n").arg(projectName);
    result += QString("Project Path: %1\n").arg(projectManager->getProjectPath());
    result += QString("Bus Path: %1\n").arg(projectManager->getBusPath());
    result += QString("Module Path: %1\n").arg(projectManager->getModulePath());
    result += QString("Schematic Path: %1\n").arg(projectManager->getSchematicPath());
    result += QString("Output Path: %1\n").arg(projectManager->getOutputPath());

    /* Add YAML representation */
    result += "\nFull configuration:\n";
    result += QStaticDataSedes::serializeYaml(projectManager->getProjectYaml());

    return result;
}

void QSocToolProjectShow::setProjectManager(QSocProjectManager *projectManager)
{
    projectManager = projectManager;
}

/* QSocToolProjectCreate Implementation */

QSocToolProjectCreate::QSocToolProjectCreate(QObject *parent, QSocProjectManager *projectManager)
    : QSocTool(parent)
    , projectManager(projectManager)
{}

QSocToolProjectCreate::~QSocToolProjectCreate() = default;

QString QSocToolProjectCreate::getName() const
{
    return "project_create";
}

QString QSocToolProjectCreate::getDescription() const
{
    return "Create a new project with the specified name and optional directory paths. "
           "Creates the project configuration file and necessary directory structure.";
}

json QSocToolProjectCreate::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"name", {{"type", "string"}, {"description", "Name of the project to create"}}},
          {"directory",
           {{"type", "string"},
            {"description", "Project directory path (optional, uses current directory)"}}},
          {"bus_path", {{"type", "string"}, {"description", "Path to bus directory (optional)"}}},
          {"module_path",
           {{"type", "string"}, {"description", "Path to module directory (optional)"}}},
          {"schematic_path",
           {{"type", "string"}, {"description", "Path to schematic directory (optional)"}}},
          {"output_path",
           {{"type", "string"}, {"description", "Path to output directory (optional)"}}}}},
        {"required", json::array({"name"})}};
}

QString QSocToolProjectCreate::execute(const json &arguments)
{
    if (!projectManager) {
        return "Error: Project manager not configured";
    }

    if (!arguments.contains("name") || !arguments["name"].is_string()) {
        return "Error: Project name is required";
    }

    QString projectName = QString::fromStdString(arguments["name"].get<std::string>());

    /* Set paths if provided */
    if (arguments.contains("directory") && arguments["directory"].is_string()) {
        projectManager->setProjectPath(
            QString::fromStdString(arguments["directory"].get<std::string>()));
    }
    if (arguments.contains("bus_path") && arguments["bus_path"].is_string()) {
        projectManager->setBusPath(QString::fromStdString(arguments["bus_path"].get<std::string>()));
    }
    if (arguments.contains("module_path") && arguments["module_path"].is_string()) {
        projectManager->setModulePath(
            QString::fromStdString(arguments["module_path"].get<std::string>()));
    }
    if (arguments.contains("schematic_path") && arguments["schematic_path"].is_string()) {
        projectManager->setSchematicPath(
            QString::fromStdString(arguments["schematic_path"].get<std::string>()));
    }
    if (arguments.contains("output_path") && arguments["output_path"].is_string()) {
        projectManager->setOutputPath(
            QString::fromStdString(arguments["output_path"].get<std::string>()));
    }

    /* Save project */
    if (!projectManager->save(projectName)) {
        return QString("Error: Failed to create project '%1'").arg(projectName);
    }

    return QString("Project '%1' created successfully.").arg(projectName);
}

void QSocToolProjectCreate::setProjectManager(QSocProjectManager *projectManager)
{
    projectManager = projectManager;
}
