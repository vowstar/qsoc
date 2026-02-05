// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsoctool.h"
#include "agent/tool/qsoctoolbus.h"
#include "agent/tool/qsoctooldoc.h"
#include "agent/tool/qsoctoolfile.h"
#include "agent/tool/qsoctoolgenerate.h"
#include "agent/tool/qsoctoolmodule.h"
#include "agent/tool/qsoctoolproject.h"
#include "agent/tool/qsoctoolshell.h"
#include "cli/qagentreadline.h"
#include "cli/qterminalcapability.h"
#include "common/qstaticlog.h"

#include <QDir>
#include <QEventLoop>
#include <QTextStream>

bool QSocCliWorker::parseAgent(const QStringList &appArguments)
{
    /* Clear upstream positional arguments and setup subcommand */
    parser.clearPositionalArguments();
    parser.addOptions({
        {{"d", "directory"},
         QCoreApplication::translate("main", "The path to the project directory."),
         "project directory"},
        {{"p", "project"},
         QCoreApplication::translate("main", "The name of the project to use."),
         "project name"},
        {{"q", "query"},
         QCoreApplication::translate("main", "Single query mode (non-interactive)."),
         "query"},
        {"max-tokens",
         QCoreApplication::translate("main", "Maximum context tokens (default: 128000)."),
         "tokens"},
        {"temperature",
         QCoreApplication::translate("main", "LLM temperature (0.0-1.0, default: 0.2)."),
         "temperature"},
        {"stream",
         QCoreApplication::translate("main", "Enable streaming output (real-time display).")},
        {"no-stream", QCoreApplication::translate("main", "Disable streaming output.")},
    });

    parser.parse(appArguments);

    if (parser.isSet("help")) {
        return showHelp(0);
    }

    /* Set up project path if specified */
    if (parser.isSet("directory")) {
        projectManager->setProjectPath(parser.value("directory"));
    }

    /* Load project if specified */
    if (parser.isSet("project")) {
        QString projectName = parser.value("project");
        if (!projectManager->load(projectName)) {
            return showError(
                1,
                QCoreApplication::translate("main", "Error: failed to load project %1.")
                    .arg(projectName));
        }
    } else {
        /* Try to load first available project */
        projectManager->loadFirst();
    }

    /* Create agent configuration */
    QSocAgentConfig config;
    config.verbose = QStaticLog::getLevel() >= QStaticLog::Level::Debug;

    /* Load config from QSocConfig */
    if (socConfig) {
        QString tempStr = socConfig->getValue("agent.temperature");
        if (!tempStr.isEmpty()) {
            config.temperature = tempStr.toDouble();
        }

        QString maxTokensStr = socConfig->getValue("agent.max_tokens");
        if (!maxTokensStr.isEmpty()) {
            config.maxContextTokens = maxTokensStr.toInt();
        }

        QString maxIterStr = socConfig->getValue("agent.max_iterations");
        if (!maxIterStr.isEmpty()) {
            config.maxIterations = maxIterStr.toInt();
        }

        QString systemPrompt = socConfig->getValue("agent.system_prompt");
        if (!systemPrompt.isEmpty()) {
            config.systemPrompt = systemPrompt;
        }
    }

    /* Command line overrides config file */
    if (parser.isSet("max-tokens")) {
        config.maxContextTokens = parser.value("max-tokens").toInt();
    }
    if (parser.isSet("temperature")) {
        config.temperature = parser.value("temperature").toDouble();
    }

    /* Determine streaming mode */
    bool streaming = parser.isSet("stream");
    if (parser.isSet("no-stream")) {
        streaming = false;
    }

    /* Create tool registry and register tools */
    auto *toolRegistry = new QSocToolRegistry(this);

    /* Project tools */
    auto *projectListTool   = new QSocToolProjectList(this, projectManager);
    auto *projectShowTool   = new QSocToolProjectShow(this, projectManager);
    auto *projectCreateTool = new QSocToolProjectCreate(this, projectManager);
    toolRegistry->registerTool(projectListTool);
    toolRegistry->registerTool(projectShowTool);
    toolRegistry->registerTool(projectCreateTool);

    /* Module tools */
    auto *moduleListTool   = new QSocToolModuleList(this, moduleManager);
    auto *moduleShowTool   = new QSocToolModuleShow(this, moduleManager);
    auto *moduleImportTool = new QSocToolModuleImport(this, moduleManager);
    auto *moduleBusAddTool = new QSocToolModuleBusAdd(this, moduleManager);
    toolRegistry->registerTool(moduleListTool);
    toolRegistry->registerTool(moduleShowTool);
    toolRegistry->registerTool(moduleImportTool);
    toolRegistry->registerTool(moduleBusAddTool);

    /* Bus tools */
    auto *busListTool   = new QSocToolBusList(this, busManager);
    auto *busShowTool   = new QSocToolBusShow(this, busManager);
    auto *busImportTool = new QSocToolBusImport(this, busManager);
    toolRegistry->registerTool(busListTool);
    toolRegistry->registerTool(busShowTool);
    toolRegistry->registerTool(busImportTool);

    /* Generate tools */
    auto *generateVerilogTool  = new QSocToolGenerateVerilog(this, generateManager);
    auto *generateTemplateTool = new QSocToolGenerateTemplate(this, generateManager);
    toolRegistry->registerTool(generateVerilogTool);
    toolRegistry->registerTool(generateTemplateTool);

    /* File tools */
    auto *fileReadTool  = new QSocToolFileRead(this, projectManager);
    auto *fileListTool  = new QSocToolFileList(this, projectManager);
    auto *fileWriteTool = new QSocToolFileWrite(this, projectManager);
    auto *fileEditTool  = new QSocToolFileEdit(this, projectManager);
    toolRegistry->registerTool(fileReadTool);
    toolRegistry->registerTool(fileListTool);
    toolRegistry->registerTool(fileWriteTool);
    toolRegistry->registerTool(fileEditTool);

    /* Shell tools */
    auto *shellBashTool = new QSocToolShellBash(this, projectManager);
    toolRegistry->registerTool(shellBashTool);

    /* Documentation tools */
    auto *docQueryTool = new QSocToolDocQuery(this);
    toolRegistry->registerTool(docQueryTool);

    /* Create agent */
    auto *agent = new QSocAgent(this, llmService, toolRegistry, config);

    /* Connect verbose output signal */
    connect(agent, &QSocAgent::verboseOutput, [](const QString &message) {
        QStaticLog::logD(Q_FUNC_INFO, message);
    });

    /* Connect tool signals for verbose output */
    connect(agent, &QSocAgent::toolCalled, [](const QString &toolName, const QString &arguments) {
        QStaticLog::logD(
            Q_FUNC_INFO, QString("Tool called: %1 with args: %2").arg(toolName, arguments));
    });

    connect(agent, &QSocAgent::toolResult, [](const QString &toolName, const QString &result) {
        QString truncated = result.length() > 200 ? result.left(200) + "..." : result;
        QStaticLog::logD(Q_FUNC_INFO, QString("Tool result: %1 -> %2").arg(toolName, truncated));
    });

    /* Check if single query mode */
    if (parser.isSet("query")) {
        QString     query = parser.value("query");
        QTextStream qout(stdout);

        if (streaming) {
            /* Streaming single query mode */
            QEventLoop loop;

            connect(agent, &QSocAgent::contentChunk, [&qout](const QString &chunk) {
                qout << chunk << Qt::flush;
            });

            connect(agent, &QSocAgent::runComplete, [&qout, &loop](const QString &) {
                qout << Qt::endl;
                loop.quit();
            });

            connect(agent, &QSocAgent::runError, [this, &loop](const QString &error) {
                showError(1, error);
                loop.quit();
            });

            agent->runStream(query);
            loop.exec();
        } else {
            QString result = agent->run(query);
            return showInfo(0, result);
        }

        return true;
    }

    /* Interactive mode */
    return runAgentLoop(agent, streaming);
}

bool QSocCliWorker::runAgentLoop(QSocAgent *agent, bool streaming)
{
    /* Detect terminal capabilities */
    QTerminalCapability termCap;

    if (termCap.useEnhancedMode()) {
        /* Enhanced mode with readline */
        auto *readline = new QAgentReadline(this);

        /* Setup history file in project directory */
        QString projectPath = projectManager->getProjectPath();
        if (!projectPath.isEmpty()) {
            QString historyDir  = QDir(projectPath).filePath(".qsoc");
            QString historyFile = QDir(historyDir).filePath("history");

            /* Create .qsoc directory if needed */
            QDir dir(historyDir);
            if (!dir.exists()) {
                dir.mkpath(".");
            }

            readline->setHistoryFile(historyFile);
        }

        /* Setup completion for common commands */
        readline->setCompletionCallback([](const QString &input, int &contextLen) -> QStringList {
            QStringList completions;
            QString     trimmed = input.trimmed().toLower();

            /* Complete built-in commands */
            QStringList commands = {"exit", "quit", "clear", "help"};
            for (const QString &cmd : commands) {
                if (cmd.startsWith(trimmed)) {
                    completions.append(cmd);
                }
            }

            contextLen = static_cast<int>(trimmed.length());
            return completions;
        });

        return runAgentLoopEnhanced(agent, readline, streaming);
    }

    /* Simple mode for pipes/non-TTY */
    return runAgentLoopSimple(agent, streaming);
}

bool QSocCliWorker::runAgentLoopSimple(QSocAgent *agent, bool streaming)
{
    QTextStream qin(stdin);
    QTextStream qout(stdout);

    /* Print welcome message only if stdout is TTY */
    QTerminalCapability termCap;
    if (termCap.isOutputInteractive()) {
        qout << "QSoC Agent - Interactive AI Assistant for SoC Design" << Qt::endl;
        qout << "Type 'exit' or 'quit' to exit, 'clear' to clear history" << Qt::endl;
        qout << "(Running in simple mode)" << Qt::endl;
        qout << Qt::endl;
    }

    /* Main loop */
    while (true) {
        if (termCap.isOutputInteractive()) {
            qout << "qsoc> " << Qt::flush;
        }

        QString input = qin.readLine();

        /* Check for EOF */
        if (input.isNull()) {
            if (termCap.isOutputInteractive()) {
                qout << Qt::endl << "Goodbye!" << Qt::endl;
            }
            break;
        }

        input = input.trimmed();

        /* Handle special commands */
        if (input.isEmpty()) {
            continue;
        }
        if (input.toLower() == "exit" || input.toLower() == "quit") {
            if (termCap.isOutputInteractive()) {
                qout << "Goodbye!" << Qt::endl;
            }
            break;
        }
        if (input.toLower() == "clear") {
            agent->clearHistory();
            if (termCap.isOutputInteractive()) {
                qout << "History cleared." << Qt::endl;
            }
            continue;
        }
        if (input.toLower() == "help") {
            qout << "Commands:" << Qt::endl;
            qout << "  exit, quit  - Exit the agent" << Qt::endl;
            qout << "  clear       - Clear conversation history" << Qt::endl;
            qout << "  help        - Show this help message" << Qt::endl;
            qout << Qt::endl;
            qout << "Or just type your question/request in natural language." << Qt::endl;
            continue;
        }

        /* Run agent */
        if (streaming) {
            QEventLoop loop;

            QObject::connect(
                agent,
                &QSocAgent::contentChunk,
                &loop,
                [&qout](const QString &chunk) { qout << chunk << Qt::flush; },
                Qt::UniqueConnection);

            QObject::connect(
                agent,
                &QSocAgent::runComplete,
                &loop,
                [&qout, &loop](const QString &) {
                    qout << Qt::endl << Qt::endl;
                    loop.quit();
                },
                Qt::UniqueConnection);

            QObject::connect(
                agent,
                &QSocAgent::runError,
                &loop,
                [&qout, &loop](const QString &error) {
                    qout << Qt::endl << "Error: " << error << Qt::endl << Qt::endl;
                    loop.quit();
                },
                Qt::UniqueConnection);

            agent->runStream(input);
            loop.exec();
        } else {
            QString result = agent->run(input);
            qout << Qt::endl << result << Qt::endl << Qt::endl;
        }
    }

    return true;
}

bool QSocCliWorker::runAgentLoopEnhanced(QSocAgent *agent, QAgentReadline *readline, bool streaming)
{
    QTextStream qout(stdout);

    /* Print welcome message */
    qout << "QSoC Agent - Interactive AI Assistant for SoC Design" << Qt::endl;
    qout << "Type 'exit' or 'quit' to exit, 'clear' to clear history" << Qt::endl;

    if (readline->terminalCapability().supportsColor()) {
        qout << "(Enhanced mode with readline support";
        if (streaming) {
            qout << ", streaming enabled";
        }
        qout << ")" << Qt::endl;
    }
    qout << Qt::endl;

    /* Main loop */
    while (true) {
        QString input = readline->readLine("qsoc> ");

        /* Check for EOF */
        if (readline->isEof()) {
            qout << Qt::endl << "Goodbye!" << Qt::endl;
            break;
        }

        input = input.trimmed();

        /* Handle special commands */
        if (input.isEmpty()) {
            continue;
        }
        if (input.toLower() == "exit" || input.toLower() == "quit") {
            qout << "Goodbye!" << Qt::endl;
            break;
        }
        if (input.toLower() == "clear") {
            agent->clearHistory();
            qout << "History cleared." << Qt::endl;
            continue;
        }
        if (input.toLower() == "help") {
            qout << "Commands:" << Qt::endl;
            qout << "  exit, quit  - Exit the agent" << Qt::endl;
            qout << "  clear       - Clear conversation history" << Qt::endl;
            qout << "  help        - Show this help message" << Qt::endl;
            qout << Qt::endl;
            qout << "Keyboard shortcuts:" << Qt::endl;
            qout << "  Up/Down     - Browse history" << Qt::endl;
            qout << "  Ctrl+R      - Search history" << Qt::endl;
            qout << "  Ctrl+A/E    - Move to start/end of line" << Qt::endl;
            qout << "  Ctrl+K      - Delete to end of line" << Qt::endl;
            qout << "  Ctrl+W      - Delete word" << Qt::endl;
            qout << "  Ctrl+L      - Clear screen" << Qt::endl;
            qout << Qt::endl;
            qout << "Or just type your question/request in natural language." << Qt::endl;
            continue;
        }

        /* Run agent */
        if (streaming) {
            QEventLoop loop;

            QObject::connect(
                agent,
                &QSocAgent::contentChunk,
                &loop,
                [&qout](const QString &chunk) { qout << chunk << Qt::flush; },
                Qt::UniqueConnection);

            QObject::connect(
                agent,
                &QSocAgent::runComplete,
                &loop,
                [&qout, &loop](const QString &) {
                    qout << Qt::endl << Qt::endl;
                    loop.quit();
                },
                Qt::UniqueConnection);

            QObject::connect(
                agent,
                &QSocAgent::runError,
                &loop,
                [&qout, &loop](const QString &error) {
                    qout << Qt::endl << "Error: " << error << Qt::endl << Qt::endl;
                    loop.quit();
                },
                Qt::UniqueConnection);

            agent->runStream(input);
            loop.exec();
        } else {
            QString result = agent->run(input);
            qout << Qt::endl << result << Qt::endl << Qt::endl;
        }
    }

    return true;
}
