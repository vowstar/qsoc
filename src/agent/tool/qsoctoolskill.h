// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLSKILL_H
#define QSOCTOOLSKILL_H

#include "agent/qsoctool.h"
#include "common/qsocprojectmanager.h"

class QSocWorkspaceFs;

/**
 * @brief Tool to discover, search, and read user-defined skills (SKILL.md)
 * @details Skills are markdown prompt templates resolved across layers
 *          (high to low priority): `env` ($QSOC_HOME/skills), `remote` (the
 *          workspace file system's `.qsoc/skills`, when one is given),
 *          `local` (<project>/.qsoc/skills), `user` (~/.config/qsoc/skills),
 *          `system` (a platform-native dir) and `extra` ($QSOC_SKILLS_PATH).
 *          Same-name skills in higher layers shadow lower ones.
 */
class QSocToolSkillFind : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolSkillFind(
        QObject            *parent         = nullptr,
        QSocProjectManager *projectManager = nullptr,
        QSocWorkspaceFs    *projectFs      = nullptr);
    ~QSocToolSkillFind() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    bool    isReadOnly() const override { return true; }

    void                setProjectManager(QSocProjectManager *projectManager);
    QSocProjectManager *getProjectManager() const;

    struct SkillInfo
    {
        QString name;
        QString description;
        QString argumentHint; /* e.g. "-m 'message'" for /commit */
        QString whenToUse;    /* trigger hint for the LLM */
        QString path;
        QString scope;                          /* the layer: "local", "remote", "user", ... */
        bool    userInvocable          = true;  /* register as /name slash command */
        bool    disableModelInvocation = false; /* hide from model-facing listings */
        QString parseError;                     /* non-empty if the SKILL.md was malformed */
        QString workspacePath;                  /* relative to the workspace when read through it */
    };

    /* Scan all skill directories like scanAllSkills(), but include entries
     * whose SKILL.md failed to parse so the caller can surface diagnostics
     * to the user. Each entry's path is set; name is empty when broken. */
    QList<SkillInfo> scanAllSkillFiles() const;

    /* Build the system-prompt listing block. Each description is truncated
     * to keep the prefix small and stable so the prompt cache can hit even
     * when one skill's description grows by a few words. */
    static QString formatPromptListing(const QList<SkillInfo> &skills);

    /* Replace ${ARGS}, ${CWD} and ${PROJECT} placeholders in a skill body.
     * Returns the substituted text and sets argsConsumed to true if the
     * body referenced ${ARGS} (so callers can avoid double-appending it). */
    static QString substitutePlaceholders(
        const QString &body,
        const QString &args,
        const QString &cwd,
        const QString &projectPath,
        bool          *argsConsumed = nullptr);

    /* Scan all skill directories and return a merged, deduplicated list.
     * Project-scoped skills take priority over user-scoped ones with the
     * same name. Public so the REPL can use it for prompt injection and
     * slash command registration. */
    QList<SkillInfo> scanAllSkills() const;

    /* Read the full SKILL.md content (frontmatter + body). */
    QString readSkillContent(const SkillInfo &skill) const;

private:
    /* One skill root; an empty dir names the workspace project layer. */
    struct Layer
    {
        QString dir;
        QString scope;
    };

    QSocProjectManager *projectManager = nullptr;
    QSocWorkspaceFs    *projectFs      = nullptr;

    QList<Layer>     layers() const;
    QList<SkillInfo> scanSkillsDir(const QString &dirPath, const QString &scope) const;
    QList<SkillInfo> scanWorkspaceSkills() const;
    static SkillInfo parseSkill(const QString &content, const QString &path, const QString &scope);
};

/**
 * @brief Tool to create new skill files (SKILL.md)
 * @details Creates a SKILL.md file with YAML frontmatter in the specified scope.
 */
class QSocToolSkillCreate : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolSkillCreate(
        QObject            *parent         = nullptr,
        QSocProjectManager *projectManager = nullptr,
        QSocWorkspaceFs    *projectFs      = nullptr);
    ~QSocToolSkillCreate() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setProjectManager(QSocProjectManager *projectManager);

private:
    QSocProjectManager *projectManager = nullptr;
    QSocWorkspaceFs    *projectFs      = nullptr;

    QString userSkillsPath() const;
    QString projectSkillsPath() const;
    bool    isValidSkillName(const QString &name) const;
    QString createInWorkspace(const QString &name, const QString &content) const;
};

#endif // QSOCTOOLSKILL_H
