// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/daemon/qsocdaemonresources.h"

#include "common/qsocipc.h"
#include "common/qsocresourceusage.h"

#include <cstdio>
#include <utility>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#endif

namespace {

constexpr int outputLimit = 256 * 1024;
constexpr int inputLimit  = 64 * 1024;

QJsonObject unavailable(const QString &status, const QString &reason)
{
    return {
        {"scope", "local_daemon"},
        {"status", status},
        {"reason", reason},
        {"system", QJsonObject{}},
        {"processes", QJsonArray{}},
        {"storage", QJsonArray{}}};
}

} // namespace

QSocDaemonResources::QSocDaemonResources(QObject *parent, QString program, QStringList arguments)
    : QObject(parent)
    , program_(program.isEmpty() ? QCoreApplication::applicationFilePath() : std::move(program))
    , arguments_(std::move(arguments))
{
    if (arguments_.isEmpty())
        arguments_
            = {"--resource-probe",
               "--parent-pid",
               QString::number(QCoreApplication::applicationPid())};
    deadline_.setSingleShot(true);
    connect(&deadline_, &QTimer::timeout, this, [this] {
        stop("timeout", "Resource sampling exceeded its deadline");
    });
}

QSocDaemonResources::~QSocDaemonResources()
{
    shutdown();
}

bool QSocDaemonResources::validatePaths(const QJsonObject &params, QJsonArray &paths)
{
    paths = {};
    if (params.isEmpty())
        return true;
    if (params.size() != 1 || !params.value("paths").isArray())
        return false;
    paths = params.value("paths").toArray();
    if (paths.size() > 8)
        return false;
    for (const auto &entry : paths) {
        const auto path = entry.toString();
        if (!entry.isString() || path.isEmpty() || path.size() > 4096 || path.contains(QChar::Null)
            || !QDir::isAbsolutePath(path))
            return false;
    }
    return !QSocIpc::frame(params, inputLimit).isEmpty();
}

void QSocDaemonResources::request(QObject *requester, const QJsonArray &paths, Callback callback)
{
    if (stopped_) {
        callback(unavailable("unknown", "Resource sampling has stopped"));
        return;
    }
    if (process_) {
        callback(unavailable("busy", "A resource probe is still running"));
        return;
    }
    requester_ = requester;
    callback_  = std::move(callback);
    output_.clear();
    requesterConnection_ = connect(requester, &QObject::destroyed, this, [this] {
        complete({});
        if (process_)
            process_->kill();
    });
    auto *process        = new QProcess;
    process_             = process;
    process->setProcessChannelMode(QProcess::ForwardedErrorChannel);
    connect(process, &QProcess::started, this, [this, paths] {
        if (!callback_) {
            process_->kill();
            return;
        }
        process_->write(QSocIpc::frame(QJsonObject{{"paths", paths}}, inputLimit));
        process_->closeWriteChannel();
    });
    connect(process, &QProcess::readyReadStandardOutput, this, &QSocDaemonResources::readOutput);
    connect(process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error != QProcess::FailedToStart)
            return;
        process_->deleteLater();
        process_ = nullptr;
        complete(unavailable("unknown", "Could not start the resource probe"));
    });
    connect(process, &QProcess::finished, process, &QObject::deleteLater);
    connect(process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
        readOutput();
        process_ = nullptr;
        if (!callback_)
            return;
        QJsonObject result;
        if (status != QProcess::NormalExit || code != 0
            || QSocIpc::decode(output_, result, outputLimit) != QSocIpc::DecodeResult::Complete
            || !output_.isEmpty() || result.value("scope") != "local_daemon") {
            complete(unavailable("unknown", "The resource probe did not return a valid snapshot"));
            return;
        }
        complete(result);
    });
    deadline_.start(3000);
    process->start(program_, arguments_);
}

void QSocDaemonResources::readOutput()
{
    if (!process_)
        return;
    output_ += process_->read(outputLimit + QSocIpc::headerBytes + 1 - output_.size());
    if (output_.size() > outputLimit + QSocIpc::headerBytes || process_->bytesAvailable() > 0)
        stop("unknown", "The resource probe exceeded its output limit");
}

void QSocDaemonResources::complete(const QJsonObject &result)
{
    deadline_.stop();
    disconnect(requesterConnection_);
    auto callback      = std::move(callback_);
    callback_          = {};
    const bool deliver = requester_ && !result.isEmpty();
    requester_.clear();
    if (deliver && callback)
        callback(result);
}

void QSocDaemonResources::stop(const QString &status, const QString &reason)
{
    if (process_)
        process_->kill();
    complete(unavailable(status, reason));
}

void QSocDaemonResources::cancel(QObject *requester)
{
    if (requester_ == requester) {
        complete({});
        if (process_)
            process_->kill();
    }
}

void QSocDaemonResources::shutdown()
{
    stopped_ = true;
    complete({});
    if (process_) {
        disconnect(process_, nullptr, this, nullptr);
        process_->kill();
        // A blocked filesystem syscall can outlive SIGKILL. Do not wait in the supervisor.
        process_ = nullptr;
    }
}

int QSocDaemonResources::runProbe(qint64 supervisorPid)
{
#ifdef Q_OS_WIN
    if (::_setmode(::_fileno(stdin), _O_BINARY) < 0 || ::_setmode(::_fileno(stdout), _O_BINARY) < 0)
        return 2;
#endif
    QByteArray input(QSocIpc::headerBytes, Qt::Uninitialized);
    if (std::fread(input.data(), 1, input.size(), stdin) != static_cast<size_t>(input.size()))
        return 2;
    const int length = QSocIpc::payloadLength(input, inputLimit);
    if (length <= 0)
        return 2;
    input.resize(QSocIpc::headerBytes + length);
    if (std::fread(input.data() + QSocIpc::headerBytes, 1, length, stdin)
        != static_cast<size_t>(length))
        return 2;
    QJsonObject request;
    QJsonArray  paths;
    if (QSocIpc::decode(input, request, inputLimit) != QSocIpc::DecodeResult::Complete
        || !validatePaths(request, paths))
        return 2;
    const auto    sampledAt = QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs);
    QElapsedTimer elapsed;
    elapsed.start();
    const auto system  = QSocResourceUsage::system();
    bool       partial = false;
    for (const auto *key :
         {"memory_total_bytes", "memory_available_bytes", "cpu_total_ns", "cpu_busy_ns"})
        partial = partial || !system.value(QLatin1String(key)).isDouble();
    QJsonArray processes;
    for (const auto &entry : QSocResourceUsage::processTree(supervisorPid)) {
        const auto process = entry.toObject();
        if (process.value("pid").toInteger() == QCoreApplication::applicationPid())
            continue;
        partial = partial || !process.value("resident_bytes").isDouble()
                  || !process.value("cpu_time_ns").isDouble();
        processes.append(entry);
    }
    QJsonArray storage;
    partial = partial || processes.isEmpty();
    for (const auto &path : paths) {
        const auto measurement = QSocResourceUsage::storage(path.toString());
        partial                = partial || !measurement.value("valid").toBool();
        storage.append(measurement);
    }
    const QJsonObject result{
        {"scope", "local_daemon"},
        {"status", partial ? "partial" : "ok"},
        {"sampled_at_utc", sampledAt},
        {"collection_duration_ms", elapsed.elapsed()},
        {"system", system},
        {"processes", processes},
        {"storage", storage}};
    const auto bytes = QSocIpc::frame(result, outputLimit);
    if (bytes.isEmpty())
        return 3;
    return std::fwrite(bytes.constData(), 1, bytes.size(), stdout)
                       == static_cast<size_t>(bytes.size())
                   && std::fflush(stdout) == 0
               ? 0
               : 3;
}
