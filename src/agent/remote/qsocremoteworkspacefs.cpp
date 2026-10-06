// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocremoteworkspacefs.h"

#include "agent/qsocprojectrules.h"
#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocsftpclient.h"

namespace {

constexpr int kListLimit = 1000;

void setError(QString *sink, const QString &text)
{
    if (sink != nullptr) {
        *sink = text;
    }
}

} // namespace

QSocRemoteWorkspaceFs::QSocRemoteWorkspaceFs(QSocRemoteConnection *conn)
    : m_conn(conn)
{}

QString QSocRemoteWorkspaceFs::root() const
{
    return m_conn->workspace();
}

QString QSocRemoteWorkspaceFs::absolute(const QString &path, QString *error) const
{
    if (!m_conn->isUsable() || m_conn->sftp() == nullptr || root().isEmpty()) {
        setError(error, m_conn->unusableText());
        return {};
    }
    if (!isContainedRelative(path)) {
        setError(error, QStringLiteral("path leaves the workspace: %1").arg(path));
        return {};
    }
    return displayPath(path);
}

QSocWorkspaceFs::Result QSocRemoteWorkspaceFs::resolveInside(
    const QString &path, QString *target, QString *error) const
{
    using Presence    = QSocSftpClient::Presence;
    const QString abs = absolute(path, error);
    if (abs.isEmpty()) {
        return Result::Failed;
    }
    QSocSftpClient *sftp = m_conn->sftp();
    QString         canonicalRoot;
    if (sftp->realPath(root(), &canonicalRoot, error) != Presence::Present) {
        setError(error, QStringLiteral("the workspace root %1 cannot be resolved").arg(root()));
        return Result::Failed;
    }
    switch (sftp->realPath(abs, target, error)) {
    case Presence::Absent:
        if (sftp->linkPresence(abs, error) == Presence::Absent) {
            return Result::Absent;
        }
        setError(error, QStringLiteral("%1 is a link to a missing entry").arg(abs));
        return Result::Failed;
    case Presence::Unknown:
        return Result::Failed;
    case Presence::Present:
        break;
    }
    if (!QSocProjectRules::contains(canonicalRoot, *target)) {
        setError(error, QStringLiteral("%1 resolves outside the workspace").arg(abs));
        return Result::Failed;
    }
    return Result::Ok;
}

QSocWorkspaceFs::Result QSocRemoteWorkspaceFs::read(
    const QString &path, QByteArray *bytes, QString *error)
{
    const QString abs = absolute(path, error);
    if (abs.isEmpty()) {
        return Result::Failed;
    }
    const auto got = QSocProjectRules::readRemote(m_conn->sftp(), root(), abs);
    switch (got.status) {
    case QSocProjectRules::Status::Absent:
        return Result::Absent;
    case QSocProjectRules::Status::Loaded:
        *bytes = got.bytes;
        return Result::Ok;
    default:
        setError(
            error,
            QStringLiteral("%1 was not read: %2").arg(abs, QSocProjectRules::reason(got.status)));
        return Result::Failed;
    }
}

QSocWorkspaceFs::Result QSocRemoteWorkspaceFs::list(
    const QString &path, QList<Entry> *entries, QString *error)
{
    QString      dir;
    const Result where = resolveInside(path, &dir, error);
    if (where != Result::Ok) {
        return where;
    }
    QString    err;
    const auto listed = m_conn->sftp()->listDir(dir, kListLimit, &err);
    if (listed.isEmpty() && !err.isEmpty()) {
        setError(error, err);
        return Result::Failed;
    }
    entries->clear();
    for (const auto &item : listed) {
        if (item.name != QStringLiteral(".") && item.name != QStringLiteral("..")) {
            entries->append({item.name, !item.isDirectory && !item.isSymlink, item.size});
        }
    }
    return Result::Ok;
}

QSocWorkspaceFs::Result QSocRemoteWorkspaceFs::stat(const QString &path, Entry *entry, QString *error)
{
    QString      target;
    const Result where = resolveInside(path, &target, error);
    if (where != Result::Ok) {
        return where;
    }
    QSocSftpClient::LinkStat info;
    if (m_conn->sftp()->linkStat(target, &info, error) != QSocSftpClient::Presence::Present) {
        return Result::Failed;
    }
    *entry = {path.section(QLatin1Char('/'), -1), info.regular, info.size};
    return Result::Ok;
}

bool QSocRemoteWorkspaceFs::write(const QString &path, const QByteArray &bytes, QString *error)
{
    const QString abs = absolute(path, error);
    QString       canonical;
    return !abs.isEmpty() && m_conn->resolveWritablePath(abs, &canonical, error)
           && m_conn->sftp()->writeFile(canonical, bytes, error);
}
