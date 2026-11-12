// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocinputsnapshot.h"
#include "agent/remote/qsocsftpclient.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

QSocInputSnapshot::QSocInputSnapshot()
    : directory_(QDir::tempPath() + QStringLiteral("/qsoc-input-XXXXXX"))
{}

std::unique_ptr<QSocInputSnapshot> QSocInputSnapshot::copy(
    const QString   &source,
    qint64           maxBytes,
    QString         *error,
    const Cancelled &cancelled,
    QDeadlineTimer   deadline,
    const Producer  &producer)
{
    QString    failure;
    const auto fail = [&](const QString &message) -> std::unique_ptr<QSocInputSnapshot> {
        if (error)
            *error = message;
        return nullptr;
    };
    const auto interrupted = [&] {
        if (cancelled && cancelled())
            failure = QStringLiteral("Input snapshot cancelled");
        else if (deadline.hasExpired())
            failure = QStringLiteral("Input snapshot deadline expired");
        return !failure.isEmpty();
    };
    if (maxBytes <= 0)
        return fail(QStringLiteral("Input snapshot requires a positive size limit"));
    if (interrupted())
        return fail(failure);
    auto snapshot = std::unique_ptr<QSocInputSnapshot>(new QSocInputSnapshot);
    if (!snapshot->directory_.isValid())
        return fail(QStringLiteral("Cannot create private input directory"));
    snapshot->source_ = source;
    snapshot->path_   = snapshot->directory_.filePath(QStringLiteral("input"));
    QFile output(snapshot->path_);
    if (!output.open(QIODevice::WriteOnly | QIODevice::NewOnly)
        || !output.setPermissions(QFile::ReadOwner | QFile::WriteOwner))
        return fail(QStringLiteral("Cannot create private input snapshot"));
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const bool         complete = producer(
        [&](const QByteArray &chunk) {
            if (interrupted())
                return false;
            if (chunk.size() > maxBytes - snapshot->size_) {
                failure = QStringLiteral("Input exceeds its %1 byte limit").arg(maxBytes);
                return false;
            }
            if (output.write(chunk) != chunk.size()) {
                failure
                    = QStringLiteral("Cannot write input snapshot: %1").arg(output.errorString());
                return false;
            }
            hash.addData(chunk);
            snapshot->size_ += chunk.size();
            return true;
        },
        &failure);
    const bool stopped = interrupted();
    if (!complete || stopped)
        return fail(
            failure.isEmpty() ? QStringLiteral("Input read did not reach end of file") : failure);
    if (!output.flush() || !output.setPermissions(QFile::ReadOwner))
        return fail(QStringLiteral("Cannot seal input snapshot"));
    output.close();
    snapshot->sha256_ = hash.result().toHex();
    return snapshot;
}

std::unique_ptr<QSocInputSnapshot> QSocInputSnapshot::fromLocal(
    const QString   &path,
    qint64           maxBytes,
    QString         *error,
    const Cancelled &cancelled,
    QDeadlineTimer   deadline)
{
    return copy(path, maxBytes, error, cancelled, deadline, [&](const Sink &sink, QString *failure) {
        QFile input(path);
        if (!QFileInfo(path).isFile() || !input.open(QIODevice::ReadOnly)) {
            *failure = QStringLiteral("Cannot read input: %1").arg(path);
            return false;
        }
        while (!input.atEnd()) {
            const auto chunk = input.read(16384);
            if (input.error() != QFile::NoError) {
                *failure = QStringLiteral("Cannot read input: %1").arg(input.errorString());
                return false;
            }
            if (!sink(chunk))
                return false;
        }
        return true;
    });
}

std::unique_ptr<QSocInputSnapshot> QSocInputSnapshot::fromRemote(
    QSocSftpClient  *sftp,
    const QString   &path,
    qint64           maxBytes,
    QString         *error,
    const Cancelled &cancelled,
    QDeadlineTimer   deadline)
{
    return copy(path, maxBytes, error, cancelled, deadline, [&](const Sink &sink, QString *failure) {
        if (!sftp) {
            *failure = QStringLiteral("Remote SFTP input is not connected");
            return false;
        }
        bool eof = false;
        sftp->runWithin(
            deadline,
            [&] {
                QSocSftpClient::LinkStat stat;
                if (sftp->linkStat(path, &stat, failure) != QSocSftpClient::Presence::Present
                    || !stat.regular) {
                    if (failure->isEmpty())
                        *failure
                            = QStringLiteral("Remote input is not a regular file: %1").arg(path);
                    return false;
                }
                if (stat.size > maxBytes) {
                    *failure = QStringLiteral("Input exceeds its %1 byte limit").arg(maxBytes);
                    return false;
                }
                const auto result = sftp->readStreamResult(path, sink, failure, cancelled);
                eof               = result.end == QSocSftpClient::ReadEnd::Eof;
                return eof;
            },
            cancelled);
        return eof;
    });
}
