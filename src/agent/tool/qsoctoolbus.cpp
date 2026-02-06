// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolbus.h"

#include "common/qstaticdatasedes.h"

#include <QRegularExpression>

/* QSocToolBusList Implementation */

QSocToolBusList::QSocToolBusList(QObject *parent, QSocBusManager *busManager)
    : QSocTool(parent)
    , busManager(busManager)
{}

QSocToolBusList::~QSocToolBusList() = default;

QString QSocToolBusList::getName() const
{
    return "bus_list";
}

QString QSocToolBusList::getDescription() const
{
    return "List all bus definitions in the bus library. "
           "Returns a list of bus names that match the optional regex pattern. "
           "Common bus types include AXI, AHB, APB, Wishbone, etc.";
}

json QSocToolBusList::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"regex",
           {{"type", "string"},
            {"description",
             "Regular expression pattern to filter bus names (default: '.*' matches all)"}}},
          {"library",
           {{"type", "string"},
            {"description",
             "Library name or regex to load before listing (default: '.*' loads all "
             "libraries)"}}}}},
        {"required", json::array()}};
}

QString QSocToolBusList::execute(const json &arguments)
{
    if (!busManager) {
        return "Error: Bus manager not configured";
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

    if (!busManager->load(libraryRegex)) {
        return "Warning: No libraries found or failed to load some libraries.";
    }

    /* Get bus regex pattern */
    QString busPattern = ".*";
    if (arguments.contains("regex") && arguments["regex"].is_string()) {
        busPattern = QString::fromStdString(arguments["regex"].get<std::string>());
    }

    QRegularExpression busRegex(busPattern);
    if (!busRegex.isValid()) {
        return QString("Error: Invalid bus regex pattern: %1").arg(busRegex.errorString());
    }

    QStringList buses = busManager->listBus(busRegex);

    if (buses.isEmpty()) {
        return "No bus definitions found.";
    }

    return QString("Found %1 bus definition(s):\n%2").arg(buses.size()).arg(buses.join("\n"));
}

void QSocToolBusList::setBusManager(QSocBusManager *busManager)
{
    busManager = busManager;
}

/* QSocToolBusShow Implementation */

QSocToolBusShow::QSocToolBusShow(QObject *parent, QSocBusManager *busManager)
    : QSocTool(parent)
    , busManager(busManager)
{}

QSocToolBusShow::~QSocToolBusShow() = default;

QString QSocToolBusShow::getName() const
{
    return "bus_show";
}

QString QSocToolBusShow::getDescription() const
{
    return "Show detailed information about a specific bus definition. "
           "Returns the bus signal definitions, modes, and interface specifications in YAML "
           "format.";
}

json QSocToolBusShow::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"name", {{"type", "string"}, {"description", "Name of the bus to show"}}},
          {"library",
           {{"type", "string"},
            {"description",
             "Library name or regex to load before showing (default: '.*' loads all "
             "libraries)"}}}}},
        {"required", json::array({"name"})}};
}

QString QSocToolBusShow::execute(const json &arguments)
{
    if (!busManager) {
        return "Error: Bus manager not configured";
    }

    if (!arguments.contains("name") || !arguments["name"].is_string()) {
        return "Error: Bus name is required";
    }

    QString busName = QString::fromStdString(arguments["name"].get<std::string>());

    /* Load library if specified, otherwise load all */
    QString libraryPattern = ".*";
    if (arguments.contains("library") && arguments["library"].is_string()) {
        libraryPattern = QString::fromStdString(arguments["library"].get<std::string>());
    }

    QRegularExpression libraryRegex(libraryPattern);
    if (!busManager->load(libraryRegex)) {
        return "Warning: Failed to load some libraries.";
    }

    /* Check if bus exists */
    if (!busManager->isBusExist(busName)) {
        return QString("Error: Bus '%1' not found").arg(busName);
    }

    /* Get bus YAML */
    YAML::Node busYaml = busManager->getBusYaml(busName);
    if (!busYaml.IsDefined() || busYaml.IsNull()) {
        return QString("Error: Failed to get bus '%1' data").arg(busName);
    }

    /* Format output */
    QString result = QString("Bus: %1\n").arg(busName);
    result += QString("Library: %1\n\n").arg(busManager->getBusLibrary(busName));
    result += "Definition:\n";
    result += QStaticDataSedes::serializeYaml(busYaml);

    return result;
}

void QSocToolBusShow::setBusManager(QSocBusManager *busManager)
{
    busManager = busManager;
}

/* QSocToolBusImport Implementation */

QSocToolBusImport::QSocToolBusImport(QObject *parent, QSocBusManager *busManager)
    : QSocTool(parent)
    , busManager(busManager)
{}

QSocToolBusImport::~QSocToolBusImport() = default;

QString QSocToolBusImport::getName() const
{
    return "bus_import";
}

QString QSocToolBusImport::getDescription() const
{
    return "Import bus definitions from CSV files. "
           "Creates a bus library entry with signal definitions, modes, and interface "
           "specifications.";
}

json QSocToolBusImport::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"files",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "List of CSV file paths containing bus signal definitions"}}},
          {"library_name",
           {{"type", "string"}, {"description", "Name for the bus library (required)"}}},
          {"bus_name",
           {{"type", "string"}, {"description", "Name of the bus being imported (required)"}}}}},
        {"required", json::array({"files", "library_name", "bus_name"})}};
}

QString QSocToolBusImport::execute(const json &arguments)
{
    if (!busManager) {
        return "Error: Bus manager not configured";
    }

    if (!arguments.contains("files") || !arguments["files"].is_array()
        || arguments["files"].empty()) {
        return "Error: At least one CSV file path is required";
    }

    if (!arguments.contains("library_name") || !arguments["library_name"].is_string()) {
        return "Error: library_name is required";
    }

    if (!arguments.contains("bus_name") || !arguments["bus_name"].is_string()) {
        return "Error: bus_name is required";
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

    QString libraryName = QString::fromStdString(arguments["library_name"].get<std::string>());
    QString busName     = QString::fromStdString(arguments["bus_name"].get<std::string>());

    /* Import from file list */
    if (!busManager->importFromFileList(libraryName, busName, filePaths)) {
        return QString("Error: Failed to import bus '%1' from file(s)").arg(busName);
    }

    /* Save the library */
    if (!busManager->save(libraryName)) {
        return QString("Warning: Imported bus '%1' but failed to save library '%2'")
            .arg(busName, libraryName);
    }

    return QString("Successfully imported bus '%1' to library '%2' from %3 file(s).")
        .arg(busName, libraryName)
        .arg(filePaths.size());
}

void QSocToolBusImport::setBusManager(QSocBusManager *busManager)
{
    busManager = busManager;
}
