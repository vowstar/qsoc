// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCINPUTSNAPSHOT_H
#define QSOCINPUTSNAPSHOT_H

#include <QByteArray>
#include <QDeadlineTimer>
#include <QString>
#include <QTemporaryDir>

#include <functional>
#include <memory>

class QSocSftpClient;

class QSocInputSnapshot
{
public:
    using Cancelled = std::function<bool()>;

    static std::unique_ptr<QSocInputSnapshot> fromLocal(
        const QString   &path,
        qint64           maxBytes,
        QString         *error,
        const Cancelled &cancelled = {},
        QDeadlineTimer   deadline  = QDeadlineTimer(30000));
    static std::unique_ptr<QSocInputSnapshot> fromRemote(
        QSocSftpClient  *sftp,
        const QString   &path,
        qint64           maxBytes,
        QString         *error,
        const Cancelled &cancelled = {},
        QDeadlineTimer   deadline  = QDeadlineTimer(30000));

    QString    path() const { return path_; }
    QString    source() const { return source_; }
    QByteArray sha256() const { return sha256_; }
    qint64     size() const { return size_; }

private:
    using Sink     = std::function<bool(const QByteArray &)>;
    using Producer = std::function<bool(const Sink &, QString *)>;
    QSocInputSnapshot();
    static std::unique_ptr<QSocInputSnapshot> copy(
        const QString   &source,
        qint64           maxBytes,
        QString         *error,
        const Cancelled &cancelled,
        QDeadlineTimer   deadline,
        const Producer  &producer);

    QTemporaryDir directory_;
    QString       path_;
    QString       source_;
    QByteArray    sha256_;
    qint64        size_ = 0;
};

#endif // QSOCINPUTSNAPSHOT_H
