// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolwaveform.h"

#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/tool/qsoctoolfilecore.h"
#include "agent/tool/qsoctoolpath.h"
#include "agent/waveform/qsocfstclient.h"
#include "agent/waveform/qsocvcdcheckpoints.h"

#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QPointer>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QScopedValueRollback>
#include <QThread>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <limits>
#include <mutex>
#include <stop_token>
#include <utility>

#ifdef Q_OS_WIN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <io.h>
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace {

constexpr qint64    kScanLimit   = 128 * 1024 * 1024;
constexpr int       kMaxSignals  = 50000;
constexpr int       kMaxSelected = 32;
constexpr qsizetype kOutputLimit = 1024 * 1024;
constexpr int       kCursorTtlMs = 5 * 60 * 1000;

struct FileVersion
{
    QByteArray identity;
    qint64     size       = -1;
    qint64     modified   = 0;
    qint64     modifiedNs = 0;
    qint64     changed    = 0;
    qint64     changedNs  = 0;

    bool valid() const { return size >= 0 && !identity.isEmpty(); }
    bool operator==(const FileVersion &) const = default;
};

#ifdef Q_OS_WIN
FileVersion handleVersion(HANDLE handle)
{
    BY_HANDLE_FILE_INFORMATION info{};
    FILE_BASIC_INFO            basic{};
    if (handle == INVALID_HANDLE_VALUE || !GetFileInformationByHandle(handle, &info)
        || !GetFileInformationByHandleEx(handle, FileBasicInfo, &basic, sizeof(basic))
        || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
        return {};
    const quint64 size = (quint64(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
    if (size > quint64(std::numeric_limits<qint64>::max()))
        return {};
    FileVersion version;
    version.identity = QStringLiteral("%1:%2:%3")
                           .arg(info.dwVolumeSerialNumber)
                           .arg(info.nFileIndexHigh)
                           .arg(info.nFileIndexLow)
                           .toLatin1();
    version.size     = static_cast<qint64>(size);
    version.modified = basic.LastWriteTime.QuadPart;
    version.changed  = basic.ChangeTime.QuadPart;
    return version;
}

FileVersion pathVersion(const QString &path)
{
    const QString native = QDir::toNativeSeparators(path);
    const HANDLE  handle = CreateFileW(
        reinterpret_cast<LPCWSTR>(native.utf16()),
        FILE_READ_ATTRIBUTES,
        FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
        nullptr,
        OPEN_EXISTING,
        FILE_FLAG_BACKUP_SEMANTICS,
        nullptr);
    const auto version = handleVersion(handle);
    if (handle != INVALID_HANDLE_VALUE)
        CloseHandle(handle);
    return version;
}

FileVersion fileVersion(const QFile &file)
{
    return handleVersion(reinterpret_cast<HANDLE>(_get_osfhandle(file.handle())));
}
#else
FileVersion statVersion(const struct stat &info)
{
    if (!S_ISREG(info.st_mode) || info.st_size < 0)
        return {};
    FileVersion version;
    version.identity = QByteArray::number(quint64(info.st_dev)) + ':'
                       + QByteArray::number(quint64(info.st_ino));
    version.size     = info.st_size;
#ifdef Q_OS_MACOS
    version.modified   = info.st_mtimespec.tv_sec;
    version.modifiedNs = info.st_mtimespec.tv_nsec;
    version.changed    = info.st_ctimespec.tv_sec;
    version.changedNs  = info.st_ctimespec.tv_nsec;
#else
    version.modified   = info.st_mtim.tv_sec;
    version.modifiedNs = info.st_mtim.tv_nsec;
    version.changed    = info.st_ctim.tv_sec;
    version.changedNs  = info.st_ctim.tv_nsec;
#endif
    return version;
}

FileVersion pathVersion(const QString &path)
{
    struct stat info{};
    const auto  encoded = QFile::encodeName(path);
    return ::stat(encoded.constData(), &info) == 0 ? statVersion(info) : FileVersion{};
}

FileVersion fileVersion(const QFile &file)
{
    struct stat info{};
    return ::fstat(file.handle(), &info) == 0 ? statVersion(info) : FileVersion{};
}
#endif

class WaveformSource
{
public:
    WaveformSource(QString path, QSocRemoteConnection *remote)
        : path_(std::move(path))
        , remote_(remote)
        , file_(path_)
    {}

    bool open(const std::function<bool()> &cancelled, QString *error)
    {
        cancelled_ = cancelled;
        if (remote_) {
            generation_ = remote_->generation();
            endpoint_   = remote_->endpointIdentity();
            if (!remote_->sftp()) {
                *error = QStringLiteral("source_expired: SFTP input is not connected");
                return false;
            }
            remoteFile_ = remote_->sftp()->openReadFile(path_, error, cancelled_);
            if (!remoteFile_)
                return false;
            size_ = remoteFile_->size();
        } else {
            version_ = pathVersion(path_);
            if (!version_.valid() || !file_.open(QIODevice::ReadOnly)) {
                *error = QStringLiteral("Cannot read waveform: %1").arg(path_);
                return false;
            }
            size_ = static_cast<quint64>(version_.size);
        }
        if (size_ > quint64(std::numeric_limits<qint64>::max())) {
            *error = QStringLiteral("Waveform size exceeds the supported offset range");
            return false;
        }
        return verify(error);
    }

    bool verify(QString *error)
    {
        if (cancelled_ && cancelled_()) {
            *error = QStringLiteral("Waveform read cancelled");
            return false;
        }
        if (remote_) {
            if (!bindingMatches(error))
                return false;
            // An EOF range verifies the pinned handle without seeking or reading data.
            const auto result = remoteFile_->readAt(size_, 1, error, cancelled_);
            return result.end == QSocSftpClient::ReadEnd::Eof && bindingMatches(error);
        }
        if (version_.valid() && fileVersion(file_) == version_ && pathVersion(path_) == version_)
            return true;
        *error = QStringLiteral("source_changed: waveform identity or attributes changed");
        return false;
    }

    QByteArray readAt(quint64 offset, quint32 count, QString *error)
    {
        error->clear();
        if (!count || count > 128 * 1024 || offset > size_ || count > size_ - offset) {
            *error = QStringLiteral("Invalid waveform input range");
            return {};
        }
        if (remote_) {
            if (!bindingMatches(error))
                return {};
            const auto result = remoteFile_->readAt(offset, count, error, cancelled_);
            if ((result.end != QSocSftpClient::ReadEnd::Eof
                 && result.end != QSocSftpClient::ReadEnd::Stopped)
                || !bindingMatches(error))
                return {};
            return result.data;
        }
        if (!verify(error))
            return {};
        if (!file_.seek(static_cast<qint64>(offset))) {
            *error = QStringLiteral("Cannot seek waveform input");
            return {};
        }
        const auto bytes = file_.read(count);
        if (file_.error() != QFileDevice::NoError) {
            *error = QStringLiteral("Cannot read waveform input");
            return {};
        }
        return verify(error) ? bytes : QByteArray{};
    }

    void close()
    {
        remoteFile_.reset();
        file_.close();
    }

    QString               path() const { return path_; }
    quint64               size() const { return size_; }
    QSocRemoteConnection *remote() const { return remote_; }

    bool bindingMatches(QString *error) const
    {
        if (!remote_)
            return true;
        if (remote_->generation() == generation_ && remote_->endpointIdentity() == endpoint_
            && remote_->sftp() && remoteFile_ && remoteFile_->isOpen())
            return true;
        *error = QStringLiteral("source_expired: waveform transport was replaced or closed");
        return false;
    }

private:
    QString                                   path_;
    QSocRemoteConnection                     *remote_ = nullptr;
    QFile                                     file_;
    FileVersion                               version_;
    std::unique_ptr<QSocSftpClient::ReadFile> remoteFile_;
    QSocRemoteConnection::Generation          generation_ = 0;
    QString                                   endpoint_;
    quint64                                   size_ = 0;
    std::function<bool()>                     cancelled_;
};

class Tokens
{
public:
    explicit Tokens(WaveformSource &source)
        : source_(source)
    {
        beginQuantum(nullptr);
    }

    void beginQuantum(QSocToolCallContext *context)
    {
        context_ = context;
        bytes_   = 0;
        error.clear();
        limitReason.clear();
        timer_.start();
    }

    void restore(quint64 offset)
    {
        buffer_.clear();
        token_.clear();
        position_  = 0;
        endOffset_ = offset;
    }

    void resetCancellation() { cancelled_ = std::make_shared<std::atomic_bool>(false); }

    bool stopped() const
    {
        return cancelled_->load() || (context_ && context_->isCancellationRequested());
    }
    void                              cancel() { cancelled_->store(true); }
    std::shared_ptr<std::atomic_bool> cancellationFlag() const { return cancelled_; }

    QByteArray next()
    {
        if (stopped()) {
            error = QStringLiteral("VCD read cancelled");
            return {};
        }
        while (error.isEmpty()) {
            if (position_ >= buffer_.size()) {
                if (endOffset_ >= source_.size())
                    return std::exchange(token_, {});
                if (context_ && context_->isCancellationRequested()) {
                    error = QStringLiteral("VCD read cancelled");
                    return {};
                }
                if (bytes_ >= kScanLimit || timer_.elapsed() > 5000) {
                    limitReason = bytes_ >= kScanLimit ? "scan_bytes" : "scan_time";
                    return {};
                }
                const auto count = qMin<quint64>(
                    65536, qMin<quint64>(kScanLimit - bytes_, source_.size() - endOffset_));
                buffer_ = source_.readAt(endOffset_, static_cast<quint32>(count), &error);
                bytes_ += buffer_.size();
                endOffset_ += buffer_.size();
                position_ = 0;
                if (!error.isEmpty() || buffer_.isEmpty()) {
                    if (error.isEmpty())
                        error = QStringLiteral("Cannot read VCD input");
                    return {};
                }
            }
            const char ch = buffer_.at(position_++);
            if (ch == ' ' || ch == '\t' || ch == '\r' || ch == '\n') {
                if (!token_.isEmpty())
                    return std::exchange(token_, {});
                continue;
            }
            token_ += ch;
            if (token_.size() > 65536) {
                error = QStringLiteral("VCD token exceeds 64 KiB");
            }
        }
        return {};
    }

    qint64 scanBytes() const { return bytes_; }
    qint64 offset() const { return static_cast<qint64>(endOffset_) - buffer_.size() + position_; }

    QString error;
    QString limitReason;

private:
    WaveformSource                   &source_;
    QByteArray                        buffer_;
    QByteArray                        token_;
    qsizetype                         position_  = 0;
    qint64                            bytes_     = 0;
    quint64                           endOffset_ = 0;
    QElapsedTimer                     timer_;
    QPointer<QSocToolCallContext>     context_;
    std::shared_ptr<std::atomic_bool> cancelled_ = std::make_shared<std::atomic_bool>(false);
};

struct Signal
{
    QString    name;
    QString    baseName;
    QByteArray id;
    QString    type;
    int        width = 0;
};

bool unsignedArgument(const json &arguments, const char *name, quint64 fallback, quint64 *value)
{
    const auto field = arguments.find(name);
    *value           = fallback;
    if (field == arguments.end())
        return true;
    if (!field->is_number_integer())
        return false;
    if (field->is_number_unsigned()) {
        *value = field->get<quint64>();
        return true;
    }
    const auto number = field->get<qint64>();
    if (number < 0)
        return false;
    *value = static_cast<quint64>(number);
    return true;
}

bool readHeader(Tokens &tokens, QList<Signal> *declarations, QString *timescale, QString *error)
{
    QStringList scope;
    qsizetype   declarationBytes = 0;
    for (QByteArray token = tokens.next(); !token.isEmpty(); token = tokens.next()) {
        if (!token.startsWith('$')) {
            *error = QStringLiteral("Expected a VCD header directive");
            return false;
        }
        QList<QByteArray> fields;
        qsizetype         fieldBytes = 0;
        QByteArray        field;
        while (!(field = tokens.next()).isEmpty() && field != "$end") {
            fields.append(field);
            fieldBytes += field.size();
            if (fields.size() > 1024 || fieldBytes > 65536) {
                *error = QStringLiteral("VCD header directive exceeds its size limit");
                return false;
            }
        }
        if (field != "$end")
            break;
        if (token == "$enddefinitions")
            return true;
        if (token == "$scope" && fields.size() >= 2) {
            if (scope.size() >= 64 || fields.at(1).size() > 256) {
                *error = QStringLiteral("VCD scope exceeds its depth or name limit");
                return false;
            }
            scope.append(QString::fromUtf8(fields.at(1)));
        } else if (token == "$upscope") {
            if (scope.isEmpty()) {
                *error = QStringLiteral("Unbalanced VCD scope");
                return false;
            }
            scope.removeLast();
        } else if (token == "$timescale") {
            timescale->clear();
            for (const auto &part : fields)
                *timescale += QString::fromLatin1(part);
        } else if (token == "$var") {
            bool      ok    = false;
            const int width = fields.size() >= 4 ? fields.at(1).toInt(&ok) : 0;
            if (!ok || width <= 0 || width > 65536 || declarations->size() >= kMaxSignals) {
                *error = QStringLiteral("Invalid VCD variable or more than 50000 declarations");
                return false;
            }
            const QString prefix   = scope.isEmpty() ? QString() : scope.join('.') + '.';
            const QString baseName = prefix + QString::fromUtf8(fields.at(3));
            QString       range;
            for (qsizetype index = 4; index < fields.size(); ++index)
                range += QString::fromLatin1(fields.at(index));
            static const QRegularExpression rangePattern(
                QStringLiteral("^\\[[+-]?[0-9]+(?::[+-]?[0-9]+)?\\]$"));
            if (!range.isEmpty() && !rangePattern.match(range).hasMatch()) {
                *error = QStringLiteral("Invalid VCD reference range");
                return false;
            }
            const QString name = baseName + range;
            declarationBytes += (name.size() + baseName.size()) * 2 + fields.at(2).size();
            if (name.size() > 4096 || declarationBytes > 8 * 1024 * 1024) {
                *error = QStringLiteral("VCD declarations exceed the 8 MiB name limit");
                return false;
            }
            declarations->append(
                {name, baseName, fields.at(2), QString::fromLatin1(fields.at(0)), width});
        }
    }
    *error = tokens.error.isEmpty() ? QStringLiteral("VCD enddefinitions is missing")
                                    : tokens.error;
    return false;
}

QList<Signal> selectSignals(const QList<Signal> &all, const json &arguments, QString *error)
{
    if (!arguments.contains("signals"))
        return {};
    const auto &names = arguments["signals"];
    if (!names.is_array() || names.empty() || names.size() > kMaxSelected) {
        *error = QStringLiteral("signals must contain 1 to 32 signal names or glob patterns");
        return {};
    }
    struct Pattern
    {
        QString            text;
        QRegularExpression regex;
        bool               exact = false;
    };
    QSet<QString>           exactNames;
    QHash<QString, QString> baseAliases;
    for (const auto &signal : all) {
        exactNames.insert(signal.name);
        auto alias = baseAliases.find(signal.baseName);
        if (alias == baseAliases.end())
            baseAliases.insert(signal.baseName, signal.name);
        else if (alias.value() != signal.name)
            alias.value().clear();
    }
    QList<Pattern> patterns;
    for (const auto &name : names) {
        if (!name.is_string()) {
            *error = QStringLiteral("Each signal request must be a string");
            return {};
        }
        QString    literal = QString::fromStdString(name.get<std::string>());
        bool       exact   = exactNames.contains(literal);
        const auto alias   = baseAliases.constFind(literal);
        if (!exact && alias != baseAliases.cend()) {
            if (alias.value().isEmpty()) {
                *error = QStringLiteral(
                    "Signal base name is ambiguous; select a complete indexed or ranged name from "
                    "the signal list");
                return {};
            }
            literal = alias.value();
            exact   = true;
        }
        if (!exact && name.get_ref<const std::string &>().size() > 256) {
            *error = QStringLiteral(
                "Signal glob patterns must be at most 256 bytes; listed exact names may be longer");
            return {};
        }
        patterns.append(
            {literal,
             exact ? QRegularExpression()
                   : QRegularExpression(QRegularExpression::wildcardToRegularExpression(literal)),
             exact});
    }
    QList<Signal> selected;
    for (const auto &signal : all) {
        for (const auto &pattern : patterns) {
            if (pattern.exact ? pattern.text == signal.name
                              : pattern.regex.match(signal.name).hasMatch()) {
                selected.append(signal);
                break;
            }
        }
        if (selected.size() > kMaxSelected) {
            *error = QStringLiteral("More than 32 signals matched; narrow the signal patterns");
            return {};
        }
    }
    if (selected.isEmpty())
        *error = QStringLiteral("No VCD signals match the requested patterns");
    return selected;
}

struct WaveformQuery
{
    WaveformQuery(const QString &path, QSocRemoteConnection *remote)
        : source(path, remote)
        , tokens(source)
    {}

    ~WaveformQuery()
    {
        QObject::disconnect(ownerConnection);
        QObject::disconnect(cancellationConnection);
        fst.reset();
        closeSources();
    }

    void closeSources()
    {
        const auto close = [this] {
            source.close();
            hierarchy.reset();
            return true;
        };
        if (source.remote() && source.remote()->sftp())
            source.remote()->sftp()->runWithin(QDeadlineTimer(2000), close, [] { return false; });
        else
            close();
    }

    bool sourceMatches()
    {
        sourceError.clear();
        return source.verify(&sourceError)
               && (!hierarchyUsed || (hierarchy && hierarchy->verify(&sourceError)));
    }

    std::unique_ptr<QSocFstClient>                             fst;
    QList<Signal>                                              fstSignals;
    bool                                                       fstHierarchy = false;
    quint64                                                    fstEnd       = 0;
    WaveformSource                                             source;
    Tokens                                                     tokens;
    std::unique_ptr<WaveformSource>                            hierarchy;
    QString                                                    hierarchyError;
    QString                                                    sourceError;
    bool                                                       hierarchyUsed = false;
    QSocVcdCheckpoints                                         checkpoints;
    quint64                                                    bodyOffset = 0;
    json                                                       request;
    json                                                       metadata;
    json                                                       initial = json::object();
    QHash<QByteArray, QList<Signal>>                           byId;
    QHash<QByteArray, QByteArray>                              previous;
    QList<Signal>                                              pendingSignals;
    QByteArray                                                 pendingValue;
    QByteArray                                                 pendingOld;
    QByteArray                                                 vectorToken;
    qsizetype                                                  pendingIndex    = 0;
    quint64                                                    start           = 0;
    quint64                                                    end             = 0;
    quint64                                                    time            = 0;
    quint64                                                    maxEvents       = 200;
    bool                                                       pendingReal     = false;
    bool                                                       comment         = false;
    bool                                                       initialComplete = false;
    bool                                                       done            = false;
    bool                                                       cacheEvicted    = false;
    QString                                                    cursor;
    QString                                                    jobId;
    QPointer<QObject>                                          owner;
    QMetaObject::Connection                                    ownerConnection;
    QMetaObject::Connection                                    cancellationConnection;
    std::unique_ptr<std::stop_callback<std::function<void()>>> stopCallback;
};

QString prepareVcd(
    WaveformQuery       &scan,
    const json          &arguments,
    QSocToolCallContext *context,
    bool                 reuseHeader = false)
{
    quint64 signalOffset = 0;
    if (!unsignedArgument(arguments, "start_time", 0, &scan.start)
        || !unsignedArgument(arguments, "end_time", std::numeric_limits<quint64>::max(), &scan.end)
        || !unsignedArgument(arguments, "max_events", 200, &scan.maxEvents)
        || !unsignedArgument(arguments, "signal_offset", 0, &signalOffset) || scan.end < scan.start
        || scan.maxEvents == 0 || scan.maxEvents > 2000) {
        return QStringLiteral(
            "Error: Use nonnegative time ticks, end_time >= start_time, and max_events from 1 to "
            "2000");
    }
    scan.tokens.beginQuantum(context);
    if (reuseHeader) {
        const auto checkpoint = scan.checkpoints.before(scan.start);
        scan.previous         = checkpoint ? checkpoint->values : QSocVcdCheckpoints::Values{};
        scan.time             = checkpoint ? checkpoint->time : 0;
        scan.tokens.restore(checkpoint ? checkpoint->offset : scan.bodyOffset);
        scan.pendingSignals.clear();
        scan.pendingValue.clear();
        scan.pendingOld.clear();
        scan.vectorToken.clear();
        scan.pendingIndex           = 0;
        scan.pendingReal            = false;
        scan.comment                = false;
        scan.initialComplete        = false;
        scan.done                   = false;
        scan.metadata["start_time"] = scan.start;
        scan.metadata.erase("end_time");
        if (arguments.contains("end_time"))
            scan.metadata["end_time"] = scan.end;
        scan.initial          = json::object();
        qsizetype outputBytes = scan.metadata.dump().size() + 512;
        for (auto it = scan.byId.cbegin(); it != scan.byId.cend(); ++it) {
            for (const auto &signal : it.value()) {
                const auto name  = signal.name.toStdString();
                const auto value = scan.previous.value(it.key());
                outputBytes += json(name).dump().size() + (value.isEmpty() ? 4 : value.size() + 2)
                               + 2;
                if (outputBytes > kOutputLimit)
                    return QStringLiteral("Error: VCD initial values exceed the 1 MiB output limit");
                scan.initial[name] = value.isEmpty() ? json(nullptr) : json(value.toStdString());
            }
        }
        scan.request = arguments;
        return {};
    }
    QList<Signal> all;
    QString       timescale;
    QString       error;
    if (!readHeader(scan.tokens, &all, &timescale, &error)) {
        if (!scan.tokens.limitReason.isEmpty())
            error = QStringLiteral("VCD header exceeds the scan limit");
        return QStringLiteral("Error: %1").arg(error);
    }
    scan.bodyOffset     = scan.tokens.offset();
    const auto selected = selectSignals(all, arguments, &error);
    if (!error.isEmpty())
        return QStringLiteral("Error: %1").arg(error);
    json result
        = {{"format", "vcd"},
           {"timescale", timescale.toStdString()},
           {"time_unit", "ticks"},
           {"signals", json::array()}};
    const bool  listOnly    = !arguments.contains("signals");
    const auto &listed      = listOnly ? all : selected;
    qsizetype   signalBytes = static_cast<qsizetype>(result.dump().size()) + 256;
    for (qsizetype index = 0; index < listed.size(); ++index) {
        if (listOnly && static_cast<quint64>(index) < signalOffset)
            continue;
        const auto &signal = listed.at(index);
        if (result["signals"].size() == 200)
            break;
        json entry
            = {{"name", signal.name.toStdString()},
               {"width", signal.width},
               {"type", signal.type.toStdString()}};
        const qsizetype entryBytes = static_cast<qsizetype>(entry.dump().size()) + 1;
        if (listOnly && signalBytes + entryBytes > kOutputLimit)
            break;
        signalBytes += entryBytes;
        result["signals"].push_back(std::move(entry));
    }
    if (listOnly) {
        result["total_signals"] = all.size();
        result["signal_offset"] = signalOffset;
        const quint64 next      = signalOffset + result["signals"].size();
        result["truncated"]     = next < static_cast<quint64>(all.size());
        if (result["truncated"] == true)
            result["next_signal_offset"] = next;
        return QString::fromStdString(result.dump());
    }

    for (const auto &signal : selected) {
        scan.byId[signal.id].append(signal);
        scan.initial[signal.name.toStdString()] = nullptr;
    }
    result["start_time"] = scan.start;
    if (arguments.contains("end_time"))
        result["end_time"] = scan.end;
    scan.metadata = std::move(result);
    for (const char *field : {"signals", "start_time", "end_time"}) {
        if (arguments.contains(field))
            scan.request[field] = arguments[field];
    }
    return {};
}

QString appendPending(WaveformQuery &scan, json &result, qsizetype *outputBytes)
{
    for (; scan.pendingIndex < scan.pendingSignals.size(); ++scan.pendingIndex) {
        const auto &signal = scan.pendingSignals.at(scan.pendingIndex);
        const auto  name   = signal.name.toStdString();
        if (scan.time <= scan.start) {
            const json      value = scan.pendingValue.toStdString();
            const qsizetype delta = static_cast<qsizetype>(value.dump().size())
                                    - static_cast<qsizetype>(
                                        result["values_at_start"][name].dump().size());
            if (*outputBytes + delta > kOutputLimit)
                return QStringLiteral("initial_values");
            *outputBytes += delta;
            result["values_at_start"][name] = value;
            scan.initial[name]              = value;
        }
        if (scan.time < scan.start || scan.pendingValue == scan.pendingOld)
            continue;
        if (result["events"].size() >= scan.maxEvents)
            return QStringLiteral("events");
        const bool  digitalBit = signal.width == 1 && signal.type != "real"
                                 && signal.type != "realtime" && !scan.pendingReal;
        const char *edge       = digitalBit && scan.pendingOld == "0" && scan.pendingValue == "1"
                                     ? "rising"
                                 : digitalBit && scan.pendingOld == "1" && scan.pendingValue == "0"
                                     ? "falling"
                                     : "change";
        json        event
            = {{"time", scan.time},
               {"signal", name},
               {"value", scan.pendingValue.toStdString()},
               {"edge", edge}};
        const auto eventBytes = static_cast<qsizetype>(event.dump().size()) + 1;
        if (*outputBytes + eventBytes > kOutputLimit)
            return QStringLiteral("output");
        *outputBytes += eventBytes;
        result["events"].push_back(std::move(event));
    }
    scan.pendingSignals.clear();
    scan.pendingIndex = 0;
    return {};
}

QString readChange(WaveformQuery &scan, const QByteArray &token)
{
    QByteArray value;
    QByteArray id;
    const bool realValue = token.startsWith('r') || token.startsWith('R');
    if (token.startsWith('b') || token.startsWith('B') || realValue) {
        value = token.mid(1).toLower();
        id    = scan.tokens.next();
        if (id.isEmpty() && !scan.tokens.limitReason.isEmpty()) {
            scan.vectorToken = token;
            return {};
        }
    } else if (token.size() >= 2 && QByteArray("01xXzZ").contains(token.at(0))) {
        value = token.left(1).toLower();
        id    = token.mid(1);
    } else {
        return QStringLiteral("Error: Invalid VCD value change");
    }
    bool valid = !value.isEmpty() && !id.isEmpty();
    if (realValue) {
        bool         numberOk = false;
        const double number   = value.toDouble(&numberOk);
        valid                 = valid && numberOk && std::isfinite(number);
    } else {
        valid = valid && std::all_of(value.cbegin(), value.cend(), [](char bit) {
                    return bit == '0' || bit == '1' || bit == 'x' || bit == 'z';
                });
    }
    if (!valid)
        return scan.tokens.error.isEmpty() ? QStringLiteral("Error: Invalid VCD value change")
                                           : QStringLiteral("Error: %1").arg(scan.tokens.error);
    if (scan.byId.contains(id)) {
        scan.pendingSignals = scan.byId.value(id);
        scan.pendingValue   = value;
        scan.pendingOld     = scan.previous.value(id);
        scan.pendingReal    = realValue;
        scan.previous[id]   = value;
    }
    return {};
}

QString readVcdPage(WaveformQuery &scan)
{
    json result               = scan.metadata;
    result["values_at_start"] = scan.initial;
    result["events"]          = json::array();
    qsizetype outputBytes     = static_cast<qsizetype>(result.dump().size()) + 512;
    if (outputBytes > kOutputLimit)
        return QStringLiteral("Error: VCD initial values exceed the 1 MiB output limit");
    QString reason;
    while (reason.isEmpty() && !scan.done) {
        reason = appendPending(scan, result, &outputBytes);
        if (!reason.isEmpty())
            break;
        const QByteArray token = scan.vectorToken.isEmpty() ? scan.tokens.next()
                                                            : std::exchange(scan.vectorToken, {});
        if (token.isEmpty()) {
            if (!scan.tokens.error.isEmpty())
                return QStringLiteral("Error: %1").arg(scan.tokens.error);
            reason = scan.tokens.limitReason;
            if (reason.isEmpty()) {
                if (scan.comment)
                    return QStringLiteral("Error: Unterminated VCD comment");
                scan.done            = true;
                scan.initialComplete = true;
            }
            break;
        }
        if (scan.comment) {
            scan.comment = token != "$end";
            continue;
        }
        if (token == "$comment") {
            scan.comment = true;
            continue;
        }
        if (token.startsWith('$'))
            continue;
        if (token.startsWith('#')) {
            bool          ok   = false;
            const quint64 next = token.mid(1).toULongLong(&ok);
            if (!ok || next < scan.time)
                return QStringLiteral("Error: Invalid or decreasing VCD timestamp");
            scan.time = next;
            scan.checkpoints.record(scan.time, scan.tokens.offset(), scan.previous);
            if (scan.time > scan.start)
                scan.initialComplete = true;
            scan.done = scan.time > scan.end;
            continue;
        }
        const QString error = readChange(scan, token);
        if (!error.isEmpty())
            return error;
        reason = scan.tokens.limitReason;
    }
    if (reason == "initial_values")
        return QStringLiteral("Error: VCD initial values exceed the 1 MiB output limit");
    result["truncated"]                = !scan.done;
    result["window_complete"]          = scan.done;
    result["values_at_start_complete"] = scan.initialComplete;
    result["last_time"]                = scan.time;
    result["scan_bytes"]               = scan.tokens.scanBytes();
    result["scan_offset"]              = scan.tokens.offset();
    if (!scan.done) {
        result["limit_reason"] = reason.toStdString();
        result["next_cursor"]  = scan.cursor.toStdString();
        if (reason == "scan_bytes" || reason == "scan_time")
            result["scan_error"] = "VCD scan limit reached; continue with next_cursor";
    }
    const auto text = QString::fromStdString(result.dump());
    if (text.toUtf8().size() > kOutputLimit)
        return QStringLiteral("Error: VCD output exceeds the 1 MiB output limit");
    return text;
}

QString prepareFst(
    WaveformQuery &scan, const json &arguments, QSocToolCallContext *context, bool reuse = false)
{
    quint64 signalOffset = 0;
    if (!unsignedArgument(arguments, "start_time", 0, &scan.start)
        || !unsignedArgument(arguments, "end_time", std::numeric_limits<quint64>::max(), &scan.end)
        || !unsignedArgument(arguments, "max_events", 200, &scan.maxEvents)
        || !unsignedArgument(arguments, "signal_offset", 0, &signalOffset) || scan.end < scan.start
        || scan.maxEvents == 0 || scan.maxEvents > 2000)
        return QStringLiteral(
            "Error: Use nonnegative ticks, end_time >= start_time, and max_events from 1 to 2000");
    scan.request = arguments;
    scan.tokens.beginQuantum(context);
    if (reuse) {
        scan.done            = false;
        scan.initialComplete = false;
        scan.time            = 0;
        scan.pendingSignals.clear();
        scan.pendingValue.clear();
        scan.pendingOld.clear();
        scan.pendingIndex = 0;
        scan.previous.clear();
        scan.initial = json::object();
        QJsonArray handles;
        for (auto it = scan.byId.cbegin(); it != scan.byId.cend(); ++it) {
            handles.append(static_cast<double>(it.key().toUInt()));
            for (const auto &signal : it.value())
                scan.initial[signal.name.toStdString()] = nullptr;
        }
        scan.metadata.erase("cache");
        scan.metadata.erase("end_time");
        scan.metadata["start_time"] = scan.start;
        if (arguments.contains("end_time"))
            scan.metadata["end_time"] = scan.end;
        scan.fst->beginQuantum();
        QString error;
        return scan.fst->select(
                   {{"handles", handles},
                    {"start", QString::number(scan.start)},
                    {"end", QString::number(scan.end)}},
                   &error,
                   scan.tokens.cancellationFlag())
                   ? QString()
                   : QStringLiteral("Error: %1").arg(error);
    }
    scan.metadata
        = {{"format", "fst"}, {"timescale", ""}, {"time_unit", "ticks"}, {"signals", json::array()}};
    scan.fst = std::make_unique<QSocFstClient>();
    QString    error;
    const auto readAt = [&scan](quint64 offset, quint32 count, QString *readError) {
        return scan.source.readAt(offset, count, readError);
    };
    scan.hierarchy
        = std::make_unique<WaveformSource>(scan.source.path() + ".hier", scan.source.remote());
    const auto                              cancelled = [&scan] { return scan.tokens.stopped(); };
    std::optional<QSocFstClient::Companion> companion;
    if (scan.hierarchy->open(cancelled, &scan.hierarchyError)) {
        companion = QSocFstClient::Companion{
            scan.hierarchy->size(), [&scan](quint64 offset, quint32 count, QString *readError) {
                scan.hierarchyUsed = true;
                return scan.hierarchy->readAt(offset, count, readError);
            }};
    } else {
        scan.hierarchy.reset();
    }
    if (scan.tokens.stopped())
        return QStringLiteral("Error: Waveform read cancelled");
    if (!scan.fst->start(
            scan.source.size(),
            readAt,
            scan.tokens.cancellationFlag(),
            &error,
            {},
            std::move(companion)))
        return QStringLiteral("Error: %1").arg(error);
    scan.fst->beginQuantum();
    return {};
}

QString fstTimescale(int exponent)
{
    static const QStringList units = {"as", "fs", "ps", "ns", "us", "ms", "s"};
    if (exponent < -18 || exponent > 2)
        return QStringLiteral("1e%1s").arg(exponent);
    const int index  = (exponent + 18) / 3;
    const int factor = (exponent + 18) % 3;
    return QString::number(factor == 0 ? 1 : factor == 1 ? 10 : 100) + units.at(index);
}

QString finishFstHierarchy(WaveformQuery &scan)
{
    QString    error;
    const auto selected = selectSignals(scan.fstSignals, scan.request, &error);
    if (!error.isEmpty())
        return QStringLiteral("Error: %1").arg(error);
    const bool  listOnly = !scan.request.contains("signals");
    const auto &listed   = listOnly ? scan.fstSignals : selected;
    quint64     offset   = 0;
    unsignedArgument(scan.request, "signal_offset", 0, &offset);
    qsizetype outputBytes = scan.metadata.dump().size() + 256;
    for (qsizetype index = 0; index < listed.size(); ++index) {
        if (listOnly && quint64(index) < offset)
            continue;
        const auto &signal = listed.at(index);
        json        entry
            = {{"name", signal.name.toStdString()},
               {"width", signal.width},
               {"type", signal.type.toStdString()}};
        const qsizetype bytes = entry.dump().size() + 1;
        if (listOnly
            && (scan.metadata["signals"].size() == 200 || outputBytes + bytes > kOutputLimit))
            break;
        outputBytes += bytes;
        scan.metadata["signals"].push_back(std::move(entry));
    }
    if (listOnly) {
        scan.metadata["total_signals"] = scan.fstSignals.size();
        scan.metadata["signal_offset"] = offset;
        const quint64 next             = offset + scan.metadata["signals"].size();
        scan.metadata["truncated"]     = next < quint64(scan.fstSignals.size());
        if (scan.metadata["truncated"] == true)
            scan.metadata["next_signal_offset"] = next;
        scan.done = true;
        return QString::fromStdString(scan.metadata.dump());
    }
    QJsonArray       handles;
    QSet<QByteArray> seen;
    for (const auto &signal : selected) {
        scan.byId[signal.id].append(signal);
        scan.initial[signal.name.toStdString()] = nullptr;
        if (!seen.contains(signal.id)) {
            seen.insert(signal.id);
            handles.append(static_cast<double>(signal.id.toUInt()));
        }
    }
    scan.metadata["start_time"] = scan.start;
    if (scan.request.contains("end_time"))
        scan.metadata["end_time"] = scan.end;
    if (!scan.fst->select(
            {{"handles", handles},
             {"start", QString::number(scan.start)},
             {"end", QString::number(scan.end)}},
            &error))
        return QStringLiteral("Error: %1").arg(error);
    scan.fstSignals.clear();
    scan.fstHierarchy = true;
    return {};
}

QString readFstPage(WaveformQuery &scan, QSocToolCallContext *context)
{
    json result               = scan.metadata;
    result["values_at_start"] = scan.initial;
    result["events"]          = json::array();
    qsizetype outputBytes     = result.dump().size() + 512;
    QString   reason;
    while (!scan.done && reason.isEmpty()) {
        if (context && context->isCancellationRequested())
            scan.tokens.cancel();
        reason = appendPending(scan, result, &outputBytes);
        if (!reason.isEmpty())
            break;
        QJsonObject message;
        QString     error;
        const auto  next = scan.fst->next(&message, &error);
        if (next == QSocFstClient::Next::Failed) {
            if (error.contains("hierarchy_missing") && !scan.hierarchyError.isEmpty())
                error += ": " + scan.hierarchyError;
            return QStringLiteral("Error: %1").arg(error);
        }
        if (next == QSocFstClient::Next::Paused) {
            reason = scan.fst->limitReason();
            break;
        }
        const auto type = message.value("type").toString();
        if (type == "header") {
            scan.metadata["timescale"]
                = fstTimescale(message.value("timescale").toInt()).toStdString();
            scan.fstEnd = message.value("end").toString().toULongLong();
        } else if (type == "signal") {
            QString                         name = message.value("name").toString();
            static const QRegularExpression range(
                QStringLiteral("\\s+(\\[[+-]?[0-9]+(?::[+-]?[0-9]+)?\\])$"));
            QString    base       = name;
            const auto rangeMatch = range.match(name);
            if (rangeMatch.hasMatch()) {
                base = name.left(rangeMatch.capturedStart());
                name = base + rangeMatch.captured(1);
            }
            scan.fstSignals.append(
                {name,
                 base,
                 QByteArray::number(message.value("handle").toInteger()),
                 message.value("kind").toString(),
                 message.value("width").toInt()});
        } else if (type == "hierarchy_end") {
            const auto prepared = finishFstHierarchy(scan);
            if (!prepared.isEmpty())
                return prepared;
            result                    = scan.metadata;
            result["values_at_start"] = scan.initial;
            result["events"]          = json::array();
            outputBytes               = result.dump().size() + 512;
        } else if (type == "initial") {
            const auto id     = QByteArray::number(message.value("handle").toInteger());
            const auto value  = message.value("value").toString().toLatin1().toLower();
            scan.previous[id] = value;
            for (const auto &signal : scan.byId.value(id)) {
                scan.initial[signal.name.toStdString()]              = value.toStdString();
                result["values_at_start"][signal.name.toStdString()] = value.toStdString();
            }
            outputBytes = result.dump().size() + 512;
        } else if (type == "event") {
            bool       ok   = false;
            const auto time = message.value("time").toString().toULongLong(&ok);
            const auto id   = QByteArray::number(message.value("handle").toInteger());
            if (!ok || time < scan.time || !scan.byId.contains(id))
                return QStringLiteral("Error: Invalid FST event order or handle");
            scan.time            = time;
            scan.initialComplete = time > scan.start;
            scan.pendingSignals  = scan.byId.value(id);
            scan.pendingValue    = message.value("value").toString().toLatin1().toLower();
            scan.pendingOld      = scan.previous.value(id);
            scan.pendingReal     = message.value("real").toBool();
            scan.previous[id]    = scan.pendingValue;
        } else if (type == "complete") {
            if (message.contains("cache")) {
                scan.metadata["cache"] = json::parse(QJsonDocument(message.value("cache").toObject())
                                                         .toJson(QJsonDocument::Compact)
                                                         .toStdString());
                result["cache"] = scan.metadata["cache"];
            }
            scan.done            = true;
            scan.initialComplete = true;
            scan.time            = qMax(scan.time, qMin(scan.end, scan.fstEnd));
        } else {
            return QStringLiteral("Error: Unexpected FST worker response");
        }
        if (outputBytes > kOutputLimit)
            return QStringLiteral("Error: FST initial values exceed the 1 MiB output limit");
    }
    if (reason == "initial_values")
        return QStringLiteral("Error: FST initial values exceed the 1 MiB output limit");
    result["timescale"]                = scan.metadata["timescale"];
    result["truncated"]                = !scan.done;
    result["window_complete"]          = scan.done;
    result["metadata_complete"]        = scan.fstHierarchy;
    result["values_at_start_complete"] = scan.initialComplete;
    result["last_time"]                = scan.time;
    result["scan_bytes"]               = scan.fst->scanBytes();
    result["scan_offset"]              = scan.fst->scanOffset();
    if (!scan.done) {
        result["limit_reason"] = reason.toStdString();
        result["next_cursor"]  = scan.cursor.toStdString();
    }
    const auto text = QString::fromStdString(result.dump());
    return text.toUtf8().size() <= kOutputLimit
               ? text
               : QStringLiteral("Error: FST output exceeds the 1 MiB output limit");
}

} // namespace

struct QSocToolWaveformRead::State
{
    struct Idle
    {
        QPointer<QSocToolWaveformRead>   tool;
        QString                          jobId;
        QSocRemoteConnection::Generation generation = 0;
    };
    struct Reclaimed
    {
        std::mutex              mutex;
        std::condition_variable changed;
        bool                    done    = false;
        bool                    success = false;
    };

    static std::mutex &idleMutex()
    {
        static std::mutex value;
        return value;
    }

    static QHash<QSocRemoteConnection *, Idle> &idleSources()
    {
        static QHash<QSocRemoteConnection *, Idle> value;
        return value;
    }

    static void forgetIdle(QSocToolWaveformRead *tool)
    {
        const std::lock_guard lock(idleMutex());
        auto                 &records = idleSources();
        for (auto it = records.begin(); it != records.end();) {
            if (!it->tool || it->tool == tool)
                it = records.erase(it);
            else
                ++it;
        }
    }

    static bool reclaimIdle(
        QSocRemoteConnection *remote, QSocToolWaveformRead *except, QString *error)
    {
        if (!remote)
            return true;
        Idle                  old;
        const auto            result = std::make_shared<Reclaimed>();
        std::function<void()> reclaim;
        bool                  sameThread = false;
        {
            const std::lock_guard lock(idleMutex());
            auto                 &records = idleSources();
            const auto            found   = records.find(remote);
            if (found == records.end()
                || (found->tool == except && found->generation == remote->generation()))
                return true;
            old = found.value();
            records.erase(found);
            if (!old.tool)
                return true;
            reclaim = [old, result] {
                bool success = true;
                if (old.tool && old.tool->state_->query
                    && old.tool->state_->query->jobId == old.jobId) {
                    auto &state = *old.tool->state_;
                    if (state.executing) {
                        state.query->cacheEvicted = true;
                        QMetaObject::invokeMethod(
                            old.tool,
                            [old] {
                                if (old.tool && old.tool->state_->query
                                    && old.tool->state_->query->jobId == old.jobId)
                                    old.tool->abort();
                            },
                            Qt::QueuedConnection);
                        success = false;
                    } else {
                        state.expiry.stop();
                        state.query.reset();
                    }
                }
                const std::lock_guard lock(result->mutex);
                result->success = success;
                result->done    = true;
                result->changed.notify_one();
            };
            sameThread = old.tool->thread() == QThread::currentThread();
            if (!sameThread)
                QMetaObject::invokeMethod(old.tool, reclaim, Qt::QueuedConnection);
        }
        if (sameThread)
            reclaim();
        std::unique_lock lock(result->mutex);
        if (!result->changed.wait_for(lock, std::chrono::milliseconds(8000), [&] {
                return result->done;
            })) {
            if (!old.tool)
                return true;
            *error = QStringLiteral("cleanup_failed: SFTP waveform cache cleanup timed out");
            return false;
        }
        if (!result->success)
            *error = QStringLiteral("busy: SFTP waveform cache is still executing");
        return result->success;
    }

    static bool rememberIdle(QSocToolWaveformRead *tool)
    {
        if (!tool->remote_)
            return true;
        QString error;
        if (!reclaimIdle(tool->remote_, tool, &error))
            return false;
        const std::lock_guard lock(idleMutex());
        idleSources()[tool->remote_]
            = {tool, tool->state_->query->jobId, tool->remote_->generation()};
        return true;
    }

    std::unique_ptr<WaveformQuery> query;
    QTimer                         expiry;
    bool                           executing = false;
};

QSocToolWaveformRead::QSocToolWaveformRead(
    QObject *parent, QSocPathContext *paths, QSocRemoteConnection *remote)
    : QSocTool(parent)
    , state_(std::make_unique<State>())
    , paths_(paths)
    , remote_(remote)
{
    state_->expiry.setParent(this);
    state_->expiry.setSingleShot(true);
    connect(&state_->expiry, &QTimer::timeout, this, [this] { abort(); });
    connect(
        QThread::currentThread(),
        &QThread::finished,
        this,
        [this] { abort(); },
        Qt::DirectConnection);
}

QSocToolWaveformRead::~QSocToolWaveformRead()
{
    State::forgetIdle(this);
}

void QSocToolWaveformRead::abort()
{
    State::forgetIdle(this);
    state_->expiry.stop();
    if (state_->query && state_->executing)
        state_->query->tokens.cancel();
    else
        state_->query.reset();
}

QString QSocToolWaveformRead::getName() const
{
    return QStringLiteral("read_waveform");
}

QString QSocToolWaveformRead::getDescription() const
{
    return QStringLiteral(
        "Read a local or SFTP VCD or FST waveform. Omit signals to list hierarchical names. "
        "Select up to 32 names or globs to query values and rising/falling edges. "
        "Times are raw ticks in the file's timescale. Each call has a 128 MiB input budget and "
        "a five second work quantum. Pass next_cursor as cursor to continue. A new query "
        "replaces the same owner's previous continuation. Output is limited to 1 MiB, "
        "including metadata, initial values and events. Remote input uses fixed read-only SFTP "
        "handles and requires no remote execution. Analog waveform formats are not supported.");
}

json QSocToolWaveformRead::getParametersSchema() const
{
    return {
        {"type", "object"},
        {"properties",
         {{"file_path",
           {{"type", "string"},
            {"description", "Local VCD or FST path, relative to the working directory or absolute"}}},
          {"signals",
           {{"type", "array"},
            {"items", {{"type", "string"}}},
            {"minItems", 1},
            {"maxItems", 32},
            {"description", "Hierarchical signal names or glob patterns; omit to list signals"}}},
          {"start_time",
           {{"type", "integer"},
            {"minimum", 0},
            {"description", "Inclusive start in waveform ticks, default 0"}}},
          {"end_time",
           {{"type", "integer"}, {"minimum", 0}, {"description", "Inclusive end in waveform ticks"}}},
          {"max_events",
           {{"type", "integer"},
            {"minimum", 1},
            {"maximum", 2000},
            {"description", "Maximum returned events, default 200"}}},
          {"signal_offset",
           {{"type", "integer"},
            {"minimum", 0},
            {"description", "Signal-list offset, default 0; use next_signal_offset to continue"}}},
          {"cursor",
           {{"type", "string"},
            {"description",
             "Opaque next_cursor from a limited query; expires after five idle minutes"}}}}},
        {"required", json::array({"file_path"})}};
}

QString QSocToolWaveformRead::execute(const json &arguments)
{
    if (state_->executing)
        return QStringLiteral("Error: busy: waveform reader is already executing");
    const QScopedValueRollback<bool> executing(state_->executing, true);
    const auto                       cleanup = qScopeGuard([this] {
        if (state_->query && state_->query->done
            && (state_->query->cacheEvicted || state_->query->tokens.cancellationFlag()->load())) {
            State::forgetIdle(this);
            state_->expiry.stop();
            state_->query.reset();
        }
    });
    if (!arguments.contains("file_path") || !arguments["file_path"].is_string())
        return QStringLiteral("Error: file_path is required");
    QString base = paths_ ? paths_->getWorkingDir() : QString();
    if (base.isEmpty() && paths_)
        base = paths_->getProjectDir();
    if (base.isEmpty())
        base = QDir::currentPath();
    QString home = qEnvironmentVariable("HOME");
    if (home.isEmpty())
        home = QDir::homePath();
#ifdef Q_OS_WIN
    constexpr bool windows = true;
#else
    constexpr bool windows = false;
#endif
    const auto context = currentCallContext();
    QObject   *owner   = context ? context->executionScope() : nullptr;
    if (state_->query
        && (state_->query->tokens.cancellationFlag()->load() || state_->query->cacheEvicted
            || state_->expiry.remainingTime() <= 0)) {
        state_->query.reset();
        state_->expiry.stop();
    }
    if (!arguments.contains("cursor") && state_->query && !state_->query->done
        && state_->query->owner != owner)
        return QStringLiteral("Error: busy: waveform continuation belongs to another owner");
    const QPointer<QSocToolCallContext> contextGuard(context);
    const bool                          hadContext    = context != nullptr;
    const auto                          pathCancelled = [contextGuard, hadContext] {
        return hadContext && (!contextGuard || contextGuard->isCancellationRequested());
    };
    const auto sourceFailure = [&](QString error) {
        if (state_->query && state_->query->owner == owner && arguments.contains("cursor")
            && arguments["cursor"].is_string()
            && arguments["cursor"].get<std::string>() == state_->query->cursor.toStdString()) {
            state_->query.reset();
            state_->expiry.stop();
        }
        return QStringLiteral("Error: %1").arg(error);
    };
    if (state_->query && state_->query->owner == owner && arguments.contains("cursor")
        && arguments["cursor"].is_string()
        && arguments["cursor"].get<std::string>() == state_->query->cursor.toStdString()) {
        QString error;
        if (!state_->query->source.bindingMatches(&error))
            return sourceFailure(error);
    }
    const auto requested = QString::fromStdString(arguments["file_path"].get<std::string>());
    QString    path;
    if (remote_) {
        if (!remote_->sftp())
            return sourceFailure(QStringLiteral("source_expired: SFTP input is not connected"));
        if (!arguments.contains("cursor")) {
            const bool resolved = remote_->sftp()->runWithin(
                QDeadlineTimer(30000),
                [&] {
                    remote_->learnHome(requested);
                    return true;
                },
                pathCancelled);
            if (!resolved)
                return sourceFailure(
                    QStringLiteral("Remote waveform path lookup was cancelled or timed out"));
        }
        path = remote_->path()->normalize(requested);
    } else {
        path = QFileInfo(QSocToolFileCore::localPath(requested, base, home, windows))
                   .absoluteFilePath();
    }
    const auto suffix = QFileInfo(requested).suffix().toLower();
    if (suffix != "vcd" && suffix != "fst")
        return QStringLiteral("Error: read_waveform supports VCD and FST files only");
    if (arguments.contains("cursor")) {
        if (!arguments["cursor"].is_string() || !state_->query || state_->query->done
            || arguments["cursor"].get<std::string>() != state_->query->cursor.toStdString()) {
            return QStringLiteral("Error: cursor_expired: waveform continuation is unavailable");
        }
        if (state_->expiry.remainingTime() <= 0) {
            state_->query.reset();
            return QStringLiteral("Error: cursor_expired: waveform continuation is unavailable");
        }
        auto &scan = *state_->query;
        if (scan.owner != owner || path != scan.source.path())
            return QStringLiteral("Error: Waveform cursor belongs to a different source or owner");
        for (const char *field : {"signals", "start_time", "end_time"}) {
            if (arguments.contains(field)
                && (!scan.request.contains(field) || scan.request[field] != arguments[field])) {
                return QStringLiteral("Error: Waveform cursor query parameters do not match");
            }
        }
        quint64 maxEvents = 0;
        if (!unsignedArgument(arguments, "max_events", scan.maxEvents, &maxEvents) || maxEvents == 0
            || maxEvents > 2000) {
            return QStringLiteral("Error: max_events must be from 1 to 2000");
        }
        scan.maxEvents = maxEvents;
        state_->expiry.stop();
        scan.tokens.beginQuantum(context);
        if (!scan.sourceMatches()) {
            const auto error = scan.sourceError;
            state_->query.reset();
            return QStringLiteral("Error: %1").arg(error);
        }
        scan.cursor = QUuid::createUuid().toString(QUuid::WithoutBraces);
        if (scan.fst)
            scan.fst->beginQuantum();
    } else {
        state_->expiry.stop();
        State::forgetIdle(this);
        const bool sameRequest = state_->query && !state_->query->cacheEvicted
                                 && state_->query->owner == owner
                                 && state_->query->source.path() == path
                                 && arguments.contains("signals")
                                 && state_->query->request.contains("signals")
                                 && arguments["signals"] == state_->query->request["signals"];
        bool       reuseVcd    = sameRequest && suffix == "vcd" && !state_->query->fst;
        bool reuseFst = sameRequest && suffix == "fst" && state_->query->done && state_->query->fst
                        && state_->query->fst->reusable();
        if (reuseVcd || reuseFst) {
            auto &scan = *state_->query;
            QObject::disconnect(scan.ownerConnection);
            QObject::disconnect(scan.cancellationConnection);
            scan.stopCallback.reset();
            scan.jobId = QUuid::createUuid().toString(QUuid::WithoutBraces);
            scan.cursor.clear();
            scan.tokens.resetCancellation();
            scan.tokens.beginQuantum(context);
            if (!scan.sourceMatches()) {
                reuseVcd = false;
                reuseFst = false;
            }
            if (reuseFst)
                reuseFst = scan.fst->reusable();
        }
        if (!reuseVcd && !reuseFst)
            state_->query = std::make_unique<WaveformQuery>(path, remote_);
        state_->query->tokens.beginQuantum(context);
        QString    sourceError;
        const auto stopped = [query = state_->query.get()] { return query->tokens.stopped(); };
        if (!reuseVcd && !reuseFst && !State::reclaimIdle(remote_, this, &sourceError)) {
            state_->query.reset();
            return QStringLiteral("Error: %1").arg(sourceError);
        }
        if (!reuseVcd && !reuseFst && !state_->query->source.open(stopped, &sourceError)) {
            state_->query.reset();
            return QStringLiteral("Error: %1").arg(sourceError);
        }
        const QString prepared = suffix == "fst"
                                     ? prepareFst(*state_->query, arguments, context, reuseFst)
                                     : prepareVcd(*state_->query, arguments, context, reuseVcd);
        if (!prepared.isEmpty()) {
            state_->query.reset();
            return prepared;
        }
        auto &scan  = *state_->query;
        scan.cursor = QUuid::createUuid().toString(QUuid::WithoutBraces);
        scan.jobId  = scan.cursor;
        scan.owner  = owner;
        if (scan.fst) {
            const QPointer<QSocToolWaveformRead> guard(this);
            scan.fst->setEvictionHandler([guard, jobId = scan.jobId] {
                if (!guard || !guard->state_->query || guard->state_->query->jobId != jobId)
                    return;
                auto &state               = *guard->state_;
                state.query->cacheEvicted = true;
                State::forgetIdle(guard);
                if (state.executing)
                    return;
                {
                    const QScopedValueRollback<bool> executing(state.executing, true);
                    state.query->closeSources();
                }
                QMetaObject::invokeMethod(
                    guard,
                    [guard, jobId] {
                        if (guard && guard->state_->query && guard->state_->query->jobId == jobId)
                            guard->abort();
                    },
                    Qt::QueuedConnection);
            });
        }
        const auto cancelQuery = [this, jobId = scan.jobId] {
            if (state_->query && state_->query->jobId == jobId)
                abort();
        };
        if (owner) {
            scan.ownerConnection = connect(owner, &QObject::destroyed, this, cancelQuery);
        }
        if (context) {
            scan.cancellationConnection = context->retainCancellation(this, cancelQuery);
            const QPointer<QSocToolWaveformRead> guard(this);
            const auto                           cancelled = scan.tokens.cancellationFlag();
            scan.stopCallback = std::make_unique<std::stop_callback<std::function<void()>>>(
                context->parentStopToken(), [guard, jobId = scan.jobId, cancelled] {
                    cancelled->store(true);
                    if (guard) {
                        QMetaObject::invokeMethod(
                            guard,
                            [guard, jobId] {
                                if (guard && guard->state_->query
                                    && guard->state_->query->jobId == jobId)
                                    guard->abort();
                            },
                            Qt::QueuedConnection);
                    }
                });
        }
    }
    const QString result = state_->query->fst ? readFstPage(*state_->query, context)
                                              : readVcdPage(*state_->query);
    if (result.startsWith("Error:")) {
        state_->query.reset();
        state_->expiry.stop();
        return result;
    }
    if (!state_->query->sourceMatches()) {
        const auto error = state_->query->sourceError;
        state_->query.reset();
        return QStringLiteral("Error: %1").arg(error);
    }
    if (state_->query->done && state_->query->fst && !state_->query->fst->reusable()) {
        state_->query.reset();
        state_->expiry.stop();
    } else {
        if (state_->query->done)
            state_->query->cursor.clear();
        if (state_->query->fst)
            state_->query->fst->keepAlive();
        state_->expiry.start(kCursorTtlMs);
    }
    if (state_->query && state_->query->done
        && (state_->query->cacheEvicted || state_->query->tokens.stopped()
            || !State::rememberIdle(this))) {
        state_->query.reset();
        state_->expiry.stop();
    }
    return result;
}

#include "moc_qsoctoolwaveform.cpp"
