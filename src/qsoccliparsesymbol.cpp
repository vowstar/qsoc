#include "qsoccliworker.h"

#include "qslangdriver.h"
#include "qsocprojectmanager.h"
#include "qsocsymbolmanager.h"
#include "qstaticlog.h"

void QSocCliWorker::parseSymbol(const QStringList &appArguments)
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
        if (!parser.isSet("help")) {
            qCritical() << "Error: missing subcommand.";
            parser.showHelp(1);
        } else {
            parser.showHelp(0);
        }
    } else {
        const QString &command       = cmdArguments.first();
        QStringList    nextArguments = appArguments;
        if (command == "import") {
            nextArguments.removeOne(command);
            parseSymbolImport(nextArguments);
        } else if (command == "update") {
            nextArguments.removeOne(command);
            parseSymbolUpdate(nextArguments);
        } else if (command == "remove") {
            nextArguments.removeOne(command);
            parseSymbolRemove(nextArguments);
        } else {
            if (!parser.isSet("help")) {
                qCritical() << "Error: unknown subcommand." << command;
                parser.showHelp(1);
            } else {
                parser.showHelp(0);
            }
        }
    }
}

void QSocCliWorker::parseSymbolImport(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"p", "path"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project path"},
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
        if (!parser.isSet("help")) {
            qCritical() << "Error: missing symbol name.";
            parser.showHelp(1);
        } else {
            parser.showHelp(0);
        }
    } else {
        const QString &symbolName   = cmdArguments.first();
        QStringList    filePathList = cmdArguments;
        filePathList.removeFirst();
        if (filePathList.isEmpty() && !parser.isSet("filelist")) {
            if (!parser.isSet("help")) {
                qCritical() << "Error: missing verilog files.";
                parser.showHelp(1);
            } else {
                parser.showHelp(0);
            }
        } else {
            /* Setup project manager and project path  */
            QSocProjectManager projectManager(this);
            if (parser.isSet("path")) {
                projectManager.setProjectPath(parser.value("path"));
            }
            if (!projectManager.isValid()) {
                qCritical() << "Error: invalid project path.";
                parser.showHelp(1);
                return;
            }
            /* Setup symbol manager */
            QSocSymbolManager symbolManager(this, &projectManager);
            QString           filelistPath = "";
            if (parser.isSet("filelist")) {
                filelistPath = parser.value("filelist");
            }
            symbolManager
                .importFromFileList(QRegularExpression(symbolName), filelistPath, filePathList);
        }
    }
}

void QSocCliWorker::parseSymbolUpdate(const QStringList &appArguments) {}

void QSocCliWorker::parseSymbolRemove(const QStringList &appArguments) {}
