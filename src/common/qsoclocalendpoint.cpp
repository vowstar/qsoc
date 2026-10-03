// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoclocalendpoint.h"

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifdef Q_OS_UNIX
#include <cerrno>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#endif

QString QSocLocalEndpoint::resolve(const QString &path)
{
#ifdef Q_OS_UNIX
    const QString absolute = QFileInfo(path).absoluteFilePath();
    if (QFile::encodeName(absolute).size()
        >= static_cast<qsizetype>(sizeof(sockaddr_un::sun_path))) {
        const auto digest = QCryptographicHash::hash(absolute.toUtf8(), QCryptographicHash::Sha256);
        return QStringLiteral("/tmp/qsoc-%1/%2.sock")
            .arg(static_cast<qulonglong>(::geteuid()))
            .arg(QString::fromLatin1(digest.toHex()));
    }
    return absolute;
#else
    return path;
#endif
}

bool QSocLocalEndpoint::prepareDirectory(const QString &path, QString *error)
{
    const QString directory = QFileInfo(path).absolutePath();
#ifdef Q_OS_UNIX
    const QByteArray encoded = QFile::encodeName(directory);
    if (::mkdir(encoded.constData(), 0700) != 0 && errno != EEXIST) {
        *error = QStringLiteral("could not create private socket directory %1").arg(directory);
        return false;
    }
    struct stat status = {};
    if (::lstat(encoded.constData(), &status) != 0 || !S_ISDIR(status.st_mode)
        || status.st_uid != ::geteuid() || (status.st_mode & 0077) != 0) {
        *error = QStringLiteral("socket directory must be private and owned by this user: %1")
                     .arg(directory);
        return false;
    }
#else
    if (!QDir().mkpath(directory)) {
        *error = QStringLiteral("could not create socket directory %1").arg(directory);
        return false;
    }
#endif
    return true;
}
