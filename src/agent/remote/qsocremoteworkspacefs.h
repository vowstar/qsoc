// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCREMOTEWORKSPACEFS_H
#define QSOCREMOTEWORKSPACEFS_H

#include "agent/qsocworkspacefs.h"

class QSocRemoteConnection;

/**
 * @brief The bound remote workspace over SFTP.
 * @details Reads, listings and stats are answered only for entries whose
 *          host-resolved path stays inside the workspace root, so a link in
 *          the tree cannot reach another file. Reads follow the size limit
 *          of project instruction files. Writes pass the same writable
 *          directory and workspace identity check as `write_file`.
 */
class QSocRemoteWorkspaceFs : public QSocWorkspaceFs
{
public:
    explicit QSocRemoteWorkspaceFs(QSocRemoteConnection *conn);

    QString root() const override;
    Result  read(const QString &path, QByteArray *bytes, QString *error) override;
    Result  list(const QString &path, QList<Entry> *entries, QString *error) override;
    Result  stat(const QString &path, Entry *entry, QString *error) override;
    bool    write(const QString &path, const QByteArray &bytes, QString *error) override;

private:
    /* The absolute remote path, or empty with @p error set. */
    QString absolute(const QString &path, QString *error) const;

    /* Resolve @p path on the host and prove it stays inside the root. */
    Result resolveInside(const QString &path, QString *target, QString *error) const;

    QSocRemoteConnection *m_conn = nullptr;
};

#endif // QSOCREMOTEWORKSPACEFS_H
