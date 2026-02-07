// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolshell.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

/* Static members */
QMap<int, QSocBashProcessInfo> QSocToolShellBash::activeProcesses;
int                            QSocToolShellBash::nextProcessId = 1;

QSocToolShellBash::QSocToolShellBash(QObject *parent, QSocProjectManager *projectManager)
    : QSocTool(parent)
    , projectManager(projectManager)
{}

QSocToolShellBash::~QSocToolShellBash()
{
    for (auto &info : activeProcesses) {
        if (info.process) {
            info.process->kill();
            info.process->waitForFinished(1000);
            delete info.process;
        }
        QDir(QFileInfo(info.outputPath).absolutePath()).removeRecursively();
    }
    activeProcesses.clear();
}

QString QSocToolShellBash::getName() const
{
    return "bash";
}

QString QSocToolShellBash::getDescription() const
{
    return "Execute a bash command in the project directory. "
           "Returns stdout and stderr. Set timeout as needed (no upper limit). "
           "If command times out, process keeps running and can be managed via bash_manage tool.";
}

json QSocToolShellBash::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"command", {{"type", "string"}, {"description", "The bash command to execute"}}},
          {"timeout",
           {{"type", "integer"},
            {"description",
             "Timeout in milliseconds (default: 60000). "
             "On timeout, process keeps running and can be managed via bash_manage tool."}}},
          {"working_directory",
           {{"type", "string"},
            {"description", "Working directory for the command (default: project directory)"}}}}},
        {"required", json::array({"command"})}};
}

QString QSocToolShellBash::readLastLines(const QString &path, int count)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {};
    }

    QStringList allLines;
    QTextStream stream(&file);
    while (!stream.atEnd()) {
        allLines.append(stream.readLine());
    }

    int start = static_cast<int>(qMax(qsizetype(0), allLines.size() - count));
    return allLines.mid(start).join('\n');
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

    /* Create temp dir for output */
    auto *tempDir = new QTemporaryDir(QDir::tempPath() + "/qsoc-bash-XXXXXX");
    if (!tempDir->isValid()) {
        delete tempDir;
        return "Error: Failed to create temporary directory";
    }
    tempDir->setAutoRemove(false);
    QString outputPath = tempDir->path() + "/output.log";
    delete tempDir;

    QFile outputFile(outputPath);
    if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
        QDir(QFileInfo(outputPath).absolutePath()).removeRecursively();
        return "Error: Failed to create output file";
    }

    /* Create process on heap (survives timeout) */
    auto *process = new QProcess();
    process->setWorkingDirectory(workingDir);
    process->setProcessChannelMode(QProcess::MergedChannels);

    /* Connect readyRead to write output incrementally */
    QObject::connect(process, &QProcess::readyRead, process, [process, &outputFile]() {
        QByteArray data = process->readAll();
        outputFile.write(data);
        outputFile.flush();
    });

    process->start("/bin/bash", QStringList() << "-c" << command);

    if (!process->waitForStarted(5000)) {
        QString error
            = QString("Error: Failed to start bash process: %1").arg(process->errorString());
        delete process;
        outputFile.close();
        QDir(QFileInfo(outputPath).absolutePath()).removeRecursively();
        return error;
    }

    /* Use local event loop + timer instead of waitForFinished */
    QEventLoop loop;
    bool       finished = false;

    QObject::connect(
        process,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        &loop,
        [&finished, &loop](int, QProcess::ExitStatus) {
            finished = true;
            loop.quit();
        });

    QTimer::singleShot(timeout, &loop, [&loop]() { loop.quit(); });

    loop.exec();

    if (finished) {
        /* Process completed within timeout */
        QByteArray remaining = process->readAll();
        if (!remaining.isEmpty()) {
            outputFile.write(remaining);
            outputFile.flush();
        }
        outputFile.close();

        /* Read output from file */
        QFile   readFile(outputPath);
        QString output;
        if (readFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            output = QString::fromUtf8(readFile.readAll());
            readFile.close();
        }

        int exitCode = process->exitCode();
        delete process;
        QDir(QFileInfo(outputPath).absolutePath()).removeRecursively();

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

    /* Timeout: process still running, store it */
    outputFile.close();

    int processId = nextProcessId++;

    QSocBashProcessInfo info;
    info.process    = process;
    info.outputPath = outputPath;
    info.command    = command;
    info.startTime  = QDateTime::currentMSecsSinceEpoch();
    activeProcesses.insert(processId, info);

    /* Read last lines for immediate feedback */
    QString lastOutput = readLastLines(outputPath, 50);

    return QString(
               "Command timed out after %1ms but is STILL RUNNING.\n"
               "Process ID: %2\n"
               "Output file: %3\n"
               "Last output:\n%4\n\n"
               "Use bash_manage tool with process_id=%2 to: "
               "check status, wait more, read output, kill, or terminate.")
        .arg(timeout)
        .arg(processId)
        .arg(outputPath)
        .arg(lastOutput);
}

void QSocToolShellBash::setProjectManager(QSocProjectManager *projectManager)
{
    this->projectManager = projectManager;
}

/* ========== QSocToolBashManage ========== */

QSocToolBashManage::QSocToolBashManage(QObject *parent)
    : QSocTool(parent)
{}

QSocToolBashManage::~QSocToolBashManage() = default;

QString QSocToolBashManage::getName() const
{
    return "bash_manage";
}

QString QSocToolBashManage::getDescription() const
{
    return "Manage a timed-out bash process: check status, wait more, read output, kill, "
           "or terminate. Use process_id from bash tool timeout response.";
}

json QSocToolBashManage::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"process_id",
           {{"type", "integer"}, {"description", "Process ID from bash timeout response"}}},
          {"action",
           {{"type", "string"},
            {"enum", json::array({"status", "wait", "output", "kill", "terminate"})},
            {"description",
             "Action: status (check state), wait (wait more time), "
             "output (read last 200 lines), kill (force kill), terminate (graceful stop)"}}},
          {"timeout",
           {{"type", "integer"},
            {"description", "Additional wait time in ms for 'wait' action (default: 60000)"}}}}},
        {"required", json::array({"process_id", "action"})}};
}

void QSocToolBashManage::cleanupProcess(int processId)
{
    if (!QSocToolShellBash::activeProcesses.contains(processId)) {
        return;
    }

    auto &info = QSocToolShellBash::activeProcesses[processId];
    delete info.process;
    QDir(QFileInfo(info.outputPath).absolutePath()).removeRecursively();
    QSocToolShellBash::activeProcesses.remove(processId);
}

QString QSocToolBashManage::collectOutput(int processId, int exitCode)
{
    if (!QSocToolShellBash::activeProcesses.contains(processId)) {
        return {};
    }

    auto &info = QSocToolShellBash::activeProcesses[processId];

    /* Flush remaining output */
    QByteArray remaining = info.process->readAll();
    if (!remaining.isEmpty()) {
        QFile outputFile(info.outputPath);
        if (outputFile.open(QIODevice::Append | QIODevice::Text)) {
            outputFile.write(remaining);
            outputFile.close();
        }
    }

    /* Read full output */
    QFile   readFile(info.outputPath);
    QString output;
    if (readFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
        output = QString::fromUtf8(readFile.readAll());
        readFile.close();
    }

    /* Truncate if too large */
    constexpr int maxOutputSize = 50000;
    if (output.size() > maxOutputSize) {
        output = output.left(maxOutputSize) + "\n... (output truncated)";
    }

    if (exitCode != 0) {
        return QString("Command exited with code %1:\n%2").arg(exitCode).arg(output);
    }

    return output.isEmpty() ? "(no output)" : output;
}

QString QSocToolBashManage::execute(const json &arguments)
{
    if (!arguments.contains("process_id") || !arguments["process_id"].is_number_integer()) {
        return "Error: process_id is required";
    }
    if (!arguments.contains("action") || !arguments["action"].is_string()) {
        return "Error: action is required";
    }

    int     processId = arguments["process_id"].get<int>();
    QString action    = QString::fromStdString(arguments["action"].get<std::string>());

    if (!QSocToolShellBash::activeProcesses.contains(processId)) {
        return QString(
                   "Error: No active process with ID %1. "
                   "It may have already been cleaned up.")
            .arg(processId);
    }

    auto &info = QSocToolShellBash::activeProcesses[processId];

    if (action == "status") {
        bool    running   = (info.process->state() != QProcess::NotRunning);
        qint64  elapsed   = QDateTime::currentMSecsSinceEpoch() - info.startTime;
        QString lastLines = QSocToolShellBash::readLastLines(info.outputPath, 10);

        QFile  outputFile(info.outputPath);
        qint64 fileSize = outputFile.size();

        QString result = QString(
                             "Process ID: %1\n"
                             "Command: %2\n"
                             "Status: %3\n"
                             "Running time: %4ms\n"
                             "Output size: %5 bytes\n"
                             "Last output:\n%6")
                             .arg(processId)
                             .arg(info.command)
                             .arg(running ? "RUNNING" : "FINISHED")
                             .arg(elapsed)
                             .arg(fileSize)
                             .arg(lastLines);

        if (!running) {
            int     exitCode = info.process->exitCode();
            QString output   = collectOutput(processId, exitCode);
            cleanupProcess(processId);
            result
                += QString("\n\nProcess has finished (exit code %1):\n%2").arg(exitCode).arg(output);
        }

        return result;
    }

    if (action == "wait") {
        if (info.process->state() == QProcess::NotRunning) {
            int     exitCode = info.process->exitCode();
            QString output   = collectOutput(processId, exitCode);
            cleanupProcess(processId);
            return QString("Process already finished (exit code %1):\n%2").arg(exitCode).arg(output);
        }

        int waitTimeout = 60000;
        if (arguments.contains("timeout") && arguments["timeout"].is_number_integer()) {
            waitTimeout = arguments["timeout"].get<int>();
            if (waitTimeout <= 0) {
                waitTimeout = 60000;
            }
        }

        /* Reconnect readyRead to keep writing output */
        QFile outputFile(info.outputPath);
        if (!outputFile.open(QIODevice::Append | QIODevice::Text)) {
            return "Error: Failed to open output file for writing";
        }

        auto connRead = QObject::connect(info.process, &QProcess::readyRead, info.process, [&]() {
            QByteArray data = info.process->readAll();
            outputFile.write(data);
            outputFile.flush();
        });

        QEventLoop loop;
        bool       finished = false;

        auto connFinish = QObject::connect(
            info.process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            &loop,
            [&finished, &loop](int, QProcess::ExitStatus) {
                finished = true;
                loop.quit();
            });

        QTimer::singleShot(waitTimeout, &loop, [&loop]() { loop.quit(); });

        loop.exec();

        QObject::disconnect(connRead);
        QObject::disconnect(connFinish);
        outputFile.close();

        if (finished) {
            int     exitCode = info.process->exitCode();
            QString output   = collectOutput(processId, exitCode);
            cleanupProcess(processId);
            return QString("Process completed (exit code %1):\n%2").arg(exitCode).arg(output);
        }

        QString lastOutput = QSocToolShellBash::readLastLines(info.outputPath, 50);
        return QString(
                   "Process still running after additional %1ms wait.\n"
                   "Last output:\n%2")
            .arg(waitTimeout)
            .arg(lastOutput);
    }

    if (action == "output") {
        QString lastOutput = QSocToolShellBash::readLastLines(info.outputPath, 200);
        bool    running    = (info.process->state() != QProcess::NotRunning);
        return QString("Process %1 (%2):\n%3")
            .arg(processId)
            .arg(running ? "RUNNING" : "FINISHED")
            .arg(lastOutput);
    }

    if (action == "kill") {
        info.process->kill();
        info.process->waitForFinished(1000);
        int     exitCode = info.process->exitCode();
        QString output   = collectOutput(processId, exitCode);
        cleanupProcess(processId);
        return QString("Process killed (exit code %1):\n%2").arg(exitCode).arg(output);
    }

    if (action == "terminate") {
        info.process->terminate();

        /* Wait up to 5s for graceful exit */
        QEventLoop loop;
        bool       finished = false;

        auto connFinish = QObject::connect(
            info.process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            &loop,
            [&finished, &loop](int, QProcess::ExitStatus) {
                finished = true;
                loop.quit();
            });

        QTimer::singleShot(5000, &loop, [&loop]() { loop.quit(); });

        loop.exec();

        QObject::disconnect(connFinish);

        if (!finished) {
            info.process->kill();
            info.process->waitForFinished(1000);
        }

        int     exitCode = info.process->exitCode();
        QString output   = collectOutput(processId, exitCode);
        cleanupProcess(processId);
        return QString("Process terminated (exit code %1):\n%2").arg(exitCode).arg(output);
    }

    return QString("Error: Unknown action '%1'. Use: status, wait, output, kill, terminate")
        .arg(action);
}
