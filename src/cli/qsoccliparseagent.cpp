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
#include "agent/tool/qsoctoolskill.h"
#include "agent/tool/qsoctooltodo.h"
#include "agent/tool/qsoctoolweb.h"
#include "cli/qagentinputmonitor.h"
#include "cli/qterminalcapability.h"
#include "common/qstaticlog.h"
#include "tui/qtuicompositor.h"
#include "tui/qtuiinputline.h"
#include "tui/qtuimenu.h"
#include "tui/qtuiqueuedlist.h"
#include "tui/qtuistatusbar.h"
#include "tui/qtuitodolist.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPair>
#include <QRegularExpression>
#include <QTextStream>

#include <atomic>
#include <csignal>
#include <cstdlib>
#include <fstream>
#include <sys/wait.h>
#include <yaml-cpp/yaml.h>

namespace {

/* Double Ctrl+C exit tracking */
std::atomic<qint64> g_lastInterruptMs{0};
constexpr qint64    DOUBLE_PRESS_MS = 2000;

bool checkDoubleInterrupt()
{
    qint64 now  = QDateTime::currentMSecsSinceEpoch();
    qint64 last = g_lastInterruptMs.exchange(now);
    return (last > 0 && (now - last) < DOUBLE_PRESS_MS);
}

/* SIGINT handler for non-raw-mode states */
volatile sig_atomic_t g_sigintReceived = 0;

void sigintHandler(int)
{
    g_sigintReceived = 1;
}

void installSigintHandler()
{
    struct sigaction sigact = {};
    sigact.sa_handler       = sigintHandler;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(SIGINT, &sigact, nullptr);
}

/**
 * @brief Format model list for /model command output
 */
QString formatModelList(QLLMService *llmService)
{
    QStringList models  = llmService->availableModels();
    QString     current = llmService->getCurrentModelId();
    QString     result  = "Available models:\n";

    for (const QString &modelId : models) {
        LLMModelConfig cfg    = llmService->getModelConfig(modelId);
        QString        marker = (modelId == current) ? "* " : "  ";
        QString        name   = cfg.name.isEmpty() ? cfg.id : cfg.name;
        QString        info   = QString("(%1K ctx, %2K out)")
                           .arg(cfg.contextTokens / 1000)
                           .arg(cfg.maxOutputTokens > 0 ? cfg.maxOutputTokens / 1000 : 0);
        if (cfg.reasoning) {
            info += " [reasoning]";
        }
        result += QString("  %1%-30s %2 %3\n").arg(marker, modelId, info);
    }

    return result;
}

/**
 * @brief Handle /model command, returns true if command was handled
 */
/**
 * @brief Apply model switch: update endpoint + sync agent context budget
 */
void applyModelSwitch(
    const QString &modelId, QTextStream &qout, QLLMService *llmService, QSocAgent *agent)
{
    if (llmService->setCurrentModel(modelId)) {
        LLMModelConfig  cfg      = llmService->getModelConfig(modelId);
        QSocAgentConfig agentCfg = agent->getConfig();
        if (cfg.contextTokens > 0) {
            agentCfg.maxContextTokens = cfg.contextTokens;
        }
        agent->setConfig(agentCfg);
        QString name = cfg.name.isEmpty() ? cfg.id : cfg.name;
        qout << "Model: " << modelId << " (" << name << ")" << Qt::endl;

        /* Persist model selection to the effective config file.
         * Write to project config if it exists, otherwise user config. */
        QString configPath;
        if (QFile::exists(QDir::currentPath() + "/.qsoc.yml")) {
            configPath = QDir::currentPath() + "/.qsoc.yml";
        } else {
            configPath = QDir::home().absoluteFilePath(".config/qsoc/qsoc.yml");
        }
        /* Read, update llm.model, write back */
        try {
            YAML::Node root      = YAML::LoadFile(configPath.toStdString());
            root["llm"]["model"] = modelId.toStdString();
            std::ofstream fout(configPath.toStdString());
            fout << root;
        } catch (const YAML::Exception &err) {
            qWarning() << "Failed to save model to config:" << err.what();
        }
    } else {
        qout << "Unknown model: " << modelId << Qt::endl;
        qout << "Use /model to list available models" << Qt::endl;
    }
}

/**
 * @brief Handle /model command. interactive=true opens TUI menu, false prints list only.
 */
bool handleModelCommand(
    const QString &input,
    QTextStream   &qout,
    QLLMService   *llmService,
    QSocAgent     *agent,
    bool           interactive = true)
{
    QString arg = input.mid(6).trimmed();

    if (!arg.isEmpty()) {
        applyModelSwitch(arg, qout, llmService, agent);
        return true;
    }

    QStringList models = llmService->availableModels();
    if (models.isEmpty()) {
        qout << "No models configured. Add llm.models section to .qsoc.yml" << Qt::endl;
        return true;
    }

    if (interactive) {
        /* Build menu items */
        QList<QTuiMenu::MenuItem> items;
        QString                   current = llmService->getCurrentModelId();
        for (const QString &modelId : models) {
            LLMModelConfig     cfg = llmService->getModelConfig(modelId);
            QTuiMenu::MenuItem item;
            item.label  = modelId;
            item.hint   = cfg.reasoning ? "[R]" : "";
            item.marked = (modelId == current);
            items.append(item);
        }

        QTuiMenu menu;
        menu.setTitle("Model Selection");
        menu.setItems(items);

        int currentIdx = models.indexOf(current);
        if (currentIdx >= 0) {
            menu.setHighlight(currentIdx);
        }

        int selected = menu.exec();
        if (selected >= 0 && selected < models.size()) {
            applyModelSwitch(models[selected], qout, llmService, agent);
        }
    } else {
        /* Non-interactive: just print list */
        QString current = llmService->getCurrentModelId();
        qout << "Available models:" << Qt::endl;
        for (const QString &modelId : models) {
            LLMModelConfig cfg    = llmService->getModelConfig(modelId);
            QString        marker = (modelId == current) ? "* " : "  ";
            QString        name   = cfg.name.isEmpty() ? cfg.id : cfg.name;
            qout << "  " << marker << modelId << "  " << name << Qt::endl;
        }
    }
    return true;
}

/**
 * @brief Parse todo_list result into structured items
 * @param result The result string from todo_list tool
 * @return List of parsed TodoItem structures
 */
QList<QTuiTodoList::TodoItem> parseTodoListResult(const QString &result)
{
    QList<QTuiTodoList::TodoItem> items;

    /* Match pattern: [x] or [ ] followed by ID. Title (priority) */
    QRegularExpression regex(R"(\[([ x])\]\s*(\d+)\.\s*(.+?)\s*\((\w+)\))");

    const QStringList lines = result.split('\n');
    for (const QString &line : lines) {
        QRegularExpressionMatch match = regex.match(line);
        if (match.hasMatch()) {
            QTuiTodoList::TodoItem item;
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
QTuiTodoList::TodoItem parseTodoAddResult(const QString &result)
{
    QTuiTodoList::TodoItem item;
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
    /* Match: "Updated todo #ID: Title (status: STATUS)" */
    QRegularExpression      regex(R"(Updated todo #(\d+):\s*.+?\(status:\s*(\w+)\))");
    QRegularExpressionMatch match = regex.match(result);

    if (match.hasMatch()) {
        return qMakePair(match.captured(1).toInt(), match.captured(2));
    }

    return qMakePair(-1, QString());
}

/**
 * @brief Execute a shell escape command with real-time terminal I/O
 * @param command The shell command to execute
 * @param supportsColor Whether the terminal supports ANSI colors
 * @details Uses std::system() so the child process inherits the terminal directly.
 *          Supports Ctrl+C (POSIX: parent ignores SIGINT, child gets it) and has no timeout.
 */
void runShellEscape(const QString &command, bool supportsColor)
{
    QTextStream out(stdout);
    if (supportsColor) {
        out << "\033[33m$ " << command << "\033[0m" << Qt::endl;
    } else {
        out << "$ " << command << Qt::endl;
    }
    out.flush();

    int ret = std::system(qPrintable(command));

    if (WIFEXITED(ret)) {
        int exitCode = WEXITSTATUS(ret);
        if (exitCode != 0) {
            if (supportsColor) {
                out << "\033[31m(exit code: " << exitCode << ")\033[0m" << Qt::endl;
            } else {
                out << "(exit code: " << exitCode << ")" << Qt::endl;
            }
        }
    } else if (WIFSIGNALED(ret)) {
        out << "(signal: " << WTERMSIG(ret) << ")" << Qt::endl;
    }
}

/**
 * @brief Get the conversation file path for the current project
 * @param pm Project manager
 * @return Path to the conversation JSON file
 */
QString conversationFilePath(QSocProjectManager *pm)
{
    QString projectPath = pm->getProjectPath();
    if (projectPath.isEmpty()) {
        projectPath = QDir::currentPath();
    }
    return QDir(projectPath).filePath(".qsoc/conversation.json");
}

/**
 * @brief Save the conversation history to a file
 * @param agent The agent whose messages to save
 * @param pm Project manager for path resolution
 */
void saveConversation(QSocAgent *agent, QSocProjectManager *pm)
{
    QString   filePath = conversationFilePath(pm);
    QFileInfo fileInfo(filePath);
    QDir      parentDir = fileInfo.absoluteDir();
    if (!parentDir.exists()) {
        parentDir.mkpath(".");
    }
    QFile file(filePath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QTextStream stream(&file);
        json        doc = {{"version", 1}, {"messages", agent->getMessages()}};
        stream << QString::fromStdString(doc.dump());
        file.close();
    }
}

/**
 * @brief Load a conversation history from a file
 * @param agent The agent to restore messages into
 * @param pm Project manager for path resolution
 * @return True if conversation was loaded successfully
 */
bool loadConversation(QSocAgent *agent, QSocProjectManager *pm)
{
    QString filePath = conversationFilePath(pm);
    QFile   file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }
    QTextStream stream(&file);
    QString     content = stream.readAll();
    file.close();
    try {
        json doc = json::parse(content.toStdString());
        if (doc.contains("messages") && doc["messages"].is_array()) {
            agent->setMessages(doc["messages"]);
            return true;
        }
    } catch (...) {
        /* Ignore parse errors */
    }
    return false;
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
        {"effort",
         QCoreApplication::translate("main", "Reasoning effort level (low/medium/high)."),
         "level"},
        {"model-reasoning",
         QCoreApplication::translate("main", "Model to use when reasoning effort is set."),
         "model"},
    });

    parser.parse(appArguments);

    if (parser.isSet("help")) {
        return showHelp(0);
    }

    /* Set up project path if specified */
    if (parser.isSet("directory")) {
        projectManager->setProjectPath(parser.value("directory"));
        /* Full reload: picks up project .qsoc.yml, env vars stay highest priority */
        socConfig->loadConfig();
        /* Reload LLM endpoints from updated config */
        llmService->setConfig(socConfig);
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
        projectManager->loadFirst(true); /* Silent in agent mode */
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

        QString pruneThresholdStr = socConfig->getValue("agent.prune_threshold");
        if (!pruneThresholdStr.isEmpty()) {
            config.pruneThreshold = pruneThresholdStr.toDouble();
        }

        QString compactThresholdStr = socConfig->getValue("agent.compact_threshold");
        if (!compactThresholdStr.isEmpty()) {
            config.compactThreshold = compactThresholdStr.toDouble();
        }

        QString compactionModelStr = socConfig->getValue("agent.compaction_model");
        if (!compactionModelStr.isEmpty()) {
            config.compactionModel = compactionModelStr;
        }

        QString systemPrompt = socConfig->getValue("agent.system_prompt");
        if (!systemPrompt.isEmpty()) {
            config.systemPrompt = systemPrompt;
        }

        QString effortStr = socConfig->getValue("agent.effort");
        if (!effortStr.isEmpty()) {
            config.effortLevel = effortStr;
        }

        QString reasoningModelStr = socConfig->getValue("llm.model_reasoning");
        if (!reasoningModelStr.isEmpty()) {
            config.reasoningModel = reasoningModelStr;
        }

        QString autoLoadMemoryStr = socConfig->getValue("agent.auto_load_memory");
        if (!autoLoadMemoryStr.isEmpty()) {
            config.autoLoadMemory
                = (autoLoadMemoryStr.toLower() == "true" || autoLoadMemoryStr == "1");
        }

        QString memoryMaxCharsStr = socConfig->getValue("agent.memory_max_chars");
        if (!memoryMaxCharsStr.isEmpty()) {
            config.memoryMaxChars = memoryMaxCharsStr.toInt();
        }
    }

    /* Command line overrides config file */
    if (parser.isSet("max-tokens")) {
        config.maxContextTokens = parser.value("max-tokens").toInt();
    }
    if (parser.isSet("temperature")) {
        config.temperature = parser.value("temperature").toDouble();
    }
    if (parser.isSet("effort")) {
        config.effortLevel = parser.value("effort").toLower();
    }
    if (parser.isSet("model-reasoning")) {
        config.reasoningModel = parser.value("model-reasoning");
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

    /* Path context (must be before file tools) */
    auto *pathContext     = new QSocPathContext(this, projectManager);
    auto *pathContextTool = new QSocToolPathContext(this, pathContext);
    toolRegistry->registerTool(pathContextTool);

    /* File tools - use pathContext for permission checks */
    auto *fileReadTool  = new QSocToolFileRead(this, pathContext);
    auto *fileListTool  = new QSocToolFileList(this, pathContext);
    auto *fileWriteTool = new QSocToolFileWrite(this, pathContext);
    auto *fileEditTool  = new QSocToolFileEdit(this, pathContext);
    toolRegistry->registerTool(fileReadTool);
    toolRegistry->registerTool(fileListTool);
    toolRegistry->registerTool(fileWriteTool);
    toolRegistry->registerTool(fileEditTool);

    /* Shell tools */
    auto *shellBashTool  = new QSocToolShellBash(this, projectManager);
    auto *bashManageTool = new QSocToolBashManage(this);
    toolRegistry->registerTool(shellBashTool);
    toolRegistry->registerTool(bashManageTool);

    /* Documentation tools */
    auto *docQueryTool = new QSocToolDocQuery(this);
    toolRegistry->registerTool(docQueryTool);

    /* Memory manager and tools */
    auto *memoryManager   = new QSocMemoryManager(this, projectManager);
    auto *memoryReadTool  = new QSocToolMemoryRead(this, memoryManager);
    auto *memoryWriteTool = new QSocToolMemoryWrite(this, memoryManager);
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

    /* Skill tools */
    auto *skillFindTool   = new QSocToolSkillFind(this, projectManager);
    auto *skillCreateTool = new QSocToolSkillCreate(this, projectManager);
    toolRegistry->registerTool(skillFindTool);
    toolRegistry->registerTool(skillCreateTool);

    /* Web tools */
    auto *webFetchTool = new QSocToolWebFetch(this, socConfig);
    toolRegistry->registerTool(webFetchTool);

    if (socConfig && !socConfig->getValue("web.search_api_url").isEmpty()) {
        auto *webSearchTool = new QSocToolWebSearch(this, socConfig);
        toolRegistry->registerTool(webSearchTool);
    }

    /* Sync context budget from model registry if available */
    {
        QString modelId = llmService->getCurrentModelId();
        if (!modelId.isEmpty()) {
            LLMModelConfig modelCfg = llmService->getModelConfig(modelId);
            if (modelCfg.contextTokens > 0) {
                config.maxContextTokens = modelCfg.contextTokens;
            }
        }
    }

    /* Create agent */
    auto *agent = new QSocAgent(this, llmService, toolRegistry, config);
    agent->setMemoryManager(memoryManager);

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

    /* Install SIGINT handler for Ctrl+C support in non-raw-mode states */
    installSigintHandler();

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

            connect(agent, &QSocAgent::reasoningChunk, [&qout](const QString &chunk) {
                qout << "\033[2m" << chunk << "\033[0m" << Qt::flush;
            });

            connect(agent, &QSocAgent::runComplete, [&qout, &loop](const QString &) {
                qout << Qt::endl;
                loop.quit();
            });

            connect(agent, &QSocAgent::runError, [this, &loop](const QString &error) {
                showError(1, error);
                loop.quit();
            });

            connect(agent, &QSocAgent::runAborted, &loop, [&qout, &loop](const QString &) {
                qout << Qt::endl << "(interrupted)" << Qt::endl;
                loop.quit();
            });

            /* ESC key monitor */
            QAgentInputMonitor escMonitor;
            connect(&escMonitor, &QAgentInputMonitor::escPressed, agent, &QSocAgent::abort);
            connect(&escMonitor, &QAgentInputMonitor::ctrlCPressed, agent, [agent, &escMonitor]() {
                if (checkDoubleInterrupt()) {
                    escMonitor.stop();
                    QTextStream(stderr) << "\n" << Qt::flush;
                    _exit(130);
                }
                agent->abort();
            });
            connect(
                &escMonitor,
                &QAgentInputMonitor::inputReady,
                agent,
                [agent, &qout, &escMonitor](const QString &text) {
                    if (text.startsWith("!")) {
                        QString shellCmd = text.mid(1).trimmed();
                        if (!shellCmd.isEmpty()) {
                            qout << "\n" << Qt::flush;
                            escMonitor.stop();
                            runShellEscape(shellCmd, true);
                            escMonitor.start();
                        }
                        return;
                    }
                    agent->queueRequest(text);
                    qout << "\n(queued: " << text << ")\n" << Qt::flush;
                });
            connect(
                agent,
                &QSocAgent::processingQueuedRequest,
                agent,
                [&qout](const QString &request, int) {
                    qout << "\n> " << request << "\n" << Qt::flush;
                });
            escMonitor.start();

            agent->runStream(query);
            loop.exec();
            escMonitor.stop();
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
    /* Require interactive terminal for TUI */
    QTerminalCapability termCap;

    if (!termCap.useEnhancedMode()) {
        QString reason;
        if (!termCap.isInteractive()) {
            reason = "stdin is not a TTY (piped or redirected)";
        } else if (!termCap.isOutputInteractive()) {
            reason = "stdout is not a TTY (piped or redirected)";
        } else {
            reason = "terminal does not meet requirements";
        }
        return showError(
            1,
            QCoreApplication::translate(
                "main",
                "Error: interactive terminal required (%1).\n"
                "Use 'qsoc agent -q \"your query\"' for "
                "non-interactive mode.")
                .arg(reason));
    }

    /* Minimum terminal size for TUI */
    static constexpr int MIN_COLS = 40;
    static constexpr int MIN_ROWS = 10;
    if (termCap.columns() < MIN_COLS || termCap.rows() < MIN_ROWS) {
        return showError(
            1,
            QCoreApplication::translate(
                "main",
                "Error: terminal too small (%1x%2). "
                "Minimum %3x%4 required.")
                .arg(termCap.columns())
                .arg(termCap.rows())
                .arg(MIN_COLS)
                .arg(MIN_ROWS));
    }

    QTextStream qout(stdout);

    /* Create TUI compositor — enters alt screen immediately */
    QTuiCompositor compositor(this);
    auto          &todoWidget      = compositor.todoList();
    auto          &queueWidget     = compositor.queuedList();
    auto          &statusBarWidget = compositor.statusBar();
    auto          &inputWidget     = compositor.inputLine();

    /* Set title and start full-screen TUI */
    {
        QString mid = llmService->getCurrentModelId();
        compositor.setTitle("QSoC Agent -- " + (mid.isEmpty() ? "default" : mid));
    }
    statusBarWidget.setStatus("Ready");
    statusBarWidget.setModel(llmService->getCurrentModelId());
    compositor.start();

    /* Show welcome banner in scroll view */
    compositor.printContent("QSoC Agent - Interactive AI Assistant for SoC Design\n");
    compositor.printContent("Type 'exit' to exit, '/help' for commands\n");
    if (streaming) {
        compositor.printContent("(Enhanced mode, streaming enabled)\n");
    }

    /* Connect mouse wheel from input monitor to compositor scroll */
    QAgentInputMonitor inputMonitor(this);
    connect(&inputMonitor, &QAgentInputMonitor::mouseWheel, [&compositor](int direction) {
        if (direction == 0) {
            compositor.scrollContentUp(3);
        } else {
            compositor.scrollContentDown(3);
        }
    });

    /* Connect live input display */
    connect(&inputMonitor, &QAgentInputMonitor::inputChanged, [&inputWidget](const QString &text) {
        inputWidget.setText(text);
    });

    /* Start input monitor (raw mode for entire REPL lifetime) */
    inputMonitor.start();

    /* Load previous conversation if available */
    if (loadConversation(agent, projectManager)) {
        int msgCount = static_cast<int>(agent->getMessages().size());
        compositor.printContent(
            QString("(Loaded %1 messages from previous session)\n\n").arg(msgCount));
    }

    /* Main loop: use inputMonitor for prompt (raw mode, full-screen TUI) */
    bool exitRequested = false;

    while (!exitRequested) {
        /* Wait for user input via QEventLoop + inputMonitor signals */
        QEventLoop promptLoop;
        QString    input;

        /* Show prompt hint in status bar */
        statusBarWidget.setStatus("Ready");
        inputWidget.clear();
        compositor.render();

        auto connInput = connect(
            &inputMonitor,
            &QAgentInputMonitor::inputReady,
            [&input, &promptLoop](const QString &text) {
                input = text;
                promptLoop.quit();
            });
        auto connCtrlC = connect(
            &inputMonitor,
            &QAgentInputMonitor::ctrlCPressed,
            [&promptLoop, &exitRequested, &compositor]() {
                if (checkDoubleInterrupt()) {
                    exitRequested = true;
                    promptLoop.quit();
                } else {
                    compositor.printContent("\n^C\n");
                    promptLoop.quit();
                }
            });
        auto connEsc = connect(&inputMonitor, &QAgentInputMonitor::escPressed, [&promptLoop]() {
            promptLoop.quit();
        });

        promptLoop.exec();

        QObject::disconnect(connInput);
        QObject::disconnect(connCtrlC);
        QObject::disconnect(connEsc);

        if (exitRequested) {
            break;
        }

        input = input.trimmed();

        if (input.isEmpty()) {
            continue;
        }

        /* Echo user input in scroll view */
        compositor.printContent("\nqsoc> " + input + "\n");

        if (input.startsWith("!")) {
            QString shellCmd = input.mid(1).trimmed();
            if (!shellCmd.isEmpty()) {
                compositor.pause();
                inputMonitor.stop();
                runShellEscape(shellCmd, true);
                inputMonitor.start();
                compositor.resume();
            }
            continue;
        }
        QString cmd = input.toLower();
        if (cmd == "exit" || cmd == "quit" || cmd == "/exit" || cmd == "/quit") {
            compositor.printContent("Goodbye!\n");
            break;
        }
        if (cmd == "/clear") {
            agent->clearHistory();
            QFile::remove(conversationFilePath(projectManager));
            compositor.printContent("History cleared.\n");
            continue;
        }
        if (cmd == "/help") {
            compositor.printContent("Commands:\n");
            compositor.printContent("  exit, /exit  - Exit the agent\n");
            compositor.printContent("  /clear       - Clear conversation history\n");
            compositor.printContent("  /compact     - Compact conversation context\n");
            compositor.printContent(
                "  /effort    - Show/set reasoning effort (off/low/medium/high)\n");
            compositor.printContent("  /model       - Show/switch model\n");
            compositor.printContent("  !<command>   - Execute a shell command directly\n");
            compositor.printContent("  /help        - Show this help message\n");
            compositor.printContent("\n");
            compositor.printContent("Keyboard shortcuts:\n");
            compositor.printContent("  Up/Down     - Browse history\n");
            compositor.printContent("  Ctrl+R      - Search history\n");
            compositor.printContent("  Ctrl+A/E    - Move to start/end of line\n");
            compositor.printContent("  Ctrl+K      - Delete to end of line\n");
            compositor.printContent("  Ctrl+W      - Delete word\n");
            compositor.printContent("  Ctrl+L      - Clear screen\n");
            compositor.printContent("\n");
            compositor.printContent("Or just type your question/request in natural language.\n");
            continue;
        }
        if (cmd == "/compact") {
            int saved = agent->compact();
            compositor.printContent(QString("Compacted: saved %1 tokens\n").arg(saved));
            saveConversation(agent, projectManager);
            continue;
        }
        if (cmd.startsWith("/effort")) {
            QString         level = input.mid(7).trimmed().toLower();
            QSocAgentConfig cfg   = agent->getConfig();
            if (level.isEmpty()) {
                compositor.printContent(
                    "Effort: " + (cfg.effortLevel.isEmpty() ? QString("off") : cfg.effortLevel));
                if (!cfg.reasoningModel.isEmpty()) {
                    compositor.printContent(" (model: " + cfg.reasoningModel + ")");
                }
                compositor.printContent("\n");
            } else if (level == "off") {
                agent->setEffortLevel(QString());
                compositor.printContent("Effort: off\n");
            } else if (level == "low" || level == "medium" || level == "high") {
                agent->setEffortLevel(level);
                compositor.printContent("Effort: " + level + "\n");
            } else {
                compositor.printContent("Usage: /effort [off|low|medium|high]\n");
            }
            continue;
        }
        if (cmd.startsWith("/model")) {
            handleModelCommand(input, qout, llmService, agent);
            inputMonitor.resetEscState();
            {
                QString mid = llmService->getCurrentModelId();
                statusBarWidget.setModel(mid);
                compositor.setTitle("QSoC Agent -- " + (mid.isEmpty() ? "default" : mid));
            }
            compositor.render(); /* Redraw after menu overlay */
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
                &compositor,
                [&compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget](const QString &toolName, const QString &arguments) {
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
                        } else if (args.contains("query")) {
                            detail = QString::fromStdString(args["query"].get<std::string>());
                        } else if (args.contains("url")) {
                            detail = QString::fromStdString(args["url"].get<std::string>());
                        } else if (args.contains("id")) {
                            /* todo_update, todo_delete - id only, title from result */
                            detail = "#" + QString::number(args["id"].get<int>());
                        }
                    } catch (...) {
                        /* Ignore parse errors */
                    }
                    /* Skip [Tool] output for todo tools - status shown in TODO list */
                    if (toolName.startsWith("todo_")) {
                        statusBarWidget.resetProgress();
                        statusBarWidget.setStatus(QString("Running %1...").arg(toolName));
                    } else {
                        statusBarWidget.toolCalled(toolName, detail);
                    }
                });

            auto connToolResult = QObject::connect(
                agent,
                &QSocAgent::toolResult,
                &compositor,
                [&compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget](const QString &toolName, const QString &result) {
                    statusBarWidget.resetProgress();
                    statusBarWidget.setStatus(QString("%1 done, reasoning").arg(toolName));

                    /* Parse and update todo list based on tool type */
                    if (toolName == "todo_list") {
                        auto items = parseTodoListResult(result);
                        todoWidget.setItems(items);
                    } else if (toolName == "todo_add") {
                        auto item = parseTodoAddResult(result);
                        if (item.id >= 0) {
                            todoWidget.addItem(item);
                        }
                    } else if (toolName == "todo_update") {
                        auto [todoId, newStatus] = parseTodoUpdateResult(result);
                        if (todoId >= 0) {
                            todoWidget.updateStatus(todoId, newStatus);
                        }
                    }
                    if (toolName.startsWith("todo_")) {
                        /* updateTodoDisplay deprecated: widgets auto-render */ (void) (result);
                    }
                });

            /* Track active todo via toolCalled for todo_update */
            auto connTodoTrack = QObject::connect(
                agent,
                &QSocAgent::toolCalled,
                &compositor,
                [&compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget](const QString &toolName, const QString &arguments) {
                    if (toolName == "todo_update") {
                        try {
                            auto args = json::parse(arguments.toStdString());
                            if (args.contains("status")) {
                                QString status = QString::fromStdString(
                                    args["status"].get<std::string>());
                                if (status == "in_progress" && args.contains("id")) {
                                    int todoId = args["id"].get<int>();
                                    todoWidget.setActive(todoId);
                                } else if (status == "done" || status == "pending") {
                                    todoWidget.clearActive();
                                }
                            }
                        } catch (...) {
                            /* Ignore parse errors */
                        }
                    }
                });

            auto connContentChunk = QObject::connect(
                agent,
                &QSocAgent::contentChunk,
                &loop,
                [&compositor, &statusBarWidget, &todoWidget, &queueWidget, &inputWidget](
                    const QString &chunk) { compositor.printContent(chunk); });

            auto connRunComplete = QObject::connect(
                agent,
                &QSocAgent::runComplete,
                &loop,
                [&qout,
                 &loop,
                 &compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget,
                 &loopRunning](const QString &) {
                    compositor.resetExecution();
                    compositor.printContent("\n");
                    if (loopRunning) {
                        loop.quit();
                    }
                });

            auto connRunError = QObject::connect(
                agent,
                &QSocAgent::runError,
                &loop,
                [&qout,
                 &loop,
                 &compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget,
                 &loopRunning](const QString &error) {
                    compositor.resetExecution();
                    compositor.printContent("\nError: " + error + "\n");
                    if (loopRunning) {
                        loop.quit();
                    }
                });

            /* Connect heartbeat - updates status and token display */
            auto connHeartbeat = QObject::connect(
                agent,
                &QSocAgent::heartbeat,
                &loop,
                [&compositor, &statusBarWidget, &todoWidget, &queueWidget, &inputWidget](int, int) {
                    statusBarWidget.setStatus("Working");
                });

            /* Connect token usage update */
            auto connTokens = QObject::connect(
                agent,
                &QSocAgent::tokenUsage,
                &loop,
                [&compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget](qint64 input, qint64 output) {
                    statusBarWidget.updateTokens(input, output);
                });

            /* Connect stuck detection for warning */
            auto connStuck = QObject::connect(
                agent,
                &QSocAgent::stuckDetected,
                &loop,
                [&compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget](int, int silentSeconds) {
                    statusBarWidget.setStatus(
                        QString("Working [%1s no progress]").arg(silentSeconds));
                });

            /* Connect retrying signal for user feedback */
            auto connRetry = QObject::connect(
                agent,
                &QSocAgent::retrying,
                &loop,
                [&compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget](int attempt, int maxAttempts, const QString &) {
                    statusBarWidget.setStatus(
                        QString("Retrying (%1/%2)").arg(attempt).arg(maxAttempts));
                });

            /* Connect compacting signal for context compaction feedback */
            auto connCompact = QObject::connect(
                agent,
                &QSocAgent::compacting,
                &loop,
                [&compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget](int layer, int before, int after) {
                    statusBarWidget.setStatus(
                        QString("Compacting L%1: %2->%3 tokens").arg(layer).arg(before).arg(after));
                });

            /* Connect abort signal */
            auto connAborted = QObject::connect(
                agent,
                &QSocAgent::runAborted,
                &loop,
                [&qout,
                 &loop,
                 &compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget,
                 &loopRunning](const QString &) {
                    compositor.resetExecution();
                    compositor.printContent("\n(interrupted)\n");
                    if (loopRunning) {
                        loop.quit();
                    }
                });

            /* Use existing inputMonitor for ESC/Ctrl+C during execution */
            auto &escMonitor  = inputMonitor;
            auto  connEscExec = QObject::connect(
                &escMonitor, &QAgentInputMonitor::escPressed, agent, &QSocAgent::abort);
            auto connCtrlC = QObject::connect(
                &escMonitor,
                &QAgentInputMonitor::ctrlCPressed,
                agent,
                [agent, &compositor, &escMonitor]() {
                    if (checkDoubleInterrupt()) {
                        escMonitor.stop();
                        compositor.stop();
                        _exit(130);
                    }
                    agent->abort();
                });

            /* Connect reasoning chunk display */
            auto connReasoning = QObject::connect(
                agent, &QSocAgent::reasoningChunk, &compositor, [&compositor](const QString &chunk) {
                    compositor.printContent(chunk, QTuiScrollView::Dim);
                });

            /* Connect input queuing signals */
            auto connInputReady = QObject::connect(
                &escMonitor,
                &QAgentInputMonitor::inputReady,
                agent,
                [agent,
                 &compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget,
                 &escMonitor,
                 &qout,
                 llm = this->llmService](const QString &text) {
                    if (text.startsWith("!")) {
                        QString shellCmd = text.mid(1).trimmed();
                        if (!shellCmd.isEmpty()) {
                            compositor.pause();
                            runShellEscape(shellCmd, true);
                            compositor.resume();
                        }
                        return;
                    }
                    if (text.startsWith("/model")) {
                        handleModelCommand(text, qout, llm, agent, false);
                        return;
                    }
                    if (text.startsWith("/effort")) {
                        QString level = text.mid(7).trimmed().toLower();
                        if (level.isEmpty()) {
                            QSocAgentConfig cfg = agent->getConfig();
                            QString         info
                                = QString("\nEffort: %1")
                                      .arg(cfg.effortLevel.isEmpty() ? "off" : cfg.effortLevel);
                            if (!cfg.reasoningModel.isEmpty()) {
                                info += QString(" (model: %1)").arg(cfg.reasoningModel);
                            }
                            info += "\n";
                            compositor.printContent(info);
                        } else if (level == "off") {
                            agent->setEffortLevel(QString());
                            statusBarWidget.setEffortLevel(QString());
                            compositor.printContent("\nEffort: off\n");
                        } else if (level == "low" || level == "medium" || level == "high") {
                            agent->setEffortLevel(level);
                            statusBarWidget.setEffortLevel(level);
                            compositor.printContent(QString("\nEffort: %1\n").arg(level));
                        } else {
                            compositor.printContent("\nUsage: /effort [off|low|medium|high]\n");
                        }
                        return;
                    }
                    agent->queueRequest(text);
                    queueWidget.addRequest(text);
                });
            auto connProcessingQueued = QObject::connect(
                agent,
                &QSocAgent::processingQueuedRequest,
                &compositor,
                [&compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget](const QString &request, int) {
                    queueWidget.removeRequest(request);
                    compositor.printContent(QString("\n> %1\n").arg(request));
                });
            auto connInputChanged = QObject::connect(
                &escMonitor,
                &QAgentInputMonitor::inputChanged,
                &compositor,
                [&inputWidget](const QString &text) { inputWidget.setText(text); });

            /* Start status line and agent */
            statusBarWidget.setEffortLevel(agent->getConfig().effortLevel);
            statusBarWidget.setStatus("Reasoning");
            statusBarWidget.startTimers();
            {
                QString mid = llmService->getCurrentModelId();
                compositor.setTitle("QSoC Agent -- " + (mid.isEmpty() ? "default" : mid));
            }
            compositor.start();
            agent->runStream(input);
            loop.exec();
            loopRunning = false;

            /* Save conversation after each interaction */
            saveConversation(agent, projectManager);

            /* Disconnect all signals to avoid stale connections */
            QObject::disconnect(connToolCalled);
            QObject::disconnect(connToolResult);
            QObject::disconnect(connTodoTrack);
            QObject::disconnect(connContentChunk);
            QObject::disconnect(connReasoning);
            QObject::disconnect(connRunComplete);
            QObject::disconnect(connRunError);
            QObject::disconnect(connHeartbeat);
            QObject::disconnect(connTokens);
            QObject::disconnect(connStuck);
            QObject::disconnect(connRetry);
            QObject::disconnect(connCompact);
            QObject::disconnect(connAborted);
            QObject::disconnect(connCtrlC);
            QObject::disconnect(connInputReady);
            QObject::disconnect(connProcessingQueued);
            QObject::disconnect(connInputChanged);
        } else {
            /* Non-streaming mode: use async API but collect result without chunk output */
            QEventLoop loop;
            bool       loopRunning = true;
            QString    finalResult;

            /* Connect tool signals for status updates */
            auto connToolCalled = QObject::connect(
                agent,
                &QSocAgent::toolCalled,
                &compositor,
                [&compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget](const QString &toolName, const QString &arguments) {
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
                        } else if (args.contains("query")) {
                            detail = QString::fromStdString(args["query"].get<std::string>());
                        } else if (args.contains("url")) {
                            detail = QString::fromStdString(args["url"].get<std::string>());
                        } else if (args.contains("id")) {
                            /* todo_update, todo_delete - id only, title from result */
                            detail = "#" + QString::number(args["id"].get<int>());
                        }
                    } catch (...) {
                        /* Ignore parse errors */
                    }
                    /* Skip [Tool] output for todo tools - status shown in TODO list */
                    if (toolName.startsWith("todo_")) {
                        statusBarWidget.resetProgress();
                        statusBarWidget.setStatus(QString("Running %1...").arg(toolName));
                    } else {
                        statusBarWidget.toolCalled(toolName, detail);
                    }
                });

            auto connToolResult = QObject::connect(
                agent,
                &QSocAgent::toolResult,
                &compositor,
                [&compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget](const QString &toolName, const QString &result) {
                    statusBarWidget.resetProgress();
                    statusBarWidget.setStatus(QString("%1 done, reasoning").arg(toolName));

                    /* Parse and update todo list based on tool type */
                    if (toolName == "todo_list") {
                        auto items = parseTodoListResult(result);
                        todoWidget.setItems(items);
                    } else if (toolName == "todo_add") {
                        auto item = parseTodoAddResult(result);
                        if (item.id >= 0) {
                            todoWidget.addItem(item);
                        }
                    } else if (toolName == "todo_update") {
                        auto [todoId, newStatus] = parseTodoUpdateResult(result);
                        if (todoId >= 0) {
                            todoWidget.updateStatus(todoId, newStatus);
                        }
                    }
                    if (toolName.startsWith("todo_")) {
                        /* updateTodoDisplay deprecated: widgets auto-render */ (void) (result);
                    }
                });

            /* Track active todo via toolCalled for todo_update */
            auto connTodoTrack = QObject::connect(
                agent,
                &QSocAgent::toolCalled,
                &compositor,
                [&compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget](const QString &toolName, const QString &arguments) {
                    if (toolName == "todo_update") {
                        try {
                            auto args = json::parse(arguments.toStdString());
                            if (args.contains("status")) {
                                QString status = QString::fromStdString(
                                    args["status"].get<std::string>());
                                if (status == "in_progress" && args.contains("id")) {
                                    int todoId = args["id"].get<int>();
                                    todoWidget.setActive(todoId);
                                } else if (status == "done" || status == "pending") {
                                    todoWidget.clearActive();
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
                [&loop,
                 &compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget,
                 &loopRunning](const QString &) {
                    compositor.resetExecution();
                    if (loopRunning) {
                        loop.quit();
                    }
                });

            auto connRunError = QObject::connect(
                agent,
                &QSocAgent::runError,
                &loop,
                [&qout,
                 &loop,
                 &compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget,
                 &loopRunning](const QString &error) {
                    compositor.resetExecution();
                    compositor.printContent("\nError: " + error + "\n");
                    if (loopRunning) {
                        loop.quit();
                    }
                });

            /* Connect heartbeat - updates status and token display */
            auto connHeartbeat = QObject::connect(
                agent,
                &QSocAgent::heartbeat,
                &loop,
                [&compositor, &statusBarWidget, &todoWidget, &queueWidget, &inputWidget](int, int) {
                    statusBarWidget.setStatus("Working");
                });

            /* Connect token usage update */
            auto connTokens = QObject::connect(
                agent,
                &QSocAgent::tokenUsage,
                &loop,
                [&compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget](qint64 input, qint64 output) {
                    statusBarWidget.updateTokens(input, output);
                });

            /* Connect stuck detection for warning */
            auto connStuck = QObject::connect(
                agent,
                &QSocAgent::stuckDetected,
                &loop,
                [&compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget](int, int silentSeconds) {
                    statusBarWidget.setStatus(
                        QString("Working [%1s no progress]").arg(silentSeconds));
                });

            /* Connect retrying signal for user feedback */
            auto connRetry = QObject::connect(
                agent,
                &QSocAgent::retrying,
                &loop,
                [&compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget](int attempt, int maxAttempts, const QString &) {
                    statusBarWidget.setStatus(
                        QString("Retrying (%1/%2)").arg(attempt).arg(maxAttempts));
                });

            /* Connect compacting signal for context compaction feedback */
            auto connCompact = QObject::connect(
                agent,
                &QSocAgent::compacting,
                &loop,
                [&compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget](int layer, int before, int after) {
                    statusBarWidget.setStatus(
                        QString("Compacting L%1: %2->%3 tokens").arg(layer).arg(before).arg(after));
                });

            /* Connect abort signal */
            auto connAborted = QObject::connect(
                agent,
                &QSocAgent::runAborted,
                &loop,
                [&qout,
                 &loop,
                 &compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget,
                 &loopRunning](const QString &) {
                    compositor.resetExecution();
                    compositor.printContent("\n(interrupted)\n");
                    if (loopRunning) {
                        loop.quit();
                    }
                });

            /* Use existing inputMonitor for ESC/Ctrl+C during execution */
            auto &escMonitor  = inputMonitor;
            auto  connEscExec = QObject::connect(
                &escMonitor, &QAgentInputMonitor::escPressed, agent, &QSocAgent::abort);
            auto connCtrlC = QObject::connect(
                &escMonitor,
                &QAgentInputMonitor::ctrlCPressed,
                agent,
                [agent, &compositor, &escMonitor]() {
                    if (checkDoubleInterrupt()) {
                        escMonitor.stop();
                        compositor.stop();
                        _exit(130);
                    }
                    agent->abort();
                });

            /* Connect input queuing signals */
            auto connInputReady = QObject::connect(
                &escMonitor,
                &QAgentInputMonitor::inputReady,
                agent,
                [agent,
                 &compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget,
                 &escMonitor,
                 &qout,
                 llm = this->llmService](const QString &text) {
                    if (text.startsWith("!")) {
                        QString shellCmd = text.mid(1).trimmed();
                        if (!shellCmd.isEmpty()) {
                            compositor.pause();
                            runShellEscape(shellCmd, true);
                            compositor.resume();
                        }
                        return;
                    }
                    if (text.startsWith("/model")) {
                        handleModelCommand(text, qout, llm, agent, false);
                        return;
                    }
                    if (text.startsWith("/effort")) {
                        QString level = text.mid(7).trimmed().toLower();
                        if (level.isEmpty()) {
                            QSocAgentConfig cfg = agent->getConfig();
                            QString         info
                                = QString("\nEffort: %1")
                                      .arg(cfg.effortLevel.isEmpty() ? "off" : cfg.effortLevel);
                            if (!cfg.reasoningModel.isEmpty()) {
                                info += QString(" (model: %1)").arg(cfg.reasoningModel);
                            }
                            info += "\n";
                            compositor.printContent(info);
                        } else if (level == "off") {
                            agent->setEffortLevel(QString());
                            statusBarWidget.setEffortLevel(QString());
                            compositor.printContent("\nEffort: off\n");
                        } else if (level == "low" || level == "medium" || level == "high") {
                            agent->setEffortLevel(level);
                            statusBarWidget.setEffortLevel(level);
                            compositor.printContent(QString("\nEffort: %1\n").arg(level));
                        } else {
                            compositor.printContent("\nUsage: /effort [off|low|medium|high]\n");
                        }
                        return;
                    }
                    agent->queueRequest(text);
                    queueWidget.addRequest(text);
                });
            auto connProcessingQueued = QObject::connect(
                agent,
                &QSocAgent::processingQueuedRequest,
                &compositor,
                [&compositor,
                 &statusBarWidget,
                 &todoWidget,
                 &queueWidget,
                 &inputWidget](const QString &request, int) {
                    queueWidget.removeRequest(request);
                    compositor.printContent(QString("\n> %1\n").arg(request));
                });
            auto connInputChanged = QObject::connect(
                &escMonitor,
                &QAgentInputMonitor::inputChanged,
                &compositor,
                [&inputWidget](const QString &text) { inputWidget.setText(text); });

            /* Start status line and agent */
            statusBarWidget.setEffortLevel(agent->getConfig().effortLevel);
            statusBarWidget.setStatus("Reasoning");
            statusBarWidget.startTimers();
            {
                QString mid = llmService->getCurrentModelId();
                compositor.setTitle("QSoC Agent -- " + (mid.isEmpty() ? "default" : mid));
            }
            compositor.start();
            agent->runStream(input);
            loop.exec();
            loopRunning = false;

            /* Save conversation after each interaction */
            saveConversation(agent, projectManager);

            /* Display complete result at once */
            if (!finalResult.isEmpty()) {
                compositor.printContent("\n" + finalResult + "\n");
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
            QObject::disconnect(connCompact);
            QObject::disconnect(connAborted);
            QObject::disconnect(connCtrlC);
            QObject::disconnect(connInputReady);
            QObject::disconnect(connProcessingQueued);
            QObject::disconnect(connInputChanged);
        }
    }

    /* Stop TUI and restore terminal */
    inputMonitor.stop();
    compositor.stop();

    return true;
}
