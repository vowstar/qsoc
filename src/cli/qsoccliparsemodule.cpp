#include "cli/qsoccliworker.h"

#include "common/qslangdriver.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"
#include "common/qstaticdatasedes.h"
#include "common/qstaticlog.h"

bool QSocCliWorker::parseModule(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addPositionalArgument(
        "subcommand",
        QCoreApplication::translate(
            "main",
            "import   Import Verilog modules into module libraries.\n"
            "remove   Remove modules from specified libraries.\n"
            "list     List all modules within designated libraries.\n"
            "show     Show detailed information on a chosen module.\n"
            "bus      Manage bus interfaces of modules."),
        "module <subcommand> [subcommand options]");

    parser.parse(appArguments);
    const QStringList cmdArguments = parser.positionalArguments();
    if (cmdArguments.isEmpty()) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: missing subcommand."));
    }
    const QString &command       = cmdArguments.first();
    QStringList    nextArguments = appArguments;
    if (command == "import") {
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
    });
    parser.addPositionalArgument(
        "files",
        QCoreApplication::translate("main", "The verilog files to be processed."),
        "[<verilog files>]");

    parser.parse(appArguments);
    const QStringList  cmdArguments = parser.positionalArguments();
    const QString     &libraryName  = parser.isSet("library") ? parser.value("library") : "";
    const QString     &moduleName   = parser.isSet("module") ? parser.value("module") : ".*";
    const QStringList &filePathList = cmdArguments;
    if (filePathList.isEmpty() && !parser.isSet("filelist")) {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: missing verilog files."));
    }
    /* Setup project manager and project path  */
    QSocProjectManager projectManager(this);
    if (parser.isSet("directory")) {
        projectManager.setProjectPath(parser.value("directory"));
    }
    if (parser.isSet("project")) {
        projectManager.load(parser.value("project"));
    } else {
        const QStringList &projectNameList = projectManager.list(QRegularExpression(".*"));
        if (projectNameList.length() > 1) {
            return showErrorWithHelp(
                1,
                QCoreApplication::translate(
                    "main",
                    "Error: multiple projects found, please specify the project name.\n"
                    "Available projects are:\n%1\n")
                    .arg(projectNameList.join("\n")));
        }
        projectManager.loadFirst();
    }
    if (!projectManager.isValidModulePath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid module directory: %1")
                .arg(projectManager.getModulePath()));
    }
    /* Setup module manager */
    QSocBusManager    busManager(this, &projectManager);
    QSocModuleManager moduleManager(this, &projectManager, &busManager);
    QString           filelistPath = "";
    if (parser.isSet("filelist")) {
        filelistPath = parser.value("filelist");
    }
    const QRegularExpression moduleNameRegex(moduleName);
    if (!moduleNameRegex.isValid()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of module name."));
    }
    if (!moduleManager.importFromFileList(libraryName, moduleNameRegex, filelistPath, filePathList)) {
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
    const QStringList cmdArguments   = parser.positionalArguments();
    const QString    &libraryName    = parser.isSet("library") ? parser.value("library") : ".*";
    QStringList       moduleNameList = cmdArguments;
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
    QSocProjectManager projectManager(this);
    if (parser.isSet("directory")) {
        projectManager.setProjectPath(parser.value("directory"));
    }
    if (parser.isSet("project")) {
        projectManager.load(parser.value("project"));
    } else {
        const QStringList &projectNameList = projectManager.list(QRegularExpression(".*"));
        if (projectNameList.length() > 1) {
            return showErrorWithHelp(
                1,
                QCoreApplication::translate(
                    "main",
                    "Error: multiple projects found, please specify the project name.\n"
                    "Available projects are:\n%1\n")
                    .arg(projectNameList.join("\n")));
        }
        projectManager.loadFirst();
    }
    /* Check if module path is valid */
    if (!projectManager.isValidModulePath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid module directory: %1")
                .arg(projectManager.getModulePath()));
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
    /* Setup module manager */
    QSocBusManager    busManager(this, &projectManager);
    QSocModuleManager moduleManager(this, &projectManager, &busManager);
    /* Load modules */
    if (!moduleManager.load(libraryNameRegex)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: could not load library: %1")
                .arg(libraryName));
    }
    /* Remove modules */
    for (const QString &moduleName : moduleNameList) {
        const QRegularExpression moduleNameRegex(moduleName);
        if (!moduleManager.removeModule(moduleNameRegex)) {
            return showErrorWithHelp(
                1,
                QCoreApplication::translate("main", "Error: could not remove module: %1")
                    .arg(moduleName));
        }
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

    const QStringList cmdArguments = parser.positionalArguments();
    const QString    &libraryName  = parser.isSet("library") ? parser.value("library") : ".*";
    QStringList moduleNameList = cmdArguments.length() > 0 ? cmdArguments : QStringList() << ".*";
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
    QSocProjectManager projectManager(this);
    if (parser.isSet("directory")) {
        projectManager.setProjectPath(parser.value("directory"));
    }
    if (parser.isSet("project")) {
        projectManager.load(parser.value("project"));
    } else {
        const QStringList &projectNameList = projectManager.list(QRegularExpression(".*"));
        if (projectNameList.length() > 1) {
            return showErrorWithHelp(
                1,
                QCoreApplication::translate(
                    "main",
                    "Error: multiple projects found, please specify the project name.\n"
                    "Available projects are:\n%1\n")
                    .arg(projectNameList.join("\n")));
        }
        projectManager.loadFirst();
    }
    /* Check if module path is valid */
    if (!projectManager.isValidModulePath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid module directory: %1")
                .arg(projectManager.getModulePath()));
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
    /* Setup module manager */
    QSocBusManager    busManager(this, &projectManager);
    QSocModuleManager moduleManager(this, &projectManager, &busManager);
    /* Load modules */
    if (!moduleManager.load(libraryNameRegex)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: could not load library: %1")
                .arg(libraryName));
    }
    /* list modules */
    for (const QString &moduleName : moduleNameList) {
        const QRegularExpression moduleNameRegex(moduleName);
        const QStringList        result = moduleManager.listModule(moduleNameRegex);
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

    const QStringList cmdArguments = parser.positionalArguments();
    const QString    &libraryName  = parser.isSet("library") ? parser.value("library") : ".*";
    QStringList moduleNameList = cmdArguments.length() > 0 ? cmdArguments : QStringList() << ".*";
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
    QSocProjectManager projectManager(this);
    if (parser.isSet("directory")) {
        projectManager.setProjectPath(parser.value("directory"));
    }
    if (parser.isSet("project")) {
        projectManager.load(parser.value("project"));
    } else {
        const QStringList &projectNameList = projectManager.list(QRegularExpression(".*"));
        if (projectNameList.length() > 1) {
            return showErrorWithHelp(
                1,
                QCoreApplication::translate(
                    "main",
                    "Error: multiple projects found, please specify the project name.\n"
                    "Available projects are:\n%1\n")
                    .arg(projectNameList.join("\n")));
        }
        projectManager.loadFirst();
    }
    /* Check if module path is valid */
    if (!projectManager.isValidModulePath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid module directory: %1")
                .arg(projectManager.getModulePath()));
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
    /* Setup module manager */
    QSocBusManager    busManager(this, &projectManager);
    QSocModuleManager moduleManager(this, &projectManager, &busManager);
    /* Load modules */
    if (!moduleManager.load(libraryNameRegex)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: could not load library: %1")
                .arg(libraryName));
    }
    /* list modules */
    for (const QString &moduleName : moduleNameList) {
        const QRegularExpression moduleNameRegex(moduleName);
        /* Show details about the module */
        showInfo(0, QStaticDataSedes::serializeYaml(moduleManager.getModuleYamls(moduleNameRegex)));
    }

    return true;
}

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
            "show     Show bus definitions of modules."),
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
          QCoreApplication::translate("main", "The port mode (e.g., master, slave)."),
          "port mode"},
         {{"bl", "bus-library"},
          QCoreApplication::translate("main", "The bus library name or regex."),
          "bus library name or regex"},
         {"ai", QCoreApplication::translate("main", "Use AI to generate bus interfaces."), ""}});

    parser.addPositionalArgument(
        "port",
        QCoreApplication::translate("main", "The port name to connect the bus to."),
        "<port name>");

    parser.parse(appArguments);
    const QStringList cmdArguments = parser.positionalArguments();
    const QString    &libraryName  = parser.isSet("library") ? parser.value("library") : ".*";
    const QString    &moduleName   = parser.isSet("module") ? parser.value("module") : "";
    const QString    &busName      = parser.isSet("bus") ? parser.value("bus") : "";
    const QString    &busLibrary = parser.isSet("bus-library") ? parser.value("bus-library") : ".*";
    const QString    &portMode   = parser.isSet("mode") ? parser.value("mode") : "";
    const bool        useAI      = parser.isSet("ai");

    /* Validate required parameters */
    if (busName.isEmpty()) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: bus name is required."));
    }
    if (moduleName.isEmpty()) {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: module name is required."));
    }
    if (portMode.isEmpty()) {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: port mode is required."));
    }

    /* Get port name from positional arguments or port option */
    QString portName;
    if (!cmdArguments.isEmpty()) {
        portName = cmdArguments.first();
    } else {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: port name is required."));
    }

    /* Validate port name is not empty */
    if (portName.trimmed().isEmpty()) {
        return showErrorWithHelp(
            1, QCoreApplication::translate("main", "Error: port name cannot be empty."));
    }

    /* Setup project manager and project path  */
    QSocProjectManager projectManager(this);
    if (parser.isSet("directory")) {
        projectManager.setProjectPath(parser.value("directory"));
    }
    if (parser.isSet("project")) {
        projectManager.load(parser.value("project"));
    } else {
        const QStringList &projectNameList = projectManager.list(QRegularExpression(".*"));
        if (projectNameList.length() > 1) {
            return showErrorWithHelp(
                1,
                QCoreApplication::translate(
                    "main",
                    "Error: multiple projects found, please specify the project name.\n"
                    "Available projects are:\n%1\n")
                    .arg(projectNameList.join("\n")));
        }
        projectManager.loadFirst();
    }
    /* Check if module path is valid */
    if (!projectManager.isValidModulePath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid module directory: %1")
                .arg(projectManager.getModulePath()));
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

    /* Setup module manager */
    QSocBusManager    busManager(this, &projectManager);
    QSocModuleManager moduleManager(this, &projectManager, &busManager);
    /* Load modules */
    if (!moduleManager.load(libraryNameRegex)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: could not load library: %1")
                .arg(libraryName));
    }
    /* Load bus library */
    if (!busManager.load(busLibraryRegex)) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: could not load bus library: %1")
                .arg(busLibrary));
    }

    /* Add bus interface to module using AI or standard method */
    bool success = false;
    if (useAI) {
        /* Call the LLM-based method if AI option is set */
        success = moduleManager.addModuleBusWithLLM(moduleName, busName, portName, portMode);
    } else {
        /* Call the standard method if AI option is not set */
        success = moduleManager.addModuleBus(moduleName, busName, portName, portMode);
    }

    if (!success) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: could not add bus interface to module: %1")
                .arg(moduleName));
    }

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
        {{"b", "bus"},
         QCoreApplication::translate("main", "The bus name or regex."),
         "bus name or regex"},
    });
    /* TODO: Implement module bus remove functionality */
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
        {{"b", "bus"},
         QCoreApplication::translate("main", "The bus name or regex."),
         "bus name or regex"},
    });
    /* TODO: Implement module bus list functionality */
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
        {{"b", "bus"},
         QCoreApplication::translate("main", "The bus name or regex."),
         "bus name or regex"},
    });
    /* TODO: Implement module bus show functionality */
    return true;
}
