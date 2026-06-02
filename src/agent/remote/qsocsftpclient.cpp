// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsftpclient.h"

#include "agent/remote/qsocsshsession.h"

#include <QFileInfo>

namespace {

constexpr int kWaitMs = 200;

QString makeTempPath(const QString &finalPath)
{
    /* Same dir as the final path; dot-prefix hides it in most ls output. */
    const QFileInfo info(finalPath);
    return info.absolutePath() + QStringLiteral("/.qsoc-write-")
           + QString::number(QDateTime::currentMSecsSinceEpoch()) + QStringLiteral("-")
           + info.fileName();
}

} // namespace

QSocSftpClient::QSocSftpClient(QSocSshSession &session)
    : m_session(session)
{}

QSocSftpClient::~QSocSftpClient()
{
    close();
}

void QSocSftpClient::setError(const QString &msg, QString *sink)
{
    m_lastError = msg;
    if (sink != nullptr) {
        *sink = msg;
    }
}

bool QSocSftpClient::waitReady()
{
    return QSocSshSession::waitSocket(m_session.socketFd(), m_session.rawSession(), kWaitMs) >= 0;
}

void QSocSftpClient::drainClose(LIBSSH2_SFTP_HANDLE *handle)
{
    if (handle == nullptr) {
        return;
    }
    while (libssh2_sftp_close(handle) == LIBSSH2_ERROR_EAGAIN) {
        if (!waitReady()) {
            break;
        }
    }
}

void QSocSftpClient::drainUnlink(const QByteArray &path)
{
    if (m_sftp == nullptr) {
        return;
    }
    while (libssh2_sftp_unlink(m_sftp, path.constData()) == LIBSSH2_ERROR_EAGAIN) {
        if (!waitReady()) {
            break;
        }
    }
}

bool QSocSftpClient::open(QString *errorMessage)
{
    if (m_sftp != nullptr) {
        return true;
    }
    LIBSSH2_SESSION *session = m_session.rawSession();
    if (session == nullptr) {
        setError(QStringLiteral("SSH session is not connected"), errorMessage);
        return false;
    }
    while ((m_sftp = libssh2_sftp_init(session)) == nullptr) {
        const int err = libssh2_session_last_errno(session);
        if (err != LIBSSH2_ERROR_EAGAIN) {
            setError(QStringLiteral("Failed to open SFTP subsystem"), errorMessage);
            return false;
        }
        if (!waitReady()) {
            setError(QStringLiteral("Timed out opening SFTP subsystem"), errorMessage);
            return false;
        }
    }
    return true;
}

void QSocSftpClient::close()
{
    if (m_sftp != nullptr) {
        while (libssh2_sftp_shutdown(m_sftp) == LIBSSH2_ERROR_EAGAIN) {
            if (!waitReady()) {
                break;
            }
        }
        m_sftp = nullptr;
    }
}

QByteArray QSocSftpClient::readFile(const QString &path, qint64 maxBytes, QString *errorMessage)
{
    if (!open(errorMessage)) {
        return {};
    }
    const QByteArray pathBytes = path.toUtf8();

    LIBSSH2_SFTP_HANDLE *handle = nullptr;
    while ((handle = libssh2_sftp_open(m_sftp, pathBytes.constData(), LIBSSH2_FXF_READ, 0))
           == nullptr) {
        if (libssh2_session_last_errno(m_session.rawSession()) != LIBSSH2_ERROR_EAGAIN) {
            setError(QStringLiteral("SFTP open for read failed: %1").arg(path), errorMessage);
            return {};
        }
        if (!waitReady()) {
            setError(QStringLiteral("Timed out opening %1 for read").arg(path), errorMessage);
            return {};
        }
    }

    QByteArray out;
    char       buffer[16384];
    while (true) {
        const ssize_t nread = libssh2_sftp_read(handle, buffer, sizeof(buffer));
        if (nread > 0) {
            out.append(buffer, static_cast<int>(nread));
            if (maxBytes > 0 && out.size() >= maxBytes) {
                out.truncate(static_cast<int>(maxBytes));
                break;
            }
            continue;
        }
        if (nread == 0) {
            break;
        }
        if (nread == LIBSSH2_ERROR_EAGAIN) {
            if (!waitReady()) {
                setError(QStringLiteral("Timed out reading %1").arg(path), errorMessage);
                drainClose(handle);
                return {};
            }
            continue;
        }
        setError(QStringLiteral("SFTP read error on %1").arg(path), errorMessage);
        drainClose(handle);
        return {};
    }
    drainClose(handle);
    return out;
}

bool QSocSftpClient::writeFile(const QString &path, const QByteArray &content, QString *errorMessage)
{
    if (!open(errorMessage)) {
        return false;
    }

    const QFileInfo finalInfo(path);
    if (!mkdirP(finalInfo.absolutePath(), errorMessage)) {
        return false;
    }

    const QString       tempPath      = makeTempPath(path);
    const QByteArray    tempPathBytes = tempPath.toUtf8();
    const unsigned long mode          = 0644;
    const int           flags         = LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC;

    LIBSSH2_SFTP_HANDLE *handle = nullptr;
    while ((handle = libssh2_sftp_open(m_sftp, tempPathBytes.constData(), flags, mode)) == nullptr) {
        if (libssh2_session_last_errno(m_session.rawSession()) != LIBSSH2_ERROR_EAGAIN) {
            setError(QStringLiteral("SFTP open for write failed: %1").arg(tempPath), errorMessage);
            return false;
        }
        if (!waitReady()) {
            setError(QStringLiteral("Timed out opening %1 for write").arg(tempPath), errorMessage);
            return false;
        }
    }

    qint64 offset = 0;
    while (offset < content.size()) {
        const ssize_t written = libssh2_sftp_write(
            handle, content.constData() + offset, static_cast<size_t>(content.size() - offset));
        if (written > 0) {
            offset += written;
            continue;
        }
        if (written == LIBSSH2_ERROR_EAGAIN) {
            if (!waitReady()) {
                setError(QStringLiteral("Timed out writing %1").arg(tempPath), errorMessage);
                drainClose(handle);
                drainUnlink(tempPathBytes);
                return false;
            }
            continue;
        }
        setError(QStringLiteral("SFTP write error on %1").arg(tempPath), errorMessage);
        drainClose(handle);
        drainUnlink(tempPathBytes);
        return false;
    }
    /* Drain the close: it flushes buffered writes and releases the server
     * handle. In non-blocking mode an ignored EAGAIN here can leave the
     * temp truncated or still-open when the rename below runs. */
    while (libssh2_sftp_close(handle) == LIBSSH2_ERROR_EAGAIN) {
        if (!waitReady()) {
            setError(QStringLiteral("Timed out closing %1").arg(tempPath), errorMessage);
            drainUnlink(tempPathBytes);
            return false;
        }
    }

    /* Replace the target with the temp file. The session is non-blocking,
     * so the destination unlink MUST be driven to completion: a
     * half-issued unlink (EAGAIN ignored) leaves the target in place and
     * SSH_FXP_RENAME then fails on servers that refuse to overwrite on
     * rename (standard SFTP v3 / OpenSSH). This is why editing an
     * existing file failed intermittently while writing a new file did
     * not. */
    const QByteArray finalBytes = path.toUtf8();
    int              urc        = 0;
    while ((urc = libssh2_sftp_unlink(m_sftp, finalBytes.constData())) == LIBSSH2_ERROR_EAGAIN) {
        if (!waitReady()) {
            setError(QStringLiteral("Timed out clearing %1 before rename").arg(path), errorMessage);
            drainUnlink(tempPathBytes);
            return false;
        }
    }
    /* urc == 0 (removed) or a NO_SUCH_FILE failure (target was absent)
     * are both fine; the rename below reports anything that still bites. */

    int rc = 0;
    while ((rc = libssh2_sftp_rename(m_sftp, tempPathBytes.constData(), finalBytes.constData()))
           == LIBSSH2_ERROR_EAGAIN) {
        if (!waitReady()) {
            setError(QStringLiteral("Timed out renaming temp file for %1").arg(path), errorMessage);
            drainUnlink(tempPathBytes);
            return false;
        }
    }
    if (rc != 0) {
        const unsigned long sftpErr = libssh2_sftp_last_error(m_sftp);
        setError(
            QStringLiteral("SFTP rename failed for %1 (sftp error %2)").arg(path).arg(sftpErr),
            errorMessage);
        drainUnlink(tempPathBytes);
        return false;
    }
    return true;
}

bool QSocSftpClient::mkdirP(const QString &path, QString *errorMessage)
{
    if (!open(errorMessage)) {
        return false;
    }
    if (path.isEmpty() || path == QStringLiteral("/")) {
        return true;
    }

    /* Walk ancestors, mkdir each rung. */
    QStringList parts = path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    QString     cumulative;
    for (const QString &seg : parts) {
        cumulative += QLatin1Char('/');
        cumulative += seg;
        const QByteArray bytes = cumulative.toUtf8();
        /* EEXIST is fine; success is also fine. */
        int rc = 0;
        while ((rc = libssh2_sftp_mkdir(m_sftp, bytes.constData(), 0755)) == LIBSSH2_ERROR_EAGAIN) {
            if (!waitReady()) {
                setError(QStringLiteral("Timed out during mkdir %1").arg(cumulative), errorMessage);
                return false;
            }
        }
        if (rc != 0) {
            const unsigned long sftpErr = libssh2_sftp_last_error(m_sftp);
            if (sftpErr != LIBSSH2_FX_FILE_ALREADY_EXISTS
                && sftpErr != LIBSSH2_FX_FAILURE /* many servers map EEXIST to FAILURE */) {
                setError(
                    QStringLiteral("mkdir failed at %1 (sftp err %2)").arg(cumulative).arg(sftpErr),
                    errorMessage);
                return false;
            }
        }
    }
    return true;
}

bool QSocSftpClient::rename(const QString &oldPath, const QString &newPath, QString *errorMessage)
{
    if (!open(errorMessage)) {
        return false;
    }
    const QByteArray from = oldPath.toUtf8();
    const QByteArray to   = newPath.toUtf8();
    int              rc   = 0;
    while ((rc = libssh2_sftp_rename(m_sftp, from.constData(), to.constData()))
           == LIBSSH2_ERROR_EAGAIN) {
        if (!waitReady()) {
            setError(QStringLiteral("Timed out renaming %1").arg(oldPath), errorMessage);
            return false;
        }
    }
    if (rc != 0) {
        setError(QStringLiteral("SFTP rename failed: %1 -> %2").arg(oldPath, newPath), errorMessage);
        return false;
    }
    return true;
}

QList<QSocSftpClient::Entry> QSocSftpClient::listDir(
    const QString &path, int limit, QString *errorMessage)
{
    QList<Entry> entries;
    if (!open(errorMessage)) {
        return entries;
    }
    const QByteArray     pathBytes = path.toUtf8();
    LIBSSH2_SFTP_HANDLE *handle    = nullptr;
    while ((handle = libssh2_sftp_opendir(m_sftp, pathBytes.constData())) == nullptr) {
        if (libssh2_session_last_errno(m_session.rawSession()) != LIBSSH2_ERROR_EAGAIN) {
            setError(QStringLiteral("SFTP opendir failed: %1").arg(path), errorMessage);
            return entries;
        }
        if (!waitReady()) {
            setError(QStringLiteral("Timed out opening directory %1").arg(path), errorMessage);
            return entries;
        }
    }

    char                    name[1024];
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    while (true) {
        const int rc = libssh2_sftp_readdir(handle, name, sizeof(name), &attrs);
        if (rc > 0) {
            const QString entryName = QString::fromUtf8(name, rc);
            if (entryName == QStringLiteral(".") || entryName == QStringLiteral("..")) {
                continue;
            }
            Entry entry;
            entry.name = entryName;
            if ((attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) != 0) {
                entry.size = static_cast<qint64>(attrs.filesize);
            }
            if ((attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) != 0) {
                entry.isDirectory = (attrs.permissions & LIBSSH2_SFTP_S_IFDIR) != 0;
                entry.isSymlink   = (attrs.permissions & LIBSSH2_SFTP_S_IFLNK) != 0;
            }
            entries.push_back(entry);
            if (limit > 0 && entries.size() >= limit) {
                break;
            }
            continue;
        }
        if (rc == LIBSSH2_ERROR_EAGAIN) {
            if (!waitReady()) {
                break;
            }
            continue;
        }
        break;
    }
    while (libssh2_sftp_closedir(handle) == LIBSSH2_ERROR_EAGAIN) {
        if (!waitReady()) {
            break;
        }
    }
    return entries;
}

bool QSocSftpClient::exists(const QString &path, QString *errorMessage)
{
    if (!open(errorMessage)) {
        return false;
    }
    const QByteArray        pathBytes = path.toUtf8();
    LIBSSH2_SFTP_ATTRIBUTES attrs;
    int                     rc = 0;
    while ((rc = libssh2_sftp_stat(m_sftp, pathBytes.constData(), &attrs)) == LIBSSH2_ERROR_EAGAIN) {
        if (!waitReady()) {
            setError(QStringLiteral("Timed out statting %1").arg(path), errorMessage);
            return false;
        }
    }
    return rc == 0;
}
