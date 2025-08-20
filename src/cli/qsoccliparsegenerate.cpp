// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/qsocconfig.h"
#include "common/qsocgeneratemanager.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"
#include "common/qsocyamlutils.h"
#include "common/qstaticlog.h"

#include <fstream>
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QTextStream>

bool QSocCliWorker::parseGenerate(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addPositionalArgument(
        "subcommand",
        QCoreApplication::translate(
            "main",
            "verilog    Generate Verilog code from netlist file.\n"
            "template   Generate files from Jinja2 templates.\n"
            "stub       Generate stub files for modules."),
        "generate <subcommand> [subcommand options]");

    parser.parse(appArguments);
    const QStringList cmdArguments = parser.positionalArguments();
    if (cmdArguments.isEmpty()) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: missing subcommand."));
    }
    const QString &command       = cmdArguments.first();
    QStringList    nextArguments = appArguments;
    if (command == "verilog") {
        nextArguments.removeOne(command);
        if (!parseGenerateVerilog(nextArguments)) {
            return false;
        }
    } else if (command == "template") {
        nextArguments.removeOne(command);
        if (!parseGenerateTemplate(nextArguments)) {
            return false;
        }
    } else if (command == "stub") {
        nextArguments.removeOne(command);
        if (!parseGenerateStub(nextArguments)) {
            return false;
        }
    } else {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: unknown subcommand: %1.").arg(command));
    }

    return true;
}

bool QSocCliWorker::parseGenerateVerilog(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"d", "directory"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project directory"},
        {{"p", "project"}, QCoreApplication::translate("main", "The project name."), "project name"},
        {{"m", "merge"},
         QCoreApplication::translate(
             "main", "Merge multiple netlist files in order before processing.")},
        {{"f", "force"},
         QCoreApplication::translate(
             "main", "Force overwrite existing primitive cell files (clock_cell.v, reset_cell.v).")},
    });

    parser.addPositionalArgument(
        "files",
        QCoreApplication::translate("main", "The netlist files to be processed."),
        "[<netlist files>]");

    parser.parse(appArguments);

    if (parser.isSet("help")) {
        return showHelp(0);
    }

    const QStringList  cmdArguments = parser.positionalArguments();
    const QStringList &filePathList = cmdArguments;
    if (filePathList.isEmpty()) {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: missing netlist files."));
    }

    /* Setup project manager and project path  */
    if (parser.isSet("directory")) {
        const QString dirPath = parser.value("directory");
        projectManager->setProjectPath(dirPath);
    }

    if (parser.isSet("project")) {
        projectManager->load(parser.value("project"));
    } else {
        const QStringList &projectNameList = projectManager->list(QRegularExpression(".*"));
        if (projectNameList.length() > 1) {
            return showErrorWithHelp(
                1,
                QCoreApplication::translate(
                    "main",
                    "Error: multiple projects found, please specify the project name.\n"
                    "Available projects are:\n%1\n")
                    .arg(projectNameList.join("\n")));
        }
        projectManager->loadFirst();
    }

    /* Check if output path is valid */
    if (!projectManager->isValidOutputPath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid output directory: %1")
                .arg(projectManager->getOutputPath()));
    }

    /* Load modules */
    if (!moduleManager->load(QRegularExpression(".*"))) {
        return showErrorWithHelp(
            1, QCoreApplication::translate("main", "Error: could not load library"));
    }

    /* Load buses */
    if (!busManager->load(QRegularExpression(".*"))) {
        return showErrorWithHelp(
            1, QCoreApplication::translate("main", "Error: could not load buses"));
    }

    /* Check if merge mode is enabled */
    const bool mergeMode = parser.isSet("merge");

    /* Set force overwrite mode if enabled */
    if (parser.isSet("force")) {
        generateManager->setForceOverwrite(true);
    }

    if (mergeMode && filePathList.size() > 1) {
        /* Merge mode: combine multiple netlist files */
        return processMergedNetlists(filePathList);
    }
    /* Normal mode: process each netlist file separately */
    return processIndividualNetlists(filePathList);
}

bool QSocCliWorker::processMergedNetlists(const QStringList &filePathList)
{
    /* Validate all files exist first */
    for (const QString &netlistFilePath : filePathList) {
        if (!QFile::exists(netlistFilePath)) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: Netlist file does not exist: \"%1\"")
                    .arg(netlistFilePath));
        }
    }

    /* Load and merge all netlist files */
    YAML::Node mergedNetlist;
    QString    outputFileName;

    for (int i = 0; i < filePathList.size(); ++i) {
        const QString &netlistFilePath = filePathList.at(i);

        /* Load the current netlist file */
        std::ifstream fileStream(netlistFilePath.toStdString());
        if (!fileStream.is_open()) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: Unable to open netlist file: \"%1\"")
                    .arg(netlistFilePath));
        }

        try {
            const YAML::Node currentNetlist = YAML::Load(fileStream);
            fileStream.close();

            if (i == 0) {
                /* For the first file, use it as the base */
                mergedNetlist = currentNetlist;

                /* Use the first file's basename for output */
                const QFileInfo fileInfo(netlistFilePath);
                outputFileName = fileInfo.baseName();
            } else {
                /* For subsequent files, merge them using the QSocYamlUtils mergeNodes function */
                mergedNetlist = QSocYamlUtils::mergeNodes(mergedNetlist, currentNetlist);

                /* Keep using the first file's basename for output filename */
                /* No need to append additional names for merged content */
            }

            showInfo(
                0,
                QCoreApplication::translate("main", "Loaded netlist file: %1").arg(netlistFilePath));

        } catch (const YAML::Exception &e) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error parsing YAML file: %1: %2")
                    .arg(netlistFilePath, e.what()));
        }
    }

    /* Set the merged netlist data in the generate manager */
    if (!generateManager->setNetlistData(mergedNetlist)) {
        return showError(
            1, QCoreApplication::translate("main", "Error: failed to set merged netlist data"));
    }

    /* Process the merged netlist */
    if (!generateManager->processNetlist()) {
        return showError(
            1, QCoreApplication::translate("main", "Error: failed to process merged netlist"));
    }

    /* Generate Verilog code for the merged netlist */
    if (!generateManager->generateVerilog(outputFileName)) {
        return showError(
            1,
            QCoreApplication::translate(
                "main", "Error: failed to generate Verilog code for merged netlist: %1")
                .arg(outputFileName));
    }

    showInfo(
        0,
        QCoreApplication::translate(
            "main", "Successfully generated Verilog code for merged netlist: %1")
            .arg(QDir(projectManager->getOutputPath()).filePath(outputFileName + ".v")));

    return true;
}

bool QSocCliWorker::processIndividualNetlists(const QStringList &filePathList)
{
    /* Generate Verilog code for each netlist file individually */
    for (const QString &netlistFilePath : filePathList) {
        /* Check if the netlist file exists before trying to load it */
        if (!QFile::exists(netlistFilePath)) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: Netlist file does not exist: \"%1\"")
                    .arg(netlistFilePath));
        }

        /* Load the netlist file */
        if (!generateManager->loadNetlist(netlistFilePath)) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: failed to load netlist file: %1")
                    .arg(netlistFilePath));
        }

        /* Process the netlist */
        if (!generateManager->processNetlist()) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: failed to process netlist file: %1")
                    .arg(netlistFilePath));
        }

        /* Generate Verilog code */
        const QFileInfo fileInfo(netlistFilePath);
        const QString   outputFileName = fileInfo.baseName();
        if (!generateManager->generateVerilog(outputFileName)) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: failed to generate Verilog code for: %1")
                    .arg(outputFileName));
        }

        showInfo(
            0,
            QCoreApplication::translate("main", "Successfully generated Verilog code: %1")
                .arg(QDir(projectManager->getOutputPath()).filePath(outputFileName + ".v")));
    }

    return true;
}

bool QSocCliWorker::parseGenerateTemplate(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"d", "directory"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project directory"},
        {{"p", "project"}, QCoreApplication::translate("main", "The project name."), "project name"},
        {"csv",
         QCoreApplication::translate("main", "CSV data file (can be used multiple times)."),
         "csv file"},
        {"yaml",
         QCoreApplication::translate("main", "YAML data file (can be used multiple times)."),
         "yaml file"},
        {"json",
         QCoreApplication::translate("main", "JSON data file (can be used multiple times)."),
         "json file"},
        {"rdl",
         QCoreApplication::translate("main", "SystemRDL data file (can be used multiple times)."),
         "rdl file"},
        {"rcsv",
         QCoreApplication::translate(
             "main", "RCSV (Register-CSV) data file (can be used multiple times)."),
         "rcsv file"},
    });

    parser.addPositionalArgument(
        "templates",
        QCoreApplication::translate("main", "The Jinja2 template files to be processed."),
        "<template.j2> [<template2.j2>...]");

    parser.parse(appArguments);

    if (parser.isSet("help")) {
        return showHelp(0);
    }

    const QStringList  cmdArguments     = parser.positionalArguments();
    const QStringList &templateFileList = cmdArguments;
    if (templateFileList.isEmpty()) {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: missing template files."));
    }

    /* Setup project manager and project path  */
    if (parser.isSet("directory")) {
        const QString dirPath = parser.value("directory");
        projectManager->setProjectPath(dirPath);
    }

    if (parser.isSet("project")) {
        projectManager->load(parser.value("project"));
    } else {
        const QStringList &projectNameList = projectManager->list(QRegularExpression(".*"));
        if (projectNameList.length() > 1) {
            return showErrorWithHelp(
                1,
                QCoreApplication::translate(
                    "main",
                    "Error: multiple projects found, please specify the project name.\n"
                    "Available projects are:\n%1\n")
                    .arg(projectNameList.join("\n")));
        }
        projectManager->loadFirst();
    }

    /* Check if output path is valid */
    if (!projectManager->isValidOutputPath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid output directory: %1")
                .arg(projectManager->getOutputPath()));
    }

    /* Collect data files */
    QStringList csvFiles;
    QStringList yamlFiles;
    QStringList jsonFiles;
    QStringList rdlFiles;
    QStringList rcsvFiles;

    if (parser.isSet("csv")) {
        csvFiles = parser.values("csv");
    }

    if (parser.isSet("yaml")) {
        yamlFiles = parser.values("yaml");
    }

    if (parser.isSet("json")) {
        jsonFiles = parser.values("json");
    }

    if (parser.isSet("rdl")) {
        rdlFiles = parser.values("rdl");
    }

    if (parser.isSet("rcsv")) {
        rcsvFiles = parser.values("rcsv");
    }

    /* Process each template file */
    for (const QString &templateFilePath : templateFileList) {
        /* Check if the template file exists before trying to load it */
        if (!QFile::exists(templateFilePath)) {
            return showError(
                101,
                QCoreApplication::translate("main", "Error: Template file does not exist: \"%1\"")
                    .arg(templateFilePath));
        }

        /* Process the template */
        const QFileInfo fileInfo(templateFilePath);
        QString         outputFileName = fileInfo.fileName();
        /* Remove only the template extension (the last extension) */
        const int lastDotIndex = static_cast<int>(outputFileName.lastIndexOf('.'));
        if (lastDotIndex > 0) {
            outputFileName = outputFileName.left(lastDotIndex);
        }

        if (!generateManager->renderTemplate(
                templateFilePath,
                csvFiles,
                yamlFiles,
                jsonFiles,
                rdlFiles,
                rcsvFiles,
                outputFileName)) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: failed to render template: %1")
                    .arg(templateFilePath));
        }

        showInfo(
            0,
            QCoreApplication::translate("main", "Successfully generated file from template: %1")
                .arg(QDir(projectManager->getOutputPath()).filePath(outputFileName)));
    }

    return true;
}

bool QSocCliWorker::parseGenerateStub(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"d", "directory"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project directory"},
        {{"p", "project"}, QCoreApplication::translate("main", "The project name."), "project name"},
        {{"l", "library"},
         QCoreApplication::translate("main", "The library base name or regex."),
         "library base name or regex"},
        {{"m", "module"},
         QCoreApplication::translate("main", "The module name or regex."),
         "module name or regex"},
    });

    parser.addPositionalArgument(
        "stubname", QCoreApplication::translate("main", "The stub name to generate."), "<stubname>");

    parser.parse(appArguments);

    if (parser.isSet("help")) {
        return showHelp(0);
    }

    const QStringList cmdArguments = parser.positionalArguments();
    if (cmdArguments.isEmpty()) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: missing stub name."));
    }

    const QString &stubName = cmdArguments.first();

    /* Setup project manager and project path  */
    if (parser.isSet("directory")) {
        const QString dirPath = parser.value("directory");
        projectManager->setProjectPath(dirPath);
    }

    if (parser.isSet("project")) {
        projectManager->load(parser.value("project"));
    } else {
        const QStringList &projectNameList = projectManager->list(QRegularExpression(".*"));
        if (projectNameList.length() > 1) {
            return showErrorWithHelp(
                1,
                QCoreApplication::translate(
                    "main",
                    "Error: multiple projects found, please specify the project name.\n"
                    "Available projects are:\n%1\n")
                    .arg(projectNameList.join("\n")));
        }
        projectManager->loadFirst();
    }

    /* Check if output path is valid */
    if (!projectManager->isValidOutputPath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid output directory: %1")
                .arg(projectManager->getOutputPath()));
    }

    /* Load modules */
    QRegularExpression libraryRegex(".*");
    if (parser.isSet("library")) {
        libraryRegex = QRegularExpression(parser.value("library"));
    }

    if (!moduleManager->load(libraryRegex)) {
        return showErrorWithHelp(
            1, QCoreApplication::translate("main", "Error: could not load library"));
    }

    QRegularExpression moduleRegex(".*");
    if (parser.isSet("module")) {
        moduleRegex = QRegularExpression(parser.value("module"));
    }

    /* Generate stub files */
    if (!generateManager->generateStub(stubName, libraryRegex, moduleRegex)) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: failed to generate stub files for: %1")
                .arg(stubName));
    }

    showInfo(
        0,
        QCoreApplication::translate("main", "Successfully generated stub files: %1").arg(stubName));

    return true;
}
