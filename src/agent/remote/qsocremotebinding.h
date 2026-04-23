// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCREMOTEBINDING_H
#define QSOCREMOTEBINDING_H

#include <QString>

/**
 * @brief Persistent per-project remote target binding.
 * @details Stored at `<project>/.qsoc/remote.yml` as a single YAML file
 *          with one leading field: `target: <profile-or-user@host:port>`.
 *          Writes go through a temporary file + rename so a crash midway
 *          cannot leave a truncated config. No credentials or private key
 *          paths are persisted here - the target is either a profile name
 *          from the global config or a direct `user@host:port` string.
 */
class QSocRemoteBinding
{
public:
    /** @brief Return `<projectRoot>/.qsoc/remote.yml`. */
    static QString pathFor(const QString &projectRoot);

    /**
     * @brief Read the `target:` field from the binding file.
     * @return Target string, or empty if the file is missing or has no target.
     */
    static QString readTarget(const QString &projectRoot);

    /**
     * @brief Atomically write or update the `target:` field.
     * @details Preserves any unknown siblings already present in the file.
     */
    static bool writeTarget(
        const QString &projectRoot, const QString &target, QString *errorMessage = nullptr);

    /**
     * @brief Remove the `target:` field (or delete the file when it only had
     *        that field).
     */
    static bool removeTarget(const QString &projectRoot, QString *errorMessage = nullptr);

private:
    QSocRemoteBinding()  = delete;
    ~QSocRemoteBinding() = delete;
};

#endif // QSOCREMOTEBINDING_H
