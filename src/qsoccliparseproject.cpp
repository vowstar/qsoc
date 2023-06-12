#include "qsoccliworker.h"

#include "qstaticlog.h"

void QSocCliWorker::parseProject(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addPositionalArgument(
        "subcommand",
        QCoreApplication::translate(
            "main",
            "create   Create project.\n"
            "update   Update project.\n"
            "remove   Remove project."),
        "project <subcommand> [subcommand options]");

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
        if (command == "create") {
            nextArguments.removeOne(command);
            parseProjectCreate(nextArguments);
        } else if (command == "config") {
            nextArguments.removeOne(command);
            parseProjectUpdate(nextArguments);
        } else if (command == "remove") {
            nextArguments.removeOne(command);
            parseProjectRemove(nextArguments);
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

void QSocCliWorker::parseProjectCreate(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"p", "path"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project path"},
        {{"b", "bus"},
         QCoreApplication::translate("main", "The path to the bus directory."),
         "bus directory"},
        {{"m", "sym"},
         QCoreApplication::translate("main", "The path to the symbol directory."),
         "symbol directory"},
        {{"s", "sch"},
         QCoreApplication::translate("main", "The path to the schematic directory."),
         "schematic directory"},
        {{"o", "output"},
         QCoreApplication::translate("main", "The path to the output file."),
         "output directory"},
    });
    parser.addPositionalArgument(
        "name",
        QCoreApplication::translate("main", "The name of the project to be create."),
        "[<name>]");

    parser.parse(appArguments);
    const QStringList cmdArguments = parser.positionalArguments();
    if (cmdArguments.isEmpty()) {
        if (!parser.isSet("help")) {
            qCritical() << "Error: missing project name.";
            parser.showHelp(1);
        } else {
            parser.showHelp(0);
        }
    } else {
        const QString &projectName = cmdArguments.first();
    }
}

void QSocCliWorker::parseProjectUpdate(const QStringList &appArguments) {}

void QSocCliWorker::parseProjectRemove(const QStringList &appArguments) {}
