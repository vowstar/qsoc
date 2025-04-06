// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"

#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"
#include "common/qstaticdatasedes.h"
#include "common/qstaticlog.h"

bool QSocCliWorker::parseModuleBus(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addPositionalArgument(
        "subcommand",
        QCoreApplication::translate(
            "main",
            "add      Add bus definitions to modules.\n"
            "remove   Remove bus definitions from modules.\n"
            "list     List bus definitions of modules.\n"
            "show     Show bus definitions of modules.\n"
            "explain  Explain potential bus interfaces in modules."),
        "module bus <subcommand> [subcommand options]");

    parser.parse(appArguments);
    const QStringList cmdArguments = parser.positionalArguments();
    if (cmdArguments.isEmpty()) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: missing subcommand."));
    }
    const QString &command       = cmdArguments.first();
    QStringList    nextArguments = appArguments;
    if (command == "add") {
        nextArguments.removeOne(command);
        if (!parseModuleBusAdd(nextArguments)) {
            return false;
        }
    } else if (command == "remove") {
        nextArguments.removeOne(command);
        if (!parseModuleBusRemove(nextArguments)) {
            return false;
        }
    } else if (command == "list") {
        nextArguments.removeOne(command);
        if (!parseModuleBusList(nextArguments)) {
            return false;
        }
    } else if (command == "show") {
        nextArguments.removeOne(command);
        if (!parseModuleBusShow(nextArguments)) {
            return false;
        }
    } else if (command == "explain") {
        nextArguments.removeOne(command);
        if (!parseModuleBusExplain(nextArguments)) {
            return false;
        }
    } else {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: unknown subcommand: %1.").arg(command));
    }

    return true;
}

bool QSocCliWorker::parseModuleBusAdd(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addOptions(
        {{{"d", "directory"},
          QCoreApplication::translate("main", "The path to the project directory."),
          "project directory"},
         {{"p", "project"}, QCoreApplication::translate("main", "The project name."), "project name"},
         {{"l", "library"},
          QCoreApplication::translate("main", "The library base name or regex."),
          "library base name or regex"},
         {{"m", "module"},
          QCoreApplication::translate("main", "The module name or regex."),
          "module name or regex"},
         {{"b", "bus"}, QCoreApplication::translate("main", "The specified bus name."), "bus name"},
         {{"o", "mode"},
          QCoreApplication::translate("main", "The bus mode (e.g., master, slave)."),
          "bus mode"},
         {{"bl", "bus-library"},
          QCoreApplication::translate("main", "The bus library name or regex."),
          "bus library name or regex"},
         {"ai", QCoreApplication::translate("main", "Use AI to generate bus interfaces."), ""}});

    parser.addPositionalArgument(
        "interface",
        QCoreApplication::translate("main", "The bus interface name to create."),
        "<bus interface name>");

    parser.parse(appArguments);
    const QStringList cmdArguments = parser.positionalArguments();
    const QString    &libraryName  = parser.isSet("library") ? parser.value("library") : ".*";
    const QString    &moduleName   = parser.isSet("module") ? parser.value("module") : "";
    const QString    &busName      = parser.isSet("bus") ? parser.value("bus") : "";
    const QString    &busLibrary = parser.isSet("bus-library") ? parser.value("bus-library") : ".*";
    const QString    &busMode    = parser.isSet("mode") ? parser.value("mode") : "";
    const bool        useAI      = parser.isSet("ai");

    /* Validate required parameters */
    if (busName.isEmpty()) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: bus name is required."));
    }
    if (moduleName.isEmpty()) {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: module name is required."));
    }
    if (busMode.isEmpty()) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: bus mode is required."));
    }

    /* Get bus interface name from positional arguments */
    QString busInterface;
    if (!cmdArguments.isEmpty()) {
        busInterface = cmdArguments.first();
    } else {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: bus interface name is required."));
    }

    /* Validate bus interface name is not empty */
    if (busInterface.trimmed().isEmpty()) {
        return showErrorWithHelp(
            1, QCoreApplication::translate("main", "Error: bus interface name cannot be empty."));
    }

    /* Setup project manager and project path  */
    if (parser.isSet("directory")) {
        projectManager->setProjectPath(parser.value("directory"));
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
    /* Check if module path is valid */
    if (!projectManager->isValidModulePath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid module directory: %1")
                .arg(projectManager->getModulePath()));
    }
    /* Check if library name is valid */
    const QRegularExpression libraryNameRegex(libraryName);
    if (!libraryNameRegex.isValid()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of library name: %1")
                .arg(libraryName));
    }
    /* Check if bus library name is valid */
    const QRegularExpression busLibraryRegex(busLibrary);
    if (!busLibraryRegex.isValid()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate(
                "main", "Error: invalid regular expression of bus library name: %1")
                .arg(busLibrary));
    }
    /* Check if module name is valid */
    const QRegularExpression moduleNameRegex(moduleName);
    if (!moduleNameRegex.isValid()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of module name: %1")
                .arg(moduleName));
    }

    /* Load bus library */
    if (!busManager->load(busLibraryRegex)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: could not load bus library: %1")
                .arg(busLibrary));
    }

    /* Load modules */
    if (!moduleManager->load(libraryNameRegex)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: could not load library: %1")
                .arg(libraryName));
    }

    /* Load configuration */
    socConfig->loadConfig();

    /* Update LLM service configuration */
    llmService->setConfig(socConfig);

    /* Add bus interface to module using AI or standard method */
    bool success = false;
    if (useAI) {
        /* Call the LLM-based method if AI option is set */
        success = moduleManager->addModuleBusWithLLM(moduleName, busName, busMode, busInterface);
    } else {
        /* Call the standard method if AI option is not set */
        success = moduleManager->addModuleBus(moduleName, busName, busMode, busInterface);
    }

    if (!success) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: could not add bus interface to module: %1")
                .arg(moduleName));
    }

    /* Print a success message with the bus interface information */
    showInfo(
        0,
        QCoreApplication::translate("main", "Bus added: %1 with bus type %2 in %3 mode to module %4")
            .arg(busInterface, busName, busMode, moduleName));

    return true;
}

bool QSocCliWorker::parseModuleBusRemove(const QStringList &appArguments)
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
        "interface",
        QCoreApplication::translate("main", "The bus interface name or regex."),
        "<bus interface name or regex>");

    parser.parse(appArguments);

    if (parser.isSet("help")) {
        return showHelp(0);
    }

    const QStringList cmdArguments = parser.positionalArguments();
    const QString    &libraryName  = parser.isSet("library") ? parser.value("library") : ".*";
    const QString    &moduleName   = parser.isSet("module") ? parser.value("module") : "";
    const QString    &busName      = cmdArguments.isEmpty() ? "" : cmdArguments.first();

    /* Validate required parameters */
    if (moduleName.isEmpty()) {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: module name is required."));
    }

    if (busName.isEmpty()) {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: bus interface name is required."));
    }

    /* Setup project manager and project path  */
    if (parser.isSet("directory")) {
        projectManager->setProjectPath(parser.value("directory"));
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

    /* Check if module path is valid */
    if (!projectManager->isValidModulePath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid module directory: %1")
                .arg(projectManager->getModulePath()));
    }

    /* Check if library name is valid */
    const QRegularExpression libraryNameRegex(libraryName);
    if (!libraryNameRegex.isValid()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of library name: %1")
                .arg(libraryName));
    }

    /* Check if module name is valid */
    const QRegularExpression moduleNameRegex(moduleName);
    if (!moduleNameRegex.isValid()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of module name: %1")
                .arg(moduleName));
    }

    /* Check if bus interface name is valid */
    const QRegularExpression busInterfaceRegex(busName);
    if (!busInterfaceRegex.isValid()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate(
                "main", "Error: invalid regular expression of bus interface name: %1")
                .arg(busName));
    }

    /* Load modules */
    if (!moduleManager->load(libraryNameRegex)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: could not load library: %1")
                .arg(libraryName));
    }

    /* Find modules matching the pattern */
    const QStringList moduleList = moduleManager->listModule(moduleNameRegex);
    if (moduleList.isEmpty()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: no modules found matching: %1")
                .arg(moduleName));
    }

    /* Process each module */
    bool allSucceeded = true;
    for (const QString &currentModule : moduleList) {
        if (!moduleManager->removeModuleBus(currentModule, busInterfaceRegex)) {
            showError(
                1,
                QCoreApplication::translate(
                    "main", "Error: failed to remove bus interface from module: %1")
                    .arg(currentModule));
            allSucceeded = false;
        } else {
            /* Print a success message for each removed bus interface */
            showInfo(
                0,
                QCoreApplication::translate("main", "Bus removed: %1 from module %2")
                    .arg(busName, currentModule));
        }
    }

    if (!allSucceeded) {
        return showErrorWithHelp(
            1, QCoreApplication::translate("main", "Error: some operations failed."));
    }

    return true;
}

bool QSocCliWorker::parseModuleBusList(const QStringList &appArguments)
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
        "interface",
        QCoreApplication::translate("main", "The bus interface name or regex."),
        "[<bus interface name or regex>]");

    parser.parse(appArguments);

    if (parser.isSet("help")) {
        return showHelp(0);
    }

    const QStringList cmdArguments = parser.positionalArguments();
    const QString    &libraryName  = parser.isSet("library") ? parser.value("library") : ".*";
    const QString    &moduleName   = parser.isSet("module") ? parser.value("module") : ".*";
    const QString    &busName      = cmdArguments.isEmpty() ? ".*" : cmdArguments.first();

    /* Setup project manager and project path  */
    if (parser.isSet("directory")) {
        projectManager->setProjectPath(parser.value("directory"));
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

    /* Check if module path is valid */
    if (!projectManager->isValidModulePath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid module directory: %1")
                .arg(projectManager->getModulePath()));
    }

    /* Check if library name is valid */
    const QRegularExpression libraryNameRegex(libraryName);
    if (!libraryNameRegex.isValid()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of library name: %1")
                .arg(libraryName));
    }

    /* Check if module name is valid */
    const QRegularExpression moduleNameRegex(moduleName);
    if (!moduleNameRegex.isValid()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of module name: %1")
                .arg(moduleName));
    }

    /* Check if bus interface name is valid */
    const QRegularExpression busInterfaceRegex(busName);
    if (!busInterfaceRegex.isValid()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate(
                "main", "Error: invalid regular expression of bus interface name: %1")
                .arg(busName));
    }

    /* Load modules */
    if (!moduleManager->load(libraryNameRegex)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: could not load library: %1")
                .arg(libraryName));
    }

    /* Find modules matching the pattern */
    const QStringList moduleList = moduleManager->listModule(moduleNameRegex);
    if (moduleList.isEmpty()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: no modules found matching: %1")
                .arg(moduleName));
    }

    /* List bus interfaces for each module */
    for (const QString &currentModule : moduleList) {
        const QStringList busInterfaces
            = moduleManager->listModuleBus(currentModule, busInterfaceRegex);

        if (busInterfaces.isEmpty()) {
            showInfo(
                0,
                QCoreApplication::translate("main", "Module '%1' has no matching bus interfaces.")
                    .arg(currentModule));
        } else {
            showInfo(
                0,
                QCoreApplication::translate("main", "Bus interfaces for module '%1':\n%2")
                    .arg(currentModule)
                    .arg(busInterfaces.join("\n")));
        }
    }

    return true;
}

bool QSocCliWorker::parseModuleBusShow(const QStringList &appArguments)
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
        "interface",
        QCoreApplication::translate("main", "The bus interface name or regex."),
        "[<bus interface name or regex>]");

    parser.parse(appArguments);

    if (parser.isSet("help")) {
        return showHelp(0);
    }

    const QStringList cmdArguments = parser.positionalArguments();
    const QString    &libraryName  = parser.isSet("library") ? parser.value("library") : ".*";
    const QString    &moduleName   = parser.isSet("module") ? parser.value("module") : ".*";
    const QString    &busName      = cmdArguments.isEmpty() ? ".*" : cmdArguments.first();

    /* Validate required parameters */
    if (moduleName.isEmpty()) {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: module name is required."));
    }

    /* Setup project manager and project path  */
    if (parser.isSet("directory")) {
        projectManager->setProjectPath(parser.value("directory"));
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

    /* Check if module path is valid */
    if (!projectManager->isValidModulePath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid module directory: %1")
                .arg(projectManager->getModulePath()));
    }

    /* Check if library name is valid */
    const QRegularExpression libraryNameRegex(libraryName);
    if (!libraryNameRegex.isValid()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of library name: %1")
                .arg(libraryName));
    }

    /* Check if module name is valid */
    const QRegularExpression moduleNameRegex(moduleName);
    if (!moduleNameRegex.isValid()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of module name: %1")
                .arg(moduleName));
    }

    /* Check if bus interface name is valid */
    const QRegularExpression busInterfaceRegex(busName);
    if (!busInterfaceRegex.isValid()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate(
                "main", "Error: invalid regular expression of bus interface name: %1")
                .arg(busName));
    }

    /* Load modules */
    if (!moduleManager->load(libraryNameRegex)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: could not load library: %1")
                .arg(libraryName));
    }

    /* Find modules matching the pattern */
    const QStringList moduleList = moduleManager->listModule(moduleNameRegex);
    if (moduleList.isEmpty()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: no modules found matching: %1")
                .arg(moduleName));
    }

    /* Show detailed bus information for each module */
    for (const QString &currentModule : moduleList) {
        const YAML::Node busDetails = moduleManager->showModuleBus(currentModule, busInterfaceRegex);

        if (!busDetails["bus"] || busDetails["bus"].size() == 0) {
            showInfo(
                0,
                QCoreApplication::translate("main", "Module '%1' has no matching bus interfaces.")
                    .arg(currentModule));
        } else {
            showInfo(
                0,
                QCoreApplication::translate("main", "Bus interfaces for module '%1':")
                    .arg(currentModule));
            showInfo(0, QStaticDataSedes::serializeYaml(busDetails));
        }
    }

    return true;
}

bool QSocCliWorker::parseModuleBusExplain(const QStringList &appArguments)
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
        {{"b", "bus"}, QCoreApplication::translate("main", "The specified bus name."), "bus name"},
        {{"bl", "bus-library"},
         QCoreApplication::translate("main", "The bus library name or regex."),
         "bus library name or regex"},
    });

    parser.parse(appArguments);

    if (parser.isSet("help")) {
        return showHelp(0);
    }

    const QString &libraryName = parser.isSet("library") ? parser.value("library") : ".*";
    const QString &moduleName  = parser.isSet("module") ? parser.value("module") : "";
    const QString &busName     = parser.isSet("bus") ? parser.value("bus") : "";
    const QString &busLibrary  = parser.isSet("bus-library") ? parser.value("bus-library") : ".*";

    /* Validate required parameters */
    if (moduleName.isEmpty()) {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: module name is required."));
    }
    if (busName.isEmpty()) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: bus name is required."));
    }

    /* Setup project manager and project path  */
    if (parser.isSet("directory")) {
        projectManager->setProjectPath(parser.value("directory"));
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

    /* Check if module path is valid */
    if (!projectManager->isValidModulePath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid module directory: %1")
                .arg(projectManager->getModulePath()));
    }

    /* Check if library name is valid */
    const QRegularExpression libraryNameRegex(libraryName);
    if (!libraryNameRegex.isValid()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of library name: %1")
                .arg(libraryName));
    }

    /* Check if module name is valid */
    const QRegularExpression moduleNameRegex(moduleName);
    if (!moduleNameRegex.isValid()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of module name: %1")
                .arg(moduleName));
    }
    /* Check if bus library name is valid */
    const QRegularExpression busLibraryRegex(busLibrary);
    if (!busLibraryRegex.isValid()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate(
                "main", "Error: invalid regular expression of bus library name: %1")
                .arg(busLibrary));
    }

    /* Load bus library */
    if (!busManager->load(busLibraryRegex)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: could not load bus library: %1")
                .arg(busLibrary));
    }

    /* Load modules */
    if (!moduleManager->load(libraryNameRegex)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: could not load library: %1")
                .arg(libraryName));
    }

    /* Load configuration */
    socConfig->loadConfig();

    /* Update LLM service configuration */
    llmService->setConfig(socConfig);

    /* Explain bus interface using LLM */
    QString explanation;
    if (!moduleManager->explainModuleBusWithLLM(moduleName, busName, explanation)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: could not explain bus interface for module: %1")
                .arg(moduleName));
    }

    /* Show the explanation */
    showInfo(0, explanation);

    return true;
}
