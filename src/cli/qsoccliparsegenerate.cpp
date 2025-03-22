#include "cli/qsoccliworker.h"

#include "common/qslangdriver.h"
#include "common/qsocbusmanager.h"
#include "common/qsocgeneratemanager.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"
#include "common/qstaticlog.h"

bool QSocCliWorker::parseGenerate(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addPositionalArgument(
        "subcommand",
        QCoreApplication::translate("main", "verilog    Generate Verilog code from netlist file."),
        "generate <subcommand> [subcommand options]");

    parser.parse(appArguments);
    const QStringList cmdArguments = parser.positionalArguments();
    if (cmdArguments.isEmpty()) {
        return showHelpOrError(1, QCoreApplication::translate("main", "Error: missing subcommand."));
    }
    const QString &command       = cmdArguments.first();
    QStringList    nextArguments = appArguments;
    if (command == "verilog") {
        nextArguments.removeOne(command);
        if (!parseGenerateVerilog(nextArguments)) {
            return false;
        }
    } else {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: unknown subcommand: %1.").arg(command));
    }

    return true;
}

bool QSocCliWorker::parseGenerateVerilog(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"d", "directory"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project directory"},
        {{"p", "project"}, QCoreApplication::translate("main", "The project name."), "project name"},
    });

    parser.addPositionalArgument(
        "files",
        QCoreApplication::translate("main", "The netlist files to be processed."),
        "[<netlist files>]");

    parser.parse(appArguments);

    if (parser.isSet("help")) {
        return showHelp(0);
    }

    const QStringList  cmdArguments = parser.positionalArguments();
    const QStringList &filePathList = cmdArguments;
    if (filePathList.isEmpty()) {
        return showHelpOrError(
            1, QCoreApplication::translate("main", "Error: missing netlist files."));
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

    /* Check if output path is valid */
    if (!projectManager.isValidOutputPath()) {
        return showErrorWithHelp(
            1,
            QCoreApplication::translate("main", "Error: invalid output directory: %1")
                .arg(projectManager.getOutputPath()));
    }

    /* Setup generate manager */
    QSocConfig          socConfig(this, &projectManager);
    QLLMService         llmService(this, &socConfig);
    QSocBusManager      busManager(this, &projectManager);
    QSocModuleManager   moduleManager(this, &projectManager, &busManager, &llmService);
    QSoCGenerateManager generateManager(this, &projectManager, &moduleManager, &busManager);

    /* Load modules */
    if (!moduleManager.load(QRegularExpression(".*"))) {
        return showErrorWithHelp(
            1, QCoreApplication::translate("main", "Error: could not load library"));
    }

    /* Load buses */
    if (!busManager.load(QRegularExpression(".*"))) {
        return showErrorWithHelp(
            1, QCoreApplication::translate("main", "Error: could not load buses"));
    }

    /* Generate Verilog code for each netlist file  */
    for (const QString &netlistFilePath : filePathList) {
        /* Load the netlist file */
        if (!generateManager.loadNetlist(netlistFilePath)) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: failed to load netlist file: %1")
                    .arg(netlistFilePath));
        }

        /* Process the netlist */
        if (!generateManager.processNetlist()) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: failed to process netlist file: %1")
                    .arg(netlistFilePath));
        }

        /* Generate Verilog code */
        QFileInfo fileInfo(netlistFilePath);
        QString   outputFileName = fileInfo.baseName();
        if (!generateManager.generateVerilog(outputFileName)) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: failed to generate Verilog code for: %1")
                    .arg(outputFileName));
        }

        showInfo(
            0,
            QCoreApplication::translate("main", "Successfully generated Verilog code: %1")
                .arg(QDir(projectManager.getOutputPath()).filePath(outputFileName + ".v")));
    }

    return true;
}
