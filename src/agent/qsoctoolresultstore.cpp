// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctoolresultstore.h"

#include <nlohmann/json.hpp>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QMap>
#include <QMutexLocker>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStringDecoder>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <limits>

namespace {
using json                   = nlohmann::json;
constexpr qint64 headerLimit = 4096;

void fail(QString *error, const QString &message)
{
    if (error)
        *error = message;
}
QString digest(const QByteArray &data)
{
    return QString::fromLatin1(QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex());
}
bool allowed(const QSocToolResultStore::Guard &guard)
{
    return !guard || guard();
}
} // namespace

QSocToolResultStore::QSocToolResultStore(QString directory, QString owner)
    : QSocToolResultStore(std::move(directory), std::move(owner), Limits{})
{}

QSocToolResultStore::QSocToolResultStore(QString directory, QString owner, Limits limits)
    : owner_(std::move(owner))
    , limits_(limits)
{
    const QFileInfo root(directory);
    if (root.isSymLink() || owner_.isEmpty() || owner_.toUtf8().size() > 256
        || limits.artifactBytes <= 0 || limits.sessionBytes <= 0 || limits.pageBytes < 4)
        return;
    if (!QDir().mkpath(root.absoluteFilePath()))
        return;
    directory_           = QFileInfo(root.absoluteFilePath()).canonicalFilePath();
    const QString marker = QDir(directory_).filePath(QStringLiteral(".scope"));
    QLockFile     writer(QDir(directory_).filePath(QStringLiteral(".write.lock")));
    if (!writer.tryLock(0) || QFileInfo(marker).isSymLink()) {
        directory_.clear();
        return;
    }
    QFile current(marker);
    if (current.exists()) {
        if (!current.open(QIODevice::ReadOnly) || current.size() > headerLimit) {
            directory_.clear();
            return;
        }
        binding_ = current.readAll();
        try {
            const auto value = json::parse(binding_.toStdString());
            if (value.at("version") != 1 || value.at("owner") != owner_.toStdString()
                || !validId(QString::fromStdString(value.at("nonce").get<std::string>())))
                directory_.clear();
        } catch (const json::exception &) {
            directory_.clear();
        }
        return;
    }
    const json value
        = {{"version", 1},
           {"owner", owner_.toStdString()},
           {"nonce", QUuid::createUuid().toString(QUuid::Id128).toStdString()}};
    binding_ = QByteArray::fromStdString(value.dump());
    QSaveFile output(marker);
    output.setDirectWriteFallback(false);
    if (!output.open(QIODevice::WriteOnly) || output.write(binding_) != binding_.size()
        || !output.commit())
        directory_.clear();
}

std::shared_ptr<QSocToolResultStore> QSocToolResultStore::temporary(Limits limits)
{
    auto directory = std::make_shared<QTemporaryDir>(
        QDir::tempPath() + QStringLiteral("/qsoc-tool-results-XXXXXX"));
    if (!directory->isValid())
        return {};
    auto store = std::make_shared<QSocToolResultStore>(
        directory->path(), QUuid::createUuid().toString(QUuid::Id128), limits);
    if (!store->isBound())
        return {};
    store->temporaryDirectory_ = std::move(directory);
    return store;
}

bool QSocToolResultStore::isBound() const
{
    const QFileInfo info(directory_);
    if (directory_.isEmpty() || !info.isDir() || info.isSymLink()
        || info.canonicalFilePath() != directory_)
        return false;
    const QString marker = QDir(directory_).filePath(QStringLiteral(".scope"));
    QFile         file(marker);
    return !QFileInfo(marker).isSymLink() && file.open(QIODevice::ReadOnly)
           && file.size() == binding_.size() && file.readAll() == binding_;
}

bool QSocToolResultStore::validId(const QString &id)
{
    if (id.size() != 32)
        return false;
    return std::all_of(id.cbegin(), id.cend(), [](QChar ch) {
        return (ch >= QLatin1Char('0') && ch <= QLatin1Char('9'))
               || (ch >= QLatin1Char('a') && ch <= QLatin1Char('f'));
    });
}

QString QSocToolResultStore::path(const QString &id) const
{
    return QDir(directory_).filePath(id + QStringLiteral(".qtr"));
}

qint64 QSocToolResultStore::reservedBytes() const
{
    QMutexLocker lock(&mutex_);
    return reservedBytes_;
}

qint64 QSocToolResultStore::storedBytes(QString *error) const
{
    QMutexLocker lock(&mutex_);
    if (!isBound()) {
        fail(error, QStringLiteral("Artifact storage binding is unavailable."));
        return -1;
    }
    qint64 total = 0;
    for (const auto &info : QDir(directory_).entryInfoList({QStringLiteral("*.qtr")}, QDir::Files)) {
        if (info.isSymLink() || info.size() > std::numeric_limits<qint64>::max() - total) {
            fail(error, QStringLiteral("Artifact storage contains an invalid record."));
            return -1;
        }
        total += info.size();
    }
    return total;
}

QByteArray QSocToolResultStore::encode(const Reference &reference, const QByteArray &data) const
{
    const json header
        = {{"version", 1},
           {"capture_state", "complete"},
           {"owner", owner_.toStdString()},
           {"artifact_id", reference.id.toStdString()},
           {"sha256", reference.sha256.toStdString()},
           {"captured_bytes", reference.capturedBytes},
           {"origin", reference.origin.toStdString()},
           {"completion", reference.completion.toStdString()},
           {"source_completeness", reference.sourceCompleteness.toStdString()}};
    return QByteArray::fromStdString(header.dump()) + '\n' + data;
}

bool QSocToolResultStore::write(
    const Reference &reference, const QByteArray &payload, QString *error, const Guard &guard)
{
    if (!isBound() || QFileInfo::exists(path(reference.id)) || !allowed(guard)) {
        fail(
            error,
            QStringLiteral("Artifact publication was cancelled or its target is unavailable."));
        return false;
    }
    QSaveFile file(path(reference.id));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly)) {
        fail(error, QStringLiteral("Artifact storage could not create a temporary record."));
        return false;
    }
    for (qint64 offset = 0; offset < payload.size(); offset += 32768) {
        const qint64 count = std::min(qint64(32768), payload.size() - offset);
        if (!allowed(guard) || file.write(payload.constData() + offset, count) != count) {
            file.cancelWriting();
            fail(error, QStringLiteral("Artifact write failed or was cancelled."));
            return false;
        }
    }
    if (!allowed(guard) || !isBound() || QFileInfo::exists(path(reference.id)) || !file.commit()) {
        fail(error, QStringLiteral("Artifact publication failed or was cancelled."));
        return false;
    }
    return true;
}

std::optional<QSocToolResultStore::Reference> QSocToolResultStore::publish(
    const QString &text,
    const QString &completion,
    const QString &sourceCompleteness,
    QString       *error,
    const Guard   &guard)
{
    QMutexLocker lock(&mutex_);
    if (error)
        error->clear();
    if (!isBound()) {
        fail(error, QStringLiteral("Artifact storage binding is unavailable."));
        return std::nullopt;
    }
    if (text.size() > limits_.artifactBytes) {
        fail(
            error,
            QStringLiteral("Captured text exceeds the artifact quota; no artifact was saved."));
        return std::nullopt;
    }
    if (!QStringList{"ok", "failed", "uncertain", "dispatched"}.contains(completion)
        || !QStringList{"unknown", "truncated", "complete"}.contains(sourceCompleteness)) {
        fail(error, QStringLiteral("Artifact completion metadata is invalid."));
        return std::nullopt;
    }
    const auto data = text.toUtf8();
    if (data.size() > limits_.artifactBytes) {
        fail(
            error,
            QStringLiteral("Captured text exceeds the artifact quota; no artifact was saved."));
        return std::nullopt;
    }
    QLockFile writer(QDir(directory_).filePath(QStringLiteral(".write.lock")));
    if (!writer.tryLock(0)) {
        fail(error, QStringLiteral("Artifact storage is busy."));
        return std::nullopt;
    }
    Reference reference;
    reference.id                 = QUuid::createUuid().toString(QUuid::Id128);
    reference.sha256             = digest(data);
    reference.capturedBytes      = data.size();
    reference.origin             = owner_;
    reference.completion         = completion;
    reference.sourceCompleteness = sourceCompleteness;
    const auto   payload         = encode(reference, data);
    const qint64 used            = storedBytes(error);
    if (used < 0)
        return std::nullopt;
    if (reservedBytes_ > limits_.sessionBytes || used > limits_.sessionBytes - reservedBytes_
        || payload.size() > limits_.sessionBytes - reservedBytes_ - used) {
        fail(error, QStringLiteral("Session artifact quota is exhausted; no artifact was saved."));
        return std::nullopt;
    }
    reservedBytes_ += payload.size();
    const auto release = qScopeGuard([&] { reservedBytes_ -= payload.size(); });
    if (!write(reference, payload, error, guard))
        return std::nullopt;
    return reference;
}

std::optional<QSocToolResultStore::Record> QSocToolResultStore::load(
    const QString &id, QString *error) const
{
    if (!validId(id) || !isBound()) {
        fail(error, QStringLiteral("Artifact is not authorized in this session."));
        return std::nullopt;
    }
    const QFileInfo info(path(id));
    if (!info.isFile() || info.isSymLink()) {
        fail(error, QStringLiteral("Artifact is missing or not authorized in this session."));
        return std::nullopt;
    }
    QFile file(path(id));
    if (!file.open(QIODevice::ReadOnly)) {
        fail(error, QStringLiteral("Artifact could not be read."));
        return std::nullopt;
    }
    const QByteArray line = file.readLine(headerLimit + 1);
    if (line.size() > headerLimit || !line.endsWith('\n')) {
        fail(error, QStringLiteral("Artifact header is corrupt."));
        return std::nullopt;
    }
    Record record;
    try {
        const auto header = json::parse(line.toStdString());
        if (header.at("version") != 1 || header.at("capture_state") != "complete"
            || header.at("owner") != owner_.toStdString()
            || header.at("artifact_id") != id.toStdString()) {
            fail(error, QStringLiteral("Artifact is not authorized in this session."));
            return std::nullopt;
        }
        record.reference.id     = id;
        record.reference.sha256 = QString::fromStdString(header.at("sha256").get<std::string>());
        record.reference.capturedBytes = header.at("captured_bytes").get<qint64>();
        record.reference.origin = QString::fromStdString(header.at("origin").get<std::string>());
        record.reference.completion = QString::fromStdString(
            header.at("completion").get<std::string>());
        record.reference.sourceCompleteness = QString::fromStdString(
            header.at("source_completeness").get<std::string>());
    } catch (const json::exception &) {
        fail(error, QStringLiteral("Artifact header is corrupt."));
        return std::nullopt;
    }
    const qint64 length = record.reference.capturedBytes;
    if (length < 0 || info.size() - line.size() != length) {
        fail(error, QStringLiteral("Artifact length is invalid."));
        return std::nullopt;
    }
    record.data = file.read(length);
    QStringDecoder decoder(QStringDecoder::Utf8);
    const QString  decoded = decoder(record.data);
    if (record.data.size() != length || digest(record.data) != record.reference.sha256
        || decoder.hasError() || decoded.toUtf8() != record.data) {
        fail(error, QStringLiteral("Artifact content is corrupt."));
        return std::nullopt;
    }
    return record;
}

qint64 QSocToolResultStore::utf8End(const QByteArray &data, qint64 begin, qint64 limit)
{
    qint64 end = begin + std::min(limit, qint64(data.size()) - begin);
    while (end > begin && end < data.size()
           && (static_cast<unsigned char>(data[end]) & 0xc0) == 0x80)
        --end;
    return end;
}

std::optional<QSocToolResultStore::Page> QSocToolResultStore::read(
    const QString &id, qint64 offset, qint64 limit, QString *error) const
{
    QMutexLocker lock(&mutex_);
    if (error)
        error->clear();
    if (offset < 0 || limit < 1) {
        fail(error, QStringLiteral("Artifact offset and page limit are invalid."));
        return std::nullopt;
    }
    QLockFile writer(QDir(directory_).filePath(QStringLiteral(".write.lock")));
    if (!isBound() || !writer.tryLock(0)) {
        fail(error, QStringLiteral("Artifact storage is unavailable or busy."));
        return std::nullopt;
    }
    const auto record = load(id, error);
    if (!record)
        return std::nullopt;
    if (offset > record->data.size()
        || (offset < record->data.size()
            && (static_cast<unsigned char>(record->data[offset]) & 0xc0) == 0x80)) {
        fail(error, QStringLiteral("Artifact offset must be a UTF-8 character boundary."));
        return std::nullopt;
    }
    const qint64 end = utf8End(record->data, offset, std::min(limit, limits_.pageBytes));
    if (end == offset && offset != record->data.size()) {
        fail(error, QStringLiteral("The page limit cannot hold the next UTF-8 character."));
        return std::nullopt;
    }
    return Page{
        record->reference,
        QString::fromUtf8(record->data.mid(offset, end - offset)),
        offset,
        end,
        end == record->data.size()};
}

bool QSocToolResultStore::inherit(
    const QSocToolResultStore &source,
    const QList<Reference>    &references,
    QString                   *error,
    const Guard               &guard)
{
    if (error)
        error->clear();
    if (&source == this || source.directory() == directory_) {
        fail(error, QStringLiteral("Artifact inheritance requires a distinct session."));
        return false;
    }
    QMutexLocker lock(&mutex_);
    if (!isBound() || !source.isBound()) {
        fail(error, QStringLiteral("Artifact storage binding is unavailable."));
        return false;
    }
    QLockFile writer(QDir(directory_).filePath(QStringLiteral(".write.lock")));
    if (!writer.tryLock(0)) {
        fail(error, QStringLiteral("Artifact storage is busy."));
        return false;
    }
    QList<Record>          records;
    QMap<QString, QString> seen;
    qint64                 bytes = 0;
    for (const auto &reference : references) {
        if (seen.contains(reference.id)) {
            if (seen.value(reference.id) != reference.sha256) {
                fail(error, QStringLiteral("Inherited artifact versions conflict."));
                return false;
            }
            continue;
        }
        seen.insert(reference.id, reference.sha256);
        const auto record = source.load(reference.id, error);
        if (!record || record->reference.sha256 != reference.sha256
            || record->reference.capturedBytes != reference.capturedBytes) {
            fail(
                error,
                QStringLiteral("Inherited artifact is missing or has a different content version."));
            return false;
        }
        if (QFileInfo::exists(path(reference.id))) {
            const auto existing = load(reference.id, error);
            if (!existing || existing->reference.sha256 != reference.sha256) {
                fail(error, QStringLiteral("Inherited artifact conflicts with the session record."));
                return false;
            }
            continue;
        }
        const auto size = encode(record->reference, record->data).size();
        if (size > limits_.sessionBytes - bytes) {
            fail(error, QStringLiteral("Inherited artifacts exceed the session quota."));
            return false;
        }
        bytes += size;
        records.append(*record);
    }
    const qint64 used = storedBytes(error);
    if (used < 0 || reservedBytes_ > limits_.sessionBytes
        || used > limits_.sessionBytes - reservedBytes_
        || bytes > limits_.sessionBytes - reservedBytes_ - used) {
        fail(error, QStringLiteral("Inherited artifacts exceed the remaining session quota."));
        return false;
    }
    reservedBytes_ += bytes;
    const auto  release = qScopeGuard([&] { reservedBytes_ -= bytes; });
    QStringList published;
    bool        committed = false;
    const auto  rollback  = qScopeGuard([&] {
        if (!committed)
            for (const auto &id : published)
                QFile::remove(path(id));
    });
    for (const auto &record : records) {
        if (!write(record.reference, encode(record.reference, record.data), error, guard))
            return false;
        published.append(record.reference.id);
    }
    committed = true;
    return true;
}

bool QSocToolResultStore::discardPublished(const Reference &reference, QString *error)
{
    QMutexLocker lock(&mutex_);
    if (error)
        error->clear();
    QLockFile writer(QDir(directory_).filePath(QStringLiteral(".write.lock")));
    if (!isBound() || !writer.tryLock(0)) {
        fail(error, QStringLiteral("Artifact storage is unavailable or busy."));
        return false;
    }
    const auto record = load(reference.id, error);
    if (!record || record->reference.origin != owner_ || reference.origin != owner_
        || record->reference.sha256 != reference.sha256
        || record->reference.capturedBytes != reference.capturedBytes) {
        fail(error, QStringLiteral("Only this session's matching published record can be discarded."));
        return false;
    }
    if (!QFile::remove(path(reference.id))) {
        fail(error, QStringLiteral("The unpublished artifact could not be removed."));
        return false;
    }
    return true;
}
