// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolmonitor.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <utility>

namespace {

constexpr qint64    kMaxOutputBytes        = qint64{5} * 1024 * 1024;
constexpr qsizetype kMaxLineChars          = 4096;
constexpr int       kMaxEventsPerSecond    = 20;
constexpr int       kOverflowFlushMs       = 250;
constexpr int       kPartialFlushMs        = 500;
constexpr int       kDefaultStartTimeoutMs = 30000;

QString statusToSummary(const QString &status, int exitCode)
{
    if (status == QStringLiteral("completed"))
        return QStringLiteral("completed");
    if (status == QStringLiteral("stopped"))
        return QStringLiteral("stopped");
    return QStringLiteral("failed with exit code %1").arg(exitCode);
}

} /* namespace */

QSocMonitorTaskSource::QSocMonitorTaskSource(
    QObject *parent, QSocTaskEventQueue *eventQueue, QSocProjectManager *projectManager)
    : QSocTaskSource(parent)
    , eventQueue_(eventQueue)
    , projectManager_(projectManager)
{}

QSocMonitorTaskSource::~QSocMonitorTaskSource()
{
    stopAll();
    qDeleteAll(runs_);
    runs_.clear();
}

QList<QSocTask::Row> QSocMonitorTaskSource::listTasks() const
{
    QList<QSocTask::Row> out;
    for (auto it = runs_.constBegin(); it != runs_.constEnd(); ++it) {
        const Run    *run = it.value();
        QSocTask::Row row;
        row.id          = run->id;
        row.label       = run->description.isEmpty() ? run->command : run->description;
        row.summary     = run->remote ? QStringLiteral("remote monitor")
                                      : QStringLiteral("local monitor");
        row.kind        = QSocTask::Kind::Monitor;
        row.status      = run->status == QStringLiteral("running")  ? QSocTask::Status::Running
                          : run->status == QStringLiteral("failed") ? QSocTask::Status::Failed
                                                                    : QSocTask::Status::Completed;
        row.startedAtMs = run->startedAtMs;
        row.canKill     = (run->status == QStringLiteral("running"));
        out.append(row);
    }
    return out;
}

QString QSocMonitorTaskSource::tailFor(const QString &id, int maxBytes) const
{
    const auto it = runs_.constFind(id);
    if (it == runs_.constEnd())
        return {};
    const QString tail = readTail((*it)->outputPath, maxBytes);
    return tail.isEmpty() ? QStringLiteral("(no output yet)\n") : tail;
}

bool QSocMonitorTaskSource::killTask(const QString &id)
{
    return stop(id);
}

QSocMonitorTaskSource::StartResult QSocMonitorTaskSource::startLocal(
    const QString &command,
    const QString &description,
    int            timeoutMs,
    bool           persistent,
    const QString &name)
{
    QString workingDir;
    if (projectManager_ != nullptr) {
        workingDir = projectManager_->getProjectPath();
    }
    if (workingDir.isEmpty()) {
        workingDir = QDir::currentPath();
    }
    return startProcess(
        command,
        description,
        timeoutMs,
        persistent,
        name,
        QStringLiteral("/bin/bash"),
        QStringList() << QStringLiteral("-lc") << command,
        workingDir,
        false);
}

QSocMonitorTaskSource::StartResult QSocMonitorTaskSource::startRemote(
    const QString    &command,
    const QString    &description,
    int               timeoutMs,
    bool              persistent,
    const RemoteSpec &remote,
    const QString    &name)
{
    QStringList args = sshArgsForTarget(remote.targetKey);
    if (args.isEmpty()) {
        return {false, {}, {}, QStringLiteral("invalid remote target")};
    }
    const QString remoteScript = QStringLiteral("cd %1 && /bin/bash -lc %2")
                                     .arg(shellQuote(remote.workspace), shellQuote(command));
    args << remoteScript;
    return startProcess(
        command,
        description,
        timeoutMs,
        persistent,
        name,
        QStringLiteral("ssh"),
        args,
        QString(),
        true);
}

QSocMonitorTaskSource::StartResult QSocMonitorTaskSource::startProcess(
    const QString     &command,
    const QString     &description,
    int                timeoutMs,
    bool               persistent,
    const QString     &name,
    const QString     &program,
    const QStringList &args,
    const QString     &workingDir,
    bool               remote)
{
    auto *tempDir = new QTemporaryDir(QDir::tempPath() + QStringLiteral("/qsoc-monitor-XXXXXX"));
    if (!tempDir->isValid()) {
        delete tempDir;
        return {false, {}, {}, QStringLiteral("failed to create temporary directory")};
    }
    tempDir->setAutoRemove(false);
    const QString outputPath = tempDir->path() + QStringLiteral("/output.log");
    delete tempDir;

    QFile sentinel(outputPath);
    if (!sentinel.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QDir(QFileInfo(outputPath).absolutePath()).removeRecursively();
        return {false, {}, {}, QStringLiteral("failed to create output file")};
    }
    sentinel.close();

    auto *process = new QProcess(this);
    process->setProcessChannelMode(QProcess::SeparateChannels);
    if (!workingDir.isEmpty()) {
        process->setWorkingDirectory(workingDir);
    }

    auto *run          = new Run;
    run->id            = QStringLiteral("m") + QString::number(nextId_++);
    run->name          = name;
    run->command       = command;
    run->description   = description.isEmpty() ? command.left(80) : description;
    run->outputPath    = outputPath;
    run->startedAtMs   = QDateTime::currentMSecsSinceEpoch();
    run->persistent    = persistent;
    run->remote        = remote;
    run->process       = process;
    run->windowStartMs = run->startedAtMs;
    runs_.insert(run->id, run);

    run->flushTimer = new QTimer(this);
    run->flushTimer->setSingleShot(true);
    run->flushTimer->setInterval(kPartialFlushMs);
    connect(run->flushTimer, &QTimer::timeout, this, [this, taskId = run->id]() {
        if (auto it = runs_.find(taskId); it != runs_.end()) {
            flushPartial(it.value());
        }
    });

    run->overflowTimer = new QTimer(this);
    run->overflowTimer->setSingleShot(true);
    run->overflowTimer->setInterval(kOverflowFlushMs);
    connect(run->overflowTimer, &QTimer::timeout, this, [this, taskId = run->id]() {
        if (auto it = runs_.find(taskId); it != runs_.end()) {
            flushOverflow(it.value());
        }
    });

    connect(process, &QProcess::readyReadStandardOutput, this, [this, taskId = run->id]() {
        if (auto it = runs_.find(taskId); it != runs_.end()) {
            drainProcess(it.value(), false);
        }
    });
    connect(process, &QProcess::readyReadStandardError, this, [this, taskId = run->id]() {
        if (auto it = runs_.find(taskId); it != runs_.end()) {
            drainProcess(it.value(), true);
        }
    });
    connect(
        process,
        QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished),
        this,
        [this, taskId = run->id](int exitCode, QProcess::ExitStatus exitStatus) {
            finishRun(taskId, exitCode, exitStatus);
        });
    connect(
        process,
        &QProcess::errorOccurred,
        this,
        [this, taskId = run->id](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                finishRun(taskId, -1, QProcess::CrashExit);
            }
        });

    process->start(program, args);
    if (!process->waitForStarted(timeoutMs > 0 ? timeoutMs : kDefaultStartTimeoutMs)) {
        const QString err = process->errorString();
        runs_.remove(run->id);
        cleanupProcess(run);
        delete run;
        QDir(QFileInfo(outputPath).absolutePath()).removeRecursively();
        return {false, {}, {}, err};
    }

    if (!persistent && timeoutMs > 0) {
        QTimer::singleShot(timeoutMs, this, [this, taskId = run->id]() {
            if (auto it = runs_.find(taskId); it != runs_.end()) {
                stop(taskId, QStringLiteral("timeout"));
            }
        });
    }

    emitEvent(run, QStringLiteral("task_started"), QStringLiteral("running"), run->description);
    emit tasksChanged();
    return {true, run->id, outputPath, {}};
}

bool QSocMonitorTaskSource::stop(const QString &taskId, const QString &reason)
{
    auto it = runs_.find(taskId);
    if (it == runs_.end())
        return false;
    Run *run = it.value();
    if (run->status != QStringLiteral("running"))
        return false;
    run->stopping = true;
    run->status   = QStringLiteral("stopped");
    if (run->process != nullptr && run->process->state() != QProcess::NotRunning) {
        run->process->terminate();
        if (!run->process->waitForFinished(1000)) {
            run->process->kill();
            run->process->waitForFinished(1000);
        }
    }
    flushPartial(run);
    flushOverflow(run);
    emitEvent(run, QStringLiteral("task_notification"), QStringLiteral("stopped"), reason);
    emit tasksChanged();
    return true;
}

void QSocMonitorTaskSource::stopAll()
{
    const auto ids = runs_.keys();
    for (const QString &id : ids) {
        stop(id, QStringLiteral("session ending"));
    }
}

void QSocMonitorTaskSource::drainProcess(Run *run, bool stderrStream)
{
    if (run == nullptr || run->process == nullptr)
        return;
    QByteArray data = stderrStream ? run->process->readAllStandardError()
                                   : run->process->readAllStandardOutput();
    acceptBytes(
        run,
        stderrStream ? run->stderrBuffer : run->stdoutBuffer,
        data,
        stderrStream ? QStringLiteral("stderr") : QStringLiteral("stdout"));
}

void QSocMonitorTaskSource::acceptBytes(
    Run *run, QByteArray &buffer, const QByteArray &data, const QString &stream)
{
    if (data.isEmpty())
        return;
    buffer.append(data);
    while (true) {
        const qsizetype idx = buffer.indexOf('\n');
        if (idx < 0)
            break;
        QByteArray line = buffer.left(idx);
        if (line.endsWith('\r')) {
            line.chop(1);
        }
        buffer.remove(0, idx + 1);
        acceptLine(run, stream, QString::fromUtf8(line));
    }
    if (!buffer.isEmpty() && run->flushTimer != nullptr) {
        run->flushTimer->start();
    }
}

void QSocMonitorTaskSource::acceptLine(Run *run, const QString &stream, const QString &line)
{
    QString capped = line;
    if (capped.size() > kMaxLineChars) {
        capped = capped.left(kMaxLineChars) + QStringLiteral("... [line truncated]");
    }
    const QString payload = stream == QStringLiteral("stderr")
                                ? QStringLiteral("[stderr] ") + capped
                                : capped;
    appendOutput(run, payload);
    if (run->bytesWritten > kMaxOutputBytes) {
        stop(run->id, QStringLiteral("output exceeded 5242880 bytes"));
        return;
    }

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (now - run->windowStartMs >= 1000) {
        run->windowStartMs = now;
        run->windowCount   = 0;
    }
    if (run->windowCount < kMaxEventsPerSecond) {
        run->windowCount++;
        emitEvent(run, QStringLiteral("monitor_line"), QStringLiteral("running"), payload);
        return;
    }

    run->overflowCount++;
    run->overflowLast = payload;
    if (run->overflowTimer != nullptr && !run->overflowTimer->isActive()) {
        run->overflowTimer->start();
    }
}

void QSocMonitorTaskSource::flushPartial(Run *run)
{
    if (run == nullptr)
        return;
    if (!run->stdoutBuffer.isEmpty()) {
        QByteArray line = run->stdoutBuffer;
        run->stdoutBuffer.clear();
        acceptLine(run, QStringLiteral("stdout"), QString::fromUtf8(line));
    }
    if (!run->stderrBuffer.isEmpty()) {
        QByteArray line = run->stderrBuffer;
        run->stderrBuffer.clear();
        acceptLine(run, QStringLiteral("stderr"), QString::fromUtf8(line));
    }
}

void QSocMonitorTaskSource::flushOverflow(Run *run)
{
    if (run == nullptr || run->overflowCount <= 0)
        return;
    const QString summary = QStringLiteral("%1 monitor lines coalesced; last: %2")
                                .arg(run->overflowCount)
                                .arg(run->overflowLast);
    run->overflowCount    = 0;
    run->overflowLast.clear();
    emitEvent(run, QStringLiteral("monitor_line"), QStringLiteral("running"), summary);
}

void QSocMonitorTaskSource::finishRun(
    const QString &taskId, int exitCode, QProcess::ExitStatus exitStatus)
{
    auto it = runs_.find(taskId);
    if (it == runs_.end())
        return;
    Run *run = it.value();
    drainProcess(run, false);
    drainProcess(run, true);
    flushPartial(run);
    flushOverflow(run);
    if (run->status == QStringLiteral("running")) {
        run->status = (exitStatus == QProcess::NormalExit && exitCode == 0)
                          ? QStringLiteral("completed")
                          : QStringLiteral("failed");
        emitEvent(
            run,
            QStringLiteral("task_notification"),
            run->status,
            statusToSummary(run->status, exitCode));
    }
    cleanupProcess(run);
    emit tasksChanged();
}

void QSocMonitorTaskSource::appendOutput(Run *run, const QString &line)
{
    QFile file(run->outputPath);
    if (file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        const QByteArray bytes = (line + QLatin1Char('\n')).toUtf8();
        file.write(bytes);
        run->bytesWritten += bytes.size();
    }
}

void QSocMonitorTaskSource::emitEvent(
    Run *run, const QString &kind, const QString &status, const QString &content)
{
    if (eventQueue_ == nullptr || run == nullptr)
        return;
    QSocTaskEvent event;
    event.taskId      = run->id;
    event.sourceTag   = sourceTag();
    event.kind        = kind;
    event.status      = status;
    event.description = run->description;
    event.content     = content;
    event.outputFile  = run->outputPath;
    event.createdAtMs = QDateTime::currentMSecsSinceEpoch();
    eventQueue_->enqueue(event);
}

void QSocMonitorTaskSource::cleanupProcess(Run *run)
{
    if (run == nullptr)
        return;
    if (run->flushTimer != nullptr) {
        run->flushTimer->stop();
        run->flushTimer->deleteLater();
        run->flushTimer = nullptr;
    }
    if (run->overflowTimer != nullptr) {
        run->overflowTimer->stop();
        run->overflowTimer->deleteLater();
        run->overflowTimer = nullptr;
    }
    if (run->process != nullptr) {
        run->process->deleteLater();
        run->process = nullptr;
    }
}

QString QSocMonitorTaskSource::readTail(const QString &path, int maxBytes)
{
    QFile file(path);
    if (!file.exists() || !file.open(QIODevice::ReadOnly))
        return {};
    const qint64 size = file.size();
    if (maxBytes > 0 && size > maxBytes) {
        file.seek(size - maxBytes);
    }
    return QString::fromUtf8(file.readAll());
}

QString QSocMonitorTaskSource::shellQuote(const QString &value)
{
    return QStringLiteral("'") + QString(value).replace(QLatin1Char('\''), QStringLiteral("'\\''"))
           + QStringLiteral("'");
}

QStringList QSocMonitorTaskSource::sshArgsForTarget(const QString &targetKey)
{
    const int atIdx    = targetKey.indexOf(QLatin1Char('@'));
    const int colonIdx = targetKey.lastIndexOf(QLatin1Char(':'));
    if (atIdx <= 0 || colonIdx <= atIdx + 1 || colonIdx == targetKey.size() - 1)
        return {};
    const QString user = targetKey.left(atIdx);
    const QString host = targetKey.mid(atIdx + 1, colonIdx - atIdx - 1);
    const QString port = targetKey.mid(colonIdx + 1);
    return QStringList() << QStringLiteral("-p") << port << QStringLiteral("-l") << user << host;
}

QSocToolMonitor::QSocToolMonitor(
    QObject                                         *parent,
    QSocMonitorTaskSource                           *source,
    std::optional<QSocMonitorTaskSource::RemoteSpec> remote)
    : QSocTool(parent)
    , source_(source)
    , remote_(std::move(remote))
{}

QString QSocToolMonitor::getDescription() const
{
    return QStringLiteral(
        "Run a background watcher command and deliver each output line as a task notification "
        "event to the agent. Use this proactively when the user asks to watch, monitor, tail, "
        "poll, wait for a status change, or react to events while continuing the conversation. "
        "Write a small self-contained shell command or script yourself; do not require the user "
        "to configure monitors. Prefer this over background bash when output should wake the "
        "agent as events arrive.");
}

json QSocToolMonitor::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"command",
           {{"type", "string"},
            {"description",
             "Long-running shell watcher command. Print one concise event per line; avoid noisy "
             "heartbeats and unbounded output."}}},
          {"description",
           {{"type", "string"}, {"description", "Short description shown in task UI/events"}}},
          {"timeout_ms",
           {{"type", "integer"},
            {"description", "For non-persistent monitors, stop after this many milliseconds"}}},
          {"persistent",
           {{"type", "boolean"},
            {"description", "Keep running for the session until monitor_stop or exit"}}}}},
        {"required", json::array({"command", "description"})}};
}

QString QSocToolMonitor::execute(const json &arguments)
{
    if (source_ == nullptr) {
        return QStringLiteral(R"({"status":"error","error":"monitor source not configured"})");
    }
    if (!arguments.contains("command") || !arguments["command"].is_string()) {
        return QStringLiteral(R"({"status":"error","error":"command is required"})");
    }
    if (!arguments.contains("description") || !arguments["description"].is_string()) {
        return QStringLiteral(R"({"status":"error","error":"description is required"})");
    }
    const QString command     = QString::fromStdString(arguments["command"].get<std::string>());
    const QString description = QString::fromStdString(arguments["description"].get<std::string>());
    int           timeoutMs   = 0;
    if (arguments.contains("timeout_ms") && arguments["timeout_ms"].is_number_integer()) {
        timeoutMs = arguments["timeout_ms"].get<int>();
    }
    const bool persistent = arguments.contains("persistent") && arguments["persistent"].is_boolean()
                            && arguments["persistent"].get<bool>();

    QSocMonitorTaskSource::StartResult result;
    if (remote_.has_value()) {
        result = source_->startRemote(command, description, timeoutMs, persistent, remote_.value());
    } else {
        result = source_->startLocal(command, description, timeoutMs, persistent);
    }
    if (!result.ok) {
        return QString::fromUtf8(
            json{{"status", "error"}, {"error", result.error.toStdString()}}.dump().c_str());
    }
    return QString::fromUtf8(
        json{
            {"status", "started"},
            {"task_id", result.taskId.toStdString()},
            {"output_file", result.outputFile.toStdString()},
            {"persistent", persistent}}
            .dump()
            .c_str());
}

QSocToolMonitorStop::QSocToolMonitorStop(QObject *parent, QSocMonitorTaskSource *source)
    : QSocTool(parent)
    , source_(source)
{}

QString QSocToolMonitorStop::getDescription() const
{
    return QStringLiteral("Stop a running monitor task by task_id.");
}

json QSocToolMonitorStop::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"task_id", {{"type", "string"}, {"description", "Task id returned by monitor"}}}}},
        {"required", json::array({"task_id"})}};
}

QString QSocToolMonitorStop::execute(const json &arguments)
{
    if (source_ == nullptr) {
        return QStringLiteral(R"({"status":"error","error":"monitor source not configured"})");
    }
    if (!arguments.contains("task_id") || !arguments["task_id"].is_string()) {
        return QStringLiteral(R"({"status":"error","error":"task_id is required"})");
    }
    const QString taskId = QString::fromStdString(arguments["task_id"].get<std::string>());
    const bool    ok     = source_->stop(taskId);
    return QString::fromUtf8(
        json{{"status", ok ? "stopped" : "unknown"}, {"task_id", taskId.toStdString()}}
            .dump()
            .c_str());
}
