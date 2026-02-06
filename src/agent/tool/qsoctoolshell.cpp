// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolshell.h"

#include <QDir>
#include <QProcess>

QSocToolShellBash::QSocToolShellBash(QObject *parent, QSocProjectManager *projectManager)
    : QSocTool(parent)
    , projectManager(projectManager)
{}

QSocToolShellBash::~QSocToolShellBash() = default;

QString QSocToolShellBash::getName() const
{
    return "bash";
}

QString QSocToolShellBash::getDescription() const
{
    return "Execute a bash command in the project directory. "
           "Returns stdout and stderr. Use for git, build tools, and system commands.";
}

json QSocToolShellBash::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"command", {{"type", "string"}, {"description", "The bash command to execute"}}},
          {"timeout",
           {{"type", "integer"},
            {"description", "Timeout in milliseconds (default: 60000, max: 300000)"}}},
          {"working_directory",
           {{"type", "string"},
            {"description", "Working directory for the command (default: project directory)"}}}}},
        {"required", json::array({"command"})}};
}

QString QSocToolShellBash::execute(const json &arguments)
{
    if (!arguments.contains("command") || !arguments["command"].is_string()) {
        return "Error: command is required";
    }

    QString command = QString::fromStdString(arguments["command"].get<std::string>());

    /* Get timeout */
    int timeout = 60000;
    if (arguments.contains("timeout") && arguments["timeout"].is_number_integer()) {
        timeout = arguments["timeout"].get<int>();
        if (timeout <= 0) {
            timeout = 60000;
        }
        if (timeout > 300000) {
            timeout = 300000;
        }
    }

    /* Get working directory */
    QString workingDir;
    if (arguments.contains("working_directory") && arguments["working_directory"].is_string()) {
        workingDir = QString::fromStdString(arguments["working_directory"].get<std::string>());
    }

    if (workingDir.isEmpty() && projectManager) {
        workingDir = projectManager->getProjectPath();
    }

    if (workingDir.isEmpty()) {
        workingDir = QDir::currentPath();
    }

    /* Execute command */
    QProcess process;
    process.setWorkingDirectory(workingDir);
    process.setProcessChannelMode(QProcess::MergedChannels);

    process.start("/bin/bash", QStringList() << "-c" << command);

    if (!process.waitForStarted(5000)) {
        return QString("Error: Failed to start bash process: %1").arg(process.errorString());
    }

    if (!process.waitForFinished(timeout)) {
        process.kill();
        return QString("Error: Command timed out after %1 ms").arg(timeout);
    }

    QString output   = QString::fromUtf8(process.readAll());
    int     exitCode = process.exitCode();

    /* Truncate output if too large */
    constexpr int maxOutputSize = 50000;
    if (output.size() > maxOutputSize) {
        output = output.left(maxOutputSize) + "\n... (output truncated)";
    }

    if (exitCode != 0) {
        return QString("Command exited with code %1:\n%2").arg(exitCode).arg(output);
    }

    return output.isEmpty() ? "(no output)" : output;
}

void QSocToolShellBash::setProjectManager(QSocProjectManager *projectManager)
{
    projectManager = projectManager;
}
