// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
#include "agent/remote/qsocinterrupt.h"
#include "agent/runtime/qsocagentruntime.h"
#include "cli/qsoccliworker.h"
#include "common/qsocconsole.h"
#include <QDir>

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
        {"tool-presentation",
         QCoreApplication::translate("main", "Tool presentation: direct, catalog, or auto."),
         "mode"},
        {"effort",
         QCoreApplication::translate("main", "Reasoning effort level (low/medium/high)."),
         "level"},
        {"resume",
         QCoreApplication::translate(
             "main",
             "Resume a previous session. Pass the id (or unique prefix) as a positional "
             "argument to load it directly, or omit it to pick from a list.")},
        {"continue",
         QCoreApplication::translate("main", "Continue the most recent session for this project.")},
        {"workspace",
         QCoreApplication::translate(
             "main",
             "Working directory for tool execution. Local absolute path by "
             "default; remote absolute path when paired with --ssh."),
         "path"},
        {"ssh",
         QCoreApplication::translate(
             "main",
             "Connect to a remote workspace before the agent starts. Pass "
             "[user@]host[:port] or a ~/.ssh/config alias; --workspace is "
             "required and must be an absolute remote path."),
         "target"},
        {"connect",
         QCoreApplication::translate(
             "main",
             "Connect to a running qsoc-agentd daemon over its unix socket "
             "instead of launching an owned child. The TUI is a "
             "pure frontend; all agent infrastructure lives in the daemon."),
         "socket path"},
    });

    parser.addPositionalArgument(
        "session-id",
        QCoreApplication::translate(
            "main", "Optional session id (or unique prefix) when --resume is set."),
        "[session-id]");

    if (!parser.parse(appArguments))
        return showErrorWithHelp(2, parser.errorText());

    if (parser.isSet("help")) {
        return showHelp(0);
    }

    /* Build the runtime options from the CLI flags. The runtime reads the
     * config layers itself; these are the caller's overrides. */
    QSocAgentRuntimeOptions options;
    options.launchDirectory = QDir::currentPath();
    options.clientProgram   = QCoreApplication::arguments().value(0, QStringLiteral("qsoc"));
    if (parser.isSet("directory")) {
        options.projectDirectory = parser.value("directory");
    }
    if (parser.isSet("project")) {
        options.projectName = parser.value("project");
    }
    if (parser.isSet("workspace")) {
        options.workspace = parser.value("workspace");
    }
    if (parser.isSet("ssh")) {
        options.sshTarget = parser.value("ssh");
        if (options.workspace.isEmpty() || !options.workspace.startsWith(QLatin1Char('/'))) {
            return showError(
                2,
                QStringLiteral("--ssh requires --workspace with an absolute remote path; got '%1'")
                    .arg(options.workspace));
        }
    }
    if (parser.isSet("max-tokens")) {
        options.maxContextTokens = parser.value("max-tokens").toInt();
    }
    if (parser.isSet("temperature")) {
        options.temperature = parser.value("temperature").toDouble();
    }
    if (parser.isSet("effort")) {
        options.effortLevel = parser.value("effort").toLower();
    }
    if (parser.isSet("tool-presentation")) {
        options.toolPresentation = parser.value("tool-presentation");
        if (options.toolPresentation != "direct" && options.toolPresentation != "catalog"
            && options.toolPresentation != "auto") {
            return showError(2, "Invalid tool presentation: expected direct, catalog, or auto");
        }
    }

    options.streaming           = !parser.isSet("no-stream");
    options.streamingFromConfig = !parser.isSet("no-stream");
    options.verbose             = QSocConsole::level() >= QSocConsole::Level::Debug;

    options.projectDirectory
        = QDir(options.projectDirectory.isEmpty() ? QDir::currentPath() : options.projectDirectory)
              .absolutePath();
    if (parser.isSet("resume")) {
        const auto positional   = parser.positionalArguments();
        options.resumeSessionId = positional.isEmpty() ? QStringLiteral("-") : positional.first();
    } else if (parser.isSet("continue"))
        options.continueLatestSession = true;

    /* Interrupt bridge: the runtime's remote connect and the REPL's
     * Ctrl-C handling both need it. */
    const bool interruptBridgeInstalled = QSocInterrupt::installBridge();
#ifndef Q_OS_WIN
    if (!interruptBridgeInstalled && !QSocInterrupt::byteFallbackReady()) {
        return showError(1, QStringLiteral("Ctrl-C handling could not be installed safely"));
    }
#else
    (void) interruptBridgeInstalled;
#endif
    if (parser.isSet("query")) {
        QSocInterrupt::clearRequest();
    }

    return runAgentClientLoop(parser.value("connect"), options);
}
