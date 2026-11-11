// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocworkspacefs.h"
#include "agent/qsocprojectrules.h"
#include "agent/qsoctodomodel.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>

#include <utility>

QSocWorkspaceFs::QSocWorkspaceFs()  = default;
QSocWorkspaceFs::~QSocWorkspaceFs() = default;

QSocWorkspaceFs::Result QSocWorkspaceFs::readLimited(
    const QString &, qint64, QByteArray *, QString *error)
{
    if (error)
        *error = QStringLiteral("bounded reads are unavailable for this workspace");
    return Result::Failed;
}

QSocWorkspaceFs::Result QSocWorkspaceFs::listLimited(
    const QString &, int, QList<Entry> *entries, bool *truncated, QString *error)
{
    entries->clear();
    *truncated = false;
    if (error)
        *error = QStringLiteral("bounded directory listings are unavailable for this workspace");
    return Result::Failed;
}

QSocTodoModel *QSocWorkspaceFs::todos()
{
    if (!todos_)
        todos_ = std::make_unique<QSocTodoModel>(this);
    return todos_.get();
}

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

QSocWorkspaceFs::Result QSocLocalWorkspaceFs::readLimited(
    const QString &path, qint64 maxBytes, QByteArray *bytes, QString *error)
{
    bytes->clear();
    const QString file = absolute(path, error);
    if (file.isEmpty())
        return Result::Failed;
    const QFileInfo info(file);
    if (!info.exists())
        return Result::Absent;
    const QString canonicalRoot = QFileInfo(root()).canonicalFilePath();
    const QString canonicalFile = info.canonicalFilePath();
    if (maxBytes < 0 || maxBytes > 64 * 1024 * 1024 || !info.isFile() || canonicalRoot.isEmpty()
        || canonicalFile.isEmpty() || !QSocProjectRules::contains(canonicalRoot, canonicalFile)) {
        if (error)
            *error = QStringLiteral("file cannot be read within the workspace limits");
        return Result::Failed;
    }
    QFile handle(file);
    if (!handle.open(QIODevice::ReadOnly)) {
        if (error)
            *error = handle.errorString();
        return Result::Failed;
    }
    *bytes = handle.read(maxBytes + 1);
    if (handle.error() != QFileDevice::NoError || bytes->size() > maxBytes) {
        bytes->clear();
        if (error)
            *error = QStringLiteral("file could not be read completely within the byte limit");
        return Result::Failed;
    }
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
        entries->append(
            {info.fileName(), info.isFile(), info.isFile() ? info.size() : -1, info.isSymLink()});
    }
    return Result::Ok;
}

QSocWorkspaceFs::Result QSocLocalWorkspaceFs::listLimited(
    const QString &path, int maxEntries, QList<Entry> *entries, bool *truncated, QString *error)
{
    entries->clear();
    *truncated              = false;
    const QString directory = absolute(path, error);
    if (directory.isEmpty())
        return Result::Failed;
    const QFileInfo info(directory);
    if (!info.exists())
        return Result::Absent;
    const QString canonicalRoot      = QFileInfo(root()).canonicalFilePath();
    const QString canonicalDirectory = info.canonicalFilePath();
    if (maxEntries < 0 || maxEntries > 10000 || !info.isDir() || !info.isReadable()
        || canonicalRoot.isEmpty() || canonicalDirectory.isEmpty()
        || !QSocProjectRules::contains(canonicalRoot, canonicalDirectory)) {
        if (error)
            *error = QStringLiteral("directory cannot be listed within the workspace limits");
        return Result::Failed;
    }
    QDirIterator it(directory, QDir::AllEntries | QDir::Hidden | QDir::NoDotAndDotDot);
    while (it.hasNext()) {
        if (entries->size() >= maxEntries) {
            *truncated = true;
            break;
        }
        it.next();
        const QFileInfo child = it.fileInfo();
        entries->append(
            {child.fileName(),
             child.isFile(),
             child.isFile() ? child.size() : -1,
             child.isSymLink()});
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
    *entry = {info.fileName(), info.isFile(), info.isFile() ? info.size() : -1, info.isSymLink()};
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
    QSaveFile handle(file);
    if (!handle.open(QIODevice::WriteOnly) || handle.write(bytes) != bytes.size()
        || !handle.commit()) {
        if (error != nullptr) {
            *error = QStringLiteral("cannot write %1: %2").arg(file, handle.errorString());
        }
        return false;
    }
    return true;
}
