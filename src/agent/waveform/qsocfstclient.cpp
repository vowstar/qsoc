// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocfstclient.h"
#include "common/qsocipc.h"
#include "common/qsoclocalendpoint.h"
#include "common/qsoclocalpeer.h"
#include "common/qsocprocesslimits.h"
#include "common/qsocsibling.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>
#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QPointer>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>
#include <QThread>

#include <utility>

namespace {
constexpr int wireLimit = 256 * 1024;

QProcessEnvironment workerEnvironment(const QString &directory)
{
    QProcessEnvironment environment;
    for (const char *name :
         {"PATH",
          "SYSTEMROOT",
          "WINDIR",
          "LANG",
          "LC_ALL",
          "LC_CTYPE",
          "TZ",
          "LD_LIBRARY_PATH",
          "DYLD_LIBRARY_PATH",
          "DYLD_FALLBACK_LIBRARY_PATH"}) {
        if (qEnvironmentVariableIsSet(name))
            environment.insert(QString::fromLatin1(name), qEnvironmentVariable(name));
    }
    for (const char *name : {"HOME", "USERPROFILE", "TMPDIR", "TEMP", "TMP"})
        environment.insert(QString::fromLatin1(name), directory);
    return environment;
}
} // namespace

struct QSocFstClient::State : QObject
{
    State()
    {
        server.setParent(this);
        process->setParent(this);
        QObject::connect(
            QThread::currentThread(),
            &QThread::finished,
            this,
            [this] { ownerFinished(); },
            Qt::DirectConnection);
    }

    struct Reclaimed
    {
        std::mutex              mutex;
        std::condition_variable changed;
        bool                    done    = false;
        bool                    success = false;
    };

    struct Slot
    {
        enum class Phase { Starting, Active, Idle, Closing, Exited };
        QPointer<State>                       owner;
        Phase                                 phase = Phase::Starting;
        std::chrono::steady_clock::time_point idleSince;
        std::shared_ptr<Reclaimed>            reclamation;
        QString                               directory;
        bool                                  retired    = false;
        bool                                  ownerEnded = false;
    };

    static std::mutex &poolMutex()
    {
        static std::mutex value;
        return value;
    }

    static std::vector<std::shared_ptr<Slot>> &pool()
    {
        static std::vector<std::shared_ptr<Slot>> value;
        return value;
    }

    static void confirmExit(const std::shared_ptr<Slot> &reservation)
    {
        const std::lock_guard lock(poolMutex());
        reservation->phase = Slot::Phase::Exited;
        auto &workers      = pool();
        workers.erase(std::remove(workers.begin(), workers.end(), reservation), workers.end());
    }

    static void finishReclaim(const std::shared_ptr<Slot> &reservation, bool success)
    {
        std::shared_ptr<Reclaimed> pending;
        {
            const std::lock_guard lock(poolMutex());
            pending = std::exchange(reservation->reclamation, {});
        }
        if (pending) {
            const std::lock_guard lock(pending->mutex);
            pending->success = success;
            pending->done    = true;
            pending->changed.notify_one();
        }
    }

    bool releaseConfirmed()
    {
        if (process && process->state() != QProcess::NotRunning)
            return false;
        if (slot)
            confirmExit(slot);
        return true;
    }

    ~State() override
    {
        if (!slot)
            return;
        {
            const std::lock_guard lock(poolMutex());
            slot->owner.clear();
            if (slot->phase != Slot::Phase::Exited)
                slot->phase = Slot::Phase::Closing;
        }
        const bool stopped = stop();
        if (stopped) {
            releaseConfirmed();
        } else {
            directory.setAutoRemove(false);
            {
                const std::lock_guard lock(poolMutex());
                slot->retired   = true;
                slot->directory = directory.path();
            }
            process->setParent(nullptr);
            (void) process.release();
        }
        finishReclaim(slot, stopped);
    }

    void clearTransport()
    {
        socket.reset();
        server.close();
        readAt = {};
        companion.reset();
        const auto notify = std::exchange(evictionHandler, {});
        if (notify)
            notify();
    }

    void ownerFinished()
    {
        ownerEnded = true;
        if (cancelled)
            cancelled->store(true);
        if (slot) {
            {
                const std::lock_guard lock(poolMutex());
                slot->ownerEnded = true;
                slot->directory  = directory.path();
                if (slot->phase != Slot::Phase::Exited)
                    slot->phase = Slot::Phase::Closing;
            }
            evict();
        }
        clearTransport();
        if (auto *application = QCoreApplication::instance()) {
            if (application->thread() != thread())
                moveToThread(application->thread());
        }
    }

    void evict()
    {
        evicted            = true;
        const bool stopped = stop();
        if (stopped) {
            clearTransport();
            directory.remove();
            releaseConfirmed();
        }
        finishReclaim(slot, stopped);
    }

    bool admit(QString *error)
    {
        for (;;) {
            QPointer<State> victim;
            const auto      reclaimed  = std::make_shared<Reclaimed>();
            bool            sameThread = false;
            {
                const std::lock_guard lock(poolMutex());
                auto                 &workers = pool();
                if (workers.size() < 2) {
                    slot        = std::make_shared<Slot>();
                    slot->owner = this;
                    workers.push_back(slot);
                    admitted = true;
                    return true;
                }
                std::shared_ptr<Slot> candidate;
                bool                  closing = false;
                for (const auto &worker : workers) {
                    closing |= worker->phase == Slot::Phase::Closing;
                    if (worker->phase == Slot::Phase::Idle && worker->owner
                        && (!candidate || worker->idleSince < candidate->idleSince))
                        candidate = worker;
                }
                if (!candidate) {
                    *error = closing
                                 ? QStringLiteral("cleanup_failed: FST worker exit is unconfirmed")
                                 : QStringLiteral("busy: FST worker capacity reached");
                    return false;
                }
                candidate->phase       = Slot::Phase::Closing;
                candidate->reclamation = reclaimed;
                victim                 = candidate->owner;
                sameThread             = victim->thread() == QThread::currentThread();
                if (!sameThread)
                    QMetaObject::invokeMethod(
                        victim,
                        [victim] {
                            if (victim)
                                victim->evict();
                        },
                        Qt::QueuedConnection);
            }
            if (sameThread && victim)
                victim->evict();
            std::unique_lock lock(reclaimed->mutex);
            if (!reclaimed->changed
                     .wait_for(lock, std::chrono::milliseconds(8000), [&] { return reclaimed->done; })
                || !reclaimed->success) {
                *error = QStringLiteral("cleanup_failed: FST idle worker could not be reclaimed");
                return false;
            }
        }
    }

    void watchExit()
    {
        const auto reservation = slot;
        auto      *child       = process.get();
        QObject::connect(
            child, &QProcess::finished, child, [reservation, child](int, QProcess::ExitStatus) {
                confirmExit(reservation);
                QString directory;
                bool    retired    = false;
                bool    ownerEnded = false;
                {
                    const std::lock_guard lock(poolMutex());
                    directory  = reservation->directory;
                    retired    = reservation->retired;
                    ownerEnded = reservation->ownerEnded;
                }
                if ((retired || ownerEnded) && !directory.isEmpty())
                    QDir(directory).removeRecursively();
                if (retired) {
                    finishReclaim(reservation, true);
                    child->deleteLater();
                }
            });
        QObject::connect(
            QThread::currentThread(),
            &QThread::finished,
            child,
            [reservation, child] {
                if (child->thread() != QThread::currentThread())
                    return;
                {
                    const std::lock_guard lock(poolMutex());
                    reservation->ownerEnded = true;
                }
                if (child->state() != QProcess::NotRunning) {
                    child->kill();
                    child->waitForFinished(1900);
                }
                if (auto *application = QCoreApplication::instance()) {
                    if (application->thread() != child->thread())
                        child->moveToThread(application->thread());
                }
            },
            Qt::DirectConnection);
    }

    bool stop()
    {
        if (process->state() == QProcess::NotRunning)
            return true;
        process->terminate();
        if (!process->waitForFinished(100)) {
            process->kill();
            process->waitForFinished(1900);
        }
        return process->state() == QProcess::NotRunning;
    }

    bool send(const QJsonObject &message, QString *error)
    {
        const auto frameBytes = QSocIpc::frame(message, wireLimit);
        if (frameBytes.isEmpty() || socket->write(frameBytes) != frameBytes.size()) {
            *error = QStringLiteral("FST worker transport write failed");
            return false;
        }
        QElapsedTimer elapsed;
        elapsed.start();
        while (socket->bytesToWrite() > 0) {
            if (elapsed.elapsed() >= 2000 || stopped()) {
                *error = QStringLiteral("FST worker transport write interrupted");
                return false;
            }
            socket->waitForBytesWritten(20);
        }
        return true;
    }

    bool stopped() const { return cancelled && cancelled->load(); }

    QString exited() const
    {
        if (process->state() != QProcess::NotRunning)
            return QStringLiteral("FST worker transport interrupted before exit");
        return QStringLiteral("FST worker failed (exit status: %1, exit code: %2)")
            .arg(process->exitStatus() == QProcess::NormalExit ? "normal" : "crash")
            .arg(process->exitCode());
    }

    bool limitReached()
    {
        if (bytes >= byteLimit) {
            reason = QStringLiteral("scan_bytes");
            return true;
        }
        if (quantum.elapsed() >= timeLimit) {
            reason = QStringLiteral("scan_time");
            return true;
        }
        return false;
    }

    QTemporaryDir                     directory;
    QLocalServer                      server;
    QSocProcessLimits                 limits;
    std::unique_ptr<QProcess>         process = std::make_unique<QProcess>();
    std::unique_ptr<QLocalSocket>     socket;
    ReadAt                            readAt;
    std::optional<Companion>          companion;
    std::shared_ptr<std::atomic_bool> cancelled;
    QByteArray                        input;
    QJsonObject                       heldRead;
    QElapsedTimer                     quantum;
    quint64                           size       = 0;
    quint64                           bytes      = 0;
    quint64                           offset     = 0;
    quint64                           byteLimit  = 0;
    int                               timeLimit  = 0;
    bool                              admitted   = false;
    bool                              evicted    = false;
    bool                              ownerEnded = false;
    std::shared_ptr<Slot>             slot;
    std::function<void()>             evictionHandler;
    bool                              acknowledge = false;
    bool                              selecting   = false;
    QString                           reason;
};

QSocFstClient::QSocFstClient()
    : state_(std::make_unique<State>())
{}
QSocFstClient::~QSocFstClient()
{
    if (state_->ownerEnded && state_->thread() != QThread::currentThread())
        state_.release()->deleteLater();
}

bool QSocFstClient::start(
    quint64                           size,
    ReadAt                            readAt,
    std::shared_ptr<std::atomic_bool> cancelled,
    QString                          *error,
    const QString                    &executable)
{
    return start(size, std::move(readAt), std::move(cancelled), error, executable, std::nullopt);
}

bool QSocFstClient::start(
    quint64                           size,
    ReadAt                            readAt,
    std::shared_ptr<std::atomic_bool> cancelled,
    QString                          *error,
    const QString                    &executable,
    std::optional<Companion>          companion)
{
    auto &s = *state_;
    if (!onOwnerThread()) {
        *error = QStringLiteral("FST client belongs to another thread");
        return false;
    }
    if (s.admitted || !readAt || !cancelled || size == 0 || (companion && !companion->readAt)) {
        *error = QStringLiteral("Invalid FST input");
        return false;
    }
    if (!s.admit(error))
        return false;
    s.size          = size;
    s.readAt        = std::move(readAt);
    s.companion     = std::move(companion);
    s.cancelled     = std::move(cancelled);
    const auto path = executable.isEmpty() ? QSocSibling::path("qsoc-wave-worker") : executable;
    if (!QFileInfo(path).isAbsolute() || !QFileInfo(path).isExecutable() || !s.directory.isValid()) {
        *error = QStringLiteral("FST worker or private temporary directory is unavailable");
        return false;
    }
    const auto endpoint = QSocLocalEndpoint::resolve(s.directory.filePath("wave.sock"));
    if (!QSocLocalEndpoint::prepareDirectory(endpoint, error))
        return false;
    s.server.setSocketOptions(QLocalServer::UserAccessOption);
    s.server.setMaxPendingConnections(1);
    if (!s.server.listen(endpoint) || !s.limits.configure(*s.process, memoryLimit)) {
        *error = QStringLiteral("Cannot configure FST worker isolation");
        return false;
    }
    s.process->setProgram(path);
    s.process->setArguments(
        {"--socket", endpoint, "--owner-pid", QString::number(QCoreApplication::applicationPid())});
    s.process->setWorkingDirectory(s.directory.path());
    s.process->setProcessEnvironment(workerEnvironment(s.directory.path()));
    s.process->setStandardOutputFile(QProcess::nullDevice());
    s.process->setStandardErrorFile(QProcess::nullDevice());
    s.watchExit();
    s.process->start();
    if (!s.process->waitForStarted(2000)) {
        *error = QStringLiteral("Cannot start FST worker");
        return false;
    }
    QElapsedTimer elapsed;
    elapsed.start();
    while (!s.socket && elapsed.elapsed() < 2000 && !s.stopped()) {
        s.server.waitForNewConnection(20);
        s.socket.reset(s.server.nextPendingConnection());
        s.process->waitForFinished(0);
        if (s.process->state() == QProcess::NotRunning && !s.socket) {
            *error = s.exited();
            return false;
        }
    }
    if (!s.socket || !QSocLocalPeer::sameUser(*s.socket)
        || QSocLocalPeer::processId(*s.socket) != s.process->processId()) {
        *error = QStringLiteral("FST worker identity verification failed");
        return false;
    }
    s.socket->setReadBufferSize(wireLimit + QSocIpc::headerBytes);
    beginQuantum(2000);
    QJsonObject hello;
    if (next(&hello, error) != Next::Message || hello.value("type") != "hello"
        || hello.value("protocol").toInt() != 2) {
        if (error->isEmpty())
            *error = QStringLiteral("Invalid FST worker handshake");
        return false;
    }
    s.acknowledge = false;
    QJsonObject request{{"type", "open"}, {"size", QString::number(size)}};
    if (s.companion)
        request["hierarchy_size"] = QString::number(s.companion->size);
    return s.send(request, error);
}

void QSocFstClient::beginQuantum(int milliseconds, quint64 bytes)
{
    auto &s     = *state_;
    s.bytes     = 0;
    s.byteLimit = bytes;
    s.timeLimit = qMax(1, milliseconds);
    s.reason.clear();
    s.quantum.start();
    keepAlive();
}

void QSocFstClient::keepAlive()
{
    auto &s = *state_;
    if ((s.acknowledge || !s.heldRead.isEmpty()) && s.socket
        && s.socket->state() == QLocalSocket::ConnectedState) {
        s.socket->write(QSocIpc::frame(QJsonObject{{"type", "keepalive"}}, wireLimit));
        s.socket->flush();
    }
}

QSocFstClient::Next QSocFstClient::next(QJsonObject *message, QString *error)
{
    if (!onOwnerThread()) {
        *error = QStringLiteral("FST client belongs to another thread");
        return Next::Failed;
    }
    auto &s = *state_;
    while (true) {
        if (s.evicted) {
            *error = QStringLiteral("cache_evicted: FST idle worker was reclaimed");
            return Next::Failed;
        }
        if (s.stopped()) {
            *error = QStringLiteral("FST read cancelled");
            return Next::Failed;
        }
        if (s.limitReached())
            return Next::Paused;
        if (s.acknowledge) {
            if (s.selecting) {
                *error = QStringLiteral("FST selection is required");
                return Next::Failed;
            }
            if (!s.send({{"type", "ack"}}, error))
                return Next::Failed;
            s.acknowledge = false;
        }
        QJsonObject incoming = std::exchange(s.heldRead, {});
        while (incoming.isEmpty()) {
            s.input += s.socket->read(wireLimit + QSocIpc::headerBytes - s.input.size());
            const auto decoded = QSocIpc::decode(s.input, incoming, wireLimit);
            if (decoded == QSocIpc::DecodeResult::Complete)
                break;
            if (decoded == QSocIpc::DecodeResult::Invalid || s.input.size() > wireLimit) {
                *error = QStringLiteral("Invalid FST worker frame");
                return Next::Failed;
            }
            s.process->waitForFinished(0);
            if (s.socket->state() != QLocalSocket::ConnectedState
                || s.process->state() == QProcess::NotRunning) {
                if (s.process->state() != QProcess::NotRunning)
                    s.process->waitForFinished(100);
                *error = s.exited();
                return Next::Failed;
            }
            if (s.stopped()) {
                *error = QStringLiteral("FST read cancelled");
                return Next::Failed;
            }
            if (s.limitReached())
                return Next::Paused;
            s.socket->waitForReadyRead(20);
        }
        const auto type = incoming.value("type").toString();
        if (type == "read") {
            s.heldRead      = incoming;
            const auto role = incoming.value("source").toString(QStringLiteral("main"));
            if ((incoming.contains("source") && !incoming.value("source").isString())
                || (role != "main" && role != "hierarchy")
                || (role == "hierarchy" && !s.companion)) {
                *error = QStringLiteral("Invalid FST input source");
                return Next::Failed;
            }
            const auto sourceSize = role == "main" ? s.size : s.companion->size;
            bool       ok         = false;
            const auto offset     = incoming.value("offset").toString().toULongLong(&ok);
            const int  requested  = incoming.value("count").toInt();
            if (!ok || requested < 1 || requested > 128 * 1024 || offset > sourceSize
                || quint64(requested) > sourceSize - offset) {
                *error = QStringLiteral("Invalid FST input range");
                return Next::Failed;
            }
            if (s.limitReached())
                return Next::Paused;
            const auto issued = static_cast<quint32>(
                qMin<quint64>(requested, s.byteLimit - s.bytes));
            const auto &reader = role == "main" ? s.readAt : s.companion->readAt;
            const auto  data   = reader(offset, issued, error);
            if (!error->isEmpty() || data.isEmpty() || quint64(data.size()) > issued) {
                if (error->isEmpty())
                    *error = QStringLiteral("FST input returned an invalid short read");
                return Next::Failed;
            }
            s.bytes += data.size();
            s.offset = offset + data.size();
            if (!s.send({{"type", "data"}, {"bytes", QString::fromLatin1(data.toBase64())}}, error))
                return Next::Failed;
            s.heldRead = {};
            continue;
        }
        if (type == "progress") {
            s.acknowledge = true;
            continue;
        }
        if (type == "error") {
            *error = QStringLiteral("FST %1 failed (input status: %2)")
                         .arg(incoming.value("phase").toString())
                         .arg(incoming.value("status").toInt());
            return Next::Failed;
        }
        if (type != "hello" && type != "header" && type != "signal" && type != "hierarchy_end"
            && type != "initial" && type != "event" && type != "complete") {
            *error = QStringLiteral("Unknown FST worker message");
            return Next::Failed;
        }
        if (type == "complete") {
            const std::lock_guard lock(State::poolMutex());
            if (s.slot->phase != State::Slot::Phase::Exited) {
                s.slot->phase     = State::Slot::Phase::Idle;
                s.slot->idleSince = std::chrono::steady_clock::now();
            }
        }
        s.acknowledge = true;
        s.selecting   = type == "hierarchy_end" || type == "complete";
        *message      = incoming;
        return Next::Message;
    }
}

bool QSocFstClient::select(
    const QJsonObject &selection, QString *error, std::shared_ptr<std::atomic_bool> cancelled)
{
    auto &s = *state_;
    if (!s.selecting || !s.acknowledge) {
        *error = QStringLiteral("Unexpected FST selection");
        return false;
    }
    {
        const std::lock_guard lock(State::poolMutex());
        if (s.evicted || s.slot->phase == State::Slot::Phase::Closing
            || s.slot->phase == State::Slot::Phase::Exited) {
            *error = QStringLiteral("cache_evicted: FST idle worker was reclaimed");
            return false;
        }
        s.slot->phase = State::Slot::Phase::Active;
    }
    if (cancelled)
        s.cancelled = std::move(cancelled);
    auto request    = selection;
    request["type"] = "select";
    if (!s.send(request, error))
        return false;
    s.selecting   = false;
    s.acknowledge = false;
    return true;
}

bool QSocFstClient::onOwnerThread() const
{
    const auto *owner = state_->thread();
    return owner == QThread::currentThread() && state_->server.thread() == owner
           && state_->process->thread() == owner
           && (!state_->socket || state_->socket->thread() == owner);
}

bool QSocFstClient::reusable() const
{
    const std::lock_guard lock(State::poolMutex());
    return !state_->evicted && state_->slot && state_->slot->phase == State::Slot::Phase::Idle
           && state_->selecting && state_->process->state() != QProcess::NotRunning;
}

void QSocFstClient::setEvictionHandler(std::function<void()> handler)
{
    state_->evictionHandler = std::move(handler);
}

void QSocFstClient::cancel()
{
    if (state_->cancelled)
        state_->cancelled->store(true);
    if (state_->stop())
        state_->releaseConfirmed();
}

quint64 QSocFstClient::scanBytes() const
{
    return state_->bytes;
}
quint64 QSocFstClient::scanOffset() const
{
    return state_->offset;
}
qint64 QSocFstClient::processId() const
{
    return state_->process->processId();
}
QString QSocFstClient::limitReason() const
{
    return state_->reason;
}
