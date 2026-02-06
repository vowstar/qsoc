// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolmodule.h"

#include "common/qstaticdatasedes.h"

#include <QRegularExpression>

/* QSocToolModuleList Implementation */

QSocToolModuleList::QSocToolModuleList(QObject *parent, QSocModuleManager *moduleManager)
    : QSocTool(parent)
    , moduleManager(moduleManager)
{}

QSocToolModuleList::~QSocToolModuleList() = default;

QString QSocToolModuleList::getName() const
{
    return "module_list";
}

QString QSocToolModuleList::getDescription() const
{
    return "List all modules in the module library. "
           "Returns a list of module names that match the optional regex pattern. "
           "You need to load module libraries first before listing modules.";
}

json QSocToolModuleList::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"regex",
           {{"type", "string"},
            {"description",
             "Regular expression pattern to filter module names (default: '.*' matches all)"}}},
          {"library",
           {{"type", "string"},
            {"description",
             "Library name or regex to load before listing (default: '.*' loads all "
             "libraries)"}}}}},
        {"required", json::array()}};
}

QString QSocToolModuleList::execute(const json &arguments)
{
    if (!moduleManager) {
        return "Error: Module manager not configured";
    }

    /* Load library if specified, otherwise load all */
    QString libraryPattern = ".*";
    if (arguments.contains("library") && arguments["library"].is_string()) {
        libraryPattern = QString::fromStdString(arguments["library"].get<std::string>());
    }

    QRegularExpression libraryRegex(libraryPattern);
    if (!libraryRegex.isValid()) {
        return QString("Error: Invalid library regex pattern: %1").arg(libraryRegex.errorString());
    }

    if (!moduleManager->load(libraryRegex)) {
        return "Warning: No libraries found or failed to load some libraries.";
    }

    /* Get module regex pattern */
    QString modulePattern = ".*";
    if (arguments.contains("regex") && arguments["regex"].is_string()) {
        modulePattern = QString::fromStdString(arguments["regex"].get<std::string>());
    }

    QRegularExpression moduleRegex(modulePattern);
    if (!moduleRegex.isValid()) {
        return QString("Error: Invalid module regex pattern: %1").arg(moduleRegex.errorString());
    }

    QStringList modules = moduleManager->listModule(moduleRegex);

    if (modules.isEmpty()) {
        return "No modules found.";
    }

    return QString("Found %1 module(s):\n%2").arg(modules.size()).arg(modules.join("\n"));
}

void QSocToolModuleList::setModuleManager(QSocModuleManager *moduleManager)
{
    moduleManager = moduleManager;
}

/* QSocToolModuleShow Implementation */

QSocToolModuleShow::QSocToolModuleShow(QObject *parent, QSocModuleManager *moduleManager)
    : QSocTool(parent)
    , moduleManager(moduleManager)
{}

QSocToolModuleShow::~QSocToolModuleShow() = default;

QString QSocToolModuleShow::getName() const
{
    return "module_show";
}

QString QSocToolModuleShow::getDescription() const
{
    return "Show detailed information about a specific module. "
           "Returns the module's ports, parameters, and bus interfaces in YAML format.";
}

json QSocToolModuleShow::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"name", {{"type", "string"}, {"description", "Name of the module to show"}}},
          {"library",
           {{"type", "string"},
            {"description",
             "Library name or regex to load before showing (default: '.*' loads all "
             "libraries)"}}}}},
        {"required", json::array({"name"})}};
}

QString QSocToolModuleShow::execute(const json &arguments)
{
    if (!moduleManager) {
        return "Error: Module manager not configured";
    }

    if (!arguments.contains("name") || !arguments["name"].is_string()) {
        return "Error: Module name is required";
    }

    QString moduleName = QString::fromStdString(arguments["name"].get<std::string>());

    /* Load library if specified, otherwise load all */
    QString libraryPattern = ".*";
    if (arguments.contains("library") && arguments["library"].is_string()) {
        libraryPattern = QString::fromStdString(arguments["library"].get<std::string>());
    }

    QRegularExpression libraryRegex(libraryPattern);
    if (!moduleManager->load(libraryRegex)) {
        return "Warning: Failed to load some libraries.";
    }

    /* Check if module exists */
    if (!moduleManager->isModuleExist(moduleName)) {
        return QString("Error: Module '%1' not found").arg(moduleName);
    }

    /* Get module YAML */
    YAML::Node moduleYaml = moduleManager->getModuleYaml(moduleName);
    if (!moduleYaml.IsDefined() || moduleYaml.IsNull()) {
        return QString("Error: Failed to get module '%1' data").arg(moduleName);
    }

    /* Format output */
    QString result = QString("Module: %1\n").arg(moduleName);
    result += QString("Library: %1\n\n").arg(moduleManager->getModuleLibrary(moduleName));
    result += "Configuration:\n";
    result += QStaticDataSedes::serializeYaml(moduleYaml);

    return result;
}

void QSocToolModuleShow::setModuleManager(QSocModuleManager *moduleManager)
{
    moduleManager = moduleManager;
}

/* QSocToolModuleImport Implementation */

QSocToolModuleImport::QSocToolModuleImport(QObject *parent, QSocModuleManager *moduleManager)
    : QSocTool(parent)
    , moduleManager(moduleManager)
{}

QSocToolModuleImport::~QSocToolModuleImport() = default;

QString QSocToolModuleImport::getName() const
{
    return "module_import";
}

QString QSocToolModuleImport::getDescription() const
{
    return "Import Verilog/SystemVerilog module(s) from file(s). "
           "Parses files and creates module library entries. "
           "Example: {\"files\": [\"/path/to/adder.v\"], \"library_name\": \"my_lib\"} "
           "The module_regex defaults to '.*' (import all modules). "
           "Returns success message or error details.";
}

json QSocToolModuleImport::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"files",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "List of Verilog/SystemVerilog file paths to import"}}},
          {"library_name",
           {{"type", "string"},
            {"description",
             "Name for the library (optional, derived from module name if not specified)"}}},
          {"module_regex",
           {{"type", "string"},
            {"description",
             "Regular expression to match module names to import (default: first module)"}}}}},
        {"required", json::array({"files"})}};
}

QString QSocToolModuleImport::execute(const json &arguments)
{
    if (!moduleManager) {
        return "Error: Module manager not configured";
    }

    if (!arguments.contains("files") || !arguments["files"].is_array()
        || arguments["files"].empty()) {
        return "Error: At least one file path is required";
    }

    /* Get file paths */
    QStringList filePaths;
    for (const auto &file : arguments["files"]) {
        if (file.is_string()) {
            filePaths.append(QString::fromStdString(file.get<std::string>()));
        }
    }

    if (filePaths.isEmpty()) {
        return "Error: No valid file paths provided";
    }

    /* Get library name */
    QString libraryName;
    if (arguments.contains("library_name") && arguments["library_name"].is_string()) {
        libraryName = QString::fromStdString(arguments["library_name"].get<std::string>());
    }

    /* Get module regex - default to ".*" (match all modules) */
    QString regexStr = ".*";
    if (arguments.contains("module_regex") && arguments["module_regex"].is_string()) {
        QString providedRegex = QString::fromStdString(arguments["module_regex"].get<std::string>());
        if (!providedRegex.isEmpty()) {
            regexStr = providedRegex;
        }
    }
    QRegularExpression moduleRegex(regexStr);
    if (!moduleRegex.isValid()) {
        return QString("Error: Invalid module regex: %1").arg(moduleRegex.errorString());
    }

    /* Import from file list */
    if (!moduleManager->importFromFileList(libraryName, moduleRegex, QString(), filePaths)) {
        return "Error: Failed to import module(s) from file(s)";
    }

    return QString("Successfully imported module(s) from %1 file(s).").arg(filePaths.size());
}

void QSocToolModuleImport::setModuleManager(QSocModuleManager *moduleManager)
{
    moduleManager = moduleManager;
}

/* QSocToolModuleBusAdd Implementation */

QSocToolModuleBusAdd::QSocToolModuleBusAdd(QObject *parent, QSocModuleManager *moduleManager)
    : QSocTool(parent)
    , moduleManager(moduleManager)
{}

QSocToolModuleBusAdd::~QSocToolModuleBusAdd() = default;

QString QSocToolModuleBusAdd::getName() const
{
    return "module_bus_add";
}

QString QSocToolModuleBusAdd::getDescription() const
{
    return "Add a bus interface to a module. Uses LLM to automatically match module ports "
           "to bus signals when use_llm is true. The bus definition must exist in the bus library.";
}

json QSocToolModuleBusAdd::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"module_name", {{"type", "string"}, {"description", "Name of the module to modify"}}},
          {"bus_name", {{"type", "string"}, {"description", "Name of the bus definition to add"}}},
          {"bus_mode",
           {{"type", "string"}, {"description", "Bus mode (e.g., 'master', 'slave', 'monitor')"}}},
          {"bus_interface",
           {{"type", "string"},
            {"description", "Interface name for the bus (e.g., 'axi_m0', 'apb_slave')"}}},
          {"use_llm",
           {{"type", "boolean"},
            {"description",
             "Use LLM to automatically match module ports to bus signals (default: true)"}}}}},
        {"required", json::array({"module_name", "bus_name", "bus_mode", "bus_interface"})}};
}

QString QSocToolModuleBusAdd::execute(const json &arguments)
{
    if (!moduleManager) {
        return "Error: Module manager not configured";
    }

    /* Validate required parameters */
    if (!arguments.contains("module_name") || !arguments["module_name"].is_string()) {
        return "Error: module_name is required";
    }
    if (!arguments.contains("bus_name") || !arguments["bus_name"].is_string()) {
        return "Error: bus_name is required";
    }
    if (!arguments.contains("bus_mode") || !arguments["bus_mode"].is_string()) {
        return "Error: bus_mode is required";
    }
    if (!arguments.contains("bus_interface") || !arguments["bus_interface"].is_string()) {
        return "Error: bus_interface is required";
    }

    QString moduleName   = QString::fromStdString(arguments["module_name"].get<std::string>());
    QString busName      = QString::fromStdString(arguments["bus_name"].get<std::string>());
    QString busMode      = QString::fromStdString(arguments["bus_mode"].get<std::string>());
    QString busInterface = QString::fromStdString(arguments["bus_interface"].get<std::string>());

    /* Load all libraries first */
    QRegularExpression allLibraries(".*");
    moduleManager->load(allLibraries);

    /* Check if module exists */
    if (!moduleManager->isModuleExist(moduleName)) {
        return QString("Error: Module '%1' not found").arg(moduleName);
    }

    /* Determine whether to use LLM */
    bool useLLM = true;
    if (arguments.contains("use_llm") && arguments["use_llm"].is_boolean()) {
        useLLM = arguments["use_llm"].get<bool>();
    }

    bool success = false;
    if (useLLM) {
        success = moduleManager->addModuleBusWithLLM(moduleName, busName, busMode, busInterface);
    } else {
        success = moduleManager->addModuleBus(moduleName, busName, busMode, busInterface);
    }

    if (!success) {
        return QString("Error: Failed to add bus interface '%1' to module '%2'")
            .arg(busInterface, moduleName);
    }

    return QString("Successfully added bus interface '%1' (bus: %2, mode: %3) to module '%4'")
        .arg(busInterface, busName, busMode, moduleName);
}

void QSocToolModuleBusAdd::setModuleManager(QSocModuleManager *moduleManager)
{
    moduleManager = moduleManager;
}
