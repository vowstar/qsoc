#include "cli/qsoccliworker.h"

#include "common/qslangdriver.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"
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
            "remove   Remove selected modules from specified libraries.\n"
            "list     List all modules within designated libraries.\n"
            "show     Show detailed information on a chosen module."),
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
         QCoreApplication::translate("main", "The module library name."),
         "module library name"},
        {{"r", "regex"},
         QCoreApplication::translate("main", "The Verilog module name or regex."),
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
    const QString     &moduleName   = parser.isSet("regex") ? parser.value("regex") : "";
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
    if (!projectManager.isValidModulePath()) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: invalid module directory: %1")
                .arg(projectManager.getModulePath()));
    }
    /* Setup module manager */
    QSocModuleManager moduleManager(this, &projectManager);
    QString           filelistPath = "";
    if (parser.isSet("filelist")) {
        filelistPath = parser.value("filelist");
    }
    const QRegularExpression moduleNameRegex(moduleName);
    if (!moduleNameRegex.isValid()) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: invalid regular expression of module name."));
    }
    if (!moduleManager.importFromFileList(libraryName, moduleNameRegex, filelistPath, filePathList)) {
        return showError(1, QCoreApplication::translate("main", "Error: import failed."));
    }

    return true;
}

bool QSocCliWorker::parseModuleRemove(const QStringList &appArguments)
{
    Q_UNUSED(appArguments);
    return true;
}

bool QSocCliWorker::parseModuleList(const QStringList &appArguments)
{
    Q_UNUSED(appArguments);
    return true;
}

bool QSocCliWorker::parseModuleShow(const QStringList &appArguments)
{
    Q_UNUSED(appArguments);
    return true;
}
