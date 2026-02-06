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
#include "agent/tool/qsoctoolmemory.h"
#include "agent/tool/qsoctoolmodule.h"
#include "agent/tool/qsoctoolpath.h"
#include "agent/tool/qsoctoolproject.h"
#include "agent/tool/qsoctoolshell.h"
#include "agent/tool/qsoctooltodo.h"
#include "cli/qagentreadline.h"
#include "cli/qagentstatusline.h"
#include "cli/qterminalcapability.h"
#include "common/qstaticlog.h"

#include <QDir>
#include <QEventLoop>
#include <QPair>
#include <QRegularExpression>
#include <QTextStream>

namespace {

/**
 * @brief Parse todo_list result into structured items
 * @param result The result string from todo_list tool
 * @return List of parsed TodoItem structures
 */
QList<QAgentStatusLine::TodoItem> parseTodoListResult(const QString &result)
{
    QList<QAgentStatusLine::TodoItem> items;

    /* Match pattern: [x] or [ ] followed by ID. Title (priority) */
    QRegularExpression regex(R"(\[([ x])\]\s*(\d+)\.\s*(.+?)\s*\((\w+)\))");

    const QStringList lines = result.split('\n');
    for (const QString &line : lines) {
        QRegularExpressionMatch match = regex.match(line);
        if (match.hasMatch()) {
            QAgentStatusLine::TodoItem item;
            item.status   = (match.captured(1) == "x") ? "done" : "pending";
            item.id       = match.captured(2).toInt();
            item.title    = match.captured(3).trimmed();
            item.priority = match.captured(4);
            items.append(item);
        }
    }

    return items;
}

/**
 * @brief Parse todo_add result into a single TodoItem
 * @param result The result string from todo_add tool
 *        Format: "Added todo #37: Title here (priority)"
 * @return TodoItem if parsed successfully, empty item if not
 */
QAgentStatusLine::TodoItem parseTodoAddResult(const QString &result)
{
    QAgentStatusLine::TodoItem item;
    item.id = -1; /* Invalid by default */

    /* Match: "Added todo #ID: Title (priority)" */
    QRegularExpression      regex(R"(Added todo #(\d+):\s*(.+?)\s*\((\w+)(?:\s+priority)?\))");
    QRegularExpressionMatch match = regex.match(result);

    if (match.hasMatch()) {
        item.id       = match.captured(1).toInt();
        item.title    = match.captured(2).trimmed();
        item.priority = match.captured(3);
        item.status   = "pending";
    }

    return item;
}

/**
 * @brief Parse todo_update result to extract ID and new status
 * @param result The result string from todo_update tool
 *        Format: "Updated todo #37 status to: done"
 * @return Pair of (todoId, newStatus), todoId=-1 if parse failed
 */
QPair<int, QString> parseTodoUpdateResult(const QString &result)
{
    /* Match: "Updated todo #ID status to: STATUS" */
    QRegularExpression      regex(R"(Updated todo #(\d+) status to:\s*(\w+))");
    QRegularExpressionMatch match = regex.match(result);

    if (match.hasMatch()) {
        return qMakePair(match.captured(1).toInt(), match.captured(2));
    }

    return qMakePair(-1, QString());
}

} /* namespace */

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
        {"no-stream",
         QCoreApplication::translate(
             "main", "Disable streaming output (streaming is enabled by default).")},
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

    /* Determine streaming mode (enabled by default) */
    bool streaming = true;

    /* Config file can override default */
    if (socConfig) {
        QString streamStr = socConfig->getValue("agent.stream");
        if (!streamStr.isEmpty()) {
            streaming = (streamStr.toLower() == "true" || streamStr == "1");
        }
    }

    /* Command line overrides config file */
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

    /* Memory tools */
    auto *memoryReadTool  = new QSocToolMemoryRead(this, projectManager);
    auto *memoryWriteTool = new QSocToolMemoryWrite(this, projectManager);
    toolRegistry->registerTool(memoryReadTool);
    toolRegistry->registerTool(memoryWriteTool);

    /* Todo tools */
    auto *todoListTool   = new QSocToolTodoList(this, projectManager);
    auto *todoAddTool    = new QSocToolTodoAdd(this, projectManager);
    auto *todoUpdateTool = new QSocToolTodoUpdate(this, projectManager);
    auto *todoDeleteTool = new QSocToolTodoDelete(this, projectManager);
    toolRegistry->registerTool(todoListTool);
    toolRegistry->registerTool(todoAddTool);
    toolRegistry->registerTool(todoUpdateTool);
    toolRegistry->registerTool(todoDeleteTool);

    /* Path context tool */
    auto *pathContext     = new QSocPathContext(this, projectManager);
    auto *pathContextTool = new QSocToolPathContext(this, pathContext);
    toolRegistry->registerTool(pathContextTool);

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

    /* Detect terminal capabilities */
    QTerminalCapability termCap;
    bool                isPipeMode = !termCap.isInteractive();

    /* Pipe mode: pre-read all input into queue */
    QStringList inputQueue;
    if (isPipeMode) {
        while (!qin.atEnd()) {
            QString line = qin.readLine();
            if (!line.isNull()) {
                inputQueue.append(line);
            }
        }
    }

    /* Print welcome message only if stdout is TTY */
    if (termCap.isOutputInteractive()) {
        qout << "QSoC Agent - Interactive AI Assistant for SoC Design" << Qt::endl;
        qout << "Type 'exit' or 'quit' to exit, 'clear' to clear history" << Qt::endl;
        qout << "(Running in simple mode)" << Qt::endl;
        qout << Qt::endl;
    }

    /* Create status line for visual feedback (only if output is interactive) */
    QAgentStatusLine statusLine(this);
    bool             useStatusLine = termCap.isOutputInteractive();

    /* Main loop */
    while (true) {
        QString input;

        if (isPipeMode) {
            /* Pipe mode: take from queue */
            if (inputQueue.isEmpty()) {
                break;
            }
            input = inputQueue.takeFirst();
        } else {
            /* TTY mode: blocking read */
            qout << "qsoc> " << Qt::flush;
            input = qin.readLine();

            /* Check for EOF */
            if (input.isNull()) {
                qout << Qt::endl << "Goodbye!" << Qt::endl;
                break;
            }
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
            QEventLoop                     loop;
            bool                           loopRunning = true;
            QList<QMetaObject::Connection> connections;

            /* Connect status line to agent signals (if interactive output) */
            if (useStatusLine) {
                connections.append(
                    QObject::connect(
                        agent,
                        &QSocAgent::toolCalled,
                        &statusLine,
                        [&statusLine](const QString &toolName, const QString &arguments) {
                            /* Extract detail from arguments for better UX */
                            QString detail;
                            try {
                                auto args = json::parse(arguments.toStdString());
                                /* Tool-specific detail extraction */
                                if (args.contains("command")) {
                                    detail = QString::fromStdString(
                                        args["command"].get<std::string>());
                                } else if (args.contains("title")) {
                                    /* todo_add */
                                    detail = "\""
                                             + QString::fromStdString(
                                                 args["title"].get<std::string>())
                                             + "\"";
                                } else if (args.contains("file_path")) {
                                    /* read_file, write_file, edit_file */
                                    detail = QString::fromStdString(
                                        args["file_path"].get<std::string>());
                                } else if (args.contains("path")) {
                                    detail = QString::fromStdString(args["path"].get<std::string>());
                                } else if (args.contains("name")) {
                                    detail = QString::fromStdString(args["name"].get<std::string>());
                                } else if (args.contains("regex")) {
                                    detail = QString::fromStdString(
                                        args["regex"].get<std::string>());
                                } else if (args.contains("id")) {
                                    /* todo_update, todo_delete - id only, title from result */
                                    detail = "#" + QString::number(args["id"].get<int>());
                                }
                            } catch (...) {
                                /* Ignore parse errors */
                            }
                            statusLine.toolCalled(toolName, detail);
                        }));

                connections.append(
                    QObject::connect(
                        agent,
                        &QSocAgent::toolResult,
                        &statusLine,
                        [&statusLine](const QString &toolName, const QString &result) {
                            statusLine.resetProgress();
                            statusLine.update(QString("%1 done, thinking").arg(toolName));

                            /* Parse and update todo list based on tool type */
                            if (toolName == "todo_list") {
                                auto items = parseTodoListResult(result);
                                statusLine.setTodoList(items);
                            } else if (toolName == "todo_add") {
                                auto item = parseTodoAddResult(result);
                                if (item.id >= 0) {
                                    statusLine.addTodoItem(item);
                                }
                            } else if (toolName == "todo_update") {
                                auto [todoId, newStatus] = parseTodoUpdateResult(result);
                                if (todoId >= 0) {
                                    statusLine.updateTodoStatus(todoId, newStatus);
                                }
                            }
                            /* Show todo result for all todo tools */
                            if (toolName.startsWith("todo_")) {
                                statusLine.updateTodoDisplay(result);
                            }
                        }));

                /* Track active todo via toolCalled for todo_update */
                connections.append(
                    QObject::connect(
                        agent,
                        &QSocAgent::toolCalled,
                        &statusLine,
                        [&statusLine](const QString &toolName, const QString &arguments) {
                            if (toolName == "todo_update") {
                                try {
                                    auto args = json::parse(arguments.toStdString());
                                    if (args.contains("status")) {
                                        QString status = QString::fromStdString(
                                            args["status"].get<std::string>());
                                        if (status == "in_progress" && args.contains("id")) {
                                            int todoId = args["id"].get<int>();
                                            statusLine.setActiveTodo(todoId);
                                        } else if (status == "done" || status == "pending") {
                                            statusLine.clearActiveTodo();
                                        }
                                    }
                                } catch (...) {
                                    /* Ignore parse errors */
                                }
                            }
                        }));
            }

            connections.append(
                QObject::connect(
                    agent,
                    &QSocAgent::contentChunk,
                    &loop,
                    [&statusLine, useStatusLine](const QString &chunk) {
                        if (useStatusLine) {
                            statusLine.printContent(chunk);
                        } else {
                            QTextStream(stdout) << chunk << Qt::flush;
                        }
                    }));

            connections.append(
                QObject::connect(
                    agent,
                    &QSocAgent::runComplete,
                    &loop,
                    [&qout, &loop, &statusLine, useStatusLine, &loopRunning](const QString &) {
                        if (useStatusLine) {
                            statusLine.stop();
                        }
                        qout << Qt::endl << Qt::endl;
                        if (loopRunning) {
                            loop.quit();
                        }
                    }));

            connections.append(
                QObject::connect(
                    agent,
                    &QSocAgent::runError,
                    &loop,
                    [&qout, &loop, &statusLine, useStatusLine, &loopRunning](const QString &error) {
                        if (useStatusLine) {
                            statusLine.stop();
                        }
                        qout << Qt::endl << "Error: " << error << Qt::endl << Qt::endl;
                        if (loopRunning) {
                            loop.quit();
                        }
                    }));

            /* Connect heartbeat - updates status and token display */
            connections.append(
                QObject::connect(
                    agent, &QSocAgent::heartbeat, &loop, [&statusLine, useStatusLine](int, int) {
                        if (useStatusLine) {
                            statusLine.update("Working");
                        }
                    }));

            /* Connect token usage update */
            connections.append(
                QObject::connect(
                    agent,
                    &QSocAgent::tokenUsage,
                    &loop,
                    [&statusLine, useStatusLine](qint64 input, qint64 output) {
                        if (useStatusLine) {
                            statusLine.updateTokens(input, output);
                        }
                    }));

            /* Connect stuck detection for warning */
            connections.append(
                QObject::connect(
                    agent,
                    &QSocAgent::stuckDetected,
                    &loop,
                    [&statusLine, useStatusLine](int, int silentSeconds) {
                        if (useStatusLine) {
                            statusLine.update(
                                QString("Working [%1s no progress]").arg(silentSeconds));
                        }
                    }));

            /* Connect retrying signal for user feedback */
            connections.append(
                QObject::connect(
                    agent,
                    &QSocAgent::retrying,
                    &loop,
                    [&statusLine, useStatusLine](int attempt, int maxAttempts, const QString &) {
                        if (useStatusLine) {
                            statusLine.update(
                                QString("Retrying (%1/%2)").arg(attempt).arg(maxAttempts));
                        }
                    }));

            /* Start status line and agent */
            if (useStatusLine) {
                statusLine.start("Thinking");
            }
            agent->runStream(input);
            loop.exec();
            loopRunning = false;

            /* Disconnect all signals to avoid stale connections */
            for (const auto &conn : connections) {
                QObject::disconnect(conn);
            }
        } else {
            /* Non-streaming mode: use async API but collect result without chunk output */
            QEventLoop                     loop;
            bool                           loopRunning = true;
            QString                        finalResult;
            QList<QMetaObject::Connection> connections;

            /* Connect status line to agent signals (if interactive output) */
            if (useStatusLine) {
                connections.append(
                    QObject::connect(
                        agent,
                        &QSocAgent::toolCalled,
                        &statusLine,
                        [&statusLine](const QString &toolName, const QString &arguments) {
                            /* Extract detail from arguments for better UX */
                            QString detail;
                            try {
                                auto args = json::parse(arguments.toStdString());
                                /* Tool-specific detail extraction */
                                if (args.contains("command")) {
                                    detail = QString::fromStdString(
                                        args["command"].get<std::string>());
                                } else if (args.contains("title")) {
                                    /* todo_add */
                                    detail = "\""
                                             + QString::fromStdString(
                                                 args["title"].get<std::string>())
                                             + "\"";
                                } else if (args.contains("file_path")) {
                                    /* read_file, write_file, edit_file */
                                    detail = QString::fromStdString(
                                        args["file_path"].get<std::string>());
                                } else if (args.contains("path")) {
                                    detail = QString::fromStdString(args["path"].get<std::string>());
                                } else if (args.contains("name")) {
                                    detail = QString::fromStdString(args["name"].get<std::string>());
                                } else if (args.contains("regex")) {
                                    detail = QString::fromStdString(
                                        args["regex"].get<std::string>());
                                } else if (args.contains("id")) {
                                    /* todo_update, todo_delete - id only, title from result */
                                    detail = "#" + QString::number(args["id"].get<int>());
                                }
                            } catch (...) {
                                /* Ignore parse errors */
                            }
                            statusLine.toolCalled(toolName, detail);
                        }));

                connections.append(
                    QObject::connect(
                        agent,
                        &QSocAgent::toolResult,
                        &statusLine,
                        [&statusLine](const QString &toolName, const QString &result) {
                            statusLine.resetProgress();
                            statusLine.update(QString("%1 done, thinking").arg(toolName));

                            /* Parse and update todo list based on tool type */
                            if (toolName == "todo_list") {
                                auto items = parseTodoListResult(result);
                                statusLine.setTodoList(items);
                            } else if (toolName == "todo_add") {
                                auto item = parseTodoAddResult(result);
                                if (item.id >= 0) {
                                    statusLine.addTodoItem(item);
                                }
                            } else if (toolName == "todo_update") {
                                auto [todoId, newStatus] = parseTodoUpdateResult(result);
                                if (todoId >= 0) {
                                    statusLine.updateTodoStatus(todoId, newStatus);
                                }
                            }
                            /* Show todo result for all todo tools */
                            if (toolName.startsWith("todo_")) {
                                statusLine.updateTodoDisplay(result);
                            }
                        }));

                /* Track active todo via toolCalled for todo_update */
                connections.append(
                    QObject::connect(
                        agent,
                        &QSocAgent::toolCalled,
                        &statusLine,
                        [&statusLine](const QString &toolName, const QString &arguments) {
                            if (toolName == "todo_update") {
                                try {
                                    auto args = json::parse(arguments.toStdString());
                                    if (args.contains("status")) {
                                        QString status = QString::fromStdString(
                                            args["status"].get<std::string>());
                                        if (status == "in_progress" && args.contains("id")) {
                                            int todoId = args["id"].get<int>();
                                            statusLine.setActiveTodo(todoId);
                                        } else if (status == "done" || status == "pending") {
                                            statusLine.clearActiveTodo();
                                        }
                                    }
                                } catch (...) {
                                    /* Ignore parse errors */
                                }
                            }
                        }));
            }

            /* Don't print chunks - just accumulate for final display */
            connections.append(
                QObject::connect(
                    agent, &QSocAgent::contentChunk, &loop, [&finalResult](const QString &chunk) {
                        finalResult += chunk;
                    }));

            connections.append(
                QObject::connect(
                    agent,
                    &QSocAgent::runComplete,
                    &loop,
                    [&loop, &statusLine, useStatusLine, &loopRunning](const QString &) {
                        if (useStatusLine) {
                            statusLine.stop();
                        }
                        if (loopRunning) {
                            loop.quit();
                        }
                    }));

            connections.append(
                QObject::connect(
                    agent,
                    &QSocAgent::runError,
                    &loop,
                    [&qout, &loop, &statusLine, useStatusLine, &loopRunning](const QString &error) {
                        if (useStatusLine) {
                            statusLine.stop();
                        }
                        qout << Qt::endl << "Error: " << error << Qt::endl << Qt::endl;
                        if (loopRunning) {
                            loop.quit();
                        }
                    }));

            /* Connect heartbeat - updates status and token display */
            connections.append(
                QObject::connect(
                    agent, &QSocAgent::heartbeat, &loop, [&statusLine, useStatusLine](int, int) {
                        if (useStatusLine) {
                            statusLine.update("Working");
                        }
                    }));

            /* Connect token usage update */
            connections.append(
                QObject::connect(
                    agent,
                    &QSocAgent::tokenUsage,
                    &loop,
                    [&statusLine, useStatusLine](qint64 input, qint64 output) {
                        if (useStatusLine) {
                            statusLine.updateTokens(input, output);
                        }
                    }));

            /* Connect stuck detection for warning */
            connections.append(
                QObject::connect(
                    agent,
                    &QSocAgent::stuckDetected,
                    &loop,
                    [&statusLine, useStatusLine](int, int silentSeconds) {
                        if (useStatusLine) {
                            statusLine.update(
                                QString("Working [%1s no progress]").arg(silentSeconds));
                        }
                    }));

            /* Connect retrying signal for user feedback */
            connections.append(
                QObject::connect(
                    agent,
                    &QSocAgent::retrying,
                    &loop,
                    [&statusLine, useStatusLine](int attempt, int maxAttempts, const QString &) {
                        if (useStatusLine) {
                            statusLine.update(
                                QString("Retrying (%1/%2)").arg(attempt).arg(maxAttempts));
                        }
                    }));

            /* Start status line and agent */
            if (useStatusLine) {
                statusLine.start("Thinking");
            } else {
                qout << "Thinking" << Qt::flush;
            }
            agent->runStream(input);
            loop.exec();
            loopRunning = false;

            if (!useStatusLine) {
                qout << "\r\033[K"; /* Clear the "Thinking..." line */
            }

            /* Display complete result at once */
            if (!finalResult.isEmpty()) {
                qout << Qt::endl << finalResult << Qt::endl << Qt::endl;
            }

            /* Disconnect all signals to avoid stale connections */
            for (const auto &conn : connections) {
                QObject::disconnect(conn);
            }
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

    /* Create status line for visual feedback */
    QAgentStatusLine statusLine(this);

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
            bool       loopRunning = true;

            /* Connect status line to agent signals (use QueuedConnection for thread safety) */
            auto connToolCalled = QObject::connect(
                agent,
                &QSocAgent::toolCalled,
                &statusLine,
                [&statusLine](const QString &toolName, const QString &arguments) {
                    /* Extract detail from arguments for better UX */
                    QString detail;
                    try {
                        auto args = json::parse(arguments.toStdString());
                        /* Tool-specific detail extraction */
                        if (args.contains("command")) {
                            detail = QString::fromStdString(args["command"].get<std::string>());
                        } else if (args.contains("title")) {
                            /* todo_add */
                            detail = "\"" + QString::fromStdString(args["title"].get<std::string>())
                                     + "\"";
                        } else if (args.contains("file_path")) {
                            /* read_file, write_file, edit_file */
                            detail = QString::fromStdString(args["file_path"].get<std::string>());
                        } else if (args.contains("path")) {
                            detail = QString::fromStdString(args["path"].get<std::string>());
                        } else if (args.contains("name")) {
                            detail = QString::fromStdString(args["name"].get<std::string>());
                        } else if (args.contains("regex")) {
                            detail = QString::fromStdString(args["regex"].get<std::string>());
                        } else if (args.contains("id")) {
                            /* todo_update, todo_delete - id only, title from result */
                            detail = "#" + QString::number(args["id"].get<int>());
                        }
                    } catch (...) {
                        /* Ignore parse errors */
                    }
                    statusLine.toolCalled(toolName, detail);
                });

            auto connToolResult = QObject::connect(
                agent,
                &QSocAgent::toolResult,
                &statusLine,
                [&statusLine](const QString &toolName, const QString &result) {
                    statusLine.resetProgress();
                    statusLine.update(QString("%1 done, thinking").arg(toolName));

                    /* Parse and update todo list based on tool type */
                    if (toolName == "todo_list") {
                        auto items = parseTodoListResult(result);
                        statusLine.setTodoList(items);
                    } else if (toolName == "todo_add") {
                        auto item = parseTodoAddResult(result);
                        if (item.id >= 0) {
                            statusLine.addTodoItem(item);
                        }
                    } else if (toolName == "todo_update") {
                        auto [todoId, newStatus] = parseTodoUpdateResult(result);
                        if (todoId >= 0) {
                            statusLine.updateTodoStatus(todoId, newStatus);
                        }
                    }
                    /* Show todo result for all todo tools */
                    if (toolName.startsWith("todo_")) {
                        statusLine.updateTodoDisplay(result);
                    }
                });

            /* Track active todo via toolCalled for todo_update */
            auto connTodoTrack = QObject::connect(
                agent,
                &QSocAgent::toolCalled,
                &statusLine,
                [&statusLine](const QString &toolName, const QString &arguments) {
                    if (toolName == "todo_update") {
                        try {
                            auto args = json::parse(arguments.toStdString());
                            if (args.contains("status")) {
                                QString status = QString::fromStdString(
                                    args["status"].get<std::string>());
                                if (status == "in_progress" && args.contains("id")) {
                                    int todoId = args["id"].get<int>();
                                    statusLine.setActiveTodo(todoId);
                                } else if (status == "done" || status == "pending") {
                                    statusLine.clearActiveTodo();
                                }
                            }
                        } catch (...) {
                            /* Ignore parse errors */
                        }
                    }
                });

            auto connContentChunk = QObject::connect(
                agent, &QSocAgent::contentChunk, &loop, [&statusLine](const QString &chunk) {
                    statusLine.printContent(chunk);
                });

            auto connRunComplete = QObject::connect(
                agent,
                &QSocAgent::runComplete,
                &loop,
                [&qout, &loop, &statusLine, &loopRunning](const QString &) {
                    statusLine.stop();
                    qout << Qt::endl << Qt::endl;
                    if (loopRunning) {
                        loop.quit();
                    }
                });

            auto connRunError = QObject::connect(
                agent,
                &QSocAgent::runError,
                &loop,
                [&qout, &loop, &statusLine, &loopRunning](const QString &error) {
                    statusLine.stop();
                    qout << Qt::endl << "Error: " << error << Qt::endl << Qt::endl;
                    if (loopRunning) {
                        loop.quit();
                    }
                });

            /* Connect heartbeat - updates status and token display */
            auto connHeartbeat
                = QObject::connect(agent, &QSocAgent::heartbeat, &loop, [&statusLine](int, int) {
                      statusLine.update("Working");
                  });

            /* Connect token usage update */
            auto connTokens = QObject::connect(
                agent, &QSocAgent::tokenUsage, &loop, [&statusLine](qint64 input, qint64 output) {
                    statusLine.updateTokens(input, output);
                });

            /* Connect stuck detection for warning */
            auto connStuck = QObject::connect(
                agent, &QSocAgent::stuckDetected, &loop, [&statusLine](int, int silentSeconds) {
                    statusLine.update(QString("Working [%1s no progress]").arg(silentSeconds));
                });

            /* Connect retrying signal for user feedback */
            auto connRetry = QObject::connect(
                agent,
                &QSocAgent::retrying,
                &loop,
                [&statusLine](int attempt, int maxAttempts, const QString &) {
                    statusLine.update(QString("Retrying (%1/%2)").arg(attempt).arg(maxAttempts));
                });

            /* Start status line and agent */
            statusLine.start("Thinking");
            agent->runStream(input);
            loop.exec();
            loopRunning = false;

            /* Disconnect all signals to avoid stale connections */
            QObject::disconnect(connToolCalled);
            QObject::disconnect(connToolResult);
            QObject::disconnect(connTodoTrack);
            QObject::disconnect(connContentChunk);
            QObject::disconnect(connRunComplete);
            QObject::disconnect(connRunError);
            QObject::disconnect(connHeartbeat);
            QObject::disconnect(connTokens);
            QObject::disconnect(connStuck);
            QObject::disconnect(connRetry);
        } else {
            /* Non-streaming mode: use async API but collect result without chunk output */
            QEventLoop loop;
            bool       loopRunning = true;
            QString    finalResult;

            /* Connect tool signals for status updates */
            auto connToolCalled = QObject::connect(
                agent,
                &QSocAgent::toolCalled,
                &statusLine,
                [&statusLine](const QString &toolName, const QString &arguments) {
                    /* Extract detail from arguments for better UX */
                    QString detail;
                    try {
                        auto args = json::parse(arguments.toStdString());
                        /* Tool-specific detail extraction */
                        if (args.contains("command")) {
                            detail = QString::fromStdString(args["command"].get<std::string>());
                        } else if (args.contains("title")) {
                            /* todo_add */
                            detail = "\"" + QString::fromStdString(args["title"].get<std::string>())
                                     + "\"";
                        } else if (args.contains("file_path")) {
                            /* read_file, write_file, edit_file */
                            detail = QString::fromStdString(args["file_path"].get<std::string>());
                        } else if (args.contains("path")) {
                            detail = QString::fromStdString(args["path"].get<std::string>());
                        } else if (args.contains("name")) {
                            detail = QString::fromStdString(args["name"].get<std::string>());
                        } else if (args.contains("regex")) {
                            detail = QString::fromStdString(args["regex"].get<std::string>());
                        } else if (args.contains("id")) {
                            /* todo_update, todo_delete - id only, title from result */
                            detail = "#" + QString::number(args["id"].get<int>());
                        }
                    } catch (...) {
                        /* Ignore parse errors */
                    }
                    statusLine.toolCalled(toolName, detail);
                });

            auto connToolResult = QObject::connect(
                agent,
                &QSocAgent::toolResult,
                &statusLine,
                [&statusLine](const QString &toolName, const QString &result) {
                    statusLine.resetProgress();
                    statusLine.update(QString("%1 done, thinking").arg(toolName));

                    /* Parse and update todo list based on tool type */
                    if (toolName == "todo_list") {
                        auto items = parseTodoListResult(result);
                        statusLine.setTodoList(items);
                    } else if (toolName == "todo_add") {
                        auto item = parseTodoAddResult(result);
                        if (item.id >= 0) {
                            statusLine.addTodoItem(item);
                        }
                    } else if (toolName == "todo_update") {
                        auto [todoId, newStatus] = parseTodoUpdateResult(result);
                        if (todoId >= 0) {
                            statusLine.updateTodoStatus(todoId, newStatus);
                        }
                    }
                    /* Show todo result for all todo tools */
                    if (toolName.startsWith("todo_")) {
                        statusLine.updateTodoDisplay(result);
                    }
                });

            /* Track active todo via toolCalled for todo_update */
            auto connTodoTrack = QObject::connect(
                agent,
                &QSocAgent::toolCalled,
                &statusLine,
                [&statusLine](const QString &toolName, const QString &arguments) {
                    if (toolName == "todo_update") {
                        try {
                            auto args = json::parse(arguments.toStdString());
                            if (args.contains("status")) {
                                QString status = QString::fromStdString(
                                    args["status"].get<std::string>());
                                if (status == "in_progress" && args.contains("id")) {
                                    int todoId = args["id"].get<int>();
                                    statusLine.setActiveTodo(todoId);
                                } else if (status == "done" || status == "pending") {
                                    statusLine.clearActiveTodo();
                                }
                            }
                        } catch (...) {
                            /* Ignore parse errors */
                        }
                    }
                });

            /* Don't print chunks - just accumulate for final display */
            auto connContentChunk = QObject::connect(
                agent, &QSocAgent::contentChunk, &loop, [&finalResult](const QString &chunk) {
                    finalResult += chunk;
                });

            auto connRunComplete = QObject::connect(
                agent,
                &QSocAgent::runComplete,
                &loop,
                [&loop, &statusLine, &loopRunning](const QString &) {
                    statusLine.stop();
                    if (loopRunning) {
                        loop.quit();
                    }
                });

            auto connRunError = QObject::connect(
                agent,
                &QSocAgent::runError,
                &loop,
                [&qout, &loop, &statusLine, &loopRunning](const QString &error) {
                    statusLine.stop();
                    qout << Qt::endl << "Error: " << error << Qt::endl << Qt::endl;
                    if (loopRunning) {
                        loop.quit();
                    }
                });

            /* Connect heartbeat - updates status and token display */
            auto connHeartbeat
                = QObject::connect(agent, &QSocAgent::heartbeat, &loop, [&statusLine](int, int) {
                      statusLine.update("Working");
                  });

            /* Connect token usage update */
            auto connTokens = QObject::connect(
                agent, &QSocAgent::tokenUsage, &loop, [&statusLine](qint64 input, qint64 output) {
                    statusLine.updateTokens(input, output);
                });

            /* Connect stuck detection for warning */
            auto connStuck = QObject::connect(
                agent, &QSocAgent::stuckDetected, &loop, [&statusLine](int, int silentSeconds) {
                    statusLine.update(QString("Working [%1s no progress]").arg(silentSeconds));
                });

            /* Connect retrying signal for user feedback */
            auto connRetry = QObject::connect(
                agent,
                &QSocAgent::retrying,
                &loop,
                [&statusLine](int attempt, int maxAttempts, const QString &) {
                    statusLine.update(QString("Retrying (%1/%2)").arg(attempt).arg(maxAttempts));
                });

            /* Start status line and agent */
            statusLine.start("Thinking");
            agent->runStream(input);
            loop.exec();
            loopRunning = false;

            /* Display complete result at once */
            if (!finalResult.isEmpty()) {
                qout << Qt::endl << finalResult << Qt::endl << Qt::endl;
            }

            /* Disconnect signals */
            QObject::disconnect(connToolCalled);
            QObject::disconnect(connToolResult);
            QObject::disconnect(connTodoTrack);
            QObject::disconnect(connContentChunk);
            QObject::disconnect(connRunComplete);
            QObject::disconnect(connRunError);
            QObject::disconnect(connHeartbeat);
            QObject::disconnect(connTokens);
            QObject::disconnect(connStuck);
            QObject::disconnect(connRetry);
        }
    }

    return true;
}
