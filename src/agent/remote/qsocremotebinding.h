// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCREMOTEBINDING_H
#define QSOCREMOTEBINDING_H

#include <QString>

/**
 * @brief Persistent per-project remote target binding.
 * @details Stored at `<project>/.qsoc/remote.yml`:
 *
 *          ```
 *          target: user@host:port
 *          workspace: /home/user/work
 *          ```
 *
 *          Writes go through a temporary file + rename so a crash midway
 *          cannot leave a truncated config. No credentials or private key
 *          paths are persisted here - the target is either a profile name
 *          from the global config or a direct `user@host:port` string.
 */
class QSocRemoteBinding
{
public:
    /** @brief Parsed binding entry; either field may be empty when absent. */
    struct Entry
    {
        QString target;
        QString workspace;
    };

    /** @brief Return `<projectRoot>/.qsoc/remote.yml`. */
    static QString pathFor(const QString &projectRoot);

    /** @brief Read the full binding. Fields are empty when missing. */
    static Entry read(const QString &projectRoot);

    /**
     * @brief Atomically write or update the binding.
     * @details Preserves unrelated sibling keys. Either field may be empty
     *          to drop just that field from the file.
     */
    static bool write(
        const QString &projectRoot, const Entry &entry, QString *errorMessage = nullptr);

    /**
     * @brief Remove both `target` and `root` (delete file if no siblings).
     */
    static bool clear(const QString &projectRoot, QString *errorMessage = nullptr);

private:
    QSocRemoteBinding()  = delete;
    ~QSocRemoteBinding() = delete;
};

#endif // QSOCREMOTEBINDING_H
