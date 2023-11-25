#include "cli/qsoccliworker.h"

#include "common/qslangdriver.h"
#include "common/qsocbusmanager.h"
#include "common/qsocprojectmanager.h"
#include "common/qstaticdatasedes.h"
#include "common/qstaticlog.h"

bool QSocCliWorker::parseBus(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addPositionalArgument(
        "subcommand",
        QCoreApplication::translate(
            "main",
            "import   Import buses into bus libraries.\n"
            "remove   Remove buses from specified libraries.\n"
            "list     List all buses within designated libraries.\n"
            "show     Show detailed information on a chosen bus."),
        "bus <subcommand> [subcommand options]");

    parser.parse(appArguments);
    const QStringList cmdArguments = parser.positionalArguments();
    if (cmdArguments.isEmpty()) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: missing subcommand."));
    }
    const QString &command       = cmdArguments.first();
    QStringList    nextArguments = appArguments;
    if (command == "import") {
        nextArguments.removeOne(command);
        if (!parseBusImport(nextArguments)) {
            return false;
        }
    } else if (command == "remove") {
        nextArguments.removeOne(command);
        if (!parseBusRemove(nextArguments)) {
            return false;
        }
    } else if (command == "list") {
        nextArguments.removeOne(command);
        if (!parseBusList(nextArguments)) {
            return false;
        }
    } else if (command == "show") {
        nextArguments.removeOne(command);
        if (!parseBusShow(nextArguments)) {
            return false;
        }
    } else {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: unknown subcommand: %1.").arg(command));
    }

    return true;
}

bool QSocCliWorker::parseBusImport(const QStringList &appArguments)
{
    Q_UNUSED(appArguments);
    return true;
}

bool QSocCliWorker::parseBusRemove(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"d", "directory"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project directory"},
        {{"p", "project"}, QCoreApplication::translate("main", "The project name."), "project name"},
        {{"b", "base"},
         QCoreApplication::translate("main", "The library base name or regex."),
         "library base name or regex"},
        {{"r", "regex"},
         QCoreApplication::translate("main", "The bus name or regex."),
         "bus name or regex"},
    });
    parser.addPositionalArgument(
        "name",
        QCoreApplication::translate("main", "The bus name or regex list."),
        "[<bus name or regex list>]");

    parser.parse(appArguments);
    const QStringList cmdArguments = parser.positionalArguments();
    const QString    &libraryName  = parser.isSet("base") ? parser.value("base") : ".*";
    const QString    &busName      = parser.isSet("regex") ? parser.value("regex") : "";
    QStringList       busNameList  = cmdArguments;
    /* Append bus name from positional arguments */
    if (!busName.trimmed().isEmpty()) {
        busNameList.append(busName.trimmed());
    }
    /* Removing duplicates */
    busNameList.removeDuplicates();
    /* Removing empty strings and strings containing only whitespace */
    busNameList.erase(
        std::remove_if(
            busNameList.begin(),
            busNameList.end(),
            [](const QString &str) { return str.trimmed().isEmpty(); }),
        busNameList.end());
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
            return showError(
                1,
                QCoreApplication::translate(
                    "main",
                    "Error: multiple projects found, please specify the project name.\n"
                    "Available projects are:\n%1\n")
                    .arg(projectNameList.join("\n")));
        }
        projectManager.loadFirst();
    }
    /* Check if bus path is valid */
    if (!projectManager.isValidBusPath()) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: invalid bus directory: %1")
                .arg(projectManager.getBusPath()));
    }
    /* Check if library name is valid */
    const QRegularExpression libraryNameRegex(libraryName);
    if (!libraryNameRegex.isValid()) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of library name: %1")
                .arg(libraryName));
    }
    /* Check if all bus names in list is valid */
    bool    invalidBusNameFound = false;
    QString invalidBusName;
    /* Iterate through all bus names */
    for (const QString &busName : busNameList) {
        const QRegularExpression busNameRegex(busName);
        if (!busNameRegex.isValid()) {
            invalidBusNameFound = true;
            invalidBusName      = busName;
            break;
        }
    }
    /* Show error if invalid bus name is found */
    if (invalidBusNameFound) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of bus name: %1")
                .arg(invalidBusName));
    }
    /* Setup bus manager */
    QSocBusManager busManager(this, &projectManager);
    /* Load buses */
    if (!busManager.load(libraryNameRegex)) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: could not load library: %1")
                .arg(libraryName));
    }
    /* Remove buses */
    for (const QString &busName : busNameList) {
        const QRegularExpression busNameRegex(busName);
        if (!busManager.removeBus(busNameRegex)) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: could not remove bus: %1").arg(busName));
        }
    }

    return true;
}

bool QSocCliWorker::parseBusList(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"d", "directory"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project directory"},
        {{"p", "project"}, QCoreApplication::translate("main", "The project name."), "project name"},
        {{"b", "base"},
         QCoreApplication::translate("main", "The library base name or regex."),
         "library base name or regex"},
        {{"r", "regex"},
         QCoreApplication::translate("main", "The bus name or regex."),
         "bus name or regex"},
    });
    parser.addPositionalArgument(
        "name",
        QCoreApplication::translate("main", "The bus name or regex list."),
        "[<bus name or regex list>]");

    parser.parse(appArguments);
    const QStringList cmdArguments = parser.positionalArguments();
    const QString    &libraryName  = parser.isSet("base") ? parser.value("base") : ".*";
    const QString    &busName      = parser.isSet("regex") ? parser.value("regex") : "";
    QStringList busNameList = cmdArguments.length() > 0 ? cmdArguments : QStringList() << ".*";
    /* Append bus name from positional arguments */
    if (!busName.trimmed().isEmpty()) {
        busNameList.append(busName.trimmed());
    }
    /* Removing duplicates */
    busNameList.removeDuplicates();
    /* Removing empty strings and strings containing only whitespace */
    busNameList.erase(
        std::remove_if(
            busNameList.begin(),
            busNameList.end(),
            [](const QString &str) { return str.trimmed().isEmpty(); }),
        busNameList.end());
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
            return showError(
                1,
                QCoreApplication::translate(
                    "main",
                    "Error: multiple projects found, please specify the project name.\n"
                    "Available projects are:\n%1\n")
                    .arg(projectNameList.join("\n")));
        }
        projectManager.loadFirst();
    }
    /* Check if bus path is valid */
    if (!projectManager.isValidBusPath()) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: invalid bus directory: %1")
                .arg(projectManager.getBusPath()));
    }
    /* Check if library name is valid */
    const QRegularExpression libraryNameRegex(libraryName);
    if (!libraryNameRegex.isValid()) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of library name: %1")
                .arg(libraryName));
    }
    /* Check if all bus names in list is valid */
    bool    invalidBusNameFound = false;
    QString invalidBusName;
    /* Iterate through all bus names */
    for (const QString &busName : busNameList) {
        const QRegularExpression busNameRegex(busName);
        if (!busNameRegex.isValid()) {
            invalidBusNameFound = true;
            invalidBusName      = busName;
            break;
        }
    }
    /* Show error if invalid bus name is found */
    if (invalidBusNameFound) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of bus name: %1")
                .arg(invalidBusName));
    }
    /* Setup bus manager */
    QSocBusManager busManager(this, &projectManager);
    /* Load buses */
    if (!busManager.load(libraryNameRegex)) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: could not load library: %1")
                .arg(libraryName));
    }
    /* list buses */
    for (const QString &busName : busNameList) {
        const QRegularExpression busNameRegex(busName);
        const QStringList        result = busManager.listBus(busNameRegex);
        showInfo(0, result.join("\n"));
    }

    return true;
}

bool QSocCliWorker::parseBusShow(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"d", "directory"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project directory"},
        {{"p", "project"}, QCoreApplication::translate("main", "The project name."), "project name"},
        {{"b", "base"},
         QCoreApplication::translate("main", "The library base name or regex."),
         "library base name or regex"},
        {{"r", "regex"},
         QCoreApplication::translate("main", "The bus name or regex."),
         "bus name or regex"},
    });
    parser.addPositionalArgument(
        "name",
        QCoreApplication::translate("main", "The bus name or regex list."),
        "[<bus name or regex list>]");

    parser.parse(appArguments);
    const QStringList cmdArguments = parser.positionalArguments();
    const QString    &libraryName  = parser.isSet("base") ? parser.value("base") : ".*";
    const QString    &busName      = parser.isSet("regex") ? parser.value("regex") : "";
    QStringList busNameList = cmdArguments.length() > 0 ? cmdArguments : QStringList() << ".*";
    /* Append bus name from positional arguments */
    if (!busName.trimmed().isEmpty()) {
        busNameList.append(busName.trimmed());
    }
    /* Removing duplicates */
    busNameList.removeDuplicates();
    /* Removing empty strings and strings containing only whitespace */
    busNameList.erase(
        std::remove_if(
            busNameList.begin(),
            busNameList.end(),
            [](const QString &str) { return str.trimmed().isEmpty(); }),
        busNameList.end());
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
            return showError(
                1,
                QCoreApplication::translate(
                    "main",
                    "Error: multiple projects found, please specify the project name.\n"
                    "Available projects are:\n%1\n")
                    .arg(projectNameList.join("\n")));
        }
        projectManager.loadFirst();
    }
    /* Check if bus path is valid */
    if (!projectManager.isValidBusPath()) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: invalid bus directory: %1")
                .arg(projectManager.getBusPath()));
    }
    /* Check if library name is valid */
    const QRegularExpression libraryNameRegex(libraryName);
    if (!libraryNameRegex.isValid()) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of library name: %1")
                .arg(libraryName));
    }
    /* Check if all bus names in list is valid */
    bool    invalidBusNameFound = false;
    QString invalidBusName;
    /* Iterate through all bus names */
    for (const QString &busName : busNameList) {
        const QRegularExpression busNameRegex(busName);
        if (!busNameRegex.isValid()) {
            invalidBusNameFound = true;
            invalidBusName      = busName;
            break;
        }
    }
    /* Show error if invalid bus name is found */
    if (invalidBusNameFound) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of bus name: %1")
                .arg(invalidBusName));
    }
    /* Setup bus manager */
    QSocBusManager busManager(this, &projectManager);
    /* Load buses */
    if (!busManager.load(libraryNameRegex)) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: could not load library: %1")
                .arg(libraryName));
    }
    /* list buses */
    for (const QString &busName : busNameList) {
        const QRegularExpression busNameRegex(busName);
        /* Show details about the bus */
        showInfo(0, QStaticDataSedes::serializeYaml(busManager.getBusNode(busNameRegex)));
    }

    return true;
}
