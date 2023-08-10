#include "cli/qsoccliworker.h"

#include "common/qsocprojectmanager.h"
#include "common/qstaticlog.h"

bool QSocCliWorker::parseProject(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addPositionalArgument(
        "subcommand",
        QCoreApplication::translate(
            "main",
            "create   Create project.\n"
            "update   Update project."),
        "project <subcommand> [subcommand options]");

    parser.parse(appArguments);
    const QStringList cmdArguments = parser.positionalArguments();
    if (cmdArguments.isEmpty()) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: missing subcommand."));
    }
    const QString &command       = cmdArguments.first();
    QStringList    nextArguments = appArguments;
    if (command == "create") {
        nextArguments.removeOne(command);
        if (!parseProjectCreate(nextArguments)) {
            return false;
        }
    } else if (command == "update") {
        nextArguments.removeOne(command);
        if (!parseProjectUpdate(nextArguments)) {
            return false;
        }
    } else {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: unknown subcommand: %1.").arg(command));
    }

    return true;
}

bool QSocCliWorker::parseProjectCreate(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"d", "directory"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project directory"},
        {{"b", "bus"},
         QCoreApplication::translate("main", "The path to the bus directory."),
         "bus directory"},
        {{"m", "symbol"},
         QCoreApplication::translate("main", "The path to the symbol directory."),
         "symbol directory"},
        {{"s", "schematic"},
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
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: missing project name."));
    }

    /* Pass projectName to projectManager */
    const QString     &projectName = cmdArguments.first();
    QSocProjectManager projectManager(this);
    if (parser.isSet("directory")) {
        projectManager.setProjectPath(parser.value("directory"));
    }
    if (parser.isSet("bus")) {
        projectManager.setBusPath(parser.value("bus"));
    }
    if (parser.isSet("symbol")) {
        projectManager.setSymbolPath(parser.value("symbol"));
    }
    if (parser.isSet("schematic")) {
        projectManager.setSchematicPath(parser.value("schematic"));
    }
    if (parser.isSet("output")) {
        projectManager.setOutputPath(parser.value("output"));
    }
    if (!projectManager.save(projectName)) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: failed to create project %1.")
                .arg(projectName));
    }

    return showInfo(0, QCoreApplication::translate("main", "Project %1 created.").arg(projectName));
}

bool QSocCliWorker::parseProjectUpdate(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"d", "directory"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project directory"},
        {{"b", "bus"},
         QCoreApplication::translate("main", "The path to the bus directory."),
         "bus directory"},
        {{"m", "symbol"},
         QCoreApplication::translate("main", "The path to the symbol directory."),
         "symbol directory"},
        {{"s", "schematic"},
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

    if (cmdArguments.isEmpty()) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: missing project name."));
    }
    /* Pass projectName to projectManager */
    const QString     &projectName = cmdArguments.first();
    QSocProjectManager projectManager(this);
    if (parser.isSet("directory")) {
        projectManager.setProjectPath(parser.value("directory"));
    }
    /* Load project by name from project path */
    if (!projectManager.load(projectName)) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: failed to load project %1.")
                .arg(projectName));
    }
    /* Update project config with new paths */
    if (parser.isSet("bus")) {
        projectManager.setBusPath(parser.value("bus"));
    }
    if (parser.isSet("symbol")) {
        projectManager.setSymbolPath(parser.value("symbol"));
    }
    if (parser.isSet("schematic")) {
        projectManager.setSchematicPath(parser.value("schematic"));
    }
    if (parser.isSet("output")) {
        projectManager.setOutputPath(parser.value("output"));
    }
    /* Save project config */
    if (!projectManager.save(projectName)) {
        return showError(
            1,
            QCoreApplication::translate("main", "Error: failed to update project %1.")
                .arg(projectName));
    }
    return showInfo(0, QCoreApplication::translate("main", "Project %1 updated.").arg(projectName));
}
