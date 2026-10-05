// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocprojectrules.h"

#include "agent/remote/qsocsftpclient.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

namespace QSocProjectRules {

QString reason(Status status)
{
    switch (status) {
    case Status::Outside:
        return QStringLiteral("it is a symbolic link that resolves outside the project");
    case Status::BrokenLink:
        return QStringLiteral("it is a symbolic link to a missing file");
    case Status::NotRegular:
        return QStringLiteral("it is not a regular file");
    case Status::TooLarge:
        return QStringLiteral("it is larger than %1 KiB").arg(kMaxBytes / 1024);
    default:
        return QStringLiteral("it could not be read");
    }
}

namespace {

Read readResolved(const QString &name, const QString &target)
{
    const QFileInfo resolved(target);
    if (!resolved.isFile()) {
        return {name, Status::NotRegular, {}};
    }
    if (resolved.size() > kMaxBytes) {
        return {name, Status::TooLarge, {}};
    }
    QFile file(target);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return {name, Status::Unreadable, {}};
    }
    const QByteArray bytes = file.read(kMaxBytes + 1);
    return {name, bytes.size() > kMaxBytes ? Status::TooLarge : Status::Loaded, bytes};
}

} // namespace

QStringList fileNames()
{
    return {QStringLiteral("AGENTS.md"), QStringLiteral("AGENTS.local.md")};
}

bool contains(const QString &root, const QString &path)
{
    const QString base = root.endsWith(QLatin1Char('/')) ? root : root + QLatin1Char('/');
    return path == root || path.startsWith(base);
}

Read readLocal(const QString &root, const QString &name)
{
    const QFileInfo info(QDir(root).filePath(name));
    if (!info.exists() && !info.isSymLink()) {
        return {name, Status::Absent, {}};
    }
    const QString canonicalRoot = QFileInfo(root).canonicalFilePath();
    const QString target        = info.canonicalFilePath();
    if (target.isEmpty()) {
        return {name, Status::BrokenLink, {}};
    }
    if (canonicalRoot.isEmpty() || !contains(canonicalRoot, target)) {
        return {name, Status::Outside, {}};
    }
    return readResolved(name, target);
}

Read readRemote(QSocSftpClient *sftp, const QString &root, const QString &path)
{
    using Presence     = QSocSftpClient::Presence;
    const QString name = path.section(QLatin1Char('/'), -1);
    const auto    link = sftp->linkPresence(path);
    if (link == Presence::Absent) {
        return {name, Status::Absent, {}};
    }
    QString canonicalRoot;
    QString target;
    if (link == Presence::Unknown || sftp->realPath(root, &canonicalRoot) != Presence::Present) {
        return {name, Status::Unreadable, {}};
    }
    const auto resolved = sftp->realPath(path, &target);
    if (resolved != Presence::Present) {
        return {name, resolved == Presence::Absent ? Status::BrokenLink : Status::Unreadable, {}};
    }
    if (!contains(canonicalRoot, target)) {
        return {name, Status::Outside, {}};
    }
    QSocSftpClient::LinkStat stat;
    const auto               kind = sftp->linkStat(target, &stat);
    if (kind != Presence::Present) {
        return {name, kind == Presence::Absent ? Status::BrokenLink : Status::Unreadable, {}};
    }
    if (!stat.regular) {
        return {name, Status::NotRegular, {}};
    }
    if (stat.size > kMaxBytes) {
        return {name, Status::TooLarge, {}};
    }
    QString          error;
    const QByteArray bytes = sftp->readFile(target, kMaxBytes + 1, &error);
    if (!error.isEmpty()) {
        return {name, Status::Unreadable, {}};
    }
    return {name, bytes.size() > kMaxBytes ? Status::TooLarge : Status::Loaded, bytes};
}

QString render(const QList<Read> &reads)
{
    QStringList parts;
    for (const Read &read : reads) {
        if (read.status == Status::Absent) {
            continue;
        }
        const QString text = read.status == Status::Loaded
                                 ? QString::fromUtf8(read.bytes).trimmed()
                                 : QStringLiteral("Project rules in %1 were not loaded: %2.")
                                       .arg(read.name, reason(read.status));
        if (!text.isEmpty()) {
            parts.append(text);
        }
    }
    return parts.join(QStringLiteral("\n\n"));
}

QString loadLocal(const QString &root)
{
    QList<Read> reads;
    for (const QString &name : fileNames()) {
        reads.append(readLocal(root, name));
    }
    return render(reads);
}

QList<Read> readAllRemote(QSocSftpClient *sftp, const QString &root)
{
    QList<Read> reads;
    for (const QString &name : fileNames()) {
        reads.append(readRemote(sftp, root, root + QLatin1Char('/') + name));
    }
    return reads;
}

} // namespace QSocProjectRules
