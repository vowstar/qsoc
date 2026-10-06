// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocworkspacefs.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#include <utility>

bool QSocWorkspaceFs::isContainedRelative(const QString &path)
{
    if (path.isEmpty() || QDir::isAbsolutePath(path) || path.startsWith(QLatin1Char('/'))
        || path.contains(QLatin1Char('\\'))) {
        return false;
    }
    const QStringList parts = path.split(QLatin1Char('/'));
    return !parts.contains(QStringLiteral("..")) && !parts.contains(QString());
}

QString QSocWorkspaceFs::displayPath(const QString &path) const
{
    const QString base = root();
    return base.endsWith(QLatin1Char('/')) ? base + path : base + QLatin1Char('/') + path;
}

QSocLocalWorkspaceFs::QSocLocalWorkspaceFs(std::function<QString()> rootProvider)
    : m_rootProvider(std::move(rootProvider))
{}

QString QSocLocalWorkspaceFs::root() const
{
    return m_rootProvider ? m_rootProvider() : QString();
}

QString QSocLocalWorkspaceFs::absolute(const QString &path, QString *error) const
{
    const QString base = root();
    if (base.isEmpty() || !isContainedRelative(path)) {
        if (error != nullptr) {
            *error = base.isEmpty() ? QStringLiteral("no project directory is open")
                                    : QStringLiteral("path leaves the project: %1").arg(path);
        }
        return {};
    }
    return QDir(base).filePath(path);
}

QSocWorkspaceFs::Result QSocLocalWorkspaceFs::read(
    const QString &path, QByteArray *bytes, QString *error)
{
    const QString file = absolute(path, error);
    if (file.isEmpty()) {
        return Result::Failed;
    }
    if (!QFileInfo::exists(file)) {
        return Result::Absent;
    }
    QFile handle(file);
    if (!handle.open(QIODevice::ReadOnly)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot read %1: %2").arg(file, handle.errorString());
        }
        return Result::Failed;
    }
    *bytes = handle.readAll();
    return Result::Ok;
}

QSocWorkspaceFs::Result QSocLocalWorkspaceFs::list(
    const QString &path, QList<Entry> *entries, QString *error)
{
    const QString dirPath = absolute(path, error);
    if (dirPath.isEmpty()) {
        return Result::Failed;
    }
    const QDir dir(dirPath);
    if (!dir.exists()) {
        return Result::Absent;
    }
    entries->clear();
    const QFileInfoList infos
        = dir.entryInfoList(QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot, QDir::Name);
    for (const QFileInfo &info : infos) {
        entries->append({info.fileName(), info.isFile(), info.isFile() ? info.size() : -1});
    }
    return Result::Ok;
}

QSocWorkspaceFs::Result QSocLocalWorkspaceFs::stat(const QString &path, Entry *entry, QString *error)
{
    const QString file = absolute(path, error);
    if (file.isEmpty()) {
        return Result::Failed;
    }
    const QFileInfo info(file);
    if (!info.exists()) {
        return Result::Absent;
    }
    *entry = {info.fileName(), info.isFile(), info.isFile() ? info.size() : -1};
    return Result::Ok;
}

bool QSocLocalWorkspaceFs::write(const QString &path, const QByteArray &bytes, QString *error)
{
    const QString file = absolute(path, error);
    if (file.isEmpty()) {
        return false;
    }
    const QString parent = QFileInfo(file).absolutePath();
    if (!QDir().mkpath(parent)) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot create %1").arg(parent);
        }
        return false;
    }
    QFile handle(file);
    if (!handle.open(QIODevice::WriteOnly | QIODevice::Truncate)
        || handle.write(bytes) != bytes.size()) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot write %1: %2").arg(file, handle.errorString());
        }
        return false;
    }
    return true;
}
