// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCCLIWORKER_H
#define QSOCCLIWORKER_H

#include "common/qllmservice.h"
#include "common/qsocbusmanager.h"
#include "common/qsocconfig.h"
#include "common/qsocgeneratemanager.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QFileInfo>
#include <QObject>
#include <QStringList>

class QSocAgent;
class QAgentReadline;

/**
 * @brief The QSocCliWorker class.
 * @details This class is the main worker class for the qsoc application.
 *          It is responsible for parsing the command line arguments and
 *          executing the appropriate actions.
 */
class QSocCliWorker : public QObject
{
    Q_OBJECT
public:
    /**
     * @brief Constructor for QSocCliWorker.
     * @details This constructor will initialize the command line parser.
     * @param[in] parent parent object.
     */
    explicit QSocCliWorker(QObject *parent = nullptr);

    /**
     * @brief Destructor for QSocCliWorker.
     * @details This destructor will free the command line parser.
     */
    ~QSocCliWorker() override;

    /**
     * @brief Setup the command line parser.
     * @details This function will setup the command line parser.
     * @param[in] appArguments command line arguments for the application.
     * @param isGui indicates whether the application is running in GUI mode or
     *        not, default is false.
     *        - true indicates GUI mode.
     *        - false indicates CLI mode.
     */
    void setup(const QStringList &appArguments, bool isGui = false);

    /**
     * @brief Process the command line arguments.
     * @details This function will process the command line arguments.
     */
    void process();

public slots:
    /**
     * @brief Run the command line parser.
     * @details This function will run the command line parser.
     */
    void run();

private:
    /* Command line parser. */
    QCommandLineParser parser;

    /* Command line arguments. */
    QStringList cmdArguments;

    /* ExitCode of the application. */
    int exitCode;

    QSocProjectManager  *projectManager  = nullptr;
    QSocConfig          *socConfig       = nullptr;
    QLLMService         *llmService      = nullptr;
    QSocBusManager      *busManager      = nullptr;
    QSocModuleManager   *moduleManager   = nullptr;
    QSocGenerateManager *generateManager = nullptr;

    /**
     * @brief Parse the application command line arguments.
     * @details This function will parse the application command line arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseRoot(const QStringList &appArguments);

    /**
     * @brief Parse the project command line arguments.
     * @details This function will parse the project command line arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseProject(const QStringList &appArguments);

    /**
     * @brief Parse the project create command line arguments.
     * @details This function will parse the project create command line
     *          arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseProjectCreate(const QStringList &appArguments);

    /**
     * @brief Parse the project update command line arguments.
     * @details This function will parse the project update command line
     *          arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseProjectUpdate(const QStringList &appArguments);

    /**
     * @brief Parse the project remove command line arguments.
     * @details This function will parse the project remove command line
     *          arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseProjectRemove(const QStringList &appArguments);

    /**
     * @brief Parse the project list command line arguments.
     * @details This function will parse the project list command line
     *          arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseProjectList(const QStringList &appArguments);

    /**
     * @brief Parse the project show command line arguments.
     * @details This function will parse the project show command line
     *          arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseProjectShow(const QStringList &appArguments);

    /**
     * @brief Parse the module command line arguments.
     * @details This function will parse the module command line arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseModule(const QStringList &appArguments);

    /**
     * @brief Parse the module import command line arguments.
     * @details This function will parse the module import command line
     *          arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseModuleImport(const QStringList &appArguments);

    /**
     * @brief Parse the module remove command line arguments.
     * @details This function will parse the module remove command line
     *          arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseModuleRemove(const QStringList &appArguments);

    /**
     * @brief Parse the module list command line arguments.
     * @details This function will parse the module list command line arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseModuleList(const QStringList &appArguments);

    /**
     * @brief Parse the module show command line arguments.
     * @details This function will parse the module show command line arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseModuleShow(const QStringList &appArguments);

    /**
     * @brief Parse the module bus command line arguments.
     * @details This function will parse the module bus command line arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseModuleBus(const QStringList &appArguments);

    /**
     * @brief Parse the module bus add command line arguments.
     * @details This function will parse the module bus add command line arguments
     *          to add bus interfaces to specified module.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseModuleBusAdd(const QStringList &appArguments);

    /**
     * @brief Parse the module bus remove command line arguments.
     * @details This function will parse the module bus remove command line arguments
     *          to remove bus interfaces from specified module.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseModuleBusRemove(const QStringList &appArguments);

    /**
     * @brief Parse the module bus list command line arguments.
     * @details This function will parse the module bus list command line arguments
     *          to list all bus interfaces of specified module.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseModuleBusList(const QStringList &appArguments);

    /**
     * @brief Parse the module bus show command line arguments.
     * @details This function will parse the module bus show command line arguments
     *          to show detailed information of specified module's bus interfaces.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseModuleBusShow(const QStringList &appArguments);

    /**
     * @brief Parse the module bus explain command line arguments.
     * @details This function will parse the module bus explain command line arguments
     *          to explain the bus interfaces of specified module.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseModuleBusExplain(const QStringList &appArguments);

    /**
     * @brief Parse the bus command line arguments.
     * @details This function will parse the bus command line arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseBus(const QStringList &appArguments);

    /**
     * @brief Parse the bus import command line arguments.
     * @details This function will parse the bus import command line
     *          arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseBusImport(const QStringList &appArguments);

    /**
     * @brief Parse the bus remove command line arguments.
     * @details This function will parse the bus remove command line
     *          arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseBusRemove(const QStringList &appArguments);

    /**
     * @brief Parse the bus list command line arguments.
     * @details This function will parse the bus list command line arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseBusList(const QStringList &appArguments);

    /**
     * @brief Parse the bus show command line arguments.
     * @details This function will parse the bus show command line arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseBusShow(const QStringList &appArguments);

    /**
     * @brief Parse the generate commandline arguments.
     * @details This function will parse the generate command line arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseGenerate(const QStringList &appArguments);

    /**
     * @brief Parse the generate verilog command line arguments.
     * @details This function will parse the generate verilog command line arguments.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseGenerateVerilog(const QStringList &appArguments);

    /**
     * @brief Process multiple netlist files by merging them.
     * @details This function will load multiple netlist files, merge them in order,
     *          and then process the merged result to generate a single Verilog file.
     * @param filePathList List of netlist file paths to merge.
     * @retval true Process successfully.
     * @retval false Process failed.
     */
    bool processMergedNetlists(const QStringList &filePathList);

    /**
     * @brief Process netlist files individually.
     * @details This function will process each netlist file separately,
     *          generating one Verilog file per netlist file.
     * @param filePathList List of netlist file paths to process.
     * @retval true Process successfully.
     * @retval false Process failed.
     */
    bool processIndividualNetlists(const QStringList &filePathList);

    /**
     * @brief Parse the generate template command line arguments.
     * @details This function will parse the generate template command line arguments
     *          to render Jinja2 templates with CSV, YAML, and JSON data files.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseGenerateTemplate(const QStringList &appArguments);

    /**
     * @brief Parse the generate stub command line arguments.
     * @details This function will parse the generate stub command line arguments
     *          to generate Verilog and Lib stub files for selected modules.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseGenerateStub(const QStringList &appArguments);

    /**
     * @brief Parse the agent command line arguments.
     * @details This function will parse the agent command line arguments
     *          to run the AI agent mode for interactive SoC design.
     * @param appArguments command line arguments.
     * @retval true Parse successfully.
     * @retval false Parse failed.
     */
    bool parseAgent(const QStringList &appArguments);

    /**
     * @brief Run the agent interactive loop.
     * @details This function runs the interactive REPL loop for the agent.
     *          Uses enhanced readline if terminal supports it.
     * @param agent The agent to run.
     * @param streaming Whether to use streaming mode.
     * @retval true Run successfully.
     * @retval false Run failed.
     */
    bool runAgentLoop(QSocAgent *agent, bool streaming = false);

    /**
     * @brief Run the agent loop in simple mode (for pipes/non-TTY).
     * @details This function runs a simple input loop without readline.
     * @param agent The agent to run.
     * @param streaming Whether to use streaming mode.
     * @retval true Run successfully.
     * @retval false Run failed.
     */
    bool runAgentLoopSimple(QSocAgent *agent, bool streaming = false);

    /**
     * @brief Run the agent loop in enhanced mode (with readline).
     * @details This function runs with full readline support.
     * @param agent The agent to run.
     * @param readline The readline instance.
     * @param streaming Whether to use streaming mode.
     * @retval true Run successfully.
     * @retval false Run failed.
     */
    bool runAgentLoopEnhanced(QSocAgent *agent, QAgentReadline *readline, bool streaming = false);

    /**
     * @brief Show application name and version and emit exit with exitCode.
     * @details This function will show application name and version and emit
     *          exit with exitCode.
     * @param exitCode exit code to emit.
     * @return bool always true.
     */
    bool showVersion(int exitCode);

    /**
     * @brief Show help message and emit exit with exitCode.
     * @details This function will show help message and emit exit with
     *          exitCode.
     * @param exitCode The exit code to emit.
     * @return bool always true.
     */
    bool showHelp(int exitCode);

    /**
     * @brief Show error with help message and emit exit with exitCode.
     * @details This function will show error message and help text, and then
     *          emit exit with exitCode.
     * @param exitCode The exit code to emit.
     * @param message The error message to show.
     * @return bool always false.
     */
    bool showErrorWithHelp(int exitCode, const QString &message);

    /**
     * @brief Show error message and emit exit with exitCode.
     * @details This function will show error message and emit exit with
     *          exitCode.
     * @param exitCode The exit code to emit.
     * @param message The error message to show.
     * @return bool always false.
     */
    bool showError(int exitCode, const QString &message);

    /**
     * @brief Show info message and emit exit with exitCode.
     * @details This function will show info message and emit exit with
     *          exitCode.
     * @param exitCode The exit code to emit.
     * @param message The info message to show.
     * @return bool always true.
     */
    bool showInfo(int exitCode, const QString &message);

    /**
     * @brief Show help message or error message and emit exit.
     * @details This function will show error message if no help flag is set,
     *          or show help message if help flag is set. It exit with exitCode
     *          when it is error, otherwise it exit with 0.
     * @param exitCode The exit code to emit.
     * @param message The error message to show if no help flag is set.
     * @retval true It is a help message.
     * @retval false It is an error message.
     */
    bool showHelpOrError(int exitCode, const QString &message);

signals:
    /**
     * @brief Exit the application.
     * @details This signal will be emitted when exit the application.
     * @param returnCode The return code of the application.
     */
    void exit(int returnCode = 0);

    /**
     * @brief Quit the application.
     * @details This signal will be emitted when quit the application.
     */
    void quit();
};

#endif // QSOCCLIWORKER_H
