// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolshell.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTextStream>
#include <QThread>
#include <QTimer>

#include <limits>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace {

constexpr qint64 kDefaultMaxOutputBytes = qint64{5} * 1024 * 1024; /* 5 MB */
constexpr qint64 kStuckThresholdMs      = qint64{45} * 1000;
constexpr int    kWatchdogTickMs        = 2000;
constexpr int    kTerminateGraceMs      = 5000;
constexpr int    kActiveTerminateMs     = 2000;
constexpr int    kForceStopMs           = 1000;
constexpr auto   kBlockingWaitBusy
    = "Error: Another bash_manage wait or terminate is already active.";
constexpr auto kForegroundWaitBusy
    = "Error: Command was not started because another foreground bash command is active in this "
      "session. Use background=true for concurrent commands.";
thread_local bool foregroundWaitActive = false;
/* How many bytes from the tail to test against the interactive-prompt
 * regex. 4 KB is enough to catch a multi-line confirmation block while
 * staying tiny relative to the overall capture file. */
constexpr qint64 kStuckTailBytes = 4096;

bool processTreeRunning(
    const QPointer<QProcess> &process, qint64 processGroupId, bool trackProcessGroup);
bool trackedProcessRunning(QSocBashProcessInfo &info);

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
    if (trackedProcessRunning(it.value())) {
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

class QSocBashProcess final : public QProcess
{
public:
    QSocBashProcess()
    {
#ifdef Q_OS_UNIX
        connect(this, &QProcess::started, this, [this]() { processGroupId_ = processId(); });
#endif
    }

    qint64 processGroupId() const { return processGroupId_; }

#if defined(Q_OS_UNIX) && QT_VERSION < 0x060000
protected:
    void setupChildProcess() override
    {
        if (::setpgid(0, 0) != 0) {
            ::_exit(127);
        }
    }
#endif

private:
    qint64 processGroupId_ = 0;
};

bool processGroupRunning(const qint64 processGroupId)
{
#ifdef Q_OS_UNIX
    if (processGroupId <= 0) {
        return false;
    }
    errno = 0;
    return ::kill(-static_cast<pid_t>(processGroupId), 0) == 0 || errno == EPERM;
#else
    Q_UNUSED(processGroupId);
    return false;
#endif
}

bool processTreeRunning(
    const QPointer<QProcess> &process, const qint64 processGroupId, const bool trackProcessGroup)
{
    return (!process.isNull() && process->state() != QProcess::NotRunning)
           || (trackProcessGroup && processGroupRunning(processGroupId));
}

bool trackedProcessRunning(QSocBashProcessInfo &info)
{
    using GroupStopState = QSocBashProcessInfo::GroupStopState;

    if (info.groupStopState == GroupStopState::Stopped) {
        return false;
    }

    const QPointer<QProcess> process = info.process;
    const bool leaderRunning = !process.isNull() && process->state() != QProcess::NotRunning;
    if (info.groupStopState == GroupStopState::NotRequested) {
        return leaderRunning;
    }
    if (leaderRunning || processGroupRunning(info.processGroupId)) {
        return true;
    }

    info.groupStopState = GroupStopState::Stopped;
    info.processGroupId = 0;
    return false;
}

void prepareProcessTree(QProcess *process)
{
#if defined(Q_OS_UNIX) && QT_VERSION >= 0x060000
    process->setChildProcessModifier([]() {
        if (::setpgid(0, 0) != 0) {
            ::_exit(127);
        }
    });
#else
    Q_UNUSED(process);
#endif
}

void terminateProcessTree(const QPointer<QProcess> &process, const qint64 processGroupId)
{
#ifdef Q_OS_UNIX
    if (processGroupId > 0 && ::kill(-static_cast<pid_t>(processGroupId), SIGTERM) == 0) {
        return;
    }
#endif
    if (!process.isNull() && process->state() != QProcess::NotRunning) {
        process->terminate();
    }
}

void killProcessTree(const QPointer<QProcess> &process, const qint64 processGroupId)
{
#ifdef Q_OS_UNIX
    if (processGroupId > 0 && ::kill(-static_cast<pid_t>(processGroupId), SIGKILL) == 0) {
        return;
    }
#endif
    if (!process.isNull() && process->state() != QProcess::NotRunning) {
        process->kill();
    }
}

bool waitForProcessTreeStopped(
    const QPointer<QProcess> &process,
    const qint64              processGroupId,
    const bool                trackProcessGroup,
    const int                 timeoutMs)
{
    QElapsedTimer elapsed;
    elapsed.start();

    while (processTreeRunning(process, processGroupId, trackProcessGroup)) {
        const int remaining = timeoutMs - static_cast<int>(elapsed.elapsed());
        if (remaining <= 0) {
            return false;
        }
        const int slice = qMin(remaining, 20);
        if (!process.isNull() && process->state() != QProcess::NotRunning) {
            process->waitForFinished(slice);
        } else {
            QThread::msleep(static_cast<unsigned long>(slice));
        }
    }
    return true;
}

bool forceStopProcess(
    const QPointer<QProcess> &process, const qint64 processGroupId, const bool trackProcessGroup)
{
    if (!processTreeRunning(process, processGroupId, trackProcessGroup)) {
        return true;
    }

    killProcessTree(process, processGroupId);
    return waitForProcessTreeStopped(process, processGroupId, trackProcessGroup, kForceStopMs);
}

} /* namespace */

/* Static members */
QMap<int, QSocBashProcessInfo> QSocToolShellBash::activeProcesses;
int                            QSocToolShellBash::nextProcessId = 1;

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
    abort();
    killTracked(this);
}

void QSocToolShellBash::killAllActive()
{
    killTracked(nullptr);
}

void QSocToolShellBash::markTrackedStopped(
    const int processId, const QPointer<QProcess> &expectedProcess)
{
    auto it = activeProcesses.find(processId);
    if (it == activeProcesses.end() || expectedProcess.isNull()
        || it->process != expectedProcess.data()
        || it->groupStopState == QSocBashProcessInfo::GroupStopState::NotRequested) {
        return;
    }

    it->groupStopState  = QSocBashProcessInfo::GroupStopState::Stopped;
    it->processGroupId  = 0;
    it->groupPollActive = false;
}

void QSocToolShellBash::requestTrackedStop(const int processId)
{
    auto it = activeProcesses.find(processId);
    if (it == activeProcesses.end()
        || it->groupStopState == QSocBashProcessInfo::GroupStopState::Stopped) {
        return;
    }

    it->groupStopState = QSocBashProcessInfo::GroupStopState::Requested;
    if (it->groupPollActive) {
        return;
    }

    it->groupPollActive = true;
    pollTrackedStop(processId, it->process);
}

void QSocToolShellBash::pollTrackedStop(
    const int processId, const QPointer<QProcess> &expectedProcess)
{
    auto it = activeProcesses.find(processId);
    if (it == activeProcesses.end() || expectedProcess.isNull()
        || it->process != expectedProcess.data()) {
        return;
    }
    if (!trackedProcessRunning(it.value())) {
        it->groupPollActive = false;
        return;
    }

    QTimer::singleShot(20, expectedProcess.data(), [processId, expectedProcess]() {
        pollTrackedStop(processId, expectedProcess);
    });
}

void QSocToolShellBash::killTracked(const QSocToolShellBash *owner)
{
    const QList<int> processIds = activeProcesses.keys();
    for (const int processId : processIds) {
        auto it = activeProcesses.find(processId);
        if (it == activeProcesses.end()) {
            continue;
        }
        if (owner != nullptr && it->owner.data() != owner) {
            continue;
        }

        QPointer<QProcess> process = it->process;
        if (trackedProcessRunning(it.value())) {
            requestTrackedStop(processId);
        }
        const bool groupStopRequested = it->groupStopState
                                        == QSocBashProcessInfo::GroupStopState::Requested;
        if (!forceStopProcess(process, it->processGroupId, groupStopRequested)) {
            continue;
        }
        if (groupStopRequested) {
            markTrackedStopped(processId, process);
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
    for (auto it = activeProcesses.begin(); it != activeProcesses.end(); ++it) {
        auto              &info = it.value();
        BackgroundSnapshot snap;
        snap.id          = it.key();
        snap.command     = info.command;
        snap.startedAtMs = info.startTime;
        snap.outputPath  = info.outputPath;
        snap.isStuck     = !info.stuckReason.isEmpty();
        snap.isRunning   = trackedProcessRunning(info);
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
    QPointer<QProcess> process        = it->process;
    const qint64       processGroupId = it->processGroupId;
    bool               stopped        = true;
    if (trackedProcessRunning(it.value())) {
        requestTrackedStop(processId);
        terminateProcessTree(process, processGroupId);
        stopped = waitForProcessTreeStopped(process, processGroupId, true, kActiveTerminateMs);
        if (!stopped) {
            stopped = forceStopProcess(process, processGroupId, true);
        }
    }

    if (stopped) {
        markTrackedStopped(processId, process);
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
           "Only one foreground command can wait per session; use background=true for concurrency. "
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
        if (info.owner.data() != this) {
            continue;
        }
        if (!trackedProcessRunning(info))
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
            requestTrackedStop(it.key());
            killProcessTree(info.process, info.processGroupId);
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

    const bool ownsForegroundWait = !background && !foregroundWaitActive;
    if (!background && !ownsForegroundWait) {
        return QString::fromLatin1(kForegroundWaitBusy);
    }
    if (ownsForegroundWait) {
        foregroundWaitActive = true;
    }
    const auto releaseForegroundWait = qScopeGuard([ownsForegroundWait]() {
        if (ownsForegroundWait) {
            foregroundWaitActive = false;
        }
    });

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
    auto *process = new QSocBashProcess();
    prepareProcessTree(process);
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
    const qint64 processGroup = process->processGroupId();

    /* Background mode: register and return immediately, no event loop. */
    if (background) {
        const int           processId = nextProcessId++;
        QSocBashProcessInfo info;
        info.process        = process;
        info.owner          = this;
        info.outputPath     = outputPath;
        info.command        = command;
        info.processGroupId = processGroup;
        info.startTime      = QDateTime::currentMSecsSinceEpoch();
        info.maxOutputBytes = maxOutputBytes;
        activeProcesses.insert(processId, info);
        QObject::connect(
            process,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            this,
            [this, processId, command](int exitCode, QProcess::ExitStatus) {
                notifyBackgroundFinished(processId, exitCode, command);
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

    QPointer<QSocToolShellBash> owner(this);
    QPointer<QProcess>          processGuard(process);
    QEventLoop                  loop;
    bool                        finished = false;
    ForegroundWaitContext       wait{processGuard, &loop, processGroup, false};

    QObject::connect(
        process,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        &loop,
        [&finished, &loop](int, QProcess::ExitStatus) {
            finished = true;
            loop.quit();
        });

    QTimer::singleShot(timeout, &loop, [&loop]() { loop.quit(); });

    foregroundWaits_.insert(&wait);
    loop.exec();
    const bool aborted = wait.aborted;
    if (!owner.isNull()) {
        owner->foregroundWaits_.remove(&wait);
    }

    if (aborted && forceStopProcess(processGuard, processGroup, wait.stopRequested)) {
        delete processGuard.data();
        QDir(QFileInfo(outputPath).absolutePath()).removeRecursively();
        return QStringLiteral("Command aborted.");
    }

    if (!aborted
        && (finished || (!processGuard.isNull() && processGuard->state() == QProcess::NotRunning))) {
        /* Process completed within timeout. Output is already on disk
         * because setStandardOutputFile redirected stdout+stderr there
         * before start; just read the capture file. */
        QFile   readFile(outputPath);
        QString output;
        if (readFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            output = QString::fromUtf8(readFile.readAll());
            readFile.close();
        }

        const int exitCode = processGuard->exitCode();
        delete processGuard.data();
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
    info.process        = processGuard.data();
    info.owner          = owner;
    info.outputPath     = outputPath;
    info.command        = command;
    info.processGroupId = processGroup;
    if (wait.stopRequested) {
        info.groupStopState = QSocBashProcessInfo::GroupStopState::Requested;
    }
    info.startTime      = QDateTime::currentMSecsSinceEpoch();
    info.maxOutputBytes = maxOutputBytes;
    activeProcesses.insert(processId, info);
    if (wait.stopRequested) {
        requestTrackedStop(processId);
    }
    if (!owner.isNull()) {
        QObject::connect(
            processGuard.data(),
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            owner.data(),
            [owner, processId, command](int exitCode, QProcess::ExitStatus) {
                if (!owner.isNull()) {
                    owner->notifyBackgroundFinished(processId, exitCode, command);
                }
            },
            Qt::QueuedConnection);
    }

    /* Read last lines for immediate feedback */
    QString lastOutput = readLastLines(outputPath, 50);

    if (aborted) {
        return QString(
                   "Abort requested but the command is STILL RUNNING.\n"
                   "Process ID: %1\n"
                   "Output file: %2\n"
                   "Last output:\n%3\n\n"
                   "Use bash_manage tool with process_id=%1 to stop it.")
            .arg(processId)
            .arg(outputPath)
            .arg(lastOutput);
    }

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
    for (ForegroundWaitContext *wait : std::as_const(foregroundWaits_)) {
        wait->aborted = true;
        if (!wait->process.isNull() && wait->process->state() != QProcess::NotRunning) {
            wait->stopRequested = true;
            killProcessTree(wait->process, wait->processGroupId);
        }
        if (wait->loop != nullptr && wait->loop->isRunning()) {
            wait->loop->quit();
        }
    }
}

void QSocToolShellBash::setProjectManager(QSocProjectManager *projectManager)
{
    this->projectManager = projectManager;
}

void QSocToolShellBash::notifyBackgroundFinished(
    const int processId, const int exitCode, const QString &command)
{
    auto it = activeProcesses.find(processId);
    if (it != activeProcesses.end() && trackedProcessRunning(it.value())) {
        QTimer::singleShot(20, this, [this, processId, exitCode, command]() {
            notifyBackgroundFinished(processId, exitCode, command);
        });
        return;
    }
    emit backgroundProcessFinished(processId, exitCode, command);
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
        bool    running   = trackedProcessRunning(info);
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

        if (!trackedProcessRunning(info)) {
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
        QPointer<QProcess>           process = info.process;
        QPointer<QSocToolBashManage> guard(this);
        WaitContext                  wait{&loop};
        ++info.activeWaiters;

        const auto processStopped = [processId, process]() {
            auto it = QSocToolShellBash::activeProcesses.find(processId);
            return it == QSocToolShellBash::activeProcesses.end() || process.isNull()
                   || it->process != process.data() || !trackedProcessRunning(it.value());
        };

        auto connFinish = QObject::connect(
            process.data(),
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            &loop,
            [&loop, &processStopped](int, QProcess::ExitStatus) {
                if (processStopped()) {
                    loop.quit();
                }
            });

        QTimer treePoll;
        treePoll.setInterval(20);
        QObject::connect(&treePoll, &QTimer::timeout, &loop, [&loop, &processStopped]() {
            if (processStopped()) {
                loop.quit();
            }
        });
        treePoll.start();
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

        if (!trackedProcessRunning(it.value())) {
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
        auto   &info       = QSocToolShellBash::activeProcesses[processId];
        QString lastOutput = QSocToolShellBash::readLastLines(info.outputPath, 200);
        bool    running    = trackedProcessRunning(info);
        return QString("Process %1 (%2):\n%3")
            .arg(processId)
            .arg(running ? "RUNNING" : "FINISHED")
            .arg(lastOutput);
    }

    if (action == "kill") {
        auto &info = QSocToolShellBash::activeProcesses[processId];
        if (!trackedProcessRunning(info)) {
            const int     exitCode = info.process->exitCode();
            const QString output   = collectOutput(info, exitCode);
            requestProcessCleanup(QSocToolShellBash::activeProcesses, processId);
            return QString("Process already finished (exit code %1):\n%2").arg(exitCode).arg(output);
        }

        QPointer<QProcess> process        = info.process;
        const qint64       processGroupId = info.processGroupId;
        QSocToolShellBash::requestTrackedStop(processId);
        const bool stopped = forceStopProcess(process, processGroupId, true);
        if (stopped) {
            QSocToolShellBash::markTrackedStopped(processId, process);
        }
        auto it = QSocToolShellBash::activeProcesses.find(processId);
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
        if (!trackedProcessRunning(info)) {
            const int     exitCode = info.process->exitCode();
            const QString output   = collectOutput(info, exitCode);
            requestProcessCleanup(QSocToolShellBash::activeProcesses, processId);
            return QString("Process already finished (exit code %1):\n%2").arg(exitCode).arg(output);
        }
        if (hasActiveProcessWait(QSocToolShellBash::activeProcesses)) {
            return kBlockingWaitBusy;
        }

        QPointer<QProcess>           process        = info.process;
        const qint64                 processGroupId = info.processGroupId;
        QPointer<QSocToolBashManage> guard(this);
        ++info.activeWaiters;
        QSocToolShellBash::requestTrackedStop(processId);
        terminateProcessTree(process, processGroupId);

        /* Wait up to 5s for graceful exit */
        QEventLoop  loop;
        WaitContext wait{&loop};

        const auto processStopped = [processId, process]() {
            auto it = QSocToolShellBash::activeProcesses.find(processId);
            return it == QSocToolShellBash::activeProcesses.end() || process.isNull()
                   || it->process != process.data() || !trackedProcessRunning(it.value());
        };

        auto connFinish = QObject::connect(
            process.data(),
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
            &loop,
            [&loop, &processStopped](int, QProcess::ExitStatus) {
                if (processStopped()) {
                    loop.quit();
                }
            });

        QTimer treePoll;
        treePoll.setInterval(20);
        QObject::connect(&treePoll, &QTimer::timeout, &loop, [&loop, &processStopped]() {
            if (processStopped()) {
                loop.quit();
            }
        });
        treePoll.start();
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

        if (!trackedProcessRunning(it.value())) {
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

        const bool stopped = forceStopProcess(process, processGroupId, true);
        if (!stopped) {
            const QString lastOutput = QSocToolShellBash::readLastLines(it->outputPath, 50);
            releaseProcessWaiter(QSocToolShellBash::activeProcesses, processId, false);
            return QString(
                       "Terminate timed out and force kill was requested, but the process "
                       "is still running.\nLast output:\n%1")
                .arg(lastOutput);
        }
        QSocToolShellBash::markTrackedStopped(processId, process);

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
