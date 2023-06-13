#include "qsoccliworker.h"

#include "qsocprojectmanager.h"
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
        } else if (command == "update") {
            nextArguments.removeOne(command);
            parseProjectUpdate(nextArguments);
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
    /* Check help flag */
    if (parser.isSet("help")) {
        parser.showHelp(0);
        return;
    }
    if (cmdArguments.isEmpty()) {
        qCritical() << "Error: missing project name.";
        parser.showHelp(1);
        return;
    }
    /* Pass projectName to projectManager */
    const QString     &projectName = cmdArguments.first();
    QSocProjectManager projectManager;
    if (parser.isSet("path")) {
        projectManager.setProjectPath(parser.value("path"));
    }
    if (parser.isSet("bus")) {
        projectManager.setBusPath(parser.value("bus"));
    }
    if (parser.isSet("sym")) {
        projectManager.setSymbolPath(parser.value("sym"));
    }
    if (parser.isSet("sch")) {
        projectManager.setSchematicPath(parser.value("sch"));
    }
    if (parser.isSet("output")) {
        projectManager.setOutputPath(parser.value("output"));
    }
    if (projectManager.save(projectName)) {
        qInfo() << "Project" << projectName << "created.";
    } else {
        qCritical() << "Error: failed to create project" << projectName;
    }
}

void QSocCliWorker::parseProjectUpdate(const QStringList &appArguments)
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
        QCoreApplication::translate("main", "The name of the project to be update."),
        "[<name>]");

    parser.parse(appArguments);
    const QStringList cmdArguments = parser.positionalArguments();
    /* Check help flag */
    if (parser.isSet("help")) {
        parser.showHelp(0);
        return;
    }
    if (cmdArguments.isEmpty()) {
        qCritical() << "Error: missing project name.";
        parser.showHelp(1);
        return;
    }
    /* Pass projectName to projectManager */
    const QString     &projectName = cmdArguments.first();
    QSocProjectManager projectManager;
    if (parser.isSet("path")) {
        projectManager.setProjectPath(parser.value("path"));
    }
    /* Load project by name from project path */
    if (projectManager.load(projectName)) {
        qInfo() << "Project" << projectName << "loaded.";
        /* Update project config with new paths */
        if (parser.isSet("bus")) {
            projectManager.setBusPath(parser.value("bus"));
        }
        if (parser.isSet("sym")) {
            projectManager.setSymbolPath(parser.value("sym"));
        }
        if (parser.isSet("sch")) {
            projectManager.setSchematicPath(parser.value("sch"));
        }
        if (parser.isSet("output")) {
            projectManager.setOutputPath(parser.value("output"));
        }
        /* Save project config */
        if (projectManager.save(projectName)) {
            qInfo() << "Project" << projectName << "saved.";
        } else {
            qCritical() << "Error: failed to save project" << projectName;
        }
    } else {
        qCritical() << "Error: failed to load project" << projectName;
    }
}
