// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsocfilehistory.h"
#include "agent/qsocsession.h"
#include "agent/qsoctool.h"
#include "agent/remote/qsocremotebinding.h"
#include "agent/remote/qsocremotepathcontext.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshconfigparser.h"
#include "agent/remote/qsocsshexec.h"
#include "agent/remote/qsocsshsession.h"
#include "agent/remote/qsoctoolremote.h"
#include "agent/tool/qsoctoolbus.h"
#include "agent/tool/qsoctooldoc.h"
#include "agent/tool/qsoctoolfile.h"
#include "agent/tool/qsoctoolgenerate.h"
#include "agent/tool/qsoctoollsp.h"
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
#include "common/qlspconfigloader.h"
#include "common/qlspservice.h"
#include "common/qlspslangbackend.h"
#include "common/qsocconsole.h"
#include "common/qsoclinediff.h"
#include "common/qsocpaths.h"
#include "tui/qtuicompositor.h"
#include "tui/qtuiinputline.h"
#include "tui/qtuimenu.h"
#include "tui/qtuipathpicker.h"
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
#ifdef Q_OS_UNIX
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif
#ifdef Q_OS_WIN
#include <windows.h>
#endif
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
#ifdef Q_OS_UNIX
    struct sigaction sigact = {};
    sigact.sa_handler       = sigintHandler;
    sigemptyset(&sigact.sa_mask);
    sigact.sa_flags = 0;
    sigaction(SIGINT, &sigact, nullptr);
#else
    std::signal(SIGINT, sigintHandler);
#endif
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
            configPath = QDir(QSocPaths::userRoot()).filePath("qsoc.yml");
        }
        /* Read, update llm.model, write back */
        try {
            YAML::Node root      = YAML::LoadFile(configPath.toStdString());
            root["llm"]["model"] = modelId.toStdString();
            std::ofstream fout(configPath.toStdString());
            fout << root;
        } catch (const YAML::Exception &err) {
            QSocConsole::warn() << "Failed to save model to config:" << err.what();
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
        menu.setSearchable(true);

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
/**
 * @brief Run a shell command and return its combined stdout+stderr output.
 * @details Uses popen() so the output can be captured and displayed in the
 *          scrollView after the compositor resumes the alt-screen. Without
 *          capture, any output written to stdout during compositor.pause()
 *          would be erased the moment resume() switches back to the
 *          alt-screen buffer.
 */
QString runShellEscape(const QString &command)
{
    QString result;
    /* Redirect stderr to stdout so error messages are also captured. */
#ifdef Q_OS_WIN
    const QByteArray cmd  = (command + " 2>&1").toLocal8Bit();
    FILE            *pipe = _popen(cmd.constData(), "r");
#else
    const QByteArray cmd  = (command + " 2>&1").toLocal8Bit();
    FILE            *pipe = popen(cmd.constData(), "r");
#endif
    if (pipe == nullptr) {
        return QStringLiteral("(failed to run command)\n");
    }
    char buf[4096];
    while (fgets(buf, sizeof(buf), pipe) != nullptr) {
        result += QString::fromLocal8Bit(buf);
    }
#ifdef Q_OS_WIN
    int ret = _pclose(pipe);
#else
    int ret = pclose(pipe);
#endif
#ifdef Q_OS_UNIX
    if (WIFEXITED(ret)) {
        int exitCode = WEXITSTATUS(ret);
        if (exitCode != 0) {
            result += QString("(exit code: %1)\n").arg(exitCode);
        }
    } else if (WIFSIGNALED(ret)) {
        result += QString("(signal: %1)\n").arg(WTERMSIG(ret));
    }
#else
    if (ret != 0) {
        result += QString("(exit code: %1)\n").arg(ret);
    }
#endif
    return result;
}

/**
 * @brief Run a `!` shell-escape command on a connected remote session.
 * @details The command runs as `cd <cwd> && /bin/bash -lc <cmd>` so working
 *          directory matches the agent's view. Stdout+stderr are concatenated
 *          the same way as the local variant, and a non-zero exit code (or
 *          abort / timeout) is appended as a footer line.
 */
QString runRemoteShellEscape(QSocSshSession &session, const QString &cwd, const QString &command)
{
    auto shellEscape = [](const QString &value) {
        QString out = QStringLiteral("'");
        for (const QChar chr : value) {
            if (chr == QLatin1Char('\'')) {
                out += QStringLiteral("'\\''");
            } else {
                out += chr;
            }
        }
        out += QLatin1Char('\'');
        return out;
    };

    const QString wrapped = QStringLiteral("cd %1 && /bin/bash -lc %2")
                                .arg(shellEscape(cwd.isEmpty() ? QStringLiteral("/") : cwd))
                                .arg(shellEscape(command));

    QSocSshExec         exec(session);
    QSocSshExec::Result res = exec.run(wrapped, 0);

    QString result = QString::fromUtf8(res.stdoutBytes);
    if (!res.stderrBytes.isEmpty()) {
        result += QString::fromUtf8(res.stderrBytes);
    }
    if (res.timedOut) {
        result += QStringLiteral("(timed out)\n");
    } else if (res.aborted) {
        result += QStringLiteral("(aborted)\n");
    } else if (res.exitCode != 0) {
        result += QStringLiteral("(exit code: %1)\n").arg(res.exitCode);
    }
    if (!res.errorText.isEmpty()) {
        result += QStringLiteral("(%1)\n").arg(res.errorText);
    }
    return result;
}

/**
 * @brief Resolve the project path for session storage.
 * @details Sessions live under <projectPath>/.qsoc/sessions so they move with
 *          the project when the user copies / renames the directory. Falls
 *          back to CWD when no project is loaded so a stray `qsoc agent` from
 *          an arbitrary directory still gets persistence.
 */
QString sessionProjectPath(QSocProjectManager *pmanager)
{
    QString projectPath = pmanager->getProjectPath();
    if (projectPath.isEmpty()) {
        projectPath = QDir::currentPath();
    }
    return projectPath;
}

/**
 * @brief Persist the messages added since lastIndex to the session JSONL.
 * @return The new persisted index (always agent->getMessages().size()).
 */
int persistSessionDelta(QSocAgent *agent, QSocSession *session, int lastIndex)
{
    if (session == nullptr) {
        return lastIndex;
    }
    const json messages = agent->getMessages();
    if (!messages.is_array()) {
        return lastIndex;
    }
    const int total = static_cast<int>(messages.size());
    for (int idx = lastIndex; idx < total; idx++) {
        session->appendMessage(messages[idx]);
    }
    return total;
}

/* qwen3 thinking streams emit `\n\n` between every paragraph, which would
 * otherwise render as a blank dim row after every step. */
class ReasoningNewlineCollapser
{
public:
    QString feed(const QString &chunk)
    {
        QString out;
        out.reserve(chunk.size());
        for (const QChar chr : chunk) {
            /* Drop CR so CRLF degrades to LF and bare CR cannot
             * carriage-return the scrollback row. */
            if (chr == QLatin1Char('\r')) {
                continue;
            }
            if (chr == QLatin1Char('\n')) {
                if (atLineStart) {
                    continue;
                }
                atLineStart = true;
            } else {
                atLineStart = false;
            }
            out.append(chr);
        }
        return out;
    }

private:
    bool atLineStart = true;
};

/* Reduce a saved user prompt to a single readable line for picker labels.
 * Collapses whitespace, replaces newlines with spaces, and substitutes a
 * placeholder when the prompt is effectively empty. Slash/bash escapes
 * never reach the message stream, but defensive fallback covers paste-only
 * prompts and historic data. */
QString cleanPromptForLabel(const QString &raw)
{
    QString text = raw;
    text.replace(QLatin1Char('\r'), QLatin1Char(' '));
    text.replace(QLatin1Char('\n'), QLatin1Char(' '));
    text.replace(QLatin1Char('\t'), QLatin1Char(' '));
    /* Collapse runs of spaces. */
    QString collapsed;
    collapsed.reserve(text.size());
    bool prevSpace = true;
    for (const QChar chr : text) {
        if (chr == QLatin1Char(' ')) {
            if (!prevSpace) {
                collapsed.append(chr);
                prevSpace = true;
            }
        } else {
            collapsed.append(chr);
            prevSpace = false;
        }
    }
    while (collapsed.endsWith(QLatin1Char(' '))) {
        collapsed.chop(1);
    }
    if (collapsed.isEmpty()) {
        return QStringLiteral("(empty)");
    }
    return collapsed;
}

/* Cell-width-aware tail truncation with single-character ellipsis (U+2026,
 * width 1). Drops one cell of budget for the ellipsis when truncation
 * happens; never returns wider than maxCols cells. */
QString truncateVisual(const QString &text, int maxCols)
{
    if (maxCols <= 0) {
        return QString();
    }
    if (QTuiText::visualWidth(text) <= maxCols) {
        return text;
    }
    const int budget = maxCols - 1; /* room for the ellipsis cell */
    int       width  = 0;
    int       idx    = 0;
    const int len    = text.length();
    while (idx < len) {
        const QChar firstUnit = text.at(idx);
        uint        codePoint = firstUnit.unicode();
        int         charLen   = 1;
        if (firstUnit.isHighSurrogate() && idx + 1 < len && text[idx + 1].isLowSurrogate()) {
            codePoint = QChar::surrogateToUcs4(firstUnit, text[idx + 1]);
            charLen   = 2;
        }
        const int chWid = QTuiText::isWideChar(codePoint) ? 2 : 1;
        if (width + chWid > budget) {
            break;
        }
        width += chWid;
        idx += charLen;
    }
    return text.left(idx) + QChar(0x2026);
}

/* Short relative-time string ("just now", "5m ago", "3h ago", "2d ago",
 * "Apr 23", "2025-12-01"). Rolls over to absolute date past one week so
 * the picker stays scannable for older sessions without lying about how
 * stale they are. */
QString formatRelativeTime(const QDateTime &when)
{
    if (!when.isValid()) {
        return QStringLiteral("?");
    }
    const QDateTime now  = QDateTime::currentDateTime();
    const qint64    secs = when.secsTo(now);
    if (secs < 0) {
        return when.toString(QStringLiteral("MM-dd HH:mm"));
    }
    if (secs < 60) {
        return QStringLiteral("just now");
    }
    if (secs < 3600) {
        return QString::number(secs / 60) + QStringLiteral("m ago");
    }
    if (secs < 86400) {
        return QString::number(secs / 3600) + QStringLiteral("h ago");
    }
    constexpr qint64 oneWeek = qint64{86400} * 7;
    if (secs < oneWeek) {
        return QString::number(secs / 86400) + QStringLiteral("d ago");
    }
    if (when.date().year() == now.date().year()) {
        return when.toString(QStringLiteral("MMM dd"));
    }
    return when.toString(QStringLiteral("yyyy-MM-dd"));
}

/* Read terminal width once, conservatively defaulting to 80 if ioctl
 * fails. Picker code uses this to budget label width. */
int currentTerminalWidth()
{
#ifdef Q_OS_WIN
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    }
#else
    struct winsize winsz = {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &winsz) == 0 && winsz.ws_col > 0) {
        return winsz.ws_col;
    }
#endif
    return 80;
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
        {"resume",
         QCoreApplication::translate(
             "main",
             "Resume a previous session. Pass the id (or unique prefix) as a positional "
             "argument to load it directly, or omit it to pick from a list.")},
        {"continue",
         QCoreApplication::translate("main", "Continue the most recent session for this project.")},
    });

    /* Optional positional argument: a session id (or unique prefix). Mirrors
     * the pattern other qsoc subcommands use for "verb [target]" CLIs. The
     * value is only consulted when --resume is set; otherwise it's ignored
     * so plain `qsoc agent` keeps working unchanged. */
    parser.addPositionalArgument(
        "session-id",
        QCoreApplication::translate(
            "main", "Optional session id (or unique prefix) when --resume is set."),
        "[session-id]");

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
    config.verbose = QSocConsole::level() >= QSocConsole::Level::Debug;

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
            config.systemPromptOverride = systemPrompt;
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

    /* Expose the project path so AGENTS.md / AGENTS.local.md get loaded
     * into the system prompt by buildSystemPromptWithMemory(). */
    config.projectPath = projectManager->getProjectPath();

    /* Pre-scan skills and build a listing for system prompt injection.
     * We instantiate a temporary QSocToolSkillFind just for the scan —
     * the real tool instance is created below and registered in the
     * tool registry. */
    {
        QSocToolSkillFind scanner(nullptr, projectManager);
        config.skillListing = QSocToolSkillFind::formatPromptListing(scanner.scanAllSkills());
    }

    /* Expose the model ID for system prompt injection. */
    if (llmService) {
        config.modelId = llmService->getCurrentModelId();
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

    /* LSP service setup. Built-in slang backend goes first as fallback;
       external servers from qsoc.yml override it for matching extensions. */
    auto *lspService = QLspService::instance();
    lspService->addBackend(new QLspSlangBackend(lspService));
    QLspConfigLoader::loadAndRegister(lspService, socConfig);
    lspService->startAll(QDir::currentPath());

    auto *lspTool = new QSocToolLsp(this);
    toolRegistry->registerTool(lspTool);

    /* Sync context budget and effort from model registry. The CLI flag
     * takes precedence: when --max-tokens is explicitly set we keep it,
     * otherwise the registry value wins so each model gets its native
     * window. */
    const bool userSetMaxTokens = parser.isSet("max-tokens");
    {
        QString modelId = llmService->getCurrentModelId();
        if (!modelId.isEmpty()) {
            LLMModelConfig modelCfg = llmService->getModelConfig(modelId);
            if (modelCfg.contextTokens > 0 && !userSetMaxTokens) {
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
        QSocConsole::debug().noquote().nospace() << Q_FUNC_INFO << ":" << message;
    });

    /* Connect tool signals for verbose output */
    connect(agent, &QSocAgent::toolCalled, [](const QString &toolName, const QString &arguments) {
        QSocConsole::debug().noquote().nospace()
            << Q_FUNC_INFO << ":Tool called: " << toolName << " with args: " << arguments;
    });

    connect(agent, &QSocAgent::toolResult, [](const QString &toolName, const QString &result) {
        QString truncated = result.length() > 200 ? result.left(200) + "..." : result;
        QSocConsole::debug().noquote().nospace()
            << Q_FUNC_INFO << ":Tool result: " << toolName << " -> " << truncated;
    });

    /* Install SIGINT handler for Ctrl+C support in non-raw-mode states */
    installSigintHandler();

    /* Check if single query mode */
    if (parser.isSet("query")) {
        QString      query = parser.value("query");
        QTextStream &qout  = QSocConsole::out();

        if (streaming) {
            /* Streaming single query mode */
            QEventLoop loop;

            connect(agent, &QSocAgent::contentChunk, [&qout](const QString &chunk) {
                qout << chunk << Qt::flush;
            });

            auto reasoningCollapser = std::make_shared<ReasoningNewlineCollapser>();
            connect(
                agent,
                &QSocAgent::reasoningChunk,
                [&qout, reasoningCollapser](const QString &chunk) {
                    const QString filtered = reasoningCollapser->feed(chunk);
                    if (!filtered.isEmpty()) {
                        qout << QSocConsole::dim(filtered) << Qt::flush;
                    }
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
                    QSocConsole::err() << "\n" << Qt::flush;
                    std::exit(130);
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
                            qout << "\n$ " << shellCmd << "\n" << Qt::flush;
                            escMonitor.stop();
                            const QString output = runShellEscape(shellCmd);
                            escMonitor.start();
                            qout << output;
                            if (!output.endsWith(QLatin1Char('\n'))) {
                                qout << "\n";
                            }
                            qout.flush();
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

    /* Interactive mode — resolve --resume / --continue to a session id (or
     * empty for "fresh"). The picker for bare --resume runs inside
     * runAgentLoop after the compositor is up so we can use QTuiMenu.
     *
     * --resume piggy-backs on an optional positional argument: bare
     * --resume opens the picker, while `qsoc agent --resume <id>` resolves
     * <id> as a unique prefix and loads that session directly. We use a
     * positional instead of a value-bearing option because Qt's
     * QCommandLineParser doesn't natively support optional values, and
     * other qsoc subcommands already use the "verb [target]" positional
     * pattern. */
    QString resumeSessionId;
    if (parser.isSet("resume")) {
        const QString     projectPath = sessionProjectPath(projectManager);
        const QStringList positionals = parser.positionalArguments();
        const QString     rawValue    = positionals.isEmpty() ? QString() : positionals.first();
        if (!rawValue.isEmpty()) {
            resumeSessionId = QSocSession::resolveId(projectPath, rawValue);
            if (resumeSessionId.isEmpty()) {
                return showError(
                    1,
                    QCoreApplication::translate("main", "Error: no session matches '%1'.")
                        .arg(rawValue));
            }
        } else {
            /* Bare --resume: sentinel "-" tells runAgentLoop to open the
             * interactive picker once the compositor is up. */
            resumeSessionId = QStringLiteral("-");
        }
    } else if (parser.isSet("continue")) {
        const QString projectPath = sessionProjectPath(projectManager);
        const auto    sessions    = QSocSession::listAll(projectPath);
        if (!sessions.isEmpty()) {
            resumeSessionId = sessions.first().id;
        }
    }
    return runAgentLoop(agent, streaming, resumeSessionId, pathContext);
}

bool QSocCliWorker::runAgentLoop(
    QSocAgent *agent, bool streaming, const QString &resumeSessionId, QSocPathContext *pathContext)
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

    QTextStream &qout = QSocConsole::out();

    /* Remote workspace state. A single session at a time; created by
     * `/ssh <target>` and torn down by `/local`. Local tool registry is
     * cached here so `/local` can restore it in O(1). */
    QSocToolRegistry       *localRegistry  = agent->getToolRegistry();
    QSocSshSession         *remoteSession  = nullptr;
    QSocSftpClient         *remoteSftp     = nullptr;
    QSocToolRegistry       *remoteRegistry = nullptr;
    QList<QSocSshSession *> remoteJumps; /* ProxyJump chain, outlives target */
    QSocRemotePathContext   remotePath;

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
        compositor.setTitle("QSoC Agent · " + (mid.isEmpty() ? "default" : mid));
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

    /* If a sticky remote binding exists for this project, inject the matching
     * /ssh line so the next REPL iteration auto-connects. Cleared on first
     * consumption; users can still /local out or override manually. */
    QString pendingAutoInput;
    if (pathContext != nullptr) {
        const QString projectRootOnStart = pathContext->getProjectDir();
        if (!projectRootOnStart.isEmpty()) {
            const auto binding = QSocRemoteBinding::read(projectRootOnStart);
            if (!binding.target.isEmpty() && !binding.workspace.isEmpty()) {
                /* The /ssh handler re-reads remote.yml for the workspace,
                 * so dispatch with the bare target. Appending the
                 * workspace here used to produce a contradictory log
                 * pair: the parser printed "Ignoring workspace path"
                 * even though the binding's workspace was correctly
                 * applied moments later. */
                pendingAutoInput = QStringLiteral("/ssh ") + binding.target;
                compositor.printContent(QString("Auto-connecting remote target %1 (workspace %2)\n")
                                            .arg(binding.target, binding.workspace));
            }
        }
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

    connect(&inputMonitor, &QAgentInputMonitor::pageKey, [&compositor](int direction) {
        /* Half-screen steps so the last line of the previous view stays
         * visible as an anchor between pages. */
        const int step = qMax(1, compositor.getTerminalHeight() / 2);
        if (direction == 0) {
            compositor.scrollContentUp(step);
        } else {
            compositor.scrollContentDown(step);
        }
    });

    /* Mouse click-drag text selection: press starts, drag updates, release
     * copies to clipboard via OSC 52 and clears the highlight. SGR coords
     * are 1-based; screen buffer is 0-based. */
    connect(
        &inputMonitor,
        &QAgentInputMonitor::mouseClick,
        [&compositor](int button, int col, int row, bool pressed) {
            if (button != 0) {
                return; /* only left-button */
            }
            if (pressed) {
                compositor.selectionStart(col - 1, row - 1);
            } else {
                compositor.selectionFinish(col - 1, row - 1);
            }
        });
    connect(&inputMonitor, &QAgentInputMonitor::mouseDrag, [&compositor](int col, int row) {
        compositor.selectionUpdate(col - 1, row - 1);
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

    /* Per-edit_file diff capture: when the agent calls edit_file, the
     * toolCalled hook stashes the path + old/new strings here so the
     * toolResult hook can render a unified-style colored diff to the
     * scroll view on success. Reset after each render so a follow-up
     * tool call doesn't accidentally re-emit the same diff. */
    QString pendingDiffPath;
    QString pendingDiffOldString;
    QString pendingDiffNewString;

    /* Helper: render a unified diff to the scroll view with the diff line
     * styles defined on QTuiScrollView (Hunk = yellow bold, Add = green,
     * Del = red, Context = dim). Called from the toolResult hook when an
     * edit_file or write_file completes successfully. projectManager is
     * captured via a local pointer because it's a class member that can't
     * appear directly in the lambda's capture list. */
    auto *projectManagerPtr = projectManager;
    auto  renderDiffToScrollView =
        [&compositor,
         projectManagerPtr](const QString &path, const QString &oldText, const QString &newText) {
            const auto diff = QSocLineDiff::computeLineDiff(oldText, newText);
            if (diff.isEmpty()) {
                return;
            }
            /* Prefer project-relative paths so the header reads as
         * "--- a/bus/apb.yaml" rather than "--- a//abs/path/bus/apb.yaml".
         * When the file lives outside the project, show the absolute path
         * without the a/ + b/ prefix so we don't produce the double slash
         * "a//abs/path" that unified-diff parsers trip over. */
            QString       displayPath = path;
            QString       headerA;
            QString       headerB;
            const QString projectPath = projectManagerPtr != nullptr
                                            ? projectManagerPtr->getProjectPath()
                                            : QString();
            if (!projectPath.isEmpty() && displayPath.startsWith(projectPath)) {
                displayPath = displayPath.mid(projectPath.size());
                if (displayPath.startsWith(QLatin1Char('/'))) {
                    displayPath = displayPath.mid(1);
                }
            }
            if (QFileInfo(displayPath).isAbsolute()) {
                headerA = displayPath;
                headerB = displayPath;
            } else {
                headerA = QStringLiteral("a/") + displayPath;
                headerB = QStringLiteral("b/") + displayPath;
            }
            compositor.printContent(
                QStringLiteral("\n--- ") + headerA + QLatin1Char('\n'), QTuiScrollView::DiffDel);
            compositor.printContent(
                QStringLiteral("+++ ") + headerB + QLatin1Char('\n'), QTuiScrollView::DiffAdd);
            for (const auto &line : diff) {
                QTuiScrollView::LineStyle style = QTuiScrollView::DiffContext;
                switch (line.kind) {
                case QSocLineDiff::Kind::Hunk:
                    style = QTuiScrollView::DiffHunk;
                    break;
                case QSocLineDiff::Kind::Add:
                    style = QTuiScrollView::DiffAdd;
                    break;
                case QSocLineDiff::Kind::Del:
                    style = QTuiScrollView::DiffDel;
                    break;
                case QSocLineDiff::Kind::Context:
                    style = QTuiScrollView::DiffContext;
                    break;
                }
                compositor.printContent(line.text + QLatin1Char('\n'), style);
            }
        };

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
    /* Static inline argument hint table for slash commands. Rendered in
     * dim style right after the buffer when the user has typed "/cmd "
     * (trailing space) with no arguments yet, so the user can discover
     * what each slash command expects without re-reading help text. */
    static const QMap<QString, QString> kSlashHints{
        {QStringLiteral("/effort"), QStringLiteral("off|low|medium|high")},
        {QStringLiteral("/model"), QStringLiteral("<model-id>")},
    };

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

            /* Slash-command inline hint: exactly "/cmd " (command + trailing
             * space, no arguments yet, single line). */
            QString hint;
            if (text.startsWith(QLatin1Char('/')) && text.endsWith(QLatin1Char(' '))
                && !text.contains(QLatin1Char('\n'))) {
                int spaceIdx = text.indexOf(QLatin1Char(' '));
                if (spaceIdx == text.size() - 1) {
                    QString cmd = text.left(spaceIdx);
                    hint        = kSlashHints.value(cmd, QString());
                }
            }
            inputWidget.setTrailingHint(hint);
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

    /* Slash command table for tab completion. Built-in commands plus
     * user-invocable skills discovered at startup. The builtins are kept
     * in a fixed list so /project can rebuild the full table (builtins +
     * new project's skills) without accumulating stale entries. */
    const QStringList slashCommandBuiltins
        = {QStringLiteral("help"),
           QStringLiteral("branch"),
           QStringLiteral("clear"),
           QStringLiteral("compact"),
           QStringLiteral("context"),
           QStringLiteral("cost"),
           QStringLiteral("cwd"),
           QStringLiteral("diff"),
           QStringLiteral("effort"),
           QStringLiteral("model"),
           QStringLiteral("project"),
           QStringLiteral("rename"),
           QStringLiteral("status"),
           QStringLiteral("exit"),
           QStringLiteral("quit")};
    QStringList            slashCommands = slashCommandBuiltins;
    QMap<QString, QString> skillPaths;
    QMap<QString, QString> skillHints;

    /* Rebuild the skill dispatch table and agent system-prompt listing from
     * the current projectManager state. Called at startup and on /project. */
    auto reloadSkills = [&]() {
        slashCommands = slashCommandBuiltins;
        skillPaths.clear();
        skillHints.clear();
        QString listing;
        if (auto *reg = agent->getToolRegistry()) {
            if (auto *skillTool = dynamic_cast<QSocToolSkillFind *>(
                    reg->getTool(QStringLiteral("skill_find")))) {
                /* Warn the user about SKILL.md files that failed to parse so
                 * they don't silently disappear from the listing. */
                for (const auto &broken : skillTool->scanAllSkillFiles()) {
                    if (!broken.parseError.isEmpty()) {
                        compositor.printContent(
                            QString("Warning: skill at %1 ignored (%2).\n")
                                .arg(broken.path, broken.parseError),
                            QTuiScrollView::Dim);
                    }
                }
                const auto skills = skillTool->scanAllSkills();
                listing           = QSocToolSkillFind::formatPromptListing(skills);
                for (const auto &skill : skills) {
                    if (skill.userInvocable && !slashCommands.contains(skill.name)) {
                        slashCommands.append(skill.name);
                        skillPaths.insert(QStringLiteral("/") + skill.name, skill.path);
                        if (!skill.argumentHint.isEmpty()) {
                            skillHints.insert(skill.name, skill.argumentHint);
                        }
                    }
                }
            }
        }
        QSocAgentConfig cfg = agent->getConfig();
        cfg.skillListing    = listing;
        agent->setConfig(cfg);
    };
    reloadSkills();

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
         &skillHints,
         &remoteSession,
         &remotePath,
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

            /* Skip if user dismissed popup for this exact non-empty input via
             * Esc. Empty buffer never matches "dismissed for" so a stale
             * empty initial value can't trap the popup-close path below
             * (without this guard, typing "/" then backspacing left the
             * popup stuck on screen because dismissedFor == text == ""). */
            if (!dismissedFor.isEmpty() && dismissedFor == text) {
                return;
            }
            /* Reaching an empty buffer also resets the dismissal state, so
             * after the user wipes everything (Ctrl+U / backspace-to-empty
             * / submit) the next round of typing starts fresh and can
             * legitimately re-open the popup for the same token. */
            if (text.isEmpty()) {
                dismissedFor.clear();
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
                /* Pull argument-hints item-aligned so /commit-msg shows its
                 * "[scope]" shorthand inline. Builtins have no hint slot. */
                QStringList hints;
                hints.reserve(matches.size());
                for (const QString &cmd : matches) {
                    hints.append(skillHints.value(cmd));
                }
                popupWidget.setTitle(QStringLiteral("/command"));
                popupWidget.setItems(matches);
                popupWidget.setHints(hints);
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

            QStringList matches;
            if (remoteSession != nullptr) {
                matches
                    = completionEngine.completeRemote(remoteSession, remotePath.root(), query, 50);
            } else {
                QString projectPath = projectManager->getProjectPath();
                if (projectPath.isEmpty()) {
                    projectPath = QDir::currentPath();
                }
                matches = completionEngine.complete(projectPath, query, 50);
            }
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

    /* Session storage. Each interactive REPL run owns one session JSONL
     * under <projectPath>/.qsoc/sessions/<id>.jsonl. lastPersistedIndex
     * tracks how many entries from agent->getMessages() have already hit
     * disk so each turn only appends the delta. resumeSessionId is the
     * id selected by --resume / --continue at startup; the helper at the
     * top of QSocCliWorker stashes it before calling runAgentLoop. */
    std::unique_ptr<QSocSession>     currentSession;
    std::unique_ptr<QSocFileHistory> currentFileHistory;
    int                              lastPersistedIndex = 0;
    /* Monotonic turn counter driven by user-message count. Initialised from
     * the resumed message history so snapshots continue the sequence after
     * --continue / --resume. */
    int turnCounter = 0;

    /* Auto-compact: after each turn, check if context usage exceeds the
     * configured threshold. If so, compact and report savings. A circuit
     * breaker trips after 3 consecutive compact failures to avoid an
     * infinite retry loop when the conversation is genuinely large. */
    int                  autoCompactFailures       = 0;
    static constexpr int AUTO_COMPACT_MAX_FAILURES = 3;

    /* Cumulative session token accounting for /cost. */
    qint64 sessionInputTokens  = 0;
    qint64 sessionOutputTokens = 0;

    /* Wire the per-session file-history store into the file-writing tools so
     * their next execute() captures pre-edit backups. Registry was populated
     * in parseAgent() long before runAgentLoop runs, so we fish tools out by
     * name. Called any time currentFileHistory is rebound. */
    auto wireFileHistoryTools = [&]() {
        if (auto *registry = agent->getToolRegistry()) {
            if (auto *tool = dynamic_cast<QSocToolFileWrite *>(
                    registry->getTool(QStringLiteral("write_file")))) {
                tool->setFileHistory(currentFileHistory.get());
            }
            if (auto *tool = dynamic_cast<QSocToolFileEdit *>(
                    registry->getTool(QStringLiteral("edit_file")))) {
                tool->setFileHistory(currentFileHistory.get());
            }
        }
    };

    /* Create a fresh session + file history rooted at projectPath, stamp
     * creation meta, and rewire the file-writing tools. Used by /project
     * switch; startup uses the same building blocks inline to interleave
     * with the resume path. */
    auto startFreshSessionAt = [&](const QString &projectPath) {
        const QString newId = QSocSession::generateId();
        const QString sessionPath
            = QDir(QSocSession::sessionsDir(projectPath)).filePath(newId + ".jsonl");
        currentSession     = std::make_unique<QSocSession>(newId, sessionPath);
        currentFileHistory = std::make_unique<QSocFileHistory>(projectPath, newId);
        currentSession->appendMeta(
            QStringLiteral("created"), QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
        currentSession->appendMeta(QStringLiteral("cwd"), projectPath);
        wireFileHistoryTools();
    };

    /* Replace the TODO widget contents with the pending items in
     * <projectPath>/.qsoc/todos.md. Clears on missing/empty file so a
     * /project switch doesn't leave old project's TODOs visible. */
    auto loadTodoWidget = [&](const QString &projectPath) {
        QList<QTuiTodoList::TodoItem> widgetItems;
        if (!projectPath.isEmpty()) {
            const QString todoPath = QDir(projectPath).filePath(QStringLiteral(".qsoc/todos.md"));
            QFile         todoFile(todoPath);
            if (todoFile.exists() && todoFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
                const QStringList lines = QTextStream(&todoFile).readAll().split(QLatin1Char('\n'));
                todoFile.close();
                QRegularExpression todoRe(QStringLiteral(R"(^-\s*\[([ xX])\]\s*#(\d+)\s+(.+)$)"));
                QString            curPri = QStringLiteral("medium");
                for (const QString &line : lines) {
                    if (line.startsWith(QStringLiteral("## High"))) {
                        curPri = QStringLiteral("high");
                    } else if (line.startsWith(QStringLiteral("## Medium"))) {
                        curPri = QStringLiteral("medium");
                    } else if (line.startsWith(QStringLiteral("## Low"))) {
                        curPri = QStringLiteral("low");
                    }
                    auto match = todoRe.match(line);
                    if (!match.hasMatch()) {
                        continue;
                    }
                    if (match.captured(1) != QStringLiteral(" ")) {
                        continue; /* skip done items */
                    }
                    QTuiTodoList::TodoItem item;
                    item.id       = match.captured(2).toInt();
                    item.title    = match.captured(3).trimmed();
                    item.priority = curPri;
                    item.status   = QStringLiteral("pending");
                    widgetItems.append(item);
                }
            }
        }
        todoWidget.setItems(widgetItems);
    };

    /* Clear and reload the input history + paste maps from the given
     * project's <projectPath>/.qsoc/history.jsonl. Silent on missing /
     * malformed lines: history is advisory, not authoritative. */
    auto loadInputHistory = [&](const QString &projectPath) {
        inputHistory.clear();
        inputHistoryPastes.clear();
        if (projectPath.isEmpty()) {
            return;
        }
        QFile histFile(QDir(projectPath).filePath(QStringLiteral(".qsoc/history.jsonl")));
        if (!histFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return;
        }
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
                bool parseOk = false;
                int  pasteId = pIt.key().toInt(&parseOk);
                if (parseOk) {
                    pastes[pasteId] = pIt.value().toString();
                }
            }
            inputHistory.append(display);
            inputHistoryPastes.append(pastes);
        }
    };

    {
        const QString projectPath = sessionProjectPath(projectManager);
        QString       sessionId   = resumeSessionId;

        /* Sentinel "-" from CLI parsing means "open the picker now". The
         * picker uses QTuiMenu which needs the compositor to be running,
         * which it already is at this point. */
        if (sessionId == QStringLiteral("-")) {
            sessionId.clear();
            const auto sessions = QSocSession::listAll(projectPath);
            if (sessions.isEmpty()) {
                compositor.printContent("No previous sessions found — starting fresh.\n\n");
            } else {
                QList<QTuiMenu::MenuItem> items;
                items.reserve(sessions.size());
                const int termW = currentTerminalWidth();
                for (const QSocSession::Info &info : sessions) {
                    QTuiMenu::MenuItem item;
                    /* Title fallback chain: rename → first prompt → branch
                     * label → "(empty)". Cleaned of newlines so wide
                     * pasted prompts collapse onto one row. */
                    QString primary = !info.title.isEmpty()         ? info.title
                                      : !info.firstPrompt.isEmpty() ? info.firstPrompt
                                      : !info.branch.isEmpty()      ? info.branch
                                                                    : QString();
                    primary         = cleanPromptForLabel(primary);
                    /* Hint carries the always-on metadata. Keep it short
                     * so the label gets the wide budget; render leading
                     * separator inside the hint so single-line layouts
                     * stay readable. */
                    QString hint = QString::fromUtf8("\xc2\xb7 %1 \xc2\xb7 %2 msg")
                                       .arg(formatRelativeTime(info.lastModified))
                                       .arg(info.messageCount);
                    if (info.messageCount != 1) {
                        hint.append(QLatin1Char('s'));
                    }
                    /* Width-aware label budget: leave the hint intact
                     * since it is short and load-bearing, then give the
                     * label whatever cells remain after subtracting menu
                     * chrome ("  " + label + "  " + hint = 4 cells of
                     * gutter, plus 1 cell cushion). Floor of 8 keeps the
                     * label visible at extreme narrow widths instead of
                     * overflowing the terminal and overwriting the hint. */
                    const int hintW    = QTuiText::visualWidth(hint);
                    const int chrome   = 5;
                    const int labelMax = qMax(8, termW - hintW - chrome);
                    item.label         = truncateVisual(primary, labelMax);
                    item.hint          = hint;
                    items.append(item);
                }
                QTuiMenu menu;
                menu.setTitle("Resume session");
                menu.setItems(items);
                menu.setSearchable(true);
                menu.setHighlight(0);
                const int selected = menu.exec();
                compositor.invalidate();
                compositor.render();
                if (selected >= 0 && selected < sessions.size()) {
                    sessionId = sessions[selected].id;
                }
            }
        } else if (!sessionId.isEmpty()) {
            const QString resolved = QSocSession::resolveId(projectPath, sessionId);
            if (!resolved.isEmpty()) {
                sessionId = resolved;
            }
        }

        if (sessionId.isEmpty()) {
            sessionId = QSocSession::generateId();
        }
        const QString sessionPath
            = QDir(QSocSession::sessionsDir(projectPath)).filePath(sessionId + ".jsonl");
        currentSession     = std::make_unique<QSocSession>(sessionId, sessionPath);
        currentFileHistory = std::make_unique<QSocFileHistory>(projectPath, sessionId);

        /* Resume an existing session if the JSONL already exists; otherwise
         * stamp the new session with creation metadata. */
        if (QFile::exists(sessionPath)) {
            const json restored = QSocSession::loadMessages(sessionPath);
            if (restored.is_array() && !restored.empty()) {
                agent->setMessages(restored);
                lastPersistedIndex = static_cast<int>(agent->getMessages().size());
                /* Recreate the monotonic turn counter so the next snapshot
                 * continues the sequence from where the previous session
                 * left off. Prefer on-disk file-history state when it
                 * exists (authoritative); fall back to the user-message
                 * count for sessions created before file-history was
                 * introduced. */
                turnCounter = currentFileHistory->latestTurn();
                if (turnCounter == 0) {
                    for (const auto &msg : restored) {
                        if (msg.is_object() && msg.contains("role")
                            && msg["role"].get<std::string>() == "user") {
                            turnCounter++;
                        }
                    }
                }
                compositor.printContent(QString("(Resumed session %1, %2 messages)\n\n")
                                            .arg(sessionId.left(8))
                                            .arg(lastPersistedIndex));

                /* Restore pending TODOs from .qsoc/todos.md so the user
                 * sees what was in progress when the session was saved. */
                loadTodoWidget(projectPath);
            }
        } else {
            currentSession->appendMeta(
                QStringLiteral("created"),
                QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
            currentSession->appendMeta(QStringLiteral("cwd"), projectPath);
            compositor.printContent(QString("(New session %1)\n\n").arg(sessionId.left(8)));
        }

        wireFileHistoryTools();
    }

    /* Input history navigation position (inputHistory itself is declared above
     * so the search lambdas can capture it). */
    int     historyPos = -1; /* -1 = not browsing, 0 = most recent */
    QString savedInput;      /* Input buffer before browsing */

    loadInputHistory(projectManager->getProjectPath());

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
            const QString typedWord = current.mid(1, end - 1);
            /* Shortcut: if the user already typed the full command word,
             * auto-completing just appends a trailing space and forces a
             * second Enter. Close the popup and submit the buffer now so
             * argument-less commands like /status fire on the first Enter. */
            if (typedWord == picked && end == current.size()) {
                popupWidget.setVisible(false);
                popupKind  = PopupKind::None;
                popupAtPos = -1;
                inputMonitor.setSubmitBlocked(false);
                compositor.invalidate();
                inputMonitor.submitNow();
                return;
            }
            QString before  = current.left(0); /* buffer up to '/' == "" */
            QString after   = current.mid(end);
            QString rebuilt = QLatin1Char('/') + picked + QLatin1Char(' ') + after;
            inputMonitor.setInputBuffer(rebuilt);
        } else {
            /* AtFile: decide trailing char: '/' if directory, else ' '. Remote
             * listings only contain files so we never need to probe over SFTP. */
            QChar trailing = QLatin1Char(' ');
            if (remoteSession == nullptr) {
                QString projectPath = projectManager->getProjectPath();
                if (projectPath.isEmpty()) {
                    projectPath = QDir::currentPath();
                }
                QFileInfo info(QDir(projectPath).filePath(picked));
                if (info.isDir()) {
                    trailing = QLatin1Char('/');
                }
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
                 * input so it doesn't auto-reopen until the user types more.
                 * For AtFile, also drop the '@query' so the next '/cmd' is
                 * not swallowed as part of a chat message. */
                if (popupWidget.isVisible()) {
                    if (popupKind == PopupKind::AtFile && popupAtPos >= 0) {
                        const QString current = inputWidget.getText();
                        if (popupAtPos <= current.size()) {
                            inputMonitor.setInputBuffer(current.left(popupAtPos));
                        }
                    }
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

        /* Esc-Esc (two Esc keys within 500ms, no intervening keystrokes):
         * open a picker over previous user messages and rewind the session
         * to the point just before the chosen message. The picked message's
         * text is loaded back into the input buffer so the user can edit it
         * and branch off from there. Gated strictly on buffer empty + no
         * modal overlays — otherwise the second Esc is a harmless no-op
         * since the first one already cleared / dismissed something. */
        auto connEscEsc = connect(
            &inputMonitor,
            &QAgentInputMonitor::escEscPressed,
            [this,
             agent,
             &currentSession,
             &currentFileHistory,
             &turnCounter,
             &lastPersistedIndex,
             &compositor,
             &inputMonitor,
             &inputWidget,
             &popupWidget,
             &searching]() {
                (void) this;
                if (searching || popupWidget.isVisible()) {
                    return;
                }
                if (!inputWidget.getText().isEmpty()) {
                    return;
                }
                if (!currentSession) {
                    return;
                }

                const json allMessages = agent->getMessages();
                if (!allMessages.is_array()) {
                    return;
                }

                /* Collect user messages with their absolute indices so the
                 * picker shows only what the user typed, not the tool
                 * traffic and assistant replies in between. Each user
                 * message also gets a turn number (1-indexed) used to look
                 * up the matching file-history snapshot. */
                struct UserMsgRef
                {
                    int     index;
                    int     turn;
                    QString content;
                };
                QList<UserMsgRef> userMsgs;
                int               turnSeen = 0;
                for (int idx = 0; idx < static_cast<int>(allMessages.size()); idx++) {
                    const auto &msg = allMessages[idx];
                    if (!msg.is_object() || !msg.contains("role") || !msg.contains("content")) {
                        continue;
                    }
                    if (msg["role"].get<std::string>() != "user") {
                        continue;
                    }
                    turnSeen++;
                    if (!msg["content"].is_string()) {
                        continue;
                    }
                    const QString content = QString::fromStdString(
                        msg["content"].get<std::string>());
                    if (content.isEmpty()) {
                        continue;
                    }
                    userMsgs.append(UserMsgRef{.index = idx, .turn = turnSeen, .content = content});
                }

                if (userMsgs.isEmpty()) {
                    compositor.printContent("(no previous messages to rewind to)\n");
                    compositor.invalidate();
                    return;
                }

                /* Build per-turn change counts from snapshot diffs so the
                 * picker can show "this turn touched N files" instead of
                 * a featureless [files] flag. */
                QMap<int, QMap<QString, QString>> snapshotFiles;
                QSet<int>                         turnsWithSnapshots;
                if (currentFileHistory) {
                    for (const auto &snap : currentFileHistory->listSnapshots()) {
                        turnsWithSnapshots.insert(snap.turn);
                        snapshotFiles.insert(snap.turn, snap.files);
                    }
                }
                auto turnChangeCount = [&](int turn) -> int {
                    if (!snapshotFiles.contains(turn)) {
                        return 0;
                    }
                    const QMap<QString, QString> &cur  = snapshotFiles[turn];
                    const QMap<QString, QString> &prev = snapshotFiles.value(turn - 1);
                    int                           diff = 0;
                    for (auto it = cur.constBegin(); it != cur.constEnd(); ++it) {
                        if (prev.value(it.key()) != it.value()) {
                            diff++;
                        }
                    }
                    return diff;
                };

                QList<QTuiMenu::MenuItem> items;
                items.reserve(userMsgs.size());
                const int rewindTermW = currentTerminalWidth();
                for (const UserMsgRef &ref : userMsgs) {
                    QTuiMenu::MenuItem item;
                    const QString      cleaned = cleanPromptForLabel(ref.content);
                    /* Hint carries turn index plus per-turn file-change
                     * count so the user knows exactly how much they are
                     * rolling back without scrolling horizontally. */
                    QString   hint    = QString::fromUtf8("\xc2\xb7 #%1").arg(ref.turn);
                    const int changed = turnChangeCount(ref.turn);
                    if (changed > 0) {
                        hint += QString::fromUtf8(" \xc2\xb7 %1 file").arg(changed);
                        if (changed != 1) {
                            hint.append(QLatin1Char('s'));
                        }
                    }
                    const int hintW    = QTuiText::visualWidth(hint);
                    const int chrome   = 5;
                    const int labelMax = qMax(8, rewindTermW - hintW - chrome);
                    item.label         = truncateVisual(cleaned, labelMax);
                    item.hint          = hint;
                    items.append(item);
                }

                QTuiMenu menu;
                menu.setTitle("Rewind to earlier message");
                menu.setItems(items);
                menu.setSearchable(true);
                menu.setHighlight(static_cast<int>(items.size()) - 1);
                const int selected = menu.exec();
                inputMonitor.resetEscState();
                compositor.invalidate();
                compositor.render();

                if (selected < 0 || selected >= userMsgs.size()) {
                    return;
                }

                const UserMsgRef &pick            = userMsgs[selected];
                const bool        hasFileSnapshot = turnsWithSnapshots.contains(pick.turn - 1)
                                                    || pick.turn == 1;

                /* Second picker: choose mode. Only shown when a file
                 * snapshot exists; otherwise we implicitly fall through to
                 * context-only since there's nothing to restore anyway. */
                bool restoreFiles = false;
                if (hasFileSnapshot && currentFileHistory) {
                    QList<QTuiMenu::MenuItem> modeItems;
                    QTuiMenu::MenuItem        withFiles;
                    withFiles.label = QStringLiteral("Restore conversation and files");
                    withFiles.hint  = QStringLiteral("revert edits made after this turn");
                    modeItems.append(withFiles);
                    QTuiMenu::MenuItem convOnly;
                    convOnly.label = QStringLiteral("Restore conversation only");
                    convOnly.hint  = QStringLiteral("keep current files on disk");
                    modeItems.append(convOnly);

                    QTuiMenu modeMenu;
                    modeMenu.setTitle("Rewind mode");
                    modeMenu.setItems(modeItems);
                    modeMenu.setHighlight(0); /* default to full rewind */
                    const int modeSel = modeMenu.exec();
                    inputMonitor.resetEscState();
                    compositor.invalidate();
                    compositor.render();
                    if (modeSel < 0) {
                        return; /* user cancelled mode pick */
                    }
                    restoreFiles = (modeSel == 0);
                }

                /* Preserve the original createdAt so the session picker
                 * still shows the original timestamp after a rewind. The
                 * rewriteMessages call truncates the file entirely, so
                 * meta must be re-emitted afterwards. */
                const QSocSession::Info origInfo = QSocSession::readInfo(currentSession->filePath());

                json truncated = json::array();
                for (int idx = 0; idx < pick.index; idx++) {
                    truncated.push_back(allMessages[idx]);
                }
                agent->setMessages(truncated);
                currentSession->rewriteMessages(truncated);
                if (origInfo.createdAt.isValid()) {
                    currentSession->appendMeta(
                        QStringLiteral("created"), origInfo.createdAt.toString(Qt::ISODateWithMs));
                }
                lastPersistedIndex = static_cast<int>(truncated.size());

                /* File restore + snapshot truncation. The rewound turn's
                 * pre-state is snapshot (turn - 1); applying that puts the
                 * disk back where it was just before the picked message
                 * ran, then we drop the orphaned future snapshots. */
                QStringList restoredFiles;
                if (restoreFiles && currentFileHistory) {
                    const int targetSnapshot = pick.turn - 1;
                    restoredFiles            = currentFileHistory->applySnapshot(targetSnapshot);
                    currentFileHistory->truncateAfter(targetSnapshot);
                    turnCounter = targetSnapshot;
                }

                /* Load the picked message back into the input buffer so the
                 * user can edit and resubmit. setInputBuffer emits
                 * inputChanged which the REPL's connected slot forwards to
                 * inputWidget.setText — force a subsequent render so the
                 * restored text is visible before the user types anything. */
                inputMonitor.setInputBuffer(pick.content);

                const QString fileSummary = restoredFiles.isEmpty()
                                                ? QString()
                                                : QString(", %1 file%2 restored")
                                                      .arg(restoredFiles.size())
                                                      .arg(restoredFiles.size() == 1 ? "" : "s");
                compositor.printContent(
                    QString(
                        "\n(Rewound: kept %1 message%2%3, picked text restored for "
                        "editing)\n")
                        .arg(lastPersistedIndex)
                        .arg(lastPersistedIndex == 1 ? "" : "s")
                        .arg(fileSummary));
                compositor.invalidate();
                compositor.render();
            });

        if (!pendingAutoInput.isEmpty()) {
            input = pendingAutoInput;
            pendingAutoInput.clear();
        } else {
            promptLoop.exec();
        }

        QObject::disconnect(connInput);
        QObject::disconnect(connCtrlC);
        QObject::disconnect(connEsc);
        QObject::disconnect(connEscEsc);

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
                compositor.printContent("$ " + shellCmd + "\n", QTuiScrollView::Bold);
                /* Both runShellEscape and runRemoteShellEscape buffer
                 * stdout/stderr into a string and return it; nothing is
                 * written to the terminal during exec. That means the
                 * compositor can keep painting normally. A pause/resume
                 * here briefly swaps buffers and exposes whatever the
                 * previous alt-screen frame held (for example the last
                 * path picker), which reads as a flash. */
                const QString output
                    = remoteSession != nullptr
                          ? runRemoteShellEscape(*remoteSession, remotePath.cwd(), shellCmd)
                          : runShellEscape(shellCmd);
                if (!output.isEmpty()) {
                    compositor.printContent(output, QTuiScrollView::Dim);
                    if (!output.endsWith(QLatin1Char('\n'))) {
                        compositor.printContent("\n");
                    }
                }
            }
            continue;
        }
        QString cmd = input.toLower();
        if (cmd == "exit" || cmd == "quit" || cmd == "/exit" || cmd == "/quit") {
            compositor.printContent("Goodbye!\n");
            break;
        }
        if (cmd == "/context") {
            /* Analyse context usage by walking the message array and
             * categorising tokens into system prompt, project instructions,
             * tool definitions, memory, user text, assistant text, tool
             * calls, and tool results. Uses the same rough 4-char/token
             * estimation the agent's compaction already relies on. */

            /* --- gather raw numbers --- */
            const json allMsgs = agent->getMessages();
            const int  maxCtx  = agent->getConfig().maxContextTokens;

            /* System prompt decomposition: base, instructions, memory.
             * buildSystemPromptWithMemory() returns the full string, but
             * we can re-derive components from the config + managers. */
            const int basePromptTokens = agent->estimateTokens(agent->buildSystemPromptWithMemory())
                                         - agent->estimateTokens(
                                             agent->getMemoryManager()
                                                 ? agent->getMemoryManager()->loadMemoryForPrompt()
                                                 : QString());

            int instructionTokens = 0;
            {
                const QString pp = agent->getConfig().projectPath;
                if (!pp.isEmpty()) {
                    QDir dir(pp);
                    for (const QString &name :
                         {QStringLiteral("AGENTS.md"), QStringLiteral("AGENTS.local.md")}) {
                        QFile file(dir.filePath(name));
                        if (file.exists() && file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                            instructionTokens += agent->estimateTokens(QTextStream(&file).readAll());
                            file.close();
                        }
                    }
                }
            }

            int memoryTokens = 0;
            if (auto *mm = agent->getMemoryManager()) {
                memoryTokens = agent->estimateTokens(mm->loadMemoryForPrompt());
            }

            int toolDefTokens = 0;
            if (auto *reg = agent->getToolRegistry()) {
                json defs = reg->getToolDefinitions();
                if (!defs.empty()) {
                    toolDefTokens = agent->estimateTokens(QString::fromStdString(defs.dump()));
                }
            }

            /* Message breakdown by role + per-tool aggregation. */
            int userTokens  = 0;
            int asstTokens  = 0;
            int callTokens  = 0;
            int resultTotal = 0;
            /* Map tool_call_id → tool name for result attribution. */
            QMap<QString, QString> callIdToName;
            /* Per-tool token buckets: name → {calls, results}. */
            struct ToolBucket
            {
                int calls   = 0;
                int results = 0;
            };
            QMap<QString, ToolBucket> toolBuckets;

            for (const auto &msg : allMsgs) {
                if (!msg.is_object() || !msg.contains("role")) {
                    continue;
                }
                const std::string role = msg["role"].get<std::string>();
                if (role == "user") {
                    if (msg.contains("content") && msg["content"].is_string()) {
                        userTokens += agent->estimateTokens(
                            QString::fromStdString(msg["content"].get<std::string>()));
                    }
                    userTokens += 10; /* per-message overhead */
                } else if (role == "assistant") {
                    if (msg.contains("content") && msg["content"].is_string()) {
                        asstTokens += agent->estimateTokens(
                            QString::fromStdString(msg["content"].get<std::string>()));
                    }
                    if (msg.contains("tool_calls") && msg["tool_calls"].is_array()) {
                        for (const auto &tc : msg["tool_calls"]) {
                            int tcTokens = agent->estimateTokens(QString::fromStdString(tc.dump()));
                            callTokens += tcTokens;
                            if (tc.contains("id") && tc["id"].is_string() && tc.contains("function")
                                && tc["function"].contains("name")
                                && tc["function"]["name"].is_string()) {
                                QString name = QString::fromStdString(
                                    tc["function"]["name"].get<std::string>());
                                QString tcId = QString::fromStdString(tc["id"].get<std::string>());
                                callIdToName.insert(tcId, name);
                                toolBuckets[name].calls += tcTokens;
                            }
                        }
                    }
                    asstTokens += 10;
                } else if (role == "tool") {
                    int resultTokens = 10;
                    if (msg.contains("content") && msg["content"].is_string()) {
                        resultTokens += agent->estimateTokens(
                            QString::fromStdString(msg["content"].get<std::string>()));
                    }
                    resultTotal += resultTokens;
                    /* Attribute to tool name via tool_call_id lookup. */
                    if (msg.contains("tool_call_id") && msg["tool_call_id"].is_string()) {
                        QString tcId = QString::fromStdString(
                            msg["tool_call_id"].get<std::string>());
                        QString name = callIdToName.value(tcId, QStringLiteral("(unknown)"));
                        toolBuckets[name].results += resultTokens;
                    }
                }
            }

            const int usedTokens = basePromptTokens + instructionTokens + memoryTokens
                                   + toolDefTokens + userTokens + asstTokens + callTokens
                                   + resultTotal;
            const int freeTokens = qMax(0, maxCtx - usedTokens);
            const int pct        = maxCtx > 0 ? (usedTokens * 100 / maxCtx) : 0;

            /* --- format bar helper --- */
            constexpr int BAR_WIDTH = 20;
            auto          makeBar   = [&](int tokens) -> QString {
                int filled = maxCtx > 0 ? (tokens * BAR_WIDTH / maxCtx) : 0;
                filled     = qBound(0, filled, BAR_WIDTH);
                if (tokens > 0 && filled == 0) {
                    filled = 1; /* always show at least 1 block if non-zero */
                }
                return QString(filled, QChar(0x2588))                /* █ */
                       + QString(BAR_WIDTH - filled, QChar(0x2591)); /* ░ */
            };
            auto fmtTokens = [](int tokens) -> QString {
                if (tokens >= 1000) {
                    return QString::number(tokens / 1000.0, 'f', 1) + "k";
                }
                return QString::number(tokens);
            };

            /* --- render --- */
            compositor.printContent("\n");
            compositor.printContent("Context Usage\n", QTuiScrollView::Bold);
            compositor.printContent(QString("  %1 / %2 tokens (%3%)\n\n")
                                        .arg(fmtTokens(usedTokens), fmtTokens(maxCtx))
                                        .arg(pct));

            auto printCategory =
                [&](const QString &label, int tokens, QTuiScrollView::LineStyle style) {
                    const QString padLabel = label.leftJustified(18);
                    const QString padNum   = fmtTokens(tokens).rightJustified(6);
                    const int     pctVal   = maxCtx > 0 ? (tokens * 100 / maxCtx) : 0;
                    compositor.printContent(
                        QStringLiteral("  ") + padLabel + padNum + QStringLiteral("  ")
                            + makeBar(tokens) + QStringLiteral("  ") + QString::number(pctVal)
                            + QStringLiteral("%\n"),
                        style);
                };

            printCategory("System prompt:", basePromptTokens, QTuiScrollView::Normal);
            if (instructionTokens > 0) {
                printCategory("Project rules:", instructionTokens, QTuiScrollView::Normal);
            }
            printCategory("Tool definitions:", toolDefTokens, QTuiScrollView::Normal);
            if (memoryTokens > 0) {
                printCategory("Memory:", memoryTokens, QTuiScrollView::Normal);
            }
            printCategory("User messages:", userTokens, QTuiScrollView::Normal);
            printCategory("Asst messages:", asstTokens, QTuiScrollView::Normal);
            printCategory("Tool calls:", callTokens, QTuiScrollView::Normal);
            printCategory("Tool results:", resultTotal, QTuiScrollView::Normal);
            printCategory("Free:", freeTokens, QTuiScrollView::Dim);

            /* Per-tool breakdown (only if there are tool calls). */
            if (!toolBuckets.isEmpty()) {
                compositor.printContent("\n");
                compositor.printContent("Message breakdown by tool:\n", QTuiScrollView::Bold);
                QList<QString> toolNames = toolBuckets.keys();
                std::sort(toolNames.begin(), toolNames.end());
                for (const QString &name : toolNames) {
                    const ToolBucket &bucket = toolBuckets[name];
                    /* Both columns are token totals — the historical
                     * "calls" wording made the call-side bucket read
                     * as an invocation count. Spell out "tokens" so
                     * neither side is mistaken for a counter. */
                    compositor.printContent(
                        QStringLiteral("  ") + name.leftJustified(20)
                            + fmtTokens(bucket.calls).rightJustified(6)
                            + QStringLiteral(" call tokens + ")
                            + fmtTokens(bucket.results).rightJustified(6)
                            + QStringLiteral(" result tokens\n"),
                        QTuiScrollView::Dim);
                }
            }

            /* Project instructions listing. */
            {
                const QString pp  = agent->getConfig().projectPath;
                bool          any = false;
                if (!pp.isEmpty()) {
                    QDir dir(pp);
                    for (const QString &name :
                         {QStringLiteral("AGENTS.md"), QStringLiteral("AGENTS.local.md")}) {
                        if (QFile::exists(dir.filePath(name))) {
                            if (!any) {
                                compositor.printContent("\n");
                                compositor
                                    .printContent("Project instructions:\n", QTuiScrollView::Bold);
                                any = true;
                            }
                            compositor.printContent(
                                QString("  %1 (loaded)\n").arg(name), QTuiScrollView::Dim);
                        }
                    }
                }
            }

            /* --- Suggestions --- */
            struct Suggestion
            {
                bool    isWarning;
                QString title;
                QString detail;
                int     savings;
            };
            QList<Suggestion> suggestions;

            /* 1. Near capacity (>= 80%) */
            if (pct >= 80) {
                suggestions.append(
                    Suggestion{
                        .isWarning = true,
                        .title     = QString("Context is %1% full").arg(pct),
                        .detail    = QStringLiteral(
                            "Use /compact now to free space before the context overflows."),
                        .savings = 0});
            }

            /* 2. Large tool results (>= 15% and >= 10k tokens) */
            for (auto it = toolBuckets.begin(); it != toolBuckets.end(); ++it) {
                int total   = it.value().calls + it.value().results;
                int toolPct = maxCtx > 0 ? (total * 100 / maxCtx) : 0;
                if (toolPct < 15 || total < 10000) {
                    continue;
                }
                QString detail;
                int     savingsEst = 0;
                if (it.key() == QStringLiteral("shell_bash")) {
                    detail = QStringLiteral(
                        "Pipe output through head, tail, or grep to reduce result "
                        "size.");
                    savingsEst = total / 2;
                } else if (it.key() == QStringLiteral("read_file")) {
                    detail = QStringLiteral(
                        "Use offset and limit parameters to read only the sections "
                        "you need.");
                    savingsEst = total * 3 / 10;
                } else if (it.key() == QStringLiteral("web_fetch")) {
                    detail = QStringLiteral(
                        "Web page content can be very large. Extract only what you "
                        "need.");
                    savingsEst = total * 4 / 10;
                } else {
                    detail     = QStringLiteral("This tool is consuming a large share of context.");
                    savingsEst = total / 5;
                }
                suggestions.append(
                    Suggestion{
                        .isWarning = true,
                        .title     = QString("%1 using %2 tokens (%3%)")
                                         .arg(it.key(), fmtTokens(total))
                                         .arg(toolPct),
                        .detail    = detail,
                        .savings   = savingsEst});
            }

            /* 3. Read result bloat (>= 5% and >= 10k, not already flagged) */
            if (toolBuckets.contains(QStringLiteral("read_file"))) {
                const auto &rb      = toolBuckets[QStringLiteral("read_file")];
                int         total   = rb.calls + rb.results;
                int         readPct = maxCtx > 0 ? (rb.results * 100 / maxCtx) : 0;
                int         totPct  = maxCtx > 0 ? (total * 100 / maxCtx) : 0;
                if (readPct >= 5 && rb.results >= 10000 && (totPct < 15 || total < 10000)) {
                    suggestions.append(
                        Suggestion{
                            .isWarning = false,
                            .title     = QString("File reads using %1 tokens (%2%)")
                                             .arg(fmtTokens(rb.results))
                                             .arg(readPct),
                            .detail    = QStringLiteral(
                                "Consider referencing earlier reads. Use offset/limit for "
                                "large files."),
                            .savings = rb.results * 3 / 10});
                }
            }

            /* 4. Memory bloat (>= 5% and >= 5k tokens) */
            if (memoryTokens > 0) {
                int memPct = maxCtx > 0 ? (memoryTokens * 100 / maxCtx) : 0;
                if (memPct >= 5 && memoryTokens >= 5000) {
                    suggestions.append(
                        Suggestion{
                            .isWarning = false,
                            .title     = QString("Memory using %1 tokens (%2%)")
                                             .arg(fmtTokens(memoryTokens))
                                             .arg(memPct),
                            .detail    = QStringLiteral("Review and prune stale memory entries."),
                            .savings   = memoryTokens * 3 / 10});
                }
            }

            /* 5. Auto-compact hint (>= 50% without auto-compact being active) */
            if (pct >= 50 && pct < 80) {
                suggestions.append(
                    Suggestion{
                        .isWarning = false,
                        .title     = QStringLiteral("Context is over 50% — consider /compact"),
                        .detail    = QStringLiteral(
                            "Running /compact now preserves recent messages and frees "
                            "space for more turns."),
                        .savings = 0});
            }

            /* Sort: warnings first, then by savings descending */
            std::sort(
                suggestions.begin(),
                suggestions.end(),
                [](const Suggestion &lhs, const Suggestion &rhs) {
                    if (lhs.isWarning != rhs.isWarning) {
                        return lhs.isWarning;
                    }
                    return lhs.savings > rhs.savings;
                });

            if (!suggestions.isEmpty()) {
                compositor.printContent("\n");
                compositor.printContent("Suggestions\n", QTuiScrollView::Bold);
                for (const Suggestion &sug : suggestions) {
                    const QString icon  = sug.isWarning ? QStringLiteral("  ⚠ ")
                                                        : QStringLiteral("  ℹ ");
                    const auto    style = sug.isWarning ? QTuiScrollView::DiffHunk
                                                        : QTuiScrollView::Dim;
                    QString       line  = icon + sug.title;
                    if (sug.savings > 0) {
                        line += QString(" -> save ~%1").arg(fmtTokens(sug.savings));
                    }
                    compositor.printContent(line + "\n", style);
                    compositor.printContent("    " + sug.detail + "\n", QTuiScrollView::Dim);
                }
            }
            compositor.printContent("\n");
            continue;
        }
        if (cmd == "/diff") {
            if (!currentFileHistory || currentFileHistory->isEmpty()) {
                compositor.printContent("(no file history available yet — run an edit first)\n");
                continue;
            }
            const auto snapshots = currentFileHistory->listSnapshots();
            /* A "turn" is meaningful only when there's a post-turn snapshot
             * to diff against its predecessor. Turn 0 is the baseline. */
            struct TurnEntry
            {
                int                    turn;
                QMap<QString, QString> filesBefore;
                QMap<QString, QString> filesAfter;
            };
            QList<TurnEntry> turns;
            for (int i = 0; i < snapshots.size(); i++) {
                if (snapshots[i].turn == 0) {
                    continue; /* baseline is the "before" of turn 1, not a turn itself */
                }
                TurnEntry entry;
                entry.turn       = snapshots[i].turn;
                entry.filesAfter = snapshots[i].files;
                /* "Before" = effective state at turn-1. Walk all snapshots
                 * with turn <= (this.turn - 1), keep the latest record per
                 * path. */
                for (int j = 0; j < snapshots.size(); j++) {
                    if (snapshots[j].turn > entry.turn - 1) {
                        continue;
                    }
                    for (auto it = snapshots[j].files.begin(); it != snapshots[j].files.end();
                         ++it) {
                        entry.filesBefore.insert(it.key(), it.value());
                    }
                }
                turns.append(entry);
            }

            if (turns.isEmpty()) {
                compositor.printContent("(no completed turns to diff yet)\n");
                continue;
            }

            /* Build per-turn summary items with a +/- count rolled up over
             * every edited file in that turn. */
            QList<QTuiMenu::MenuItem> turnItems;
            turnItems.reserve(turns.size());
            for (const TurnEntry &entry : turns) {
                int           filesChanged = 0;
                int           linesAdded   = 0;
                int           linesRemoved = 0;
                QSet<QString> allPaths;
                for (auto it = entry.filesBefore.begin(); it != entry.filesBefore.end(); ++it) {
                    allPaths.insert(it.key());
                }
                for (auto it = entry.filesAfter.begin(); it != entry.filesAfter.end(); ++it) {
                    allPaths.insert(it.key());
                }
                for (const QString &path : allPaths) {
                    const QString shaBefore = entry.filesBefore.value(path);
                    const QString shaAfter  = entry.filesAfter.value(path);
                    if (shaBefore == shaAfter) {
                        continue; /* untouched this turn */
                    }
                    filesChanged++;
                    const QString before = shaBefore.isEmpty()
                                               ? QString()
                                               : currentFileHistory->contentAt(path, entry.turn - 1);
                    const QString after = shaAfter.isEmpty()
                                              ? QString()
                                              : currentFileHistory->contentAt(path, entry.turn);
                    const auto    diff  = QSocLineDiff::computeLineDiff(before, after);
                    for (const auto &line : diff) {
                        if (line.kind == QSocLineDiff::Kind::Add) {
                            linesAdded++;
                        } else if (line.kind == QSocLineDiff::Kind::Del) {
                            linesRemoved++;
                        }
                    }
                }
                QTuiMenu::MenuItem item;
                item.label = QString("Turn #%1").arg(entry.turn);
                item.hint  = QString("%1 file%2  +%3 -%4")
                                 .arg(filesChanged)
                                 .arg(filesChanged == 1 ? "" : "s")
                                 .arg(linesAdded)
                                 .arg(linesRemoved);
                turnItems.append(item);
            }

            QTuiMenu turnMenu;
            turnMenu.setTitle("Diff: pick a turn");
            turnMenu.setItems(turnItems);
            turnMenu.setSearchable(true);
            turnMenu.setHighlight(static_cast<int>(turnItems.size()) - 1);
            const int turnSel = turnMenu.exec();
            inputMonitor.resetEscState();
            compositor.invalidate();
            compositor.render();
            if (turnSel < 0 || turnSel >= turns.size()) {
                continue;
            }
            const TurnEntry &chosenTurn = turns[turnSel];

            /* Second picker: per-file list for the chosen turn. */
            struct FileEntry
            {
                QString path;
                QString shaBefore;
                QString shaAfter;
                int     added;
                int     removed;
            };
            QList<FileEntry> fileEntries;
            {
                QSet<QString> allPaths;
                for (auto it = chosenTurn.filesBefore.begin(); it != chosenTurn.filesBefore.end();
                     ++it) {
                    allPaths.insert(it.key());
                }
                for (auto it = chosenTurn.filesAfter.begin(); it != chosenTurn.filesAfter.end();
                     ++it) {
                    allPaths.insert(it.key());
                }
                for (const QString &path : allPaths) {
                    FileEntry entry;
                    entry.path      = path;
                    entry.shaBefore = chosenTurn.filesBefore.value(path);
                    entry.shaAfter  = chosenTurn.filesAfter.value(path);
                    if (entry.shaBefore == entry.shaAfter) {
                        continue;
                    }
                    const QString before
                        = entry.shaBefore.isEmpty()
                              ? QString()
                              : currentFileHistory->contentAt(path, chosenTurn.turn - 1);
                    const QString after = entry.shaAfter.isEmpty()
                                              ? QString()
                                              : currentFileHistory->contentAt(path, chosenTurn.turn);
                    const auto diff = QSocLineDiff::computeLineDiff(before, after);
                    entry.added     = 0;
                    entry.removed   = 0;
                    for (const auto &line : diff) {
                        if (line.kind == QSocLineDiff::Kind::Add) {
                            entry.added++;
                        } else if (line.kind == QSocLineDiff::Kind::Del) {
                            entry.removed++;
                        }
                    }
                    fileEntries.append(entry);
                }
                std::sort(
                    fileEntries.begin(),
                    fileEntries.end(),
                    [](const FileEntry &lhs, const FileEntry &rhs) { return lhs.path < rhs.path; });
            }

            if (fileEntries.isEmpty()) {
                compositor.printContent("(no files changed in this turn)\n");
                continue;
            }

            QList<QTuiMenu::MenuItem> fileItems;
            fileItems.reserve(fileEntries.size());
            for (const FileEntry &entry : fileEntries) {
                QTuiMenu::MenuItem item;
                /* Strip the project-path prefix from the label so long
                 * absolute paths don't dominate the picker width. */
                QString       shortPath   = entry.path;
                const QString projectPath = sessionProjectPath(projectManager);
                if (!projectPath.isEmpty() && shortPath.startsWith(projectPath)) {
                    shortPath = shortPath.mid(projectPath.size());
                    if (shortPath.startsWith(QLatin1Char('/'))) {
                        shortPath = shortPath.mid(1);
                    }
                }
                item.label = shortPath;
                item.hint  = QString("+%1 -%2").arg(entry.added).arg(entry.removed);
                fileItems.append(item);
            }

            QTuiMenu fileMenu;
            fileMenu.setTitle(QString("Diff: turn #%1 files").arg(chosenTurn.turn));
            fileMenu.setItems(fileItems);
            fileMenu.setSearchable(true);
            fileMenu.setHighlight(0);
            const int fileSel = fileMenu.exec();
            inputMonitor.resetEscState();
            compositor.invalidate();
            compositor.render();
            if (fileSel < 0 || fileSel >= fileEntries.size()) {
                continue;
            }

            const FileEntry &chosenFile = fileEntries[fileSel];
            const QString    beforeText
                = chosenFile.shaBefore.isEmpty()
                      ? QString()
                      : currentFileHistory->contentAt(chosenFile.path, chosenTurn.turn - 1);
            const QString afterText
                = chosenFile.shaAfter.isEmpty()
                      ? QString()
                      : currentFileHistory->contentAt(chosenFile.path, chosenTurn.turn);
            renderDiffToScrollView(chosenFile.path, beforeText, afterText);
            continue;
        }
        if (cmd == "/clear") {
            agent->clearHistory();
            if (currentSession) {
                /* Truncate the existing JSONL in place — keep the same id
                 * so any in-flight references stay valid, then re-stamp
                 * the creation metadata so the file isn't entirely empty. */
                QFile::remove(currentSession->filePath());
                currentSession->appendMeta(
                    QStringLiteral("created"),
                    QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs));
                currentSession->appendMeta(QStringLiteral("cwd"), sessionProjectPath(projectManager));
            }
            if (currentFileHistory) {
                /* Drop every snapshot so future rewinds don't surface ghost
                 * turns from a cleared history. The next edit will create a
                 * fresh baseline. */
                currentFileHistory->truncateAfter(-1);
            }
            lastPersistedIndex = 0;
            turnCounter        = 0;
            compositor.printContent("History cleared.\n");
            continue;
        }
        if (cmd == "/help") {
            compositor.printContent("Commands:\n");
            compositor.printContent("  exit, /exit  - Exit the agent\n");
            compositor.printContent("  /branch [n]  - Fork session (optionally name it)\n");
            compositor.printContent("  /clear       - Clear conversation history\n");
            compositor.printContent("  /compact     - Compact conversation context\n");
            compositor.printContent("  /context     - Show token usage breakdown + suggestions\n");
            compositor.printContent("  /cost        - Show session token/cost totals\n");
            compositor.printContent(
                "  /cwd [path]  - Show or change the agent working directory (same project)\n");
            compositor.printContent("  /diff        - Review file edits turn-by-turn\n");
            compositor.printContent(
                "  /effort    - Show/set reasoning effort (off/low/medium/high)\n");
            compositor.printContent("  /model       - Show/switch model\n");
            compositor.printContent(
                "  /project <p> - Switch to another project (reloads config, clears caches,\n"
                "                 starts a new session; current session is saved)\n");
            compositor.printContent("  /rename <t>  - Set session title for resume picker\n");
            compositor.printContent(
                "  /ssh <u@host:port> - Switch to SSH remote workspace; /local to return\n");
            compositor.printContent("  /local       - Leave remote workspace, back to local mode\n");
            compositor.printContent("  /status      - Show model, session, endpoint info\n");
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
            compositor.printContent("  PgUp/PgDn   - Scroll scrollback by half a screen\n");
            compositor.printContent("  Ctrl+_      - Undo the last edit\n");
            compositor.printContent("  Mouse drag  - Select + auto-copy to clipboard (OSC 52)\n");
            compositor.printContent("  Shift+drag  - Native terminal selection (fallback)\n");
            compositor.printContent("  @<name>     - Fuzzy-complete a project file path\n");
            compositor.printContent("\n");

            /* List user-invocable skills so the user can discover what
             * /<name> commands the project and user dirs have registered. */
            if (auto *reg = agent->getToolRegistry()) {
                if (auto *skillTool = dynamic_cast<QSocToolSkillFind *>(
                        reg->getTool(QStringLiteral("skill_find")))) {
                    QList<QSocToolSkillFind::SkillInfo> userSkills;
                    for (const auto &skill : skillTool->scanAllSkills()) {
                        if (skill.userInvocable) {
                            userSkills.append(skill);
                        }
                    }
                    if (!userSkills.isEmpty()) {
                        compositor.printContent("Installed skills:\n");
                        for (const auto &skill : userSkills) {
                            QString line = QStringLiteral("  /") + skill.name;
                            if (!skill.argumentHint.isEmpty()) {
                                line += QLatin1Char(' ') + skill.argumentHint;
                            }
                            line += QStringLiteral(" [") + skill.scope + QStringLiteral("] - ")
                                    + skill.description + QLatin1Char('\n');
                            compositor.printContent(line);
                        }
                        compositor.printContent("\n");
                    }
                }
            }

            compositor.printContent("Or just type your question/request in natural language.\n");
            continue;
        }
        if (cmd == "/compact") {
            const int msgsBefore = static_cast<int>(agent->getMessages().size());
            const int tokBefore  = agent->estimateMessagesTokens();
            /* Show immediate feedback so the user knows it's working — the
             * LLM summarization call inside compact() can block for several
             * seconds and we don't want a frozen screen. */
            compositor.printContent("Compacting...\n", QTuiScrollView::Dim);
            statusBarWidget.setStatus("Compacting");
            compositor.render();
            /* Wire the per-layer progress signal for live feedback. */
            auto connCompacting = connect(
                agent, &QSocAgent::compacting, [&compositor](int layer, int before, int after) {
                    compositor.printContent(
                        QString("  Layer %1: %2 -> %3 tokens\n").arg(layer).arg(before).arg(after),
                        QTuiScrollView::Dim);
                    compositor.render();
                });
            const int saved = agent->compact();
            QObject::disconnect(connCompacting);
            const int msgsAfter = static_cast<int>(agent->getMessages().size());
            const int tokAfter  = tokBefore - saved;
            compositor.printContent(
                QString("Compacted: %1 messages -> %2 | %3 -> %4 tokens (saved %5)\n")
                    .arg(msgsBefore)
                    .arg(msgsAfter)
                    .arg(tokBefore)
                    .arg(tokAfter)
                    .arg(saved));
            /* The summarizer can produce a 0% reduction when the recent
             * kept zone already exceeds the budget. Tell users why so
             * they pick /clear or trim keepRecentMessages instead of
             * burning more LLM calls. */
            if (saved <= 0 && tokBefore > 0) {
                compositor.printContent(
                    QString(
                        "  Note: 0 tokens saved — recent kept zone dominates the "
                        "budget. Use /clear to reset history if compaction loops.\n"),
                    QTuiScrollView::Dim);
            }
            statusBarWidget.setStatus("Ready");
            lastPersistedIndex
                = persistSessionDelta(agent, currentSession.get(), lastPersistedIndex);
            continue;
        }
        if (cmd == "/cost") {
            auto fmtTok = [](qint64 tokens) -> QString {
                if (tokens >= 1000) {
                    return QString::number(tokens / 1000.0, 'f', 1) + "k";
                }
                return QString::number(tokens);
            };
            compositor.printContent("\n");
            compositor.printContent("Session Cost\n", QTuiScrollView::Bold);
            compositor.printContent(
                QString("  Input tokens:  %1\n").arg(fmtTok(sessionInputTokens)));
            compositor.printContent(
                QString("  Output tokens: %1\n").arg(fmtTok(sessionOutputTokens)));

            /* Configurable cost rates from project config. If not set, show
             * a hint instead of a dollar figure so users know they can
             * configure it. Keys: llm.cost_input_per_mtok,
             * llm.cost_output_per_mtok, llm.cost_currency. */
            double  inputRate  = 0;
            double  outputRate = 0;
            QString currency;
            if (socConfig) {
                const QString inStr  = socConfig->getValue("llm.cost_input_per_mtok");
                const QString outStr = socConfig->getValue("llm.cost_output_per_mtok");
                currency             = socConfig->getValue("llm.cost_currency");
                if (!inStr.isEmpty()) {
                    inputRate = inStr.toDouble();
                }
                if (!outStr.isEmpty()) {
                    outputRate = outStr.toDouble();
                }
            }
            if (inputRate > 0 || outputRate > 0) {
                if (currency.isEmpty()) {
                    currency = QStringLiteral("USD");
                }
                const double inputCost  = sessionInputTokens * inputRate / 1e6;
                const double outputCost = sessionOutputTokens * outputRate / 1e6;
                const double totalCost  = inputCost + outputCost;
                compositor.printContent(
                    QString("  Input cost:    %1 %2\n").arg(inputCost, 0, 'f', 4).arg(currency));
                compositor.printContent(
                    QString("  Output cost:   %1 %2\n").arg(outputCost, 0, 'f', 4).arg(currency));
                compositor.printContent(
                    QString("  Total:         %1 %2\n").arg(totalCost, 0, 'f', 4).arg(currency),
                    QTuiScrollView::Bold);
                compositor.printContent(
                    QString("  Rate: %1/%2 %3/Mtok (input/output)\n")
                        .arg(inputRate, 0, 'f', 2)
                        .arg(outputRate, 0, 'f', 2)
                        .arg(currency),
                    QTuiScrollView::Dim);
            } else {
                compositor.printContent(
                    "\n  Cost rates not configured. Set in project config:\n", QTuiScrollView::Dim);
                compositor.printContent(
                    "    llm.cost_input_per_mtok = <price per million input tokens>\n",
                    QTuiScrollView::Dim);
                compositor.printContent(
                    "    llm.cost_output_per_mtok = <price per million output tokens>\n",
                    QTuiScrollView::Dim);
                compositor.printContent(
                    "    llm.cost_currency = USD  (optional, default USD)\n", QTuiScrollView::Dim);
                compositor.printContent(
                    "  Or if you're on a subscription plan, cost tracking may not apply.\n",
                    QTuiScrollView::Dim);
            }
            compositor.printContent("\n");
            continue;
        }
        if (cmd == "/status") {
            compositor.printContent("\n");
            compositor.printContent("Status\n", QTuiScrollView::Bold);
            compositor.printContent(QString("  Model:    %1\n")
                                        .arg(
                                            llmService->getCurrentModelId().isEmpty()
                                                ? QStringLiteral("(default)")
                                                : llmService->getCurrentModelId()));
            compositor.printContent(QString("  Effort:   %1\n")
                                        .arg(
                                            agent->getConfig().effortLevel.isEmpty()
                                                ? QStringLiteral("off")
                                                : agent->getConfig().effortLevel));
            if (currentSession) {
                compositor.printContent(QString("  Session:  %1 (%2 messages)\n")
                                            .arg(currentSession->id().left(8))
                                            .arg(agent->getMessages().size()));
            }
            compositor.printContent(
                QString("  Project:  %1\n").arg(sessionProjectPath(projectManager)));
            /* Endpoint URL display would require exposing the endpoints list
             * from QLLMService; skip for now — model ID is sufficient. */
            if (currentFileHistory && !currentFileHistory->isEmpty()) {
                compositor.printContent(
                    QString("  File history: %1 snapshot(s)\n")
                        .arg(currentFileHistory->listSnapshots().size()),
                    QTuiScrollView::Dim);
            }
            compositor.printContent("\n");
            continue;
        }
        if (cmd.startsWith("/branch")) {
            const QString branchName = input.mid(7).trimmed();
            if (!currentSession) {
                compositor.printContent("(no active session to branch)\n");
                continue;
            }
            const QString projectPath = sessionProjectPath(projectManager);
            const QString newId       = QSocSession::generateId();
            const QString newPath
                = QDir(QSocSession::sessionsDir(projectPath)).filePath(newId + ".jsonl");

            /* Copy session JSONL. */
            QFile::copy(currentSession->filePath(), newPath);

            /* Append forkedFrom meta to the new session. */
            QSocSession newSession(newId, newPath);
            newSession.appendMeta(QStringLiteral("forkedFrom"), currentSession->id());
            if (!branchName.isEmpty()) {
                newSession.appendMeta(QStringLiteral("title"), branchName);
            }

            /* Copy file-history directory if it exists. */
            if (currentFileHistory) {
                const QString srcHist
                    = QSocFileHistory::historyDir(projectPath, currentSession->id());
                const QString dstHist = QSocFileHistory::historyDir(projectPath, newId);
                QDir().mkpath(dstHist);
                /* Copy snapshots.jsonl */
                QFile::copy(
                    QDir(srcHist).filePath(QStringLiteral("snapshots.jsonl")),
                    QDir(dstHist).filePath(QStringLiteral("snapshots.jsonl")));
                /* Hard-link (or copy) backup blobs. */
                QDir          backupsSrc(QDir(srcHist).filePath(QStringLiteral("backups")));
                const QString backupsDst = QDir(dstHist).filePath(QStringLiteral("backups"));
                QDir().mkpath(backupsDst);
                const auto entries
                    = backupsSrc.entryInfoList({QStringLiteral("*.bak")}, QDir::Files);
                for (const QFileInfo &entry : entries) {
                    const QString dst = QDir(backupsDst).filePath(entry.fileName());
                    if (!QFile::link(entry.absoluteFilePath(), dst)) {
                        QFile::copy(entry.absoluteFilePath(), dst);
                    }
                }
            }

            const QString label = branchName.isEmpty() ? newId.left(8) : branchName;
            compositor.printContent(
                QString("(Branched to %1 — resume with: --resume %2)\n").arg(label, newId.left(8)));
            continue;
        }
        if (cmd.startsWith("/rename")) {
            const QString newTitle = input.mid(7).trimmed();
            if (newTitle.isEmpty()) {
                compositor.printContent("Usage: /rename <title>\n");
                continue;
            }
            if (!currentSession) {
                compositor.printContent("(no active session)\n");
                continue;
            }
            currentSession->appendMeta(QStringLiteral("title"), newTitle);
            compositor.printContent(QString("Session renamed to: %1\n").arg(newTitle));
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
                compositor.setTitle("QSoC Agent · " + (mid.isEmpty() ? "default" : mid));
            }
            compositor.invalidate();
            compositor.render();
            continue;
        }
        if (cmd.startsWith(QStringLiteral("/ssh"))) {
            QString arg = input.mid(4).trimmed();

            /* Bare /ssh pops a picker built from the sticky binding target
             * plus concrete aliases in ~/.ssh/config. The selection feeds
             * back into `arg` so the parser below does the actual work. */
            if (arg.isEmpty()) {
                QList<QTuiMenu::MenuItem> items;
                QStringList               targets;
                auto addItem = [&](const QString &label, const QString &hint, const QString &value) {
                    QTuiMenu::MenuItem item;
                    item.label  = label;
                    item.hint   = hint;
                    item.marked = false;
                    items.append(item);
                    targets.append(value);
                };

                const QString projectRootLocal = pathContext ? pathContext->getProjectDir()
                                                             : QString();
                if (!projectRootLocal.isEmpty()) {
                    const auto bound = QSocRemoteBinding::read(projectRootLocal);
                    if (!bound.target.isEmpty()) {
                        addItem(bound.target, QStringLiteral("saved"), bound.target);
                    }
                }

                const QString sshConfigPath = QDir::homePath() + QStringLiteral("/.ssh/config");
                if (QFileInfo::exists(sshConfigPath)) {
                    QSocSshConfigParser parser;
                    parser.parse(sshConfigPath);
                    for (const QString &alias : parser.listMenuHosts()) {
                        addItem(alias, QString(), alias);
                    }
                }
                addItem(QStringLiteral("(type /ssh [user@]host[:port])"), QString(), QString());

                QTuiMenu menu;
                menu.setTitle(QStringLiteral("Remote target"));
                menu.setItems(items);
                menu.setSearchable(true);
                const int selected = menu.exec();
                inputMonitor.resetEscState();
                compositor.invalidate();
                compositor.render();
                if (selected < 0 || selected >= targets.size()) {
                    continue;
                }
                const QString picked = targets.at(selected);
                if (picked.isEmpty()) {
                    compositor.printContent(
                        "Usage: /ssh [user@]host[:port]\n"
                        "  User defaults to the current OS user and port defaults to 22.\n"
                        "  After connect a directory browser asks for the remote workspace;\n"
                        "  the choice is stored in <project>/.qsoc/remote.yml and reused\n"
                        "  on the next connect. Use /local to return to the local workspace.\n");
                    continue;
                }
                arg = picked;
            }

            /* Parse `[user@]host[:port]`. The workspace used to ride on
             * this argument separated by ':/', but that overloaded the
             * colon with port numbers; the picker after connect now
             * handles workspace selection for both first connect and
             * subsequent changes, so the UX is uniform. */
            QString user;
            QString hostname;
            int     port = 22;
            {
                QString   rest    = arg;
                const int atIndex = rest.indexOf(QLatin1Char('@'));
                if (atIndex >= 0) {
                    user = rest.left(atIndex);
                    rest = rest.mid(atIndex + 1);
                }
                QString hostPortPart = rest;
                if (hostPortPart.contains(QLatin1Char('/'))) {
                    compositor.printContent(
                        "Ignoring workspace path in /ssh argument. Pick the workspace "
                        "from the directory browser that opens after connecting, or use "
                        "/cwd later to change it.\n");
                    hostPortPart = hostPortPart.section(QLatin1Char('/'), 0, 0);
                    if (hostPortPart.endsWith(QLatin1Char(':'))) {
                        hostPortPart.chop(1);
                    }
                }
                const int colonIndex = hostPortPart.lastIndexOf(QLatin1Char(':'));
                if (colonIndex >= 0) {
                    hostname     = hostPortPart.left(colonIndex);
                    bool      ok = false;
                    const int p  = hostPortPart.mid(colonIndex + 1).toInt(&ok);
                    if (ok && p > 0 && p < 65536) {
                        port = p;
                    } else {
                        hostname = hostPortPart;
                    }
                } else {
                    hostname = hostPortPart;
                }
            }
            if (hostname.isEmpty()) {
                compositor.printContent("Invalid /ssh target.\n");
                continue;
            }

            /* Remember the alias as typed so the binding key and status line
             * stay human-readable. Actual TCP connect uses resolved values. */
            const QString rawAlias     = hostname;
            const bool    explicitUser = !user.isEmpty();
            const bool    explicitPort = port != 22 || arg.contains(QLatin1Char(':'));

            /* Pull ~/.ssh/config (with Include chains) so aliases like
             * r9pro resolve to the real HostName/Port/User/IdentityFile. */
            QSocSshConfigParser parser;
            {
                const QString cfg = QDir::homePath() + QStringLiteral("/.ssh/config");
                if (QFileInfo::exists(cfg)) {
                    parser.parse(cfg);
                }
            }
            const QSocSshHostConfig resolvedCfg = parser.resolve(rawAlias);
            if (resolvedCfg.fromConfig) {
                if (!resolvedCfg.hostname.isEmpty()) {
                    hostname = resolvedCfg.hostname;
                }
                if (!explicitPort && resolvedCfg.port > 0) {
                    port = resolvedCfg.port;
                }
                if (!explicitUser && !resolvedCfg.user.isEmpty()) {
                    user = resolvedCfg.user;
                }
            }
            if (user.isEmpty()) {
#ifdef Q_OS_WIN
                user = qEnvironmentVariable("USERNAME");
#else
                user = qEnvironmentVariable("USER");
                if (user.isEmpty()) {
                    user = qEnvironmentVariable("LOGNAME");
                }
#endif
            }
            if (user.isEmpty()) {
                compositor.printContent(
                    "Could not determine a default username; please write "
                    "/ssh <user>@<host>.\n");
                continue;
            }

            /* Binding key tracks the alias the user typed so lookups stay
             * stable even if the config file later switches HostName. */
            const QString targetKey = QStringLiteral("%1@%2:%3").arg(user, rawAlias).arg(port);

            const QString projectRoot = pathContext ? pathContext->getProjectDir() : QString();
            QString       workspace;
            if (!projectRoot.isEmpty()) {
                const auto bound = QSocRemoteBinding::read(projectRoot);
                if (bound.target == targetKey) {
                    workspace = bound.workspace;
                }
            }

            QSocSshHostConfig host;
            host.alias         = targetKey;
            host.hostname      = hostname;
            host.port          = port;
            host.user          = user;
            host.identityFiles = resolvedCfg.identityFiles;
            /* IdentitiesOnly=yes without any configured IdentityFile would
             * starve auth of keys because our parser does not synthesize the
             * default id_* list. Flip to no so the session's default key
             * enumeration kicks in, matching first-connect UX. */
            host.identitiesOnly = resolvedCfg.identitiesOnly && !host.identityFiles.isEmpty();
            host.proxyJump      = resolvedCfg.proxyJump;
            /* Default to accept-new so first-time connects and ProxyJump
             * hops work without a pre-populated known_hosts. Mismatches
             * still abort the connect. */
            host.strictHostKey = QSocSshHostConfig::StrictHostKey::AcceptNew;

            compositor.printContent(QString("Connecting to %1 ...\n").arg(targetKey));
            compositor.render();

            /* OS default user reused for any ProxyJump hop that lacks a
             * matching User directive in ~/.ssh/config. */
            QString osDefaultUser;
#ifdef Q_OS_WIN
            osDefaultUser = qEnvironmentVariable("USERNAME");
#else
            osDefaultUser = qEnvironmentVariable("USER");
            if (osDefaultUser.isEmpty()) {
                osDefaultUser = qEnvironmentVariable("LOGNAME");
            }
#endif

            /* Resolve a hop alias into a QSocSshHostConfig, filling in
             * sensible defaults when the alias is not in the config file. */
            auto hopConfig = [&](const QString &hopAlias) -> QSocSshHostConfig {
                QSocSshHostConfig cfg = parser.resolve(hopAlias);
                if (!cfg.fromConfig) {
                    cfg.hostname = hopAlias;
                    cfg.port     = 22;
                }
                if (cfg.user.isEmpty()) {
                    cfg.user = osDefaultUser;
                }
                cfg.alias = hopAlias;
                /* Same first-time policy as the final target, an unknown
                 * jump host with strict Yes would abort a connect that is
                 * otherwise valid. */
                cfg.strictHostKey = QSocSshHostConfig::StrictHostKey::AcceptNew;
                /* Avoid starving auth when config sets IdentitiesOnly=yes
                 * but lists no IdentityFile entries. */
                cfg.identitiesOnly = cfg.identitiesOnly && !cfg.identityFiles.isEmpty();
                return cfg;
            };

            /* Build the ProxyJump chain. connectChain returns the final
             * session on success and collects every intermediate hop in
             * outJumps so the caller can tear them down later. */
            QList<QSocSshSession *> localJumps;
            std::function<QSocSshSession *(const QSocSshHostConfig &, QSocSshSession *, QString *)>
                connectChain;
            connectChain = [&](const QSocSshHostConfig &cfg,
                               QSocSshSession          *parentSession,
                               QString                 *errOut) -> QSocSshSession                 *{
                QSocSshSession *currentParent = parentSession;
                for (const QString &hopAlias : cfg.proxyJump) {
                    const QSocSshHostConfig hopCfg = hopConfig(hopAlias);
                    QString                 hopErr;
                    QSocSshSession *hopSession = connectChain(hopCfg, currentParent, &hopErr);
                    if (hopSession == nullptr) {
                        if (errOut != nullptr) {
                            *errOut = QStringLiteral("ProxyJump via %1 failed: %2")
                                          .arg(hopAlias, hopErr);
                        }
                        return nullptr;
                    }
                    localJumps.append(hopSession);
                    currentParent = hopSession;
                }
                auto                         *session = new QSocSshSession(this);
                QSocSshSession::ConnectStatus status
                    = (currentParent != nullptr) ? session->connectToVia(cfg, currentParent, errOut)
                                                 : session->connectTo(cfg, errOut);
                if (status != QSocSshSession::ConnectStatus::Ok) {
                    delete session;
                    return nullptr;
                }
                return session;
            };

            QString err;
            auto   *newSession = connectChain(host, nullptr, &err);
            if (newSession == nullptr) {
                compositor.printContent(QString("SSH connect failed: %1\n").arg(err));
                for (auto it = localJumps.rbegin(); it != localJumps.rend(); ++it) {
                    (*it)->disconnectFromHost();
                    delete *it;
                }
                continue;
            }

            auto *newSftp = new QSocSftpClient(*newSession);
            if (!newSftp->open(&err)) {
                compositor.printContent(QString("SFTP open failed: %1\n").arg(err));
                delete newSftp;
                newSession->disconnectFromHost();
                delete newSession;
                continue;
            }

            /* First connect or binding unavailable. Launch the remote path
             * picker so the user browses the live server and selects the
             * workspace, keeping the experience identical to a later
             * /cwd change. */
            if (workspace.isEmpty()) {
                QString homeHint = QStringLiteral("/");
                {
                    QSocSshExec   homeExec(*newSession);
                    const auto    homeResult = homeExec.run(QStringLiteral("echo $HOME"), 5000);
                    const QString raw        = QString::fromUtf8(homeResult.stdoutBytes).trimmed();
                    if (!raw.isEmpty() && raw.startsWith(QLatin1Char('/'))) {
                        homeHint = raw;
                    }
                }
                QTuiPathPicker picker;
                picker.setTitle(QStringLiteral("Remote workspace"));
                picker.setStartPath(homeHint);
                picker.setListDirs([newSftp](const QString &path) -> QStringList {
                    QString     ignored;
                    const auto  entries = newSftp->listDir(path, 500, &ignored);
                    QStringList names;
                    names.reserve(entries.size());
                    for (const auto &entry : entries) {
                        if (entry.isDirectory) {
                            names.append(entry.name);
                        }
                    }
                    std::sort(names.begin(), names.end());
                    return names;
                });
                compositor.pause();
                workspace = picker.exec();
                compositor.resume();
                inputMonitor.resetEscState();
                if (workspace.isEmpty()) {
                    compositor.printContent("No workspace selected. Disconnecting.\n");
                    newSftp->close();
                    delete newSftp;
                    newSession->disconnectFromHost();
                    delete newSession;
                    for (auto it = localJumps.rbegin(); it != localJumps.rend(); ++it) {
                        (*it)->disconnectFromHost();
                        delete *it;
                    }
                    continue;
                }
            }

            if (!newSftp->mkdirP(workspace, &err)) {
                compositor.printContent(QString("Workspace mkdir failed: %1\n").arg(err));
                newSftp->close();
                delete newSftp;
                newSession->disconnectFromHost();
                delete newSession;
                for (auto it = localJumps.rbegin(); it != localJumps.rend(); ++it) {
                    (*it)->disconnectFromHost();
                    delete *it;
                }
                continue;
            }

            remotePath.setRoot(workspace);
            remotePath.setCwd(workspace);
            remotePath.setWritableDirs({workspace});

            /* Tear down any previous remote session. */
            if (remoteRegistry != nullptr) {
                remoteRegistry->deleteLater();
                remoteRegistry = nullptr;
            }
            if (remoteSftp != nullptr) {
                remoteSftp->close();
                delete remoteSftp;
                remoteSftp = nullptr;
            }
            if (remoteSession != nullptr) {
                remoteSession->disconnectFromHost();
                delete remoteSession;
                remoteSession = nullptr;
            }
            /* Tear down any previous ProxyJump chain in reverse order so
             * children disconnect before their parents. */
            for (auto it = remoteJumps.rbegin(); it != remoteJumps.rend(); ++it) {
                (*it)->disconnectFromHost();
                delete *it;
            }
            remoteJumps.clear();
            remoteSession = newSession;
            remoteSftp    = newSftp;
            remoteJumps   = localJumps;

            /* Build the remote tool registry with same-named tools. */
            remoteRegistry = new QSocToolRegistry(this);
            remoteRegistry->registerTool(new QSocToolRemoteFileRead(this, remoteSftp, &remotePath));
            remoteRegistry->registerTool(new QSocToolRemoteFileList(this, remoteSftp, &remotePath));
            remoteRegistry->registerTool(new QSocToolRemoteFileWrite(this, remoteSftp, &remotePath));
            remoteRegistry->registerTool(new QSocToolRemoteFileEdit(this, remoteSftp, &remotePath));
            remoteRegistry->registerTool(
                new QSocToolRemoteShellBash(this, remoteSession, &remotePath));
            remoteRegistry->registerTool(
                new QSocToolRemoteBashManage(this, remoteSession, &remotePath));
            remoteRegistry->registerTool(new QSocToolRemotePath(this, &remotePath));
            /* Control-plane tools stay on the local side (docs/web). */
            remoteRegistry->registerTool(new QSocToolDocQuery(this));
            remoteRegistry->registerTool(new QSocToolWebFetch(this, socConfig));
            if (socConfig != nullptr && !socConfig->getValue("web.search_api_url").isEmpty()) {
                remoteRegistry->registerTool(new QSocToolWebSearch(this, socConfig));
            }

            agent->setToolRegistry(remoteRegistry);

            /* Update agent config so the system prompt reflects remote mode. */
            {
                auto newCfg               = agent->getConfig();
                newCfg.remoteMode         = true;
                newCfg.remoteName         = targetKey;
                newCfg.remoteDisplay      = targetKey + QStringLiteral(":") + workspace;
                newCfg.remoteWorkspace    = workspace;
                newCfg.remoteWorkingDir   = workspace;
                newCfg.remoteWritableDirs = remotePath.writableDirs();
                agent->setConfig(newCfg);
            }

            /* Sticky binding. */
            if (!projectRoot.isEmpty()) {
                QSocRemoteBinding::Entry toStore;
                toStore.target    = targetKey;
                toStore.workspace = workspace;
                QString bindErr;
                QSocRemoteBinding::write(projectRoot, toStore, &bindErr);
                if (!bindErr.isEmpty()) {
                    compositor.printContent(QString("Binding write warning: %1\n").arg(bindErr));
                }
            }

            statusBarWidget.setStatus(QString("Remote: %1").arg(targetKey));
            compositor.printContent(QString("Connected. Remote workspace: %1\n").arg(workspace));
            continue;
        }
        if (cmd == QStringLiteral("/local")) {
            if (remoteSession == nullptr) {
                compositor.printContent("Already in local mode.\n");
                continue;
            }
            /* Restore local tool registry and drop remote state. */
            agent->setToolRegistry(localRegistry);
            if (remoteRegistry != nullptr) {
                remoteRegistry->deleteLater();
                remoteRegistry = nullptr;
            }
            if (remoteSftp != nullptr) {
                remoteSftp->close();
                delete remoteSftp;
                remoteSftp = nullptr;
            }
            if (remoteSession != nullptr) {
                remoteSession->disconnectFromHost();
                delete remoteSession;
                remoteSession = nullptr;
            }
            /* Close ProxyJump hops in reverse order so each child channel
             * is freed before its parent session tears down its TCP. */
            for (auto it = remoteJumps.rbegin(); it != remoteJumps.rend(); ++it) {
                (*it)->disconnectFromHost();
                delete *it;
            }
            remoteJumps.clear();
            remotePath = QSocRemotePathContext{};

            {
                auto newCfg       = agent->getConfig();
                newCfg.remoteMode = false;
                newCfg.remoteName.clear();
                newCfg.remoteDisplay.clear();
                newCfg.remoteWorkspace.clear();
                newCfg.remoteWorkingDir.clear();
                newCfg.remoteWritableDirs.clear();
                agent->setConfig(newCfg);
            }

            statusBarWidget.setStatus("Ready");
            compositor.printContent("Returned to local workspace (binding kept).\n");
            continue;
        }
        if (cmd == QStringLiteral("/cwd") || cmd.startsWith(QStringLiteral("/cwd "))) {
            QString arg = input.mid(4).trimmed();

            /* Bare /cwd on a remote session opens the same two-column
             * directory browser that /ssh uses for the workspace, so the
             * user gets parent navigation and consistent UX. */
            if (arg.isEmpty() && remoteSession != nullptr && remoteSftp != nullptr) {
                QTuiPathPicker picker;
                picker.setTitle(QStringLiteral("Remote cwd"));
                picker.setStartPath(remotePath.cwd());
                QSocSftpClient *sftp = remoteSftp;
                picker.setListDirs([sftp](const QString &path) -> QStringList {
                    QString     ignored;
                    const auto  entries = sftp->listDir(path, 500, &ignored);
                    QStringList names;
                    names.reserve(entries.size());
                    for (const auto &entry : entries) {
                        if (entry.isDirectory) {
                            names.append(entry.name);
                        }
                    }
                    std::sort(names.begin(), names.end());
                    return names;
                });
                compositor.pause();
                const QString picked = picker.exec();
                compositor.resume();
                inputMonitor.resetEscState();
                if (picked.isEmpty()) {
                    continue;
                }
                const QString resolved = remotePath.resolveCwdRequest(picked);
                remotePath.setCwd(resolved);
                {
                    auto newCfg             = agent->getConfig();
                    newCfg.remoteWorkingDir = resolved;
                    agent->setConfig(newCfg);
                }
                compositor.printContent(QString("Remote cwd: %1\n").arg(resolved));
                continue;
            }

            if (arg.isEmpty()) {
                /* Local mode uses the same picker as remote so both modes
                 * share navigation keys and layout. */
                QTuiPathPicker picker;
                picker.setTitle(QStringLiteral("Local cwd"));
                const QString base = pathContext ? pathContext->getWorkingDir()
                                                 : QDir::currentPath();
                picker.setStartPath(base);
                picker.setListDirs([](const QString &path) -> QStringList {
                    QStringList names = QDir(path).entryList(
                        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks, QDir::Name);
                    return names;
                });
                compositor.pause();
                const QString picked = picker.exec();
                compositor.resume();
                inputMonitor.resetEscState();
                if (picked.isEmpty()) {
                    const QString workingDir = pathContext ? pathContext->getWorkingDir()
                                                           : QDir::currentPath();
                    compositor.printContent(QString("Working dir: %1\n").arg(workingDir));
                    const QString projectDir = projectManager->getProjectPath();
                    if (!projectDir.isEmpty()) {
                        compositor.printContent(
                            QString("Project dir: %1\n").arg(projectDir), QTuiScrollView::Dim);
                    }
                    continue;
                }
                arg = picked;
            }

            /* Remote workspace: /cwd updates the remote cwd via
             * QSocRemotePathContext, clamped to the workspace root. */
            if (remoteSession != nullptr) {
                const QString resolved = remotePath.resolveCwdRequest(arg);
                remotePath.setCwd(resolved);
                {
                    auto newCfg             = agent->getConfig();
                    newCfg.remoteWorkingDir = resolved;
                    agent->setConfig(newCfg);
                }
                compositor.printContent(QString("Remote cwd: %1\n").arg(resolved));
                continue;
            }
            /* Resolve relative paths against the current agent working dir,
             * not the launch-time process cwd, so user intent is preserved. */
            const QString base = pathContext ? pathContext->getWorkingDir() : QDir::currentPath();
            const QString resolved = QFileInfo(arg).isAbsolute() ? arg
                                                                 : QDir(base).absoluteFilePath(arg);
            const QFileInfo info(resolved);
            if (!info.exists() || !info.isDir()) {
                compositor.printContent(QString("Not a directory: %1\n").arg(resolved));
                continue;
            }
            const QString canonical = info.canonicalFilePath();
            if (pathContext) {
                pathContext->setWorkingDir(canonical);
            }
            compositor.printContent(QString("Working dir: %1\n").arg(canonical));
            continue;
        }
        if (cmd == QStringLiteral("/project") || cmd.startsWith(QStringLiteral("/project "))) {
            const QString arg = input.mid(8).trimmed();
            if (arg.isEmpty()) {
                const QString projectDir = projectManager->getProjectPath();
                compositor.printContent(
                    QString("Project: %1\n")
                        .arg(projectDir.isEmpty() ? QStringLiteral("(none)") : projectDir));
                continue;
            }
            const QString base = pathContext ? pathContext->getWorkingDir() : QDir::currentPath();
            const QString resolved = QFileInfo(arg).isAbsolute() ? arg
                                                                 : QDir(base).absoluteFilePath(arg);
            const QFileInfo info(resolved);
            if (!info.exists() || !info.isDir()) {
                compositor.printContent(QString("Not a directory: %1\n").arg(resolved));
                continue;
            }
            const QString canonical = info.canonicalFilePath();

            /* Short-circuit: switching to the project we're already on is a
             * no-op. Doing the full teardown would needlessly clear history
             * and spawn a new session. */
            if (canonical == projectManager->getProjectPath()) {
                compositor.printContent(QString("Already on project: %1\n").arg(canonical));
                continue;
            }

            /* Persist any outstanding session delta before we switch away from
             * the current project; the new project gets a brand-new session. */
            if (currentSession) {
                lastPersistedIndex
                    = persistSessionDelta(agent, currentSession.get(), lastPersistedIndex);
            }

            /* Drop all in-memory state tied to the old project. Skipping any
             * of these lets old buses/modules/netlists/LSP diagnostics leak
             * into the new project's agent view. */
            busManager->resetBusData();
            moduleManager->resetModuleData();
            generateManager->resetGenerateData();
            QSocToolShellBash::killAllActive();
            if (auto *lsp = QLspService::instance()) {
                lsp->stopAll();
            }
            if (pathContext) {
                pathContext->clearUserDirs();
            }

            /* Swap projectManager over to the new directory and re-run the
             * same init sequence the startup path uses. */
            projectManager->setProjectPath(canonical);
            socConfig->loadConfig();
            llmService->setConfig(socConfig);
            const bool loaded = projectManager->loadFirst(true);

            if (auto *lsp = QLspService::instance()) {
                lsp->startAll(projectManager->getProjectPath());
            }

            /* Agent config carries projectPath into the system prompt, so
             * AGENTS.md / skill listing from the new project only get picked
             * up after we feed the updated config back in. */
            {
                QSocAgentConfig cfg = agent->getConfig();
                cfg.projectPath     = projectManager->getProjectPath();
                agent->setConfig(cfg);
            }
            reloadSkills();

            if (pathContext) {
                pathContext->setWorkingDir(projectManager->getProjectPath());
            }

            /* Reset per-session counters so /cost and the status bar don't
             * show cumulative totals across two different projects. */
            sessionInputTokens  = 0;
            sessionOutputTokens = 0;
            statusBarWidget.updateTokens(0, 0);

            /* The previous conversation is about the previous project; its
             * JSONL stays on disk but the in-memory agent starts clean. */
            agent->clearHistory();
            startFreshSessionAt(sessionProjectPath(projectManager));
            lastPersistedIndex = 0;
            turnCounter        = 0;

            /* Reload per-project UI state: TODO widget and input history. */
            loadTodoWidget(projectManager->getProjectPath());
            loadInputHistory(projectManager->getProjectPath());

            compositor.printContent(
                QString("Project: %1 (new session %2)%3\n")
                    .arg(
                        canonical,
                        currentSession->id().left(8),
                        loaded ? QString() : QStringLiteral(" [no *.soc_pro found]")));
            continue;
        }

        /* User-invocable skill dispatch: if the command matches a registered
         * skill name, read the SKILL.md body and prepend it to the user
         * input so the LLM sees the skill prompt + any trailing arguments
         * as a single user message. This replaces `input` and falls through
         * to the normal agent query path below. */
        bool skillDispatched = false;
        if (cmd.startsWith(QLatin1Char('/')) && !skillPaths.isEmpty()) {
            /* Extract "/name args..." (cmd is already lowercased). */
            const int     spaceIdx = cmd.indexOf(QLatin1Char(' '));
            const QString skillCmd = (spaceIdx > 0) ? cmd.left(spaceIdx) : cmd;
            if (skillPaths.contains(skillCmd)) {
                const QString skillContent = QSocToolSkillFind(nullptr, projectManager)
                                                 .readSkillContent(skillPaths.value(skillCmd));
                if (!skillContent.isEmpty()) {
                    const QString args = (spaceIdx > 0) ? input.mid(spaceIdx + 1).trimmed()
                                                        : QString();
                    input              = skillContent;
                    if (!args.isEmpty()) {
                        input += QStringLiteral("\n\nArguments passed: ") + args;
                    }
                    compositor.printContent(
                        QString("(Running skill %1)\n").arg(skillCmd), QTuiScrollView::Dim);
                    skillDispatched = true;
                }
            }
        }

        /* Reject unrecognised slash commands so they don't get shipped to the
         * LLM as a typo'd query. Builtins were already handled above; skills
         * set skillDispatched. Anything still starting with '/' is unknown. */
        if (!skillDispatched && cmd.startsWith(QLatin1Char('/'))) {
            const int     spaceIdx = cmd.indexOf(QLatin1Char(' '));
            const QString unknown  = (spaceIdx > 0) ? cmd.left(spaceIdx) : cmd;
            compositor.printContent(
                QString("Unknown command: %1 (type /help for a list).\n").arg(unknown),
                QTuiScrollView::Dim);
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
                 &inputWidget,
                 &pendingDiffPath,
                 &pendingDiffOldString,
                 &pendingDiffNewString](const QString &toolName, const QString &arguments) {
                    Q_UNUSED(compositor)
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

                        /* Capture edit_file args for the diff renderer in the
                         * matching toolResult hook below. */
                        if (toolName == "edit_file" && args.contains("file_path")
                            && args.contains("old_string") && args.contains("new_string")) {
                            pendingDiffPath = QString::fromStdString(
                                args["file_path"].get<std::string>());
                            pendingDiffOldString = QString::fromStdString(
                                args["old_string"].get<std::string>());
                            pendingDiffNewString = QString::fromStdString(
                                args["new_string"].get<std::string>());
                        } else {
                            pendingDiffPath.clear();
                            pendingDiffOldString.clear();
                            pendingDiffNewString.clear();
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
                 &inputWidget,
                 &pendingDiffPath,
                 &pendingDiffOldString,
                 &pendingDiffNewString,
                 &renderDiffToScrollView](const QString &toolName, const QString &result) {
                    statusBarWidget.resetProgress();
                    statusBarWidget.setStatus(QString("%1 done, reasoning").arg(toolName));

                    /* edit_file diff: render a colored unified diff to the
                     * scroll view when the previous toolCalled hook stashed
                     * old/new strings AND the result indicates success. */
                    if (toolName == "edit_file" && !pendingDiffPath.isEmpty()
                        && result.startsWith(QStringLiteral("Successfully"))) {
                        renderDiffToScrollView(
                            pendingDiffPath, pendingDiffOldString, pendingDiffNewString);
                    }
                    pendingDiffPath.clear();
                    pendingDiffOldString.clear();
                    pendingDiffNewString.clear();

                    /* Parse and update todo list based on tool type */
                    if (toolName == "todo_list") {
                        auto items = parseTodoListResult(result);
                        todoWidget.setItems(items);
                        /* Done items expire via the 30s tick() timer. */
                    } else if (toolName == "todo_add") {
                        auto item = parseTodoAddResult(result);
                        if (item.id >= 0) {
                            todoWidget.addItem(item);
                        }
                    } else if (toolName == "todo_update") {
                        auto [todoId, newStatus] = parseTodoUpdateResult(result);
                        if (todoId >= 0) {
                            todoWidget.updateStatus(todoId, newStatus);
                            /* Done items expire via the 30s tick() timer. */
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

            /* Connect heartbeat - keeps the tick alive but does not
             * overwrite the status text. Earlier code unconditionally
             * set "Working" here, which clobbered tool-name labels
             * (e.g. "bash", "read_file") set by toolCalled, leaving
             * users with a generic status during long tool calls. */
            auto connHeartbeat = QObject::connect(
                agent,
                &QSocAgent::heartbeat,
                &loop,
                [&compositor, &statusBarWidget, &todoWidget, &queueWidget, &inputWidget](int, int) {
                });

            /* Connect token usage update + session cost accumulator */
            auto connTokens = QObject::connect(
                agent,
                &QSocAgent::tokenUsage,
                &loop,
                [&statusBarWidget,
                 &sessionInputTokens,
                 &sessionOutputTokens](qint64 input, qint64 output) {
                    statusBarWidget.updateTokens(input, output);
                    sessionInputTokens  = input;
                    sessionOutputTokens = output;
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
                        std::exit(130);
                    }
                    agent->abort();
                });

            /* Connect reasoning chunk display */
            auto reasoningCollapser = std::make_shared<ReasoningNewlineCollapser>();
            auto connReasoning      = QObject::connect(
                agent,
                &QSocAgent::reasoningChunk,
                &compositor,
                [&compositor, reasoningCollapser](const QString &chunk) {
                    const QString filtered = reasoningCollapser->feed(chunk);
                    if (!filtered.isEmpty()) {
                        compositor.printContent(filtered, QTuiScrollView::Dim);
                    }
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
                 &remoteSession,
                 &remotePath,
                 llm = this->llmService](const QString &text) {
                    if (text.startsWith("!")) {
                        QString shellCmd = text.mid(1).trimmed();
                        if (!shellCmd.isEmpty()) {
                            compositor.printContent("$ " + shellCmd + "\n", QTuiScrollView::Bold);
                            compositor.pause();
                            const QString output = remoteSession != nullptr
                                                       ? runRemoteShellEscape(
                                                             *remoteSession,
                                                             remotePath.cwd(),
                                                             shellCmd)
                                                       : runShellEscape(shellCmd);
                            compositor.resume();
                            if (!output.isEmpty()) {
                                compositor.printContent(output, QTuiScrollView::Dim);
                                if (!output.endsWith(QLatin1Char('\n'))) {
                                    compositor.printContent("\n");
                                }
                            }
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

            /* Done TODOs expire via the 30s tick() timer — no eager clear. */

            /* Start status line and agent */
            statusBarWidget.setEffortLevel(agent->getConfig().effortLevel);
            statusBarWidget.setStatus("Reasoning");
            statusBarWidget.startTimers();
            {
                QString mid = llmService->getCurrentModelId();
                compositor.setTitle("QSoC Agent · " + (mid.isEmpty() ? "default" : mid));
            }
            compositor.start();
            agent->runStream(input);
            loop.exec();
            loopRunning = false;

            /* Save conversation after each interaction */
            lastPersistedIndex
                = persistSessionDelta(agent, currentSession.get(), lastPersistedIndex);
            /* Snapshot file state after the turn settles so rewind can
             * restore whatever the agent's tools just did. The counter is
             * monotonic for the session so each call lands in its own
             * snapshots.jsonl entry. */
            if (currentFileHistory) {
                turnCounter++;
                currentFileHistory->makeSnapshot(turnCounter);
            }

            /* Auto-compact: if context usage exceeds the configured threshold
             * and the circuit breaker hasn't tripped, compact now. */
            if (autoCompactFailures < AUTO_COMPACT_MAX_FAILURES) {
                const int    currentTokens = agent->estimateTotalTokens();
                const double threshold     = agent->getConfig().compactThreshold;
                const int    maxCtx        = agent->getConfig().maxContextTokens;
                if (currentTokens > static_cast<int>(maxCtx * threshold)) {
                    compositor.printContent("(auto-compacting...)\n", QTuiScrollView::Dim);
                    statusBarWidget.setStatus("Compacting");
                    compositor.render();
                    const int before = agent->estimateMessagesTokens();
                    const int saved  = agent->compact();
                    statusBarWidget.setStatus("Ready");
                    if (saved > 0) {
                        autoCompactFailures = 0;
                        lastPersistedIndex
                            = persistSessionDelta(agent, currentSession.get(), lastPersistedIndex);
                        compositor.printContent(
                            QString("(auto-compacted: %1 -> %2 tokens, saved %3)\n")
                                .arg(before)
                                .arg(before - saved)
                                .arg(saved),
                            QTuiScrollView::Dim);
                    } else {
                        autoCompactFailures++;
                        compositor.printContent(
                            QString(
                                "(auto-compact saved 0 tokens; recent kept zone "
                                "dominates — consider /clear)\n"),
                            QTuiScrollView::Dim);
                    }
                }
            }

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
                 &inputWidget,
                 &pendingDiffPath,
                 &pendingDiffOldString,
                 &pendingDiffNewString](const QString &toolName, const QString &arguments) {
                    Q_UNUSED(compositor)
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

                        /* Capture edit_file args for the diff renderer in the
                         * matching toolResult hook below. */
                        if (toolName == "edit_file" && args.contains("file_path")
                            && args.contains("old_string") && args.contains("new_string")) {
                            pendingDiffPath = QString::fromStdString(
                                args["file_path"].get<std::string>());
                            pendingDiffOldString = QString::fromStdString(
                                args["old_string"].get<std::string>());
                            pendingDiffNewString = QString::fromStdString(
                                args["new_string"].get<std::string>());
                        } else {
                            pendingDiffPath.clear();
                            pendingDiffOldString.clear();
                            pendingDiffNewString.clear();
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
                 &inputWidget,
                 &pendingDiffPath,
                 &pendingDiffOldString,
                 &pendingDiffNewString,
                 &renderDiffToScrollView](const QString &toolName, const QString &result) {
                    statusBarWidget.resetProgress();
                    statusBarWidget.setStatus(QString("%1 done, reasoning").arg(toolName));

                    /* edit_file diff: render a colored unified diff to the
                     * scroll view when the previous toolCalled hook stashed
                     * old/new strings AND the result indicates success. */
                    if (toolName == "edit_file" && !pendingDiffPath.isEmpty()
                        && result.startsWith(QStringLiteral("Successfully"))) {
                        renderDiffToScrollView(
                            pendingDiffPath, pendingDiffOldString, pendingDiffNewString);
                    }
                    pendingDiffPath.clear();
                    pendingDiffOldString.clear();
                    pendingDiffNewString.clear();

                    /* Parse and update todo list based on tool type */
                    if (toolName == "todo_list") {
                        auto items = parseTodoListResult(result);
                        todoWidget.setItems(items);
                        /* Done items expire via the 30s tick() timer. */
                    } else if (toolName == "todo_add") {
                        auto item = parseTodoAddResult(result);
                        if (item.id >= 0) {
                            todoWidget.addItem(item);
                        }
                    } else if (toolName == "todo_update") {
                        auto [todoId, newStatus] = parseTodoUpdateResult(result);
                        if (todoId >= 0) {
                            todoWidget.updateStatus(todoId, newStatus);
                            /* Done items expire via the 30s tick() timer. */
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

            /* Connect heartbeat - keeps the tick alive but does not
             * overwrite the status text. Earlier code unconditionally
             * set "Working" here, which clobbered tool-name labels
             * (e.g. "bash", "read_file") set by toolCalled, leaving
             * users with a generic status during long tool calls. */
            auto connHeartbeat = QObject::connect(
                agent,
                &QSocAgent::heartbeat,
                &loop,
                [&compositor, &statusBarWidget, &todoWidget, &queueWidget, &inputWidget](int, int) {
                });

            /* Connect token usage update + session cost accumulator */
            auto connTokens = QObject::connect(
                agent,
                &QSocAgent::tokenUsage,
                &loop,
                [&statusBarWidget,
                 &sessionInputTokens,
                 &sessionOutputTokens](qint64 input, qint64 output) {
                    statusBarWidget.updateTokens(input, output);
                    sessionInputTokens  = input;
                    sessionOutputTokens = output;
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
                        std::exit(130);
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
                 &remoteSession,
                 &remotePath,
                 llm = this->llmService](const QString &text) {
                    if (text.startsWith("!")) {
                        QString shellCmd = text.mid(1).trimmed();
                        if (!shellCmd.isEmpty()) {
                            compositor.printContent("$ " + shellCmd + "\n", QTuiScrollView::Bold);
                            compositor.pause();
                            const QString output = remoteSession != nullptr
                                                       ? runRemoteShellEscape(
                                                             *remoteSession,
                                                             remotePath.cwd(),
                                                             shellCmd)
                                                       : runShellEscape(shellCmd);
                            compositor.resume();
                            if (!output.isEmpty()) {
                                compositor.printContent(output, QTuiScrollView::Dim);
                                if (!output.endsWith(QLatin1Char('\n'))) {
                                    compositor.printContent("\n");
                                }
                            }
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

            /* Done TODOs expire via the 30s tick() timer — no eager clear. */

            /* Start status line and agent */
            statusBarWidget.setEffortLevel(agent->getConfig().effortLevel);
            statusBarWidget.setStatus("Reasoning");
            statusBarWidget.startTimers();
            {
                QString mid = llmService->getCurrentModelId();
                compositor.setTitle("QSoC Agent · " + (mid.isEmpty() ? "default" : mid));
            }
            compositor.start();
            agent->runStream(input);
            loop.exec();
            loopRunning = false;

            /* Save conversation after each interaction */
            lastPersistedIndex
                = persistSessionDelta(agent, currentSession.get(), lastPersistedIndex);
            /* Snapshot file state for non-streaming path too. */
            if (currentFileHistory) {
                turnCounter++;
                currentFileHistory->makeSnapshot(turnCounter);
            }

            /* Auto-compact (same logic as streaming path). */
            if (autoCompactFailures < AUTO_COMPACT_MAX_FAILURES) {
                const int    currentTokens = agent->estimateTotalTokens();
                const double threshold     = agent->getConfig().compactThreshold;
                const int    maxCtx        = agent->getConfig().maxContextTokens;
                if (currentTokens > static_cast<int>(maxCtx * threshold)) {
                    const int before = agent->estimateMessagesTokens();
                    const int saved  = agent->compact();
                    if (saved > 0) {
                        autoCompactFailures = 0;
                        lastPersistedIndex
                            = persistSessionDelta(agent, currentSession.get(), lastPersistedIndex);
                        compositor.printContent(
                            QString("(auto-compacted: %1 -> %2 tokens, saved %3)\n")
                                .arg(before)
                                .arg(before - saved)
                                .arg(saved),
                            QTuiScrollView::Dim);
                    } else {
                        autoCompactFailures++;
                        compositor.printContent(
                            QString(
                                "(auto-compact saved 0 tokens; recent kept zone "
                                "dominates — consider /clear)\n"),
                            QTuiScrollView::Dim);
                    }
                }
            }

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
