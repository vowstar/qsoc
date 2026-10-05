// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCHOSTPROFILE_H
#define QSOCHOSTPROFILE_H

#include <cstdint>
#include <QList>
#include <QObject>
#include <QString>

/**
 * @brief One catalog entry: an SSH alias annotated with workspace and capability.
 * @details The alias is the primary key. When the alias also appears as a
 *          `Host` block in `~/.ssh/config`, the parser supplies HostName,
 *          User, Port, IdentityFile and ProxyJump; the catalog only adds
 *          fields ssh-config does not carry (workspace, capability).
 *          When the alias is unknown to ssh-config, `target` provides a
 *          fallback `[user@]host[:port]` string.
 */
struct QSocHostProfile
{
    QString alias;      /**< Catalog key; matches ssh-config Host alias when available. */
    QString workspace;  /**< Absolute remote path for agent operations. */
    QString capability; /**< Free-form text advertised to the parent LLM. */
    QString shell;      /**< `auto`, `bash`, `sh` or an absolute path; empty is auto. */
    QString target; /**< Optional [user@]host[:port] fallback when alias is not in ssh-config. */
    QString scope;  /**< "user" or "project" (diagnostic; project wins on conflict). */
    QString sourcePath; /**< Absolute path of the YAML file this entry came from. */
};

/**
 * @brief Mutation operation passed to QSocHostCatalog::applyOp().
 * @details The op-list shape lets the agent LLM tool grow or shrink the
 *          capability text and amend supplemental fields without a custom
 *          schema per field. set_target only updates the catalog fallback;
 *          it never edits `~/.ssh/config`.
 */
struct QSocHostCatalogOp
{
    enum class Kind : std::uint8_t {
        CapabilityAppend,
        CapabilityRemove,
        CapabilityReplace,
        SetWorkspace,
        SetTarget,
    };
    Kind    kind;
    QString value;
};

/**
 * @brief The remote host a project was last bound to with `/ssh`.
 * @details Lives in the user's local data directory, one file per project,
 *          never in the project tree: a cloned repository must not be able
 *          to choose where qsoc connects.
 */
struct QSocHostBinding
{
    QString target;    /**< Alias or [user@]host[:port] as typed. Empty means local. */
    QString workspace; /**< Remote workspace the binding used. */

    bool isLocal() const { return target.isEmpty(); }
};

namespace QSocHostBindingStore {

/** @brief Default store directory under the user's local data location. */
QString defaultDir();

/** @brief Binding file of @p projectPath; empty when either argument is. */
QString filePath(const QString &storeDir, const QString &projectPath);

/** @brief Remembered binding of @p projectPath; local when none is stored. */
QSocHostBinding load(const QString &storeDir, const QString &projectPath);

/** @brief Remember @p binding for @p projectPath, readable by the owner only. */
bool save(
    const QString         &storeDir,
    const QString         &projectPath,
    const QSocHostBinding &binding,
    QString               *errorMessage = nullptr);

} // namespace QSocHostBindingStore

/**
 * @brief Host catalog loader, writer, and live mutator.
 * @details Reads two YAML files (user scope at `~/.config/qsoc/host.yml`,
 *          project scope at `<project>/.qsoc/host.yml`). Project entries
 *          override user entries by alias. All writes go to project scope
 *          via atomic temp+rename (QSaveFile). The agent-facing tools
 *          (`host_register`, `host_update`, `host_remove`) edit only
 *          through this class. Emits `catalogChanged()` after every
 *          successful mutation so the parent agent's system prompt and the
 *          spawn tool's enum can regenerate on the next LLM turn. An
 *          `active:` key in either file is never acted on; the binding is
 *          kept by @ref QSocHostBindingStore.
 */
class QSocHostCatalog : public QObject
{
    Q_OBJECT

public:
    explicit QSocHostCatalog(QObject *parent = nullptr);

    /**
     * @brief Load catalogs from disk.
     * @param userDir Directory containing the user-scope `host.yml`
     *        (typically `~/.config/qsoc`). Empty string skips this scope.
     * @param projectDir Project root; the file lives at
     *        `<projectDir>/.qsoc/host.yml`. Empty string skips this scope
     *        and disables writes (call again with a real path to enable).
     */
    void load(const QString &userDir, const QString &projectDir);

    /** @brief Merged effective list (project entries override user). */
    QList<QSocHostProfile> allList() const;

    /** @brief Lookup by alias; returns nullptr when absent. */
    const QSocHostProfile *find(const QString &alias) const;

    /** @brief Whether the project file carries a legacy `active:` key. */
    bool projectNamesActive() const { return projectNamesActive_; }

    /**
     * @brief Insert or update a catalog entry. Writes to project scope.
     * @details Returns false on duplicate alias when `allowOverwrite` is
     *          false (the default for `host_register`); pass true for
     *          unconditional updates.
     */
    bool upsert(
        const QSocHostProfile &profile,
        bool                   allowOverwrite = false,
        QString               *errorMessage   = nullptr);

    /**
     * @brief Apply a list of ops atomically: all-or-nothing.
     * @details Builds a working copy of the targeted entry, applies each
     *          op in order, and commits to disk only if all ops succeed.
     */
    bool applyOps(
        const QString                  &alias,
        const QList<QSocHostCatalogOp> &opList,
        QString                        *errorMessage = nullptr);

    /** @brief Remove a catalog entry. */
    bool remove(const QString &alias, QString *errorMessage = nullptr);

    /** @brief Absolute path of the project-scope file (writable target). */
    QString projectFilePath() const;

    /** @brief Absolute path of the user-scope file (read-only here). */
    QString userFilePath() const;

signals:
    /**
     * @brief Emitted after any successful write. Listeners regenerate
     *        derived state (agent tool schema, parent system prompt).
     */
    void catalogChanged();

private:
    bool writeProject(QString *errorMessage);

    QList<QSocHostProfile> userList_;    /**< Entries loaded from user scope. */
    QList<QSocHostProfile> projectList_; /**< Entries loaded from project scope. */
    bool                   projectNamesActive_ = false;
    QString                userDir_;
    QString                projectDir_;
};

#endif // QSOCHOSTPROFILE_H
