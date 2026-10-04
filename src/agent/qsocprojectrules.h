// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPROJECTRULES_H
#define QSOCPROJECTRULES_H

#include <QByteArray>
#include <QList>
#include <QString>
#include <QStringList>

#include <cstdint>

class QSocSftpClient;

/**
 * @brief Project instruction files (AGENTS.md, AGENTS.local.md, remote
 *        agent definitions) and the rule every reader applies to them.
 * @details A file is loaded only when it is a regular file, its resolved path
 *          stays inside the given root, and it is at most kMaxBytes. Any
 *          other file is reported with its reason, never loaded and never
 *          silently dropped.
 */
namespace QSocProjectRules {

/** @brief Size limit per file. */
constexpr qint64 kMaxBytes = qint64{256} * 1024;

/** @brief Outcome of reading one instruction file. */
enum class Status : std::uint8_t {
    Absent,     /**< No such name in the root. */
    Loaded,     /**< The content is usable. */
    Outside,    /**< A symbolic link that resolves outside the root. */
    BrokenLink, /**< A symbolic link whose target is missing. */
    NotRegular, /**< A directory, device, pipe or socket. */
    TooLarge,   /**< Larger than kMaxBytes. */
    Unreadable, /**< No usable answer from the file system. */
};

/** @brief One instruction file as read. */
struct Read
{
    QString    name;
    Status     status = Status::Absent;
    QByteArray bytes; /**< Content when Loaded. */
};

/** @brief Instruction file names, in prompt order. */
QStringList fileNames();

/** @brief Whether canonical @p path is canonical @p root or below it. */
bool contains(const QString &root, const QString &path);

/** @brief Why a file with @p status was not loaded, as a sentence fragment. */
QString reason(Status status);

/** @brief Read @p name under the local directory @p root. */
Read readLocal(const QString &root, const QString &name);

/** @brief Read the remote @p path, which must resolve inside remote @p root. */
Read readRemote(QSocSftpClient *sftp, const QString &root, const QString &path);

/** @brief Prompt text for @p reads; every refusal becomes a notice. */
QString render(const QList<Read> &reads);

/** @brief Every instruction file under the local @p root, rendered. */
QString loadLocal(const QString &root);

/** @brief Every instruction file under the remote @p root, rendered. */
QString loadRemote(QSocSftpClient *sftp, const QString &root);

} // namespace QSocProjectRules

#endif // QSOCPROJECTRULES_H
