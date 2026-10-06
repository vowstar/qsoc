// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocshellcommand.h"

#include "agent/qsoctool.h"

#include <QHash>

#include <limits>

using json = nlohmann::json;

namespace QSocShellCommand {

namespace {

void appendLine(QString *text, const QString &line)
{
    *text += line;
    if (!text->endsWith(QLatin1Char('\n'))) {
        *text += QLatin1Char('\n');
    }
}

QString lastOutputBlock(const Handle &handle)
{
    return QStringLiteral("%1: %2\nOutput file: %3\nLast output:\n%4\n\n")
        .arg(handle.idLabel, handle.id, handle.outputFile, handle.lastOutput);
}

} // namespace

QString resultText(const Outcome &outcome)
{
    /* Every field the reader needs to judge the call goes ahead of the body,
     * where a reader that stops early still finds it. */
    QString text = QSocTool::statusLine(outcome.status);
    text += outcome.exitCode < 0 ? QStringLiteral("exit_code: unknown\n")
                                 : QStringLiteral("exit_code: %1\n").arg(outcome.exitCode);
    if (!outcome.exitSignal.isEmpty()) {
        text += QStringLiteral("exit_signal: SIG%1\n").arg(outcome.exitSignal);
    }
    if (outcome.timedOut) {
        text += QStringLiteral("timed_out: true\n");
    }
    if (outcome.aborted) {
        text += QStringLiteral("aborted: true\n");
    }
    if (outcome.transportDead) {
        text += QStringLiteral("transport_dead: true\n");
    }
    if (!outcome.error.isEmpty()) {
        appendLine(&text, QStringLiteral("error: ") + outcome.error);
    }
    appendLine(&text, outcome.output.isEmpty() ? QStringLiteral("(no output)") : outcome.output);
    return text;
}

QString timedOutText(int timeoutMs, const Handle &handle)
{
    return QSocTool::statusLine(QSocToolResultStatus::Dispatched)
           + QStringLiteral("Command timed out after %1ms but is STILL RUNNING.\n").arg(timeoutMs)
           + lastOutputBlock(handle)
           + QStringLiteral(
                 "Use bash_manage tool with %1=%2 to: check status, wait more, read output, "
                 "kill, or terminate.")
                 .arg(handle.idKey, handle.id);
}

QString stillRunningAfterAbortText(const Handle &handle)
{
    return QSocTool::statusLine(QSocToolResultStatus::Uncertain)
           + QStringLiteral("Abort requested but the command is STILL RUNNING.\n")
           + lastOutputBlock(handle)
           + QStringLiteral("Use bash_manage tool with %1=%2 to stop it.")
                 .arg(handle.idKey, handle.id);
}

QString abortedText()
{
    return QStringLiteral("Command aborted.");
}

QString signalName(int signal)
{
    static const QHash<int, QString> names{
        {1, QStringLiteral("HUP")},
        {2, QStringLiteral("INT")},
        {3, QStringLiteral("QUIT")},
        {6, QStringLiteral("ABRT")},
        {9, QStringLiteral("KILL")},
        {11, QStringLiteral("SEGV")},
        {13, QStringLiteral("PIPE")},
        {15, QStringLiteral("TERM")},
    };
    return names.value(signal, QString::number(signal));
}

int timeoutArgument(const json &arguments, int fallback)
{
    for (const char *key : {"timeout", "timeout_ms"}) {
        if (arguments.contains(key) && arguments[key].is_number_integer()) {
            const auto value = arguments[key].get<long long>();
            return value > 0 && value <= std::numeric_limits<int>::max() ? static_cast<int>(value)
                                                                         : fallback;
        }
    }
    return fallback;
}

int maxLinesArgument(const json &arguments, int fallback)
{
    if (arguments.contains("max_lines") && arguments["max_lines"].is_number_integer()) {
        const auto value = arguments["max_lines"].get<long long>();
        if (value > 0 && value <= std::numeric_limits<int>::max()) {
            return static_cast<int>(value);
        }
    }
    return fallback;
}

json bashSchema()
{
    return {
        {"type", "object"},
        {"properties",
         {{"command", {{"type", "string"}, {"description", "The bash command to execute"}}},
          {"timeout",
           {{"type", "integer"},
            {"description",
             "Timeout in milliseconds (default: 60000). "
             "On timeout, the command keeps running and can be managed via bash_manage tool."}}},
          {"working_directory",
           {{"type", "string"},
            {"description", "Working directory for the command (default: the working directory)"}}},
          {"background",
           {{"type", "boolean"},
            {"description",
             "Run in background (default: false). When true, returns an id immediately "
             "without waiting; manage via bash_manage tool."}}},
          {"max_output",
           {{"type", "integer"},
            {"description",
             "Cap on stdout+stderr bytes of a background or timed-out command "
             "(default: 5242880 = 5 MB). When its output grows past this, the command is "
             "killed and bash_manage reports why. Long output keeps its head and tail."}}}}},
        {"required", json::array({"command"})}};
}

json bashManageSchema(const char *idKey, const char *idType)
{
    return {
        {"type", "object"},
        {"properties",
         {{idKey,
           {{"type", idType}, {"description", "Id from a bash timeout or background response"}}},
          {"action",
           {{"type", "string"},
            {"enum", json::array({"status", "wait", "output", "kill", "terminate"})},
            {"description",
             "Action: status (check state), wait (wait more time), "
             "output (read the last max_lines lines), kill (force kill), "
             "terminate (graceful stop, then force kill after 5s)"}}},
          {"timeout",
           {{"type", "integer"},
            {"maximum", std::numeric_limits<int>::max()},
            {"description", "Additional wait time in ms for 'wait'; non-positive values use 60000"}}},
          {"max_lines",
           {{"type", "integer"},
            {"description", "Lines of output to return for 'output' (default 200)"}}}}},
        {"required", json::array({idKey, "action"})}};
}

} // namespace QSocShellCommand
