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
#include "cli/qagentcompletion.h"
#include "cli/qagenthistorysearch.h"
#include "cli/qagentinputmonitor.h"
#include "cli/qsocexternaleditor.h"
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
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
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
        /* Apply model's default effort level */
        agentCfg.effortLevel = cfg.effort;
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

    /* Sync context budget and effort from model registry */
    {
        QString modelId = llmService->getCurrentModelId();
        if (!modelId.isEmpty()) {
            LLMModelConfig modelCfg = llmService->getModelConfig(modelId);
            if (modelCfg.contextTokens > 0) {
                config.maxContextTokens = modelCfg.contextTokens;
            }
            if (!modelCfg.effort.isEmpty()) {
                config.effortLevel = modelCfg.effort;
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
    auto          &popupWidget     = compositor.completionPopup();

    /* Discoverability hint shown when the input line is empty. */
    inputWidget.setPlaceholder(
        QStringLiteral("try /help, @file, !shell, Ctrl+R to search, Ctrl+X Ctrl+E to edit"));

    /* File completion engine for '@file' references */
    QAgentCompletionEngine completionEngine;

    /* Set title and start full-screen TUI */
    {
        QString mid = llmService->getCurrentModelId();
        compositor.setTitle("QSoC Agent -- " + (mid.isEmpty() ? "default" : mid));
    }
    statusBarWidget.setStatus("Ready");
    statusBarWidget.setModel(llmService->getCurrentModelId());
    statusBarWidget.setEffortLevel(agent->getConfig().effortLevel);
    compositor.start();

    /* Show welcome banner in scroll view */
    compositor.printContent("QSoC Agent - Interactive AI Assistant for SoC Design\n");
    compositor.printContent("Type 'exit' to exit, '/help' for commands\n");
    if (streaming) {
        compositor.printContent("(Enhanced mode, streaming enabled)\n");
    }

    /* Connect mouse wheel from input monitor to compositor scroll */
    QAgentInputMonitor inputMonitor(this);
    /* Teach the monitor to treat "[Pasted text #N +M lines]" chip labels as
     * atomic glyphs so Backspace / Delete never leave a half-eaten chip. */
    inputMonitor.setAtomicPattern(
        QRegularExpression(QStringLiteral(R"(\[Pasted text #\d+(?: \+\d+ lines)?\])")));
    connect(&inputMonitor, &QAgentInputMonitor::mouseWheel, [&compositor](int direction) {
        if (direction == 0) {
            compositor.scrollContentUp(3);
        } else {
            compositor.scrollContentDown(3);
        }
    });

    /* Input history + reverse-i-search state — declared early so lambdas
     * below can capture them. The history file load happens after the
     * conversation restore (further down). inputHistoryPastes is aligned
     * by index with inputHistory and holds the paste content map that was
     * in scope when each entry was submitted, so chips survive a restart. */
    QStringList               inputHistory;
    QList<QMap<int, QString>> inputHistoryPastes;
    bool                      searching = false;
    QString                   searchOriginal;
    int                       searchOriginalCursor = 0;
    QString                   searchQuery; /* Live query text inside reverse-i-search mode */
    QString                   searchCurrentMatch;
    bool                      searchFailed = false;
    QAgentHistorySearch       historySearch{inputHistory};

    /* Paste chip state: pastedContents stores the full text of every chip
     * currently referenced in the input buffer. Chips are session-local —
     * not persisted across restarts. nextPasteId assigns monotonic ids so
     * two pastes never collide inside the same session. */
    QMap<int, QString>   pastedContents;
    int                  nextPasteId         = 1;
    static constexpr int PASTE_CHIP_CHAR_MIN = 500;
    static constexpr int PASTE_CHIP_LINE_MIN = 4;

    /* Helper: expand "[Pasted text #N +M lines]" chips inline with the
     * stored content from pastedContents. Unknown ids (e.g. recalled from
     * history in a later session) fall through as the literal chip text. */
    auto expandPasteChips = [&pastedContents](const QString &text) -> QString {
        static const QRegularExpression chipRe(
            QStringLiteral(R"(\[Pasted text #(\d+) \+\d+ lines\])"));
        auto matchIter = chipRe.globalMatch(text);
        if (!matchIter.hasNext()) {
            return text;
        }
        QString expanded;
        int     pos = 0;
        while (matchIter.hasNext()) {
            auto match = matchIter.next();
            expanded += text.mid(pos, match.capturedStart(0) - pos);
            bool parsed  = false;
            int  pasteId = match.captured(1).toInt(&parsed);
            if (parsed && pastedContents.contains(pasteId)) {
                expanded += pastedContents.value(pasteId);
            } else {
                expanded += match.captured(0);
            }
            pos = match.capturedEnd(0);
        }
        expanded += text.mid(pos);
        return expanded;
    };

    /* Connect live input display (text + cursor position).
     * In reverse-i-search mode, the monitor's buffer holds the live search
     * query — we route it through the search engine and drive the input
     * line's search-mode rendering instead of the normal text display. */
    connect(
        &inputMonitor,
        &QAgentInputMonitor::inputChanged,
        [&inputWidget,
         &inputMonitor,
         &searching,
         &historySearch,
         &searchQuery,
         &searchCurrentMatch,
         &searchFailed,
         &compositor](const QString &text) {
            if (searching) {
                searchQuery = text;
                historySearch.rewind();
                auto match         = historySearch.findNext(text);
                searchCurrentMatch = match.text;
                searchFailed       = (match.index < 0) && !text.isEmpty();
                inputWidget.setSearchMode(true, text, searchCurrentMatch, searchFailed);
                compositor.invalidate();
                return;
            }
            inputWidget.setText(text);
            inputWidget.setCursorPos(inputMonitor.getCursorPos());
        });

    /* Completion popup state. The popup widget is single-instance and hosts
     * either '@file' or '/slash' completion at a time — popupKind tracks
     * which one owns the current session so submitBlockedKey knows which
     * accept path to take. dismissedFor remembers the exact input that was
     * dismissed via Esc so the popup doesn't auto-reopen. */
    enum class PopupKind : std::uint8_t {
        None,
        AtFile,
        SlashCommand,
    };
    PopupKind popupKind = PopupKind::None;
    QString   dismissedFor;
    int       popupAtPos = -1; /* '@' position for AtFile, 0 for SlashCommand */

    /* Static slash command table for tab completion. Keep in sync with the
     * command dispatch below. */
    const QStringList slashCommands
        = {QStringLiteral("help"),
           QStringLiteral("clear"),
           QStringLiteral("compact"),
           QStringLiteral("effort"),
           QStringLiteral("model"),
           QStringLiteral("exit"),
           QStringLiteral("quit")};

    /* Helper: detect '/<word>' at start of buffer with cursor inside the
     * command word (no whitespace between '/' and cursor). Returns the
     * partial command after '/' via outQuery. */
    auto detectSlashCommand = [](const QString &buf, int cursor, QString &outQuery) -> bool {
        outQuery.clear();
        if (buf.isEmpty() || buf[0] != QLatin1Char('/')) {
            return false;
        }
        int limit = qMin(cursor, static_cast<int>(buf.size()));
        for (int idx = 1; idx < limit; idx++) {
            QChar chr = buf[idx];
            if (chr == QLatin1Char(' ') || chr == QLatin1Char('\n') || chr == QLatin1Char('\t')) {
                return false;
            }
        }
        outQuery = buf.mid(1, limit - 1);
        return true;
    };

    /* Helper: detect '@<token>' ending at cursorPos. Returns atPos >= 0 on match. */
    auto detectAtToken = [](const QString &buf, int cursor, int &outAtPos, QString &outQuery) {
        outAtPos = -1;
        outQuery.clear();
        if (cursor <= 0 || buf.isEmpty()) {
            return false;
        }
        /* Walk back from cursor to find '@' or a token-invalid char. */
        int scan = cursor - 1;
        while (scan >= 0) {
            QChar chr = buf[scan];
            if (chr == QLatin1Char('@')) {
                break;
            }
            /* Whitespace or newline: '@' not in current token */
            if (chr == QLatin1Char(' ') || chr == QLatin1Char('\n') || chr == QLatin1Char('\t')) {
                return false;
            }
            scan--;
        }
        if (scan < 0) {
            return false;
        }
        /* '@' must be at buffer start or preceded by whitespace/newline */
        bool atBoundary = (scan == 0) || buf[scan - 1] == QLatin1Char(' ')
                          || buf[scan - 1] == QLatin1Char('\n');
        if (!atBoundary) {
            return false;
        }
        outAtPos = scan;
        outQuery = buf.mid(scan + 1, cursor - scan - 1);
        return true;
    };

    /* Completion popup: refresh on every inputChanged */
    connect(
        &inputMonitor,
        &QAgentInputMonitor::inputChanged,
        [this,
         &inputMonitor,
         &popupWidget,
         &completionEngine,
         &compositor,
         &popupKind,
         &popupAtPos,
         &dismissedFor,
         &slashCommands,
         detectSlashCommand,
         detectAtToken](const QString &text) {
            /* Helper: close whichever popup is currently showing. */
            auto closePopup = [&]() {
                if (popupWidget.isVisible()) {
                    popupWidget.setVisible(false);
                    popupKind  = PopupKind::None;
                    popupAtPos = -1;
                    inputMonitor.setSubmitBlocked(false);
                    compositor.invalidate();
                }
            };

            /* Skip if user dismissed popup for this exact input via Esc */
            if (dismissedFor == text) {
                return;
            }

            /* Slash command completion (highest priority, position 0 only) */
            QString slashQuery;
            if (detectSlashCommand(text, inputMonitor.getCursorPos(), slashQuery)) {
                QStringList matches;
                for (const QString &cmd : slashCommands) {
                    if (cmd.startsWith(slashQuery, Qt::CaseInsensitive)) {
                        matches.append(cmd);
                    }
                }
                if (matches.isEmpty()) {
                    closePopup();
                    return;
                }
                popupWidget.setTitle(QStringLiteral("/command"));
                popupWidget.setItems(matches);
                popupWidget.setHighlight(0);
                popupWidget.setVisible(true);
                popupKind  = PopupKind::SlashCommand;
                popupAtPos = 0;
                inputMonitor.setSubmitBlocked(true);
                compositor.invalidate();
                return;
            }

            /* bash mode (leading '!'): no '@' completion */
            if (text.startsWith(QLatin1Char('!'))) {
                closePopup();
                return;
            }
            int     atPos = -1;
            QString query;
            bool    found = detectAtToken(text, inputMonitor.getCursorPos(), atPos, query);
            if (!found) {
                closePopup();
                return;
            }

            QString projectPath = projectManager->getProjectPath();
            if (projectPath.isEmpty()) {
                projectPath = QDir::currentPath();
            }
            QStringList matches = completionEngine.complete(projectPath, query, 50);
            if (matches.isEmpty()) {
                closePopup();
                return;
            }

            popupWidget.setTitle(QStringLiteral("@file"));
            popupWidget.setItems(matches);
            popupWidget.setHighlight(0);
            popupWidget.setVisible(true);
            popupKind  = PopupKind::AtFile;
            popupAtPos = atPos;
            inputMonitor.setSubmitBlocked(true);
            compositor.invalidate();
        });

    /* Start input monitor (raw mode for entire REPL lifetime) */
    inputMonitor.start();

    /* Load previous conversation if available */
    if (loadConversation(agent, projectManager)) {
        int msgCount = static_cast<int>(agent->getMessages().size());
        compositor.printContent(
            QString("(Loaded %1 messages from previous session)\n\n").arg(msgCount));
    }

    /* Input history navigation position (inputHistory itself is declared above
     * so the search lambdas can capture it). */
    int     historyPos = -1; /* -1 = not browsing, 0 = most recent */
    QString savedInput;      /* Input buffer before browsing */

    /* Load history from .qsoc/history.jsonl. Each line is a JSON object
     * {"display": "<chip form>", "pastes": {"<id>": "<content>"}}. Missing
     * or malformed lines are skipped silently — history is advisory, not a
     * source of truth we want to hard-fail on. */
    {
        QString projectPath = projectManager->getProjectPath();
        if (!projectPath.isEmpty()) {
            QFile histFile(QDir(projectPath).filePath(".qsoc/history.jsonl"));
            if (histFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                QTextStream stream(&histFile);
                while (!stream.atEnd()) {
                    QString line = stream.readLine();
                    if (line.isEmpty()) {
                        continue;
                    }
                    QJsonParseError err{};
                    QJsonDocument   doc = QJsonDocument::fromJson(line.toUtf8(), &err);
                    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
                        continue;
                    }
                    QJsonObject obj     = doc.object();
                    QString     display = obj.value(QStringLiteral("display")).toString();
                    if (display.isEmpty()) {
                        continue;
                    }
                    QMap<int, QString> pastes;
                    QJsonObject        pastesObj = obj.value(QStringLiteral("pastes")).toObject();
                    for (auto pIt = pastesObj.begin(); pIt != pastesObj.end(); ++pIt) {
                        bool ok      = false;
                        int  pasteId = pIt.key().toInt(&ok);
                        if (ok) {
                            pastes[pasteId] = pIt.value().toString();
                        }
                    }
                    inputHistory.append(display);
                    inputHistoryPastes.append(pastes);
                }
            }
        }
    }

    /* Connect arrow keys: when completion popup is visible, Up/Down navigate
     * the popup (wrapping); otherwise they walk the input history. Arrow
     * keys while in reverse-i-search mode accept the current match and
     * exit so the user can edit it further. */
    connect(&inputMonitor, &QAgentInputMonitor::arrowKey, [&](int key) {
        if (searching) {
            /* Accept whatever is currently matched and leave search mode so
             * the monitor resumes normal editing with the selection loaded. */
            searching = false;
            inputWidget.setSearchMode(false, QString(), QString(), false);
            inputMonitor.setSubmitBlocked(false);
            if (!searchCurrentMatch.isEmpty()) {
                inputMonitor.setInputBuffer(searchCurrentMatch);
            } else {
                inputMonitor.setInputBuffer(searchOriginal);
            }
            searchCurrentMatch.clear();
            searchFailed = false;
            compositor.invalidate();
            /* Fall through so Left/Right still move cursor after exit. */
        }
        if (popupWidget.isVisible()) {
            if (key == 'A') {
                popupWidget.moveHighlight(-1);
                compositor.invalidate();
            } else if (key == 'B') {
                popupWidget.moveHighlight(1);
                compositor.invalidate();
            }
            return;
        }
        /* Helper: pull history entry `idx` into the input buffer with paste
         * content recovery. Each persisted paste id is renumbered to a
         * fresh session-local id so recalled entries never clash with
         * in-session pastedContents. The chip labels in `display` are
         * rewritten with the new ids via a single regex pass. */
        auto recallHistoryEntry = [&](int idx) {
            const QString &display = inputHistory[idx];
            if (idx < 0 || idx >= inputHistoryPastes.size()) {
                inputMonitor.setInputBuffer(display);
                return;
            }
            const QMap<int, QString> &storedPastes = inputHistoryPastes[idx];
            if (storedPastes.isEmpty()) {
                inputMonitor.setInputBuffer(display);
                return;
            }
            /* Allocate fresh ids and fill pastedContents. */
            QMap<int, int> idRemap;
            for (auto pIt = storedPastes.begin(); pIt != storedPastes.end(); ++pIt) {
                int newId             = nextPasteId++;
                idRemap[pIt.key()]    = newId;
                pastedContents[newId] = pIt.value();
            }
            /* Single regex pass rewrites each chip to use the new id. */
            static const QRegularExpression chipRewriteRe(
                QStringLiteral(R"(\[Pasted text #(\d+)((?: \+\d+ lines)?)\])"));
            QString rewritten;
            int     pos       = 0;
            auto    matchIter = chipRewriteRe.globalMatch(display);
            while (matchIter.hasNext()) {
                auto match = matchIter.next();
                rewritten += display.mid(pos, match.capturedStart(0) - pos);
                bool parsed = false;
                int  oldId  = match.captured(1).toInt(&parsed);
                if (parsed && idRemap.contains(oldId)) {
                    rewritten += QStringLiteral("[Pasted text #%1%2]")
                                     .arg(idRemap.value(oldId))
                                     .arg(match.captured(2));
                } else {
                    rewritten += match.captured(0);
                }
                pos = match.capturedEnd(0);
            }
            rewritten += display.mid(pos);
            inputMonitor.setInputBuffer(rewritten);
        };

        if (key == 'A') { /* Up */
            if (inputHistory.isEmpty()) {
                return;
            }
            if (historyPos < 0) {
                savedInput = inputWidget.getText().isEmpty() ? QString() : inputWidget.getText();
                historyPos = 0;
            } else if (historyPos < inputHistory.size() - 1) {
                historyPos++;
            }
            int idx = static_cast<int>(inputHistory.size()) - 1 - historyPos;
            recallHistoryEntry(idx);
        } else if (key == 'B') { /* Down */
            if (historyPos < 0) {
                return;
            }
            historyPos--;
            if (historyPos < 0) {
                inputMonitor.setInputBuffer(savedInput);
            } else {
                int idx = static_cast<int>(inputHistory.size()) - 1 - historyPos;
                recallHistoryEntry(idx);
            }
        }
    });

    /* submitBlockedKey: Enter or Tab pressed while the input monitor is
     * blocked. Routes to either the completion popup confirmation or the
     * reverse-i-search accept path depending on which mode is active. */
    connect(&inputMonitor, &QAgentInputMonitor::submitBlockedKey, [&, this](int /*key*/) {
        if (searching) {
            /* Accept the current match and exit search mode. The loaded
             * text lands in the buffer but is NOT auto-submitted so the
             * user can edit before pressing Enter again. */
            searching = false;
            inputWidget.setSearchMode(false, QString(), QString(), false);
            inputMonitor.setSubmitBlocked(false);
            if (!searchCurrentMatch.isEmpty()) {
                inputMonitor.setInputBuffer(searchCurrentMatch);
            } else {
                inputMonitor.setInputBuffer(searchOriginal);
            }
            searchCurrentMatch.clear();
            searchFailed = false;
            compositor.invalidate();
            compositor.render();
            return;
        }
        if (!popupWidget.isVisible() || popupWidget.getItems().isEmpty() || popupAtPos < 0) {
            return;
        }
        int         idx   = popupWidget.getHighlight();
        QStringList items = popupWidget.getItems();
        if (idx < 0 || idx >= items.size()) {
            return;
        }
        const QString &picked = items[idx];

        if (popupKind == PopupKind::SlashCommand) {
            /* Replace the partial "/query" with "/picked " so the user can
             * then type arguments (if any) or press Enter to submit. */
            const QString current = inputWidget.getText();
            int           cursor  = inputMonitor.getCursorPos();
            int           end     = cursor;
            /* Find the end of the command word (first whitespace after '/') */
            while (end < current.size() && current[end] != QLatin1Char(' ')
                   && current[end] != QLatin1Char('\n')) {
                end++;
            }
            QString before  = current.left(0); /* buffer up to '/' == "" */
            QString after   = current.mid(end);
            QString rebuilt = QLatin1Char('/') + picked + QLatin1Char(' ') + after;
            inputMonitor.setInputBuffer(rebuilt);
        } else {
            /* AtFile: decide trailing char: '/' if directory, else ' '. */
            QChar   trailing    = QLatin1Char(' ');
            QString projectPath = projectManager->getProjectPath();
            if (projectPath.isEmpty()) {
                projectPath = QDir::currentPath();
            }
            QFileInfo info(QDir(projectPath).filePath(picked));
            if (info.isDir()) {
                trailing = QLatin1Char('/');
            }
            inputMonitor.insertCompletion(popupAtPos, picked, trailing);
        }

        /* Hide popup — inputChanged will fire from the buffer rewrite and
         * re-evaluate both detection paths; popup can re-open naturally
         * if the new text still matches a prefix. */
        popupWidget.setVisible(false);
        popupKind  = PopupKind::None;
        popupAtPos = -1;
        inputMonitor.setSubmitBlocked(false);
        compositor.invalidate();
    });

    /* Ctrl+R reverse-i-search: first press enters search mode, subsequent
     * presses with the same query advance to the next older unique match. */
    connect(&inputMonitor, &QAgentInputMonitor::historySearchRequested, [&]() {
        if (!searching) {
            /* Enter search mode: snapshot original, clear monitor buffer,
             * block Enter so hitting submit accepts the match instead. */
            searching            = true;
            searchOriginal       = inputWidget.getText();
            searchOriginalCursor = inputMonitor.getCursorPos();
            searchQuery.clear();
            searchCurrentMatch.clear();
            searchFailed = false;
            historySearch.rewind();
            inputWidget.setSearchMode(true, QString(), QString(), false);
            inputMonitor.setInputBuffer(QString());
            inputMonitor.setSubmitBlocked(true);
            compositor.invalidate();
            compositor.render();
            return;
        }
        /* Already searching: advance to the next older unique match for the
         * current query. The monitor's buffer holds the live query; we keep
         * a mirror in searchQuery via the inputChanged hook above. */
        auto match = historySearch.findNext(searchQuery);
        if (match.index >= 0) {
            searchCurrentMatch = match.text;
            searchFailed       = false;
        } else {
            searchFailed = true;
        }
        inputWidget.setSearchMode(true, searchQuery, searchCurrentMatch, searchFailed);
        compositor.invalidate();
        compositor.render();
    });

    /* Ctrl+T: toggle the TODO list visibility. */
    connect(&inputMonitor, &QAgentInputMonitor::toggleTodosRequested, [&]() {
        todoWidget.setVisible(!todoWidget.isVisible());
        compositor.invalidate();
        compositor.render();
    });

    /* Ctrl+L: force a full compositor repaint — readline-style clear-and-redraw. */
    connect(&inputMonitor, &QAgentInputMonitor::redrawRequested, [&]() {
        compositor.invalidate();
        compositor.render();
    });

    /* Bracketed paste: decide between literal insert and a chip reference.
     * Short pastes (< PASTE_CHIP_CHAR_MIN chars AND < PASTE_CHIP_LINE_MIN
     * lines) drop into the buffer as-is. Large pastes get a monotonic id
     * allocated, the full content stashed in pastedContents, and only a
     * "[Pasted text #N +M lines]" label inserted so the TUI layout stays
     * readable. Chips expand back to the full content at submission time. */
    connect(&inputMonitor, &QAgentInputMonitor::pastedReceived, [&](const QString &text) {
        if (text.isEmpty()) {
            return;
        }
        int  lineCount = static_cast<int>(text.count(QLatin1Char('\n'))) + 1;
        bool big       = (text.size() >= PASTE_CHIP_CHAR_MIN) || (lineCount > PASTE_CHIP_LINE_MIN);
        if (!big) {
            inputMonitor.insertText(text);
            return;
        }
        int pasteId             = nextPasteId++;
        pastedContents[pasteId] = text;
        QString chip = QStringLiteral("[Pasted text #%1 +%2 lines]").arg(pasteId).arg(lineCount);
        inputMonitor.insertText(chip);
    });

    /* External editor (Ctrl+X Ctrl+E or Ctrl+G): pause the TUI, hand off
     * to $EDITOR with the current input text, and reload the edited result
     * into the input buffer when the editor exits. Paste chips are expanded
     * into the tempfile so the user can actually edit their pasted content;
     * the edited return text replaces the buffer as-is — chips are not
     * reconstructed, so the user sees (and commits) the real payload. */
    connect(&inputMonitor, &QAgentInputMonitor::externalEditorRequested, [&]() {
        const QString rawBuffer = inputWidget.getText();
        const QString current   = expandPasteChips(rawBuffer);

        /* Tear down the alt-screen and raw-mode monitor so the editor owns
         * the terminal. Mirrors the '!<cmd>' shell-escape dance. */
        compositor.pause();
        inputMonitor.stop();

        QString editedText;
        QString errMessage;
        bool    success = QSocExternalEditor::editText(current, editedText, errMessage);

        /* Restore TUI before reporting anything so output lands in the
         * scroll view rather than the cooked terminal. */
        inputMonitor.start();
        compositor.resume();

        if (success) {
            inputMonitor.setInputBuffer(editedText);
        } else {
            /* Restore the ORIGINAL chip-containing buffer (not the expanded
             * form) so a failed editor invocation is a pure no-op — we don't
             * want to silently expand chips when the user just bailed out. */
            inputMonitor.setInputBuffer(rawBuffer);
            if (!errMessage.isEmpty()) {
                compositor.printContent(
                    QStringLiteral("External editor: ") + errMessage + QLatin1Char('\n'));
            }
        }
        compositor.invalidate();
        compositor.render();
    });

    /* Main loop */
    bool exitRequested = false;

    while (!exitRequested) {
        QEventLoop promptLoop;
        QString    input;
        historyPos = -1;

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
            [&promptLoop,
             &exitRequested,
             &compositor,
             &searching,
             &inputMonitor,
             &inputWidget,
             &searchCurrentMatch,
             &searchFailed]() {
                /* Ctrl+C in reverse-i-search mode cancels the search in
                 * addition to interrupting. Mirrors readline behavior. */
                if (searching) {
                    searching = false;
                    inputWidget.setSearchMode(false, QString(), QString(), false);
                    inputMonitor.setSubmitBlocked(false);
                    searchCurrentMatch.clear();
                    searchFailed = false;
                }
                if (checkDoubleInterrupt()) {
                    exitRequested = true;
                    promptLoop.quit();
                } else {
                    compositor.printContent("\n^C\n");
                    promptLoop.quit();
                }
            });
        auto connEsc = connect(
            &inputMonitor,
            &QAgentInputMonitor::escPressed,
            [&promptLoop,
             &popupWidget,
             &popupKind,
             &popupAtPos,
             &inputMonitor,
             &inputWidget,
             &dismissedFor,
             &compositor,
             &searching,
             &searchOriginal,
             &searchCurrentMatch,
             &searchFailed]() {
                /* Reverse-i-search takes priority: Esc restores the original
                 * input the user had before pressing Ctrl+R and leaves the
                 * prompt intact so the loop keeps running. */
                if (searching) {
                    searching = false;
                    inputWidget.setSearchMode(false, QString(), QString(), false);
                    inputMonitor.setSubmitBlocked(false);
                    inputMonitor.setInputBuffer(searchOriginal);
                    searchCurrentMatch.clear();
                    searchFailed = false;
                    compositor.invalidate();
                    return;
                }
                /* If popup is open, Esc closes the popup and remembers this
                 * input so it doesn't auto-reopen until the user types more. */
                if (popupWidget.isVisible()) {
                    dismissedFor = inputWidget.getText();
                    popupWidget.setVisible(false);
                    popupKind  = PopupKind::None;
                    popupAtPos = -1;
                    inputMonitor.setSubmitBlocked(false);
                    compositor.invalidate();
                    return;
                }
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

        /* Add to history (skip duplicates of last entry). Saved in CHIP form
         * with any referenced paste payloads captured alongside so chips
         * survive a restart. The chip form is what goes into both the
         * in-memory list and the JSONL file; the expansion below only
         * affects the downstream submission copy. */
        if (inputHistory.isEmpty() || inputHistory.last() != input) {
            /* Extract the pastes referenced by the current input. */
            QMap<int, QString> entryPastes;
            {
                static const QRegularExpression chipExtractRe(
                    QStringLiteral(R"(\[Pasted text #(\d+)(?: \+\d+ lines)?\])"));
                auto matchIter = chipExtractRe.globalMatch(input);
                while (matchIter.hasNext()) {
                    auto match   = matchIter.next();
                    bool parsed  = false;
                    int  pasteId = match.captured(1).toInt(&parsed);
                    if (parsed && pastedContents.contains(pasteId)) {
                        entryPastes[pasteId] = pastedContents.value(pasteId);
                    }
                }
            }

            inputHistory.append(input);
            inputHistoryPastes.append(entryPastes);

            /* Persist to .qsoc/history.jsonl as one JSON object per line. */
            QString projectPath = projectManager->getProjectPath();
            if (!projectPath.isEmpty()) {
                QString histDir = QDir(projectPath).filePath(".qsoc");
                QDir(histDir).mkpath(".");
                QFile histFile(QDir(histDir).filePath("history.jsonl"));
                if (histFile.open(QIODevice::Append)) {
                    QJsonObject obj;
                    obj[QStringLiteral("display")] = input;
                    QJsonObject pastesJson;
                    for (auto pIt = entryPastes.begin(); pIt != entryPastes.end(); ++pIt) {
                        pastesJson[QString::number(pIt.key())] = pIt.value();
                    }
                    obj[QStringLiteral("pastes")] = pastesJson;
                    QJsonDocument doc(obj);
                    histFile.write(doc.toJson(QJsonDocument::Compact));
                    histFile.write("\n");
                }
            }
        }

        /* From this point on, `input` is the fully expanded submission text
         * that downstream code (shell escape, agent runStream) consumes. */
        input = expandPasteChips(input);

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
            compositor.printContent("  Left/Right  - Move cursor\n");
            compositor.printContent("  Ctrl+A/E    - Move to start/end of line\n");
            compositor.printContent("  Ctrl+K      - Delete to end of line\n");
            compositor.printContent("  Ctrl+U      - Delete to start of line\n");
            compositor.printContent("  Ctrl+W      - Delete previous word\n");
            compositor.printContent("  Backspace   - Delete character before cursor\n");
            compositor.printContent("  \\ + Enter   - Continue on next line\n");
            compositor.printContent("  (paste)     - Multi-line paste preserved as one input\n");
            compositor.printContent("  Ctrl+X Ctrl+E or Ctrl+G - Edit current input in $EDITOR\n");
            compositor.printContent("  Ctrl+R      - Reverse-i-search through prompt history\n");
            compositor.printContent("  Ctrl+T      - Toggle TODO list visibility\n");
            compositor.printContent("  Ctrl+L      - Force a full screen repaint\n");
            compositor.printContent("  Ctrl+_      - Undo the last edit\n");
            compositor.printContent("  @<name>     - Fuzzy-complete a project file path\n");
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
            QString level = input.mid(7).trimmed().toLower();

            if (level.isEmpty()) {
                /* Interactive menu */
                QStringList options = {"off", "low", "medium", "high"};
                QString     current = agent->getConfig().effortLevel;
                if (current.isEmpty()) {
                    current = "off";
                }

                QList<QTuiMenu::MenuItem> items;
                for (const QString &opt : options) {
                    QTuiMenu::MenuItem item;
                    item.label  = opt;
                    item.marked = (opt == current);
                    items.append(item);
                }

                QTuiMenu menu;
                menu.setTitle("Reasoning Effort");
                menu.setItems(items);
                menu.setHighlight(options.indexOf(current));

                int selected = menu.exec();
                if (selected >= 0 && selected < options.size()) {
                    level = options[selected];
                } else {
                    compositor.invalidate();
                    compositor.render();
                    continue; /* Cancelled */
                }
                compositor.invalidate();
            }

            if (level == "off") {
                agent->setEffortLevel(QString());
                statusBarWidget.setEffortLevel(QString());
                compositor.printContent("Effort: off\n");
            } else if (level == "low" || level == "medium" || level == "high") {
                agent->setEffortLevel(level);
                statusBarWidget.setEffortLevel(level);
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
                statusBarWidget.setEffortLevel(agent->getConfig().effortLevel);
                compositor.setTitle("QSoC Agent -- " + (mid.isEmpty() ? "default" : mid));
            }
            compositor.invalidate();
            compositor.render();
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
                        todoWidget.clearDone();
                    } else if (toolName == "todo_add") {
                        auto item = parseTodoAddResult(result);
                        if (item.id >= 0) {
                            todoWidget.addItem(item);
                        }
                    } else if (toolName == "todo_update") {
                        auto [todoId, newStatus] = parseTodoUpdateResult(result);
                        if (todoId >= 0) {
                            todoWidget.updateStatus(todoId, newStatus);
                            todoWidget.clearDone();
                        }
                    } else if (toolName == "todo_delete") {
                        auto [todoId, newStatus] = parseTodoUpdateResult(result);
                        if (todoId >= 0) {
                            todoWidget.removeItem(todoId);
                        }
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

            /* Clear completed TODOs from previous query */
            todoWidget.clearDone();

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
                        todoWidget.clearDone();
                    } else if (toolName == "todo_add") {
                        auto item = parseTodoAddResult(result);
                        if (item.id >= 0) {
                            todoWidget.addItem(item);
                        }
                    } else if (toolName == "todo_update") {
                        auto [todoId, newStatus] = parseTodoUpdateResult(result);
                        if (todoId >= 0) {
                            todoWidget.updateStatus(todoId, newStatus);
                            todoWidget.clearDone();
                        }
                    } else if (toolName == "todo_delete") {
                        auto [todoId, newStatus] = parseTodoUpdateResult(result);
                        if (todoId >= 0) {
                            todoWidget.removeItem(todoId);
                        }
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

            /* Clear completed TODOs from previous query */
            todoWidget.clearDone();

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
