// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolgenerate.h"

#include <QFileInfo>

/* QSocToolGenerateVerilog Implementation */

QSocToolGenerateVerilog::QSocToolGenerateVerilog(
    QObject *parent, QSocGenerateManager *generateManager)
    : QSocTool(parent)
    , generateManager(generateManager)
{}

QSocToolGenerateVerilog::~QSocToolGenerateVerilog() = default;

QString QSocToolGenerateVerilog::getName() const
{
    return "generate_verilog";
}

QString QSocToolGenerateVerilog::getDescription() const
{
    return "Generate Verilog RTL code from a netlist file. "
           "The netlist file should be in YAML format describing module instances, "
           "connections, and bus interfaces.";
}

json QSocToolGenerateVerilog::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"netlist_file",
           {{"type", "string"}, {"description", "Path to the netlist YAML file to process"}}},
          {"output_name",
           {{"type", "string"}, {"description", "Output file name (without .v extension)"}}},
          {"force",
           {{"type", "boolean"},
            {"description", "Force overwrite existing primitive cell files (default: false)"}}}}},
        {"required", json::array({"netlist_file", "output_name"})}};
}

QString QSocToolGenerateVerilog::execute(const json &arguments)
{
    if (!generateManager) {
        return "Error: Generate manager not configured";
    }

    if (!arguments.contains("netlist_file") || !arguments["netlist_file"].is_string()) {
        return "Error: netlist_file is required";
    }

    if (!arguments.contains("output_name") || !arguments["output_name"].is_string()) {
        return "Error: output_name is required";
    }

    QString netlistFile = QString::fromStdString(arguments["netlist_file"].get<std::string>());
    QString outputName  = QString::fromStdString(arguments["output_name"].get<std::string>());

    /* Check if netlist file exists */
    QFileInfo fileInfo(netlistFile);
    if (!fileInfo.exists()) {
        return QString("Error: Netlist file not found: %1").arg(netlistFile);
    }

    /* Set force overwrite if specified */
    if (arguments.contains("force") && arguments["force"].is_boolean()) {
        generateManager->setForceOverwrite(arguments["force"].get<bool>());
    }

    /* Load netlist */
    if (!generateManager->loadNetlist(netlistFile)) {
        return QString("Error: Failed to load netlist file: %1").arg(netlistFile);
    }

    /* Process netlist */
    if (!generateManager->processNetlist()) {
        return "Error: Failed to process netlist";
    }

    /* Generate Verilog */
    if (!generateManager->generateVerilog(outputName)) {
        return QString("Error: Failed to generate Verilog for: %1").arg(outputName);
    }

    return QString("Successfully generated Verilog: %1.v").arg(outputName);
}

void QSocToolGenerateVerilog::setGenerateManager(QSocGenerateManager *generateManager)
{
    generateManager = generateManager;
}

/* QSocToolGenerateTemplate Implementation */

QSocToolGenerateTemplate::QSocToolGenerateTemplate(
    QObject *parent, QSocGenerateManager *generateManager)
    : QSocTool(parent)
    , generateManager(generateManager)
{}

QSocToolGenerateTemplate::~QSocToolGenerateTemplate() = default;

QString QSocToolGenerateTemplate::getName() const
{
    return "generate_template";
}

QString QSocToolGenerateTemplate::getDescription() const
{
    return "Render a Jinja2 template with data from CSV, YAML, JSON, SystemRDL, or RCSV files. "
           "Useful for generating configuration files, documentation, or custom RTL.";
}

json QSocToolGenerateTemplate::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"template_file",
           {{"type", "string"}, {"description", "Path to the Jinja2 template file"}}},
          {"output_name",
           {{"type", "string"}, {"description", "Output file name (with extension)"}}},
          {"csv_files",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "List of CSV data files"}}},
          {"yaml_files",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "List of YAML data files"}}},
          {"json_files",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "List of JSON data files"}}},
          {"rdl_files",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "List of SystemRDL data files"}}},
          {"rcsv_files",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"description", "List of Register-CSV data files"}}}}},
        {"required", json::array({"template_file", "output_name"})}};
}

QString QSocToolGenerateTemplate::execute(const json &arguments)
{
    if (!generateManager) {
        return "Error: Generate manager not configured";
    }

    if (!arguments.contains("template_file") || !arguments["template_file"].is_string()) {
        return "Error: template_file is required";
    }

    if (!arguments.contains("output_name") || !arguments["output_name"].is_string()) {
        return "Error: output_name is required";
    }

    QString templateFile = QString::fromStdString(arguments["template_file"].get<std::string>());
    QString outputName   = QString::fromStdString(arguments["output_name"].get<std::string>());

    /* Check if template file exists */
    QFileInfo fileInfo(templateFile);
    if (!fileInfo.exists()) {
        return QString("Error: Template file not found: %1").arg(templateFile);
    }

    /* Collect data files */
    QStringList csvFiles;
    QStringList yamlFiles;
    QStringList jsonFiles;
    QStringList rdlFiles;
    QStringList rcsvFiles;

    if (arguments.contains("csv_files") && arguments["csv_files"].is_array()) {
        for (const auto &file : arguments["csv_files"]) {
            if (file.is_string()) {
                csvFiles.append(QString::fromStdString(file.get<std::string>()));
            }
        }
    }

    if (arguments.contains("yaml_files") && arguments["yaml_files"].is_array()) {
        for (const auto &file : arguments["yaml_files"]) {
            if (file.is_string()) {
                yamlFiles.append(QString::fromStdString(file.get<std::string>()));
            }
        }
    }

    if (arguments.contains("json_files") && arguments["json_files"].is_array()) {
        for (const auto &file : arguments["json_files"]) {
            if (file.is_string()) {
                jsonFiles.append(QString::fromStdString(file.get<std::string>()));
            }
        }
    }

    if (arguments.contains("rdl_files") && arguments["rdl_files"].is_array()) {
        for (const auto &file : arguments["rdl_files"]) {
            if (file.is_string()) {
                rdlFiles.append(QString::fromStdString(file.get<std::string>()));
            }
        }
    }

    if (arguments.contains("rcsv_files") && arguments["rcsv_files"].is_array()) {
        for (const auto &file : arguments["rcsv_files"]) {
            if (file.is_string()) {
                rcsvFiles.append(QString::fromStdString(file.get<std::string>()));
            }
        }
    }

    /* Render template */
    if (!generateManager->renderTemplate(
            templateFile, csvFiles, yamlFiles, jsonFiles, rdlFiles, rcsvFiles, outputName)) {
        return QString("Error: Failed to render template: %1").arg(templateFile);
    }

    return QString("Successfully rendered template to: %1").arg(outputName);
}

void QSocToolGenerateTemplate::setGenerateManager(QSocGenerateManager *generateManager)
{
    generateManager = generateManager;
}
