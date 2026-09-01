// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"

#include <algorithm>

#include "common/qslangdriver.h"
#include "common/qsociomuxgenerator.h"
#include "common/qsocmmiogenerator.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"
#include "common/qsocverilogutils.h"
#include "common/qstaticdatasedes.h"

#include <QDir>
#include <QLockFile>

namespace {

bool isValidLibraryBasename(const QString &name)
{
    if (name.isEmpty() || name == "." || name == "..") {
        return false;
    }
    const QString invalidChars = "\\/:*?\"<>|.";
    for (const QChar character : invalidChars) {
        if (name.contains(character)) {
            return false;
        }
    }
    return true;
}

} // namespace

bool QSocCliWorker::parseModule(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addPositionalArgument(
        "subcommand",
        QCoreApplication::translate(
            "main",
            "create     Create a generated module draft.\n"
            "validate   Validate a generated module.\n"
            "import     Import Verilog modules into module libraries.\n"
            "remove     Remove modules from specified libraries.\n"
            "list       List all modules within designated libraries.\n"
            "show       Show detailed information on a chosen module.\n"
            "bus        Manage bus interfaces of modules."),
        "module <subcommand> [subcommand options]");

    parser.parse(appArguments);
    const QStringList positionalArgs = parser.positionalArguments();
    if (positionalArgs.isEmpty()) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: missing subcommand."));
    }
    const QString &command       = positionalArgs.first();
    QStringList    nextArguments = appArguments;
    if (command == "create") {
        nextArguments.removeOne(command);
        if (!parseModuleCreate(nextArguments)) {
            return false;
        }
    } else if (command == "validate") {
        nextArguments.removeOne(command);
        if (!parseModuleValidate(nextArguments)) {
            return false;
        }
    } else if (command == "import") {
        nextArguments.removeOne(command);
        if (!parseModuleImport(nextArguments)) {
            return false;
        }
    } else if (command == "remove") {
        nextArguments.removeOne(command);
        if (!parseModuleRemove(nextArguments)) {
            return false;
        }
    } else if (command == "list") {
        nextArguments.removeOne(command);
        if (!parseModuleList(nextArguments)) {
            return false;
        }
    } else if (command == "show") {
        nextArguments.removeOne(command);
        if (!parseModuleShow(nextArguments)) {
            return false;
        }
    } else if (command == "bus") {
        nextArguments.removeOne(command);
        if (!parseModuleBus(nextArguments)) {
            return false;
        }
    } else {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: unknown subcommand: %1.").arg(command));
    }

    return true;
}

bool QSocCliWorker::parseModuleCreate(const QStringList &appArguments)
{
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"d", "directory"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project directory"},
        {{"p", "project"}, QCoreApplication::translate("main", "The project name."), "project name"},
        {{"l", "library"},
         QCoreApplication::translate("main", "The exact module library name."),
         "library name"},
        {"generator", QCoreApplication::translate("main", "The generator kind."), "generator kind"},
    });
    parser.addPositionalArgument(
        "module", QCoreApplication::translate("main", "The exact module name."), "<module>");

    if (!parseOptions(appArguments)) {
        return false;
    }

    const QStringList positionalArgs = parser.positionalArguments();
    if (positionalArgs.size() != 1) {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: expected one module name."));
    }
    if (!parser.isSet("library")) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: missing library name."));
    }
    if (!parser.isSet("generator")) {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: missing generator kind."));
    }

    const QString libraryName = parser.value("library");
    const QString moduleName  = positionalArgs.first();
    if (!isValidLibraryBasename(libraryName)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid library name: %1.").arg(libraryName));
    }
    if (!QSocVerilogUtils::isValidVerilogIdentifier(moduleName)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid module name: %1.").arg(moduleName));
    }
    const QString generatorKind = parser.value("generator");
    if (generatorKind != QStringLiteral("mmio") && generatorKind != QStringLiteral("iomux")) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: unsupported generator kind: %1.")
                .arg(generatorKind));
    }

    if (!loadSelectedProject()) {
        return false;
    }
    if (!projectManager->isValidModulePath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid module directory: %1")
                .arg(projectManager->getModulePath()));
    }

    const QString libraryPath
        = QDir(projectManager->getModulePath()).filePath(libraryName + QStringLiteral(".soc_mod"));
    QLockFile libraryLock(libraryPath + QStringLiteral(".lock"));
    if (!libraryLock.tryLock()) {
        QString message;
        switch (libraryLock.error()) {
        case QLockFile::LockFailedError:
            message = QCoreApplication::translate("main", "Error: module library is locked: %1")
                          .arg(libraryPath);
            break;
        case QLockFile::PermissionError:
            message = QCoreApplication::translate(
                          "main", "Error: permission denied while locking module library: %1")
                          .arg(libraryPath);
            break;
        case QLockFile::NoError:
        case QLockFile::UnknownError:
            message = QCoreApplication::translate("main", "Error: could not lock module library: %1")
                          .arg(libraryPath);
            break;
        }
        return showError(1, message);
    }

    if (moduleManager->isLibraryFileExist(libraryName)) {
        if (!moduleManager->load(libraryName)) {
            return showError(1, QCoreApplication::translate("main", "Error: could not load library."));
        }
        if (moduleManager->listModulesInLibrary(libraryName).contains(moduleName)) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: module already exists: %1/%2.")
                    .arg(libraryName, moduleName));
        }
    }

    const QString kindLabel = generatorKind == QStringLiteral("iomux") ? QStringLiteral("IOMUX")
                                                                       : QStringLiteral("MMIO");
    QSocModuleDefinition definition;
    definition.libraryName                  = libraryName;
    definition.moduleName                   = moduleName;
    definition.extraAttributes["generator"] = generatorKind == QStringLiteral("iomux")
                                                  ? QSocIomuxGenerator::createDraftGenerator()
                                                  : QSocMmioGenerator::createDraftGenerator();
    if (!moduleManager->replaceModuleDefinition(definition)) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: could not create %1 module draft.")
                .arg(kindLabel));
    }

    return showInfo(
        0,
        QCoreApplication::translate("main", "Created %1 module draft: %2/%3.")
            .arg(kindLabel, libraryName, moduleName));
}

bool QSocCliWorker::parseModuleValidate(const QStringList &appArguments)
{
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"d", "directory"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project directory"},
        {{"p", "project"}, QCoreApplication::translate("main", "The project name."), "project name"},
        {{"l", "library"},
         QCoreApplication::translate("main", "The exact module library name."),
         "library name"},
    });
    parser.addPositionalArgument(
        "module", QCoreApplication::translate("main", "The exact module name."), "<module>");

    if (!parseOptions(appArguments)) {
        return false;
    }

    const QStringList positionalArgs = parser.positionalArguments();
    if (positionalArgs.size() != 1) {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: expected one module name."));
    }
    if (!parser.isSet("library")) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: missing library name."));
    }

    const QString libraryName = parser.value("library");
    const QString moduleName  = positionalArgs.first();
    if (!isValidLibraryBasename(libraryName)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid library name: %1.").arg(libraryName));
    }
    if (!QSocVerilogUtils::isValidVerilogIdentifier(moduleName)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid module name: %1.").arg(moduleName));
    }

    if (!loadSelectedProject()) {
        return false;
    }
    if (!projectManager->isValidModulePath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid module directory: %1")
                .arg(projectManager->getModulePath()));
    }
    if (!moduleManager->load(libraryName)
        || !moduleManager->listModulesInLibrary(libraryName).contains(moduleName)) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: module not found: %1/%2.")
                .arg(libraryName, moduleName));
    }

    const QSocModuleDefinition definition
        = moduleManager->getModuleDefinition(libraryName, moduleName);
    if (!definition.extraAttributes["generator"]) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: module does not declare a generator: %1/%2.")
                .arg(libraryName, moduleName));
    }
    if (QSocIomuxGenerator::isIomux(definition)) {
        const QStringList errors = QSocIomuxGenerator::validate(definition);
        if (!errors.isEmpty()) {
            QStringList messages;
            messages.reserve(errors.size());
            for (const QString &error : errors) {
                messages.append(QCoreApplication::translate("main", "Error: %1").arg(error));
            }
            return showError(1, messages.join('\n'));
        }
        QSocIomuxPlan plan;
        QSocIomuxGenerator::buildPlan(definition, &plan);
        const bool defaultedSlots = !definition.extraAttributes["generator"]["hs_slots"];
        return showInfo(
            0,
            QCoreApplication::translate(
                "main",
                "IOMUX source is valid: %1/%2. Pins: %3, HS slots: %4%5, routes: %6, "
                "selector registers: %7, registers total: %8. Reset selects slot 0, RX broadcasts. "
                "Integration pending merge.")
                .arg(libraryName, moduleName)
                .arg(plan.pinCount)
                .arg(plan.hsSlots)
                .arg(defaultedSlots ? QStringLiteral(" (default)") : QString())
                .arg(plan.routes.size())
                .arg(
                    std::count_if(
                        plan.mmio.registers.cbegin(),
                        plan.mmio.registers.cend(),
                        [](const QSocMmioRegisterPlan &reg) {
                            return reg.name.startsWith(QStringLiteral("hs_select_"));
                        }))
                .arg(plan.mmio.registers.size()));
    }

    const QStringList errors = QSocMmioGenerator::validate(definition);
    if (!errors.isEmpty()) {
        QStringList messages;
        messages.reserve(errors.size());
        for (const QString &error : errors) {
            messages.append(QCoreApplication::translate("main", "Error: %1").arg(error));
        }
        return showError(1, messages.join('\n'));
    }

    return showInfo(
        0,
        QCoreApplication::translate("main", "MMIO source is valid: %1/%2.")
            .arg(libraryName, moduleName));
}

bool QSocCliWorker::parseModuleImport(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"d", "directory"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project directory"},
        {{"p", "project"}, QCoreApplication::translate("main", "The project name."), "project name"},
        {{"l", "library"},
         QCoreApplication::translate("main", "The library base name."),
         "library base name"},
        {{"m", "module"},
         QCoreApplication::translate("main", "The module name or regex."),
         "module name or regex"},
        {{"f", "filelist"},
         QCoreApplication::translate(
             "main",
             "The path where the file list is located, including a list of "
             "verilog files in order."),
         "filelist"},
        {{"D", "define"},
         QCoreApplication::translate("main", "Define macro as KEY or KEY=VALUE."),
         "macro definition"},
        {{"U", "undefine"},
         QCoreApplication::translate("main", "Undefine macro KEY at the start of all source files."),
         "macro name"},
    });
    parser.addPositionalArgument(
        "files",
        QCoreApplication::translate("main", "The verilog files to be processed."),
        "[<verilog files>]");

    parser.parse(appArguments);
    const QStringList  positionalArgs = parser.positionalArguments();
    const QString      libraryName    = parser.value("library");
    const QString     &moduleName     = parser.isSet("module") ? parser.value("module") : ".*";
    const QStringList &filePathList   = positionalArgs;
    const QStringList &macroDefines   = parser.values("define");
    const QStringList &macroUndefines = parser.values("undefine");
    if (filePathList.isEmpty() && !parser.isSet("filelist")) {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: missing verilog files."));
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
    if (!projectManager->isValidModulePath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid module directory: %1")
                .arg(projectManager->getModulePath()));
    }

    QString filelistPath = "";
    if (parser.isSet("filelist")) {
        filelistPath = parser.value("filelist");
    }
    const QRegularExpression moduleNameRegex(moduleName);
    if (!moduleNameRegex.isValid()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of module name."));
    }
    /* Validate macro definitions */
    for (const QString &macro : macroDefines) {
        if (macro.trimmed().isEmpty()) {
            return showErrorWithHelp(
                1, QCoreApplication::translate("main", "Error: empty macro definition."));
        }
        /* Check for invalid characters in macro name */
        QString macroName = macro.contains('=') ? macro.split('=').first() : macro;
        if (macroName.trimmed().isEmpty()
            || !macroName.contains(QRegularExpression("^[A-Za-z_][A-Za-z0-9_]*$"))) {
            return showErrorWithHelp(
                1,
                QCoreApplication::translate(
                    "main", "Error: invalid macro name: %1. Must start with letter or underscore.")
                    .arg(macroName));
        }
    }
    /* Validate macro undefines */
    for (const QString &macro : macroUndefines) {
        if (macro.trimmed().isEmpty()) {
            return showErrorWithHelp(
                1, QCoreApplication::translate("main", "Error: empty macro name for undefine."));
        }
        if (!macro.contains(QRegularExpression("^[A-Za-z_][A-Za-z0-9_]*$"))) {
            return showErrorWithHelp(
                1,
                QCoreApplication::translate(
                    "main", "Error: invalid macro name: %1. Must start with letter or underscore.")
                    .arg(macro));
        }
    }
    if (!moduleManager->importFromFileList(
            libraryName, moduleNameRegex, filelistPath, filePathList, macroDefines, macroUndefines)) {
        return showErrorWithHelp(1, QCoreApplication::translate("main", "Error: import failed."));
    }

    return true;
}

bool QSocCliWorker::parseModuleRemove(const QStringList &appArguments)
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
    });
    parser.addPositionalArgument(
        "name",
        QCoreApplication::translate("main", "The module name or regex list."),
        "[<module name or regex list>]");

    parser.parse(appArguments);
    const QStringList positionalArgs = parser.positionalArguments();
    const QString    &libraryName    = parser.isSet("library") ? parser.value("library") : ".*";
    QStringList       moduleNameList = positionalArgs;
    if (moduleNameList.isEmpty()) {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: missing module name or regex."));
    }
    /* Removing duplicates */
    moduleNameList.removeDuplicates();
    /* Removing empty strings and strings containing only whitespace */
    moduleNameList.erase(
        std::remove_if(
            moduleNameList.begin(),
            moduleNameList.end(),
            [](const QString &str) { return str.trimmed().isEmpty(); }),
        moduleNameList.end());
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
    /* Check if all module names in list is valid */
    bool    invalidModuleNameFound = false;
    QString invalidModuleName;
    /* Iterate through all module names */
    for (const QString &moduleName : moduleNameList) {
        const QRegularExpression moduleNameRegex(moduleName);
        if (!moduleNameRegex.isValid()) {
            invalidModuleNameFound = true;
            invalidModuleName      = moduleName;
            break;
        }
    }
    /* Show error if invalid module name is found */
    if (invalidModuleNameFound) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of module name: %1")
                .arg(invalidModuleName));
    }
    /* Load modules */
    if (!moduleManager->load(libraryNameRegex)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: could not load library: %1")
                .arg(libraryName));
    }
    /* Remove modules */
    for (const QString &moduleName : moduleNameList) {
        const QRegularExpression moduleNameRegex(moduleName);
        if (!moduleManager->removeModule(moduleNameRegex)) {
            return showErrorWithHelp(
                1,
                QCoreApplication::translate("main", "Error: could not remove module: %1")
                    .arg(moduleName));
        }
        showInfo(
            0, QCoreApplication::translate("main", "Success: removed module: %1").arg(moduleName));
    }

    return true;
}

bool QSocCliWorker::parseModuleList(const QStringList &appArguments)
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
    });
    parser.addPositionalArgument(
        "name",
        QCoreApplication::translate("main", "The module name or regex list."),
        "[<module name or regex list>]");

    parser.parse(appArguments);

    if (parser.isSet("help")) {
        return showHelp(0);
    }

    const QStringList positionalArgs = parser.positionalArguments();
    const QString    &libraryName    = parser.isSet("library") ? parser.value("library") : ".*";
    QStringList moduleNameList = !positionalArgs.empty() ? positionalArgs : QStringList() << ".*";
    /* Removing duplicates */
    moduleNameList.removeDuplicates();
    /* Removing empty strings and strings containing only whitespace */
    moduleNameList.erase(
        std::remove_if(
            moduleNameList.begin(),
            moduleNameList.end(),
            [](const QString &str) { return str.trimmed().isEmpty(); }),
        moduleNameList.end());
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
    /* Check if all module names in list is valid */
    bool    invalidModuleNameFound = false;
    QString invalidModuleName;
    /* Iterate through all module names */
    for (const QString &moduleName : moduleNameList) {
        const QRegularExpression moduleNameRegex(moduleName);
        if (!moduleNameRegex.isValid()) {
            invalidModuleNameFound = true;
            invalidModuleName      = moduleName;
            break;
        }
    }
    /* Show error if invalid module name is found */
    if (invalidModuleNameFound) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of module name: %1")
                .arg(invalidModuleName));
    }
    /* Load modules */
    if (!moduleManager->load(libraryNameRegex)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: could not load library: %1")
                .arg(libraryName));
    }
    /* list modules */
    for (const QString &moduleName : moduleNameList) {
        const QRegularExpression moduleNameRegex(moduleName);
        const QStringList        result = moduleManager->listModule(moduleNameRegex);
        showInfo(0, result.join("\n"));
    }

    return true;
}

bool QSocCliWorker::parseModuleShow(const QStringList &appArguments)
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
    });
    parser.addPositionalArgument(
        "name",
        QCoreApplication::translate("main", "The module name or regex list."),
        "[<module name or regex list>]");

    parser.parse(appArguments);

    if (parser.isSet("help")) {
        return showHelp(0);
    }

    const QStringList positionalArgs = parser.positionalArguments();
    const QString    &libraryName    = parser.isSet("library") ? parser.value("library") : ".*";
    QStringList moduleNameList = !positionalArgs.empty() ? positionalArgs : QStringList() << ".*";
    /* Removing duplicates */
    moduleNameList.removeDuplicates();
    /* Removing empty strings and strings containing only whitespace */
    moduleNameList.erase(
        std::remove_if(
            moduleNameList.begin(),
            moduleNameList.end(),
            [](const QString &str) { return str.trimmed().isEmpty(); }),
        moduleNameList.end());
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
    /* Check if all module names in list is valid */
    bool    invalidModuleNameFound = false;
    QString invalidModuleName;
    /* Iterate through all module names */
    for (const QString &moduleName : moduleNameList) {
        const QRegularExpression moduleNameRegex(moduleName);
        if (!moduleNameRegex.isValid()) {
            invalidModuleNameFound = true;
            invalidModuleName      = moduleName;
            break;
        }
    }
    /* Show error if invalid module name is found */
    if (invalidModuleNameFound) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of module name: %1")
                .arg(invalidModuleName));
    }
    /* Load modules */
    if (!moduleManager->load(libraryNameRegex)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: could not load library: %1")
                .arg(libraryName));
    }
    /* List modules matching patterns */
    bool moduleFound = false;
    for (const QString &moduleName : moduleNameList) {
        const QRegularExpression moduleRegex(moduleName);
        /* Check if module exists using regular expression */
        if (moduleManager->isModuleExist(moduleRegex)) {
            moduleFound = true;
            /* Show module details */
            showInfo(0, QStaticDataSedes::serializeYaml(moduleManager->getModuleYamls(moduleRegex)));
        }
    }

    if (!moduleFound) {
        if (moduleNameList.size() == 1) {
            showInfo(
                0,
                QCoreApplication::translate("main", "Error: module not found: %1")
                    .arg(moduleNameList.first()));
        } else {
            showInfo(0, QCoreApplication::translate("main", "Error: module not found"));
        }
    }

    return true;
}
