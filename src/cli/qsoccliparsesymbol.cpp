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
            "update   Update symbol from verilog.\n"
            "remove   Remove symbol by name."),
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
    } else if (command == "update") {
        nextArguments.removeOne(command);
        if (!parseSymbolUpdate(nextArguments)) {
            return false;
        }
    } else if (command == "remove") {
        nextArguments.removeOne(command);
        if (!parseSymbolRemove(nextArguments)) {
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
        {{"f", "filelist"},
         QCoreApplication::translate(
             "main",
             "The path where the file list is located, including a list of "
             "verilog files in order."),
         "filelist"},
    });
    parser.addPositionalArgument(
        "name",
        QCoreApplication::translate("main", "The name of the symbol to be import."),
        "[<name>]");
    parser.addPositionalArgument(
        "files",
        QCoreApplication::translate("main", "The verilog files to be processed."),
        "[<verilog files>]");

    parser.parse(appArguments);
    const QStringList cmdArguments = parser.positionalArguments();

    if (cmdArguments.isEmpty()) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: missing symbol name."));
    }

    const QString &symbolName   = cmdArguments.first();
    QStringList    filePathList = cmdArguments;
    filePathList.removeFirst();
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
        projectManager.autoLoad();
    }
    if (!projectManager.isValid()) {
        return showError(1, QCoreApplication::translate("main", "Error: invalid project directory."));
    }
    /* Setup symbol manager */
    QSocSymbolManager symbolManager(this, &projectManager);
    QString           filelistPath = "";
    if (parser.isSet("filelist")) {
        filelistPath = parser.value("filelist");
    }
    symbolManager.importFromFileList(QRegularExpression(symbolName), filelistPath, filePathList);

    return true;
}

bool QSocCliWorker::parseSymbolUpdate(const QStringList &appArguments)
{
    Q_UNUSED(appArguments);
    return true;
}

bool QSocCliWorker::parseSymbolRemove(const QStringList &appArguments)
{
    Q_UNUSED(appArguments);
    return true;
}
