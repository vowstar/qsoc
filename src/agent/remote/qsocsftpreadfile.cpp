// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocsftpclient.h"

#include <utility>
#include <QScopedValueRollback>

namespace {
constexpr qint64    kRangeBytes = 1024 * 1024;
constexpr qsizetype kReadFiles  = 8;

bool sameVersion(const LIBSSH2_SFTP_ATTRIBUTES &before, const LIBSSH2_SFTP_ATTRIBUTES &after)
{
    const auto required = LIBSSH2_SFTP_ATTR_SIZE | LIBSSH2_SFTP_ATTR_PERMISSIONS;
    if ((after.flags & required) != required || !LIBSSH2_SFTP_S_ISREG(after.permissions)
        || before.filesize != after.filesize)
        return false;
    if (before.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
        return (after.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) && before.mtime == after.mtime;
    return true;
}
} // namespace

LIBSSH2_SFTP_HANDLE *QSocSftpClient::openReadHandle(const QString &path, QString *errorMessage)
{
    if (!open(errorMessage))
        return nullptr;
    const auto           bytes  = path.toUtf8();
    LIBSSH2_SFTP_HANDLE *handle = nullptr;
    while (!(handle = libssh2_sftp_open(m_sftp, bytes.constData(), LIBSSH2_FXF_READ, 0))) {
        const int code = libssh2_session_last_errno(m_session.rawSession());
        if (code != LIBSSH2_ERROR_EAGAIN) {
            noteTransport(code);
            setError(
                waitFailureText(QStringLiteral("SFTP open for read failed: %1").arg(path)),
                errorMessage);
            return nullptr;
        }
        if (!wait()) {
            setError(
                waitFailureText(QStringLiteral("Timed out opening %1 for read").arg(path)),
                errorMessage);
            return nullptr;
        }
    }
    return handle;
}

bool QSocSftpClient::readAttributes(
    LIBSSH2_SFTP_HANDLE *handle, LIBSSH2_SFTP_ATTRIBUTES *attributes, QString *errorMessage)
{
    int code = 0;
    while ((code = libssh2_sftp_fstat(handle, attributes)) == LIBSSH2_ERROR_EAGAIN) {
        if (!wait()) {
            setError(
                waitFailureText(QStringLiteral("SFTP file metadata read timed out")), errorMessage);
            return false;
        }
    }
    if (code == 0)
        return true;
    noteTransport(code);
    setError(waitFailureText(QStringLiteral("SFTP file metadata read failed")), errorMessage);
    return false;
}

std::unique_ptr<QSocSftpClient::ReadFile> QSocSftpClient::openReadFile(
    const QString &path, QString *errorMessage, const std::function<bool()> &cancelled)
{
    OpScope scope(this, m_opBudgetMs);
    QScopedValueRollback<std::function<bool()>>
        cancellation(m_cancelled, cancelled ? cancelled : m_cancelled);
    if (errorMessage)
        errorMessage->clear();
    if (m_readFiles.size() >= kReadFiles) {
        setError(QStringLiteral("SFTP source handle limit reached"), errorMessage);
        return nullptr;
    }
    if (m_session.abortRequested() || (m_cancelled && m_cancelled())) {
        setError(QStringLiteral("SFTP source open cancelled"), errorMessage);
        return nullptr;
    }
    if (m_deadline.hasExpired()) {
        setError(QStringLiteral("SFTP source open deadline expired"), errorMessage);
        return nullptr;
    }
    auto *handle = openReadHandle(path, errorMessage);
    if (!handle)
        return nullptr;
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    const auto              required = LIBSSH2_SFTP_ATTR_SIZE | LIBSSH2_SFTP_ATTR_PERMISSIONS;
    if (!readAttributes(handle, &attributes, errorMessage)
        || (attributes.flags & required) != required
        || !LIBSSH2_SFTP_S_ISREG(attributes.permissions)) {
        if (errorMessage && errorMessage->isEmpty())
            setError(
                QStringLiteral("SFTP source must be a regular file with a known size"),
                errorMessage);
        (void) drainClose(handle);
        return nullptr;
    }
    auto file = std::unique_ptr<ReadFile>(new ReadFile(this, handle, attributes));
    m_readFiles.insert(file.get());
    return file;
}

QSocSftpClient::ReadFile::ReadFile(
    QSocSftpClient *client, LIBSSH2_SFTP_HANDLE *handle, const LIBSSH2_SFTP_ATTRIBUTES &attributes)
    : m_client(client)
    , m_handle(handle)
    , m_size(attributes.filesize)
    , m_attributes(attributes)
{}

QSocSftpClient::ReadFile::~ReadFile()
{
    if (m_client)
        m_client->releaseReadFile(this);
}

void QSocSftpClient::synchronizeGeneration()
{
    if (m_generation == m_session.generation())
        return;
    invalidateReadFiles();
    m_retiredReadFiles.clear();
    m_sftp       = nullptr;
    m_stranded   = false;
    m_generation = m_session.generation();
}

void QSocSftpClient::invalidateReadFiles()
{
    const auto files = std::exchange(m_readFiles, QSet<ReadFile *>{});
    for (auto *file : files) {
        file->m_handle = nullptr;
        file->m_client = nullptr;
    }
}

void QSocSftpClient::releaseReadFile(ReadFile *file)
{
    synchronizeGeneration();
    if (file->m_client != this)
        return;
    m_readFiles.remove(file);
    auto *handle   = std::exchange(file->m_handle, nullptr);
    file->m_client = nullptr;
    if (handle)
        m_retiredReadFiles.append(handle);
    if (!m_opActive && !m_session.inOperation()) {
        OpScope cleanup(this, m_opBudgetMs);
    }
}

void QSocSftpClient::releaseReadFiles(QDeadlineTimer deadline)
{
    const auto files = std::exchange(m_readFiles, QSet<ReadFile *>{});
    for (auto *file : files) {
        auto *handle   = std::exchange(file->m_handle, nullptr);
        file->m_client = nullptr;
        if (handle)
            m_retiredReadFiles.append(handle);
    }
    drainRetiredReadFiles(deadline);
}

void QSocSftpClient::drainRetiredReadFiles(QDeadlineTimer deadline)
{
    if (!m_sftp || !m_session.isConnected() || m_stranded)
        return;
    while (!m_retiredReadFiles.isEmpty() && m_session.isConnected()) {
        auto *handle = m_retiredReadFiles.takeFirst();
        if (drainCloseWithin(handle, deadline) == CloseResult::Pending) {
            m_stranded = true;
            return;
        }
    }
}

QSocSftpClient::RangeResult QSocSftpClient::ReadFile::readAt(
    quint64 offset, qint64 maxBytes, QString *errorMessage, const std::function<bool()> &cancelled)
{
    RangeResult result;
    if (errorMessage)
        errorMessage->clear();
    if (!m_client) {
        if (errorMessage)
            *errorMessage = QStringLiteral("source_expired: SFTP source handle is closed");
        return result;
    }
    auto   *client = m_client;
    OpScope scope(client, client->m_opBudgetMs);
    if (client->m_reentered) {
        client->setError(QStringLiteral("SFTP source is busy with another operation"), errorMessage);
        return result;
    }
    if (!client->rebuildSubsystem() || !client->m_readFiles.contains(this)) {
        client->setError(
            QStringLiteral("source_expired: SFTP source subsystem was replaced"), errorMessage);
        return result;
    }
    QScopedValueRollback<std::function<bool()>>
               cancellation(client->m_cancelled, cancelled ? cancelled : client->m_cancelled);
    const auto interrupted = [&] {
        if (client->m_session.abortRequested() || (client->m_cancelled && client->m_cancelled()))
            result.end = ReadEnd::Cancelled;
        else if (client->m_deadline.hasExpired())
            result.end = ReadEnd::TimedOut;
        else
            return false;
        client
            ->setError(QStringLiteral("SFTP range read cancelled or deadline expired"), errorMessage);
        return true;
    };
    const auto failure = [&] {
        result.data.clear();
        client->releaseReadFile(this);
        return result;
    };
    if (interrupted())
        return failure();
    if (!client->m_session.isConnected()) {
        client->setError(
            QStringLiteral("source_expired: SFTP transport is no longer connected"), errorMessage);
        return failure();
    }
    if (maxBytes <= 0 || maxBytes > kRangeBytes) {
        client->setError(
            QStringLiteral("SFTP range length must be from 1 byte to 1 MiB"), errorMessage);
        return result;
    }
    const auto verify = [&] {
        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        if (!client->readAttributes(m_handle, &attributes, errorMessage)) {
            (void) interrupted();
            return false;
        }
        if (!sameVersion(m_attributes, attributes)) {
            client->setError(
                QStringLiteral("source_changed: SFTP source size or modification time changed"),
                errorMessage);
            return false;
        }
        return true;
    };
    if (!verify())
        return failure();
    if (offset >= m_size) {
        result.end = ReadEnd::Eof;
        return result;
    }
    const qint64 wanted = static_cast<qint64>(qMin<quint64>(maxBytes, m_size - offset));
    result.data.resize(wanted);
    if (libssh2_sftp_tell64(m_handle) != offset)
        libssh2_sftp_seek64(m_handle, offset);
    qint64 received = 0;
    bool   pending  = false;
    if (client->m_dataPhaseObserver)
        client->m_dataPhaseObserver();
    while (received < wanted) {
        if (interrupted()) {
            if (pending)
                client->m_stranded = true;
            return failure();
        }
        const auto count = libssh2_sftp_read(
            m_handle, result.data.data() + received, qMin<qint64>(16384, wanted - received));
        pending = count == LIBSSH2_ERROR_EAGAIN;
        if (count > 0) {
            received += count;
            continue;
        }
        if (pending && client->wait())
            continue;
        if (!pending)
            client->noteTransport(static_cast<int>(count));
        client->setError(
            client->waitFailureText(QStringLiteral("SFTP range could not be read completely")),
            errorMessage);
        (void) interrupted();
        return failure();
    }
    if (!verify() || interrupted())
        return failure();
    result.end = offset + received == m_size ? ReadEnd::Eof : ReadEnd::Stopped;
    return result;
}
