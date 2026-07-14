// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolshell.h"

#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QTextStream>
#include <QTimer>

#include <limits>

namespace {

constexpr qint64 kDefaultMaxOutputBytes = qint64{5} * 1024 * 1024; /* 5 MB */
constexpr qint64 kStuckThresholdMs      = qint64{45} * 1000;
constexpr int    kWatchdogTickMs        = 2000;
constexpr int    kTerminateGraceMs      = 5000;
constexpr int    kActiveTerminateMs     = 2000;
constexpr int    kForceStopMs           = 1000;
constexpr auto   kBlockingWaitBusy
    = "Error: Another bash_manage wait or terminate is already active.";
/* How many bytes from the tail to test against the interactive-prompt
 * regex. 4 KB is enough to catch a multi-line confirmation block while
 * staying tiny relative to the overall capture file. */
constexpr qint64 kStuckTailBytes = 4096;

void destroyProcessInfo(QSocBashProcessInfo &info)
{
    QProcess *process = info.process;
    info.process      = nullptr;
    delete process;

    if (!info.outputPath.isEmpty()) {
        QDir(QFileInfo(info.outputPath).absolutePath()).removeRecursively();
        info.outputPath.clear();
    }
}

void requestProcessCleanup(QMap<int, QSocBashProcessInfo> &processes, const int processId)
{
    auto it = processes.find(processId);
    if (it == processes.end()) {
        return;
    }
    if (it->process != nullptr && it->process->state() != QProcess::NotRunning) {
        return;
    }

    it->removeWhenIdle = true;
    if (it->activeWaiters != 0) {
        return;
    }

    QSocBashProcessInfo info = processes.take(processId);
    destroyProcessInfo(info);
}

void releaseProcessWaiter(
    QMap<int, QSocBashProcessInfo> &processes, const int processId, const bool remove)
{
    auto it = processes.find(processId);
    if (it == processes.end()) {
        return;
    }

    Q_ASSERT(it->activeWaiters > 0);
    if (it->activeWaiters == 0) {
        return;
    }

    --it->activeWaiters;
    if (remove || (it->activeWaiters == 0 && it->removeWhenIdle)) {
        requestProcessCleanup(processes, processId);
    }
}

bool hasActiveProcessWait(const QMap<int, QSocBashProcessInfo> &processes)
{
    for (const auto &info : processes) {
        if (info.activeWaiters != 0) {
            return true;
        }
    }
    return false;
}

bool waitForProcessStopped(const QPointer<QProcess> &process, const int timeoutMs)
{
    if (process.isNull() || process->state() == QProcess::NotRunning) {
        return true;
    }

    return process->waitForFinished(timeoutMs) || process.isNull()
           || process->state() == QProcess::NotRunning;
}

bool forceStopProcess(const QPointer<QProcess> &process)
{
    if (process.isNull() || process->state() == QProcess::NotRunning) {
        return true;
    }

    process->kill();
    return waitForProcessStopped(process, kForceStopMs);
}

} /* namespace */

/* Static members */
QMap<int, QSocBashProcessInfo> QSocToolShellBash::activeProcesses;
int                            QSocToolShellBash::nextProcessId = 1;
QSet<QProcess *>               QSocToolShellBash::inFlightProcs_;
QSet<QEventLoop *>             QSocToolShellBash::inFlightLoops_;

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
    const QList<int> processIds = activeProcesses.keys();
    for (const int processId : processIds) {
        auto it = activeProcesses.find(processId);
        if (it == activeProcesses.end()) {
            continue;
        }

        QPointer<QProcess> process = it->process;
        if (!forceStopProcess(process)) {
            continue;
        }

        it = activeProcesses.find(processId);
        if (it == activeProcesses.end() || (!process.isNull() && it->process != process.data())) {
            continue;
        }
        requestProcessCleanup(activeProcesses, processId);
    }
}

QList<QSocToolShellBash::BackgroundSnapshot> QSocToolShellBash::snapshotActive()
{
    QList<BackgroundSnapshot> out;
    for (auto it = activeProcesses.constBegin(); it != activeProcesses.constEnd(); ++it) {
        const auto        &info = it.value();
        BackgroundSnapshot snap;
        snap.id          = it.key();
        snap.command     = info.command;
        snap.startedAtMs = info.startTime;
        snap.outputPath  = info.outputPath;
        snap.isStuck     = !info.stuckReason.isEmpty();
        snap.isRunning = (info.process != nullptr && info.process->state() != QProcess::NotRunning);
        out.append(snap);
    }
    std::sort(out.begin(), out.end(), [](const BackgroundSnapshot &a, const BackgroundSnapshot &b) {
        return a.id < b.id;
    });
    return out;
}

int QSocToolShellBash::activeProcessCount()
{
    return static_cast<int>(activeProcesses.size());
}

bool QSocToolShellBash::killActive(int processId)
{
    auto it = activeProcesses.find(processId);
    if (it == activeProcesses.end())
        return false;
    QPointer<QProcess> process = it->process;
    bool               stopped = true;
    if (!process.isNull() && process->state() != QProcess::NotRunning) {
        process->terminate();
        stopped = waitForProcessStopped(process, kActiveTerminateMs);
        if (!stopped) {
            stopped = forceStopProcess(process);
        }
    }

    it = activeProcesses.find(processId);
    if (it == activeProcesses.end()) {
        return true;
    }
    if (process.isNull() || it->process != process.data()) {
        return false;
    }
    if (!stopped) {
        return false;
    }

    requestProcessCleanup(activeProcesses, processId);
    return true;
}

QString QSocToolShellBash::tailActive(int processId, int maxBytes)
{
    auto it = activeProcesses.constFind(processId);
    if (it == activeProcesses.constEnd())
        return QString();
    const QString &path = it.value().outputPath;
    QFile          file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return QString();
    const qint64 size = file.size();
    if (size > maxBytes)
        file.seek(size - maxBytes);
    const QByteArray bytes = file.readAll();
    file.close();
    return QString::fromUtf8(bytes);
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
        const QString reason = it->stuckReason;
        const QString tail   = readLastLines(it->outputPath, 5);
        emit          processStuckDetected(id, reason, tail);
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
            },
            Qt::QueuedConnection);
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

    /* Register this wait so abort() can reach it, even when several
     * sub-agents run a foreground bash concurrently. Removed as soon as
     * exec() returns; no event dispatch happens between, so the set
     * stays balanced and never holds a returned (destroyed) loop. */
    inFlightProcs_.insert(process);
    inFlightLoops_.insert(&loop);
    loop.exec();
    inFlightLoops_.remove(&loop);
    inFlightProcs_.remove(process);

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
        },
        Qt::QueuedConnection);

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
    /* Cancel every in-flight foreground wait, not just the most recent
     * one: concurrent sub-agents may each be blocked in their own bash. */
    for (QProcess *proc : std::as_const(inFlightProcs_)) {
        if (proc != nullptr && proc->state() != QProcess::NotRunning) {
            proc->kill();
        }
    }
    for (QEventLoop *loop : std::as_const(inFlightLoops_)) {
        if (loop != nullptr && loop->isRunning()) {
            loop->quit();
        }
    }
}

void QSocToolShellBash::setProjectManager(QSocProjectManager *projectManager)
{
    this->projectManager = projectManager;
}

/* QSocToolBashManage */

QSocToolBashManage::QSocToolBashManage(QObject *parent)
    : QSocTool(parent)
{}

QSocToolBashManage::~QSocToolBashManage()
{
    abort();
}

void QSocToolBashManage::abort()
{
    for (WaitContext *wait : std::as_const(activeWaits_)) {
        wait->aborted = true;
        wait->loop->quit();
    }
}

QString QSocToolBashManage::getName() const
{
    return "bash_manage";
}

QString QSocToolBashManage::getDescription() const
{
    return "Manage a timed-out bash process: check status, wait more, read output, kill, "
           "or terminate. Terminate requests graceful exit, then force-kills after 5 seconds. "
           "Use process_id from bash tool timeout response. A terminal status, wait, kill, "
           "or terminate returns final output and releases the process record after the active "
           "wait finishes. Aborting a wait leaves a running process tracked. Only one wait or "
           "terminate action can block at a time; another is rejected immediately. A process "
           "that does not stop remains tracked.";
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
             "output (read last 200 lines), kill (force kill), "
             "terminate (graceful stop, then force kill after 5s)"}}},
          {"timeout",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", std::numeric_limits<int>::max()},
            {"description",
             "Positive additional wait time in ms for 'wait' action (default: 60000)"}}}}},
        {"required", json::array({"process_id", "action"})}};
}

QString QSocToolBashManage::collectOutput(const QSocBashProcessInfo &info, int exitCode)
{
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

    if (action == "status") {
        auto    it        = QSocToolShellBash::activeProcesses.find(processId);
        auto   &info      = it.value();
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
            QString output   = collectOutput(info, exitCode);
            result
                += QString("\n\nProcess has finished (exit code %1):\n%2").arg(exitCode).arg(output);
            requestProcessCleanup(QSocToolShellBash::activeProcesses, processId);
        }

        return result;
    }

    if (action == "wait") {
        auto  it          = QSocToolShellBash::activeProcesses.find(processId);
        auto &info        = it.value();
        int   waitTimeout = 60000;
        if (arguments.contains("timeout")) {
            const auto &timeout = arguments["timeout"];
            if (!timeout.is_number_integer()) {
                return "Error: timeout must be a positive integer";
            }
            if (timeout.is_number_unsigned()
                && timeout.get<quint64>() > static_cast<quint64>(std::numeric_limits<int>::max())) {
                return "Error: timeout must be a positive integer";
            }
            const auto value = timeout.get<qint64>();
            if (value <= 0 || value > std::numeric_limits<int>::max()) {
                return "Error: timeout must be a positive integer";
            }
            waitTimeout = static_cast<int>(value);
        }

        if (info.process->state() == QProcess::NotRunning) {
            int     exitCode = info.process->exitCode();
            QString output   = collectOutput(info, exitCode);
            requestProcessCleanup(QSocToolShellBash::activeProcesses, processId);
            return QString("Process already finished (exit code %1):\n%2").arg(exitCode).arg(output);
        }
        if (hasActiveProcessWait(QSocToolShellBash::activeProcesses)) {
            return kBlockingWaitBusy;
        }

        /* Process stdout is already redirected to info.outputPath at the
         * OS level by setStandardOutputFile, so the file grows on its own
         * while we wait. */
        QEventLoop                   loop;
        bool                         finished = false;
        QPointer<QProcess>           process  = info.process;
        QPointer<QSocToolBashManage> guard(this);
        WaitContext                  wait{&loop};
        ++info.activeWaiters;

        auto connFinish = QObject::connect(
            process.data(),
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            &loop,
            [&finished, &loop](int, QProcess::ExitStatus) {
                finished = true;
                loop.quit();
            });

        QTimer::singleShot(waitTimeout, &loop, [&loop]() { loop.quit(); });

        activeWaits_.insert(&wait);
        loop.exec();
        if (!guard.isNull()) {
            guard->activeWaits_.remove(&wait);
        }

        QObject::disconnect(connFinish);

        it = QSocToolShellBash::activeProcesses.find(processId);
        if (it == QSocToolShellBash::activeProcesses.end() || process.isNull()
            || it->process != process.data()) {
            return QString("Error: Process %1 was removed while waiting.").arg(processId);
        }

        if (finished || process->state() == QProcess::NotRunning) {
            const int     exitCode = process->exitCode();
            const QString output   = collectOutput(it.value(), exitCode);
            const QString result
                = QString("Process completed (exit code %1):\n%2").arg(exitCode).arg(output);
            releaseProcessWaiter(QSocToolShellBash::activeProcesses, processId, true);
            return result;
        }

        if (wait.aborted) {
            releaseProcessWaiter(QSocToolShellBash::activeProcesses, processId, false);
            return "Wait aborted; process is still running.";
        }

        const QString lastOutput = QSocToolShellBash::readLastLines(it->outputPath, 50);
        const QString result     = QString(
                                       "Process still running after additional %1ms wait.\n"
                                       "Last output:\n%2")
                                       .arg(waitTimeout)
                                       .arg(lastOutput);
        releaseProcessWaiter(QSocToolShellBash::activeProcesses, processId, false);
        return result;
    }

    if (action == "output") {
        const auto &info       = QSocToolShellBash::activeProcesses[processId];
        QString     lastOutput = QSocToolShellBash::readLastLines(info.outputPath, 200);
        bool        running    = (info.process->state() != QProcess::NotRunning);
        return QString("Process %1 (%2):\n%3")
            .arg(processId)
            .arg(running ? "RUNNING" : "FINISHED")
            .arg(lastOutput);
    }

    if (action == "kill") {
        auto &info = QSocToolShellBash::activeProcesses[processId];
        if (info.process->state() == QProcess::NotRunning) {
            const int     exitCode = info.process->exitCode();
            const QString output   = collectOutput(info, exitCode);
            requestProcessCleanup(QSocToolShellBash::activeProcesses, processId);
            return QString("Process already finished (exit code %1):\n%2").arg(exitCode).arg(output);
        }

        QPointer<QProcess> process = info.process;
        const bool         stopped = forceStopProcess(process);
        auto               it      = QSocToolShellBash::activeProcesses.find(processId);
        if (it == QSocToolShellBash::activeProcesses.end() || process.isNull()
            || it->process != process.data()) {
            return QString("Error: Process %1 was removed while killing.").arg(processId);
        }
        if (!stopped) {
            const QString lastOutput = QSocToolShellBash::readLastLines(it->outputPath, 50);
            return QString("Kill requested, but process is still running.\nLast output:\n%1")
                .arg(lastOutput);
        }

        const int     exitCode = process->exitCode();
        const QString output   = collectOutput(it.value(), exitCode);
        const QString result
            = QString("Process killed (exit code %1):\n%2").arg(exitCode).arg(output);
        requestProcessCleanup(QSocToolShellBash::activeProcesses, processId);
        return result;
    }

    if (action == "terminate") {
        auto &info = QSocToolShellBash::activeProcesses[processId];
        if (info.process->state() == QProcess::NotRunning) {
            const int     exitCode = info.process->exitCode();
            const QString output   = collectOutput(info, exitCode);
            requestProcessCleanup(QSocToolShellBash::activeProcesses, processId);
            return QString("Process already finished (exit code %1):\n%2").arg(exitCode).arg(output);
        }
        if (hasActiveProcessWait(QSocToolShellBash::activeProcesses)) {
            return kBlockingWaitBusy;
        }

        QPointer<QProcess>           process = info.process;
        QPointer<QSocToolBashManage> guard(this);
        ++info.activeWaiters;
        process->terminate();

        /* Wait up to 5s for graceful exit */
        QEventLoop  loop;
        bool        finished = false;
        WaitContext wait{&loop};

        auto connFinish = QObject::connect(
            process.data(),
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            &loop,
            [&finished, &loop](int, QProcess::ExitStatus) {
                finished = true;
                loop.quit();
            });

        QTimer::singleShot(kTerminateGraceMs, &loop, [&loop]() { loop.quit(); });

        activeWaits_.insert(&wait);
        loop.exec();
        if (!guard.isNull()) {
            guard->activeWaits_.remove(&wait);
        }

        QObject::disconnect(connFinish);

        auto it = QSocToolShellBash::activeProcesses.find(processId);
        if (it == QSocToolShellBash::activeProcesses.end() || process.isNull()
            || it->process != process.data()) {
            return QString("Error: Process %1 was removed while terminating.").arg(processId);
        }

        if (finished || process->state() == QProcess::NotRunning) {
            const int     exitCode = process->exitCode();
            const QString output   = collectOutput(it.value(), exitCode);
            const QString result
                = QString("Process terminated (exit code %1):\n%2").arg(exitCode).arg(output);
            releaseProcessWaiter(QSocToolShellBash::activeProcesses, processId, true);
            return result;
        }

        if (wait.aborted) {
            releaseProcessWaiter(QSocToolShellBash::activeProcesses, processId, false);
            return "Terminate requested; wait aborted; process is still running.";
        }

        const bool stopped = forceStopProcess(process);
        if (!stopped) {
            const QString lastOutput = QSocToolShellBash::readLastLines(it->outputPath, 50);
            releaseProcessWaiter(QSocToolShellBash::activeProcesses, processId, false);
            return QString(
                       "Terminate timed out and force kill was requested, but the process "
                       "is still running.\nLast output:\n%1")
                .arg(lastOutput);
        }

        const int     exitCode = process->exitCode();
        const QString output   = collectOutput(it.value(), exitCode);
        const QString result   = QString(
                                     "Process force-killed after terminate timeout "
                                     "(exit code %1):\n%2")
                                     .arg(exitCode)
                                     .arg(output);
        releaseProcessWaiter(QSocToolShellBash::activeProcesses, processId, true);
        return result;
    }

    return QString("Error: Unknown action '%1'. Use: status, wait, output, kill, terminate")
        .arg(action);
}
