#include "cli/qsoccliworker.h"

#include "common/qslangdriver.h"
#include "common/qsocprojectmanager.h"
#include "common/qsocsymbolmanager.h"
#include "common/qstaticlog.h"

bool QSocCliWorker::parseSymbol(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addPositionalArgument(
        "subcommand",
        QCoreApplication::translate(
            "main",
            "import   Import symbol from verilog.\n"
            "remove   Remove module from the symbol library.\n"
            "list     List symbols in all symbol libraries.\n"
            "show     Show the specified symbol details."),
        "symbol <subcommand> [subcommand options]");

    parser.parse(appArguments);
    const QStringList cmdArguments = parser.positionalArguments();
    if (cmdArguments.isEmpty()) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: missing subcommand."));
    }
    const QString &command       = cmdArguments.first();
    QStringList    nextArguments = appArguments;
    if (command == "import") {
        nextArguments.removeOne(command);
        if (!parseSymbolImport(nextArguments)) {
            return false;
        }
    } else if (command == "remove") {
        nextArguments.removeOne(command);
        if (!parseSymbolRemove(nextArguments)) {
            return false;
        }
    } else if (command == "list") {
        nextArguments.removeOne(command);
        if (!parseSymbolList(nextArguments)) {
            return false;
        }
    } else if (command == "show") {
        nextArguments.removeOne(command);
        if (!parseSymbolShow(nextArguments)) {
            return false;
        }
    } else {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: unknown subcommand: %1.").arg(command));
    }

    return true;
}

bool QSocCliWorker::parseSymbolImport(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"d", "directory"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project directory"},
        {{"p", "project"}, QCoreApplication::translate("main", "The project name."), "project name"},
        {{"s", "symbol"},
         QCoreApplication::translate("main", "The symbol file basename."),
         "symbol file basename"},
        {{"r", "regex"},
         QCoreApplication::translate("main", "The verilog module name (regex)."),
         "module name (regex)"},
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
    const QStringList  cmdArguments   = parser.positionalArguments();
    const QString     &symbolFileBase = parser.isSet("symbol") ? parser.value("symbol") : "";
    const QString     &moduleName     = parser.isSet("regex") ? parser.value("regex") : "";
    const QStringList &filePathList   = cmdArguments;

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
    if (!projectManager.isValidSymbolPath()) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: invalid symbol directory: %1")
                .arg(projectManager.getSymbolPath()));
    }
    /* Setup symbol manager */
    QSocSymbolManager symbolManager(this, &projectManager);
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
    if (!symbolManager
             .importFromFileList(symbolFileBase, moduleNameRegex, filelistPath, filePathList)) {
        return showError(1, QCoreApplication::translate("main", "Error: import failed."));
    }

    return true;
}

bool QSocCliWorker::parseSymbolRemove(const QStringList &appArguments)
{
    Q_UNUSED(appArguments);
    return true;
}

bool QSocCliWorker::parseSymbolList(const QStringList &appArguments)
{
    Q_UNUSED(appArguments);
    return true;
}

bool QSocCliWorker::parseSymbolShow(const QStringList &appArguments)
{
    Q_UNUSED(appArguments);
    return true;
}
