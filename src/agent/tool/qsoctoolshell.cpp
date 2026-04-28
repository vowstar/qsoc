// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolshell.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

namespace {

constexpr qint64 kDefaultMaxOutputBytes = qint64{5} * 1024 * 1024; /* 5 MB */
constexpr qint64 kStuckThresholdMs      = qint64{45} * 1000;
constexpr int    kWatchdogTickMs        = 2000;
/* How many bytes from the tail to test against the interactive-prompt
 * regex. 4 KB is enough to catch a multi-line confirmation block while
 * staying tiny relative to the overall capture file. */
constexpr qint64 kStuckTailBytes = 4096;

} /* namespace */

/* Static members */
QMap<int, QSocBashProcessInfo> QSocToolShellBash::activeProcesses;
int                            QSocToolShellBash::nextProcessId  = 1;
QProcess                      *QSocToolShellBash::currentProcess = nullptr;
QEventLoop                    *QSocToolShellBash::currentLoop    = nullptr;

QSocToolShellBash::QSocToolShellBash(QObject *parent, QSocProjectManager *projectManager)
    : QSocTool(parent)
    , projectManager(projectManager)
{
    /* One shared timer drives every active process: cheaper than a
     * QTimer per process, and the work each tick is just a stat() plus
     * a small tail read. */
    watchdogTimer_ = new QTimer(this);
    watchdogTimer_->setInterval(kWatchdogTickMs);
    connect(watchdogTimer_, &QTimer::timeout, this, &QSocToolShellBash::tickWatchdog);
    watchdogTimer_->start();
}

QSocToolShellBash::~QSocToolShellBash()
{
    killAllActive();
}

void QSocToolShellBash::killAllActive()
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
           "If command times out, process keeps running and can be managed via bash_manage tool. "
           "Each call starts a fresh process in the project directory; cwd does not persist "
           "across calls. Use absolute paths or chain with && (e.g. 'cd build && make').";
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
            {"description", "Working directory for the command (default: project directory)"}}},
          {"background",
           {{"type", "boolean"},
            {"description",
             "Run in background (default: false). When true, returns process_id "
             "immediately without waiting; manage via bash_manage tool."}}},
          {"max_output",
           {{"type", "integer"},
            {"description",
             "Cap on captured stdout+stderr bytes (default: 5242880 = 5 MB). "
             "When the watchdog sees the file grow past this, the process is "
             "killed and bash_manage reports the kill reason."}}}}},
        {"required", json::array({"command"})}};
}

QString QSocToolShellBash::detectInteractivePrompt(const QString &tail)
{
    /* Heuristic: a stuck command's tail often ends with a question, a
     * (y/n) cluster, or a "Press <key>" / password prompt. We keep only
     * the trailing ~120 chars so a "(y/n)" that appeared mid-output ages
     * out as fresh log lines stream in below. We also iterate globally
     * and pick the LAST match: a prompt at the end of the slice is more
     * reliable signal than a "are you sure" earlier in the same line. */
    static const QRegularExpression reAll(QStringLiteral(
        "(?i)("
        "\\([yY]/[nN]\\)|\\([nN]/[yY]\\)|[yY]/[nN]\\?|"
        "press\\s+(?:any\\s+key|enter|return)|"
        "continue\\?|proceed\\?|are you sure|"
        "passw(?:or)?d:|passphrase:"
        ")"));
    const QString                   slice = tail.right(120);
    auto                            it    = reAll.globalMatch(slice);
    QString                         last;
    while (it.hasNext()) {
        last = it.next().captured(1);
    }
    return last;
}

void QSocToolShellBash::tickWatchdog()
{
    if (activeProcesses.isEmpty())
        return;
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    QList<int>   stuckHits; /* emit signals after the loop to keep
                               activeProcesses iteration re-entrancy-safe. */

    for (auto it = activeProcesses.begin(); it != activeProcesses.end(); ++it) {
        auto &info = it.value();
        if (info.process == nullptr || info.process->state() == QProcess::NotRunning)
            continue;

        const qint64 cap  = info.maxOutputBytes > 0 ? info.maxOutputBytes : kDefaultMaxOutputBytes;
        const qint64 size = QFileInfo(info.outputPath).size();

        if (size != info.lastKnownSize) {
            info.lastKnownSize    = size;
            info.lastSizeChangeAt = now;
        } else if (info.lastSizeChangeAt == 0) {
            /* First tick after start: anchor the no-progress timer to
             * the process start so a command that emits nothing for the
             * full window still gets flagged. */
            info.lastSizeChangeAt = info.startTime > 0 ? info.startTime : now;
        }

        if (size > cap && info.killReason.isEmpty()) {
            info.killReason = QStringLiteral("output exceeded %1 bytes").arg(cap);
            info.process->kill();
            continue;
        }

        if (!info.stuckNotified && now - info.lastSizeChangeAt >= kStuckThresholdMs) {
            const QString tail   = readLastLines(info.outputPath, 20);
            const QString prompt = detectInteractivePrompt(tail);
            if (!prompt.isEmpty()) {
                info.stuckReason = QStringLiteral("no output for %1s; tail looks interactive (%2)")
                                       .arg(kStuckThresholdMs / 1000)
                                       .arg(prompt);
                info.stuckNotified = true;
                stuckHits.append(it.key());
            }
        }
    }

    for (int id : stuckHits) {
        const auto it = activeProcesses.find(id);
        if (it == activeProcesses.end())
            continue;
        emit processStuckDetected(id, it->stuckReason, readLastLines(it->outputPath, 5));
    }
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

    /* Explicit background mode: return process_id immediately, never block. */
    bool background = false;
    if (arguments.contains("background") && arguments["background"].is_boolean()) {
        background = arguments["background"].get<bool>();
    }

    /* Per-call output cap. 0 means "use built-in default". */
    qint64 maxOutputBytes = 0;
    if (arguments.contains("max_output") && arguments["max_output"].is_number_integer()) {
        const auto raw = arguments["max_output"].get<long long>();
        if (raw > 0)
            maxOutputBytes = raw;
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

    /* Touch the file so bash_manage can tail it before any output lands. */
    {
        QFile sentinel(outputPath);
        if (!sentinel.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QDir(QFileInfo(outputPath).absolutePath()).removeRecursively();
            return "Error: Failed to create output file";
        }
        sentinel.close();
    }

    /* Create process on heap (survives timeout). Redirect merged stdout/stderr
     * straight to the capture file so we don't need a readyRead lambda
     * keeping a stack QFile alive past execute(). */
    auto *process = new QProcess();
    process->setWorkingDirectory(workingDir);
    process->setProcessChannelMode(QProcess::MergedChannels);
    process->setStandardOutputFile(outputPath, QIODevice::Append);

    process->start("/bin/bash", QStringList() << "-c" << command);

    if (!process->waitForStarted(5000)) {
        QString error
            = QString("Error: Failed to start bash process: %1").arg(process->errorString());
        delete process;
        QDir(QFileInfo(outputPath).absolutePath()).removeRecursively();
        return error;
    }

    /* Background mode: register and return immediately, no event loop. */
    if (background) {
        const int           processId = nextProcessId++;
        QSocBashProcessInfo info;
        info.process        = process;
        info.outputPath     = outputPath;
        info.command        = command;
        info.startTime      = QDateTime::currentMSecsSinceEpoch();
        info.maxOutputBytes = maxOutputBytes;
        activeProcesses.insert(processId, info);
        QObject::connect(
            process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this, processId, command](int exitCode, QProcess::ExitStatus) {
                emit backgroundProcessFinished(processId, exitCode, command);
            });
        return QString(
                   "Started in background.\n"
                   "Process ID: %1\n"
                   "Output file: %2\n\n"
                   "Use bash_manage tool with process_id=%1 to: "
                   "check status, wait, read output, kill, or terminate.")
            .arg(processId)
            .arg(outputPath);
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

    /* Set static pointers so abort() can reach us */
    currentProcess = process;
    currentLoop    = &loop;
    loop.exec();
    currentProcess = nullptr;
    currentLoop    = nullptr;

    if (finished) {
        /* Process completed within timeout. Output is already on disk
         * because setStandardOutputFile redirected stdout+stderr there
         * before start; just read the capture file. */
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
    int processId = nextProcessId++;

    QSocBashProcessInfo info;
    info.process        = process;
    info.outputPath     = outputPath;
    info.command        = command;
    info.startTime      = QDateTime::currentMSecsSinceEpoch();
    info.maxOutputBytes = maxOutputBytes;
    activeProcesses.insert(processId, info);
    QObject::connect(
        process,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this,
        [this, processId, command](int exitCode, QProcess::ExitStatus) {
            emit backgroundProcessFinished(processId, exitCode, command);
        });

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

void QSocToolShellBash::abort()
{
    if (currentProcess && currentProcess->state() != QProcess::NotRunning) {
        currentProcess->kill();
    }
    if (currentLoop && currentLoop->isRunning()) {
        currentLoop->quit();
    }
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

    /* No buffered remainder to flush: the process redirects stdout+stderr
     * straight to info.outputPath via setStandardOutputFile, so the file
     * already contains everything that was emitted up to exit. */

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

        if (!info.killReason.isEmpty()) {
            result += QStringLiteral("\n\nKilled by watchdog: %1").arg(info.killReason);
        }
        if (!info.stuckReason.isEmpty()) {
            result += QStringLiteral("\n\nLikely stuck: %1").arg(info.stuckReason);
        }

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

        /* Process stdout is already redirected to info.outputPath at the
         * OS level by setStandardOutputFile, so the file grows on its own
         * while we wait. We just spin an event loop until either the
         * process finishes or the wait timeout fires. */
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

        QObject::disconnect(connFinish);

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
