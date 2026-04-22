// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLSKILL_H
#define QSOCTOOLSKILL_H

#include "agent/qsoctool.h"
#include "common/qsocprojectmanager.h"

/**
 * @brief Tool to discover, search, and read user-defined skills (SKILL.md)
 * @details Skills are markdown prompt templates resolved across four layers
 *          (high to low priority): $QSOC_HOME/skills, <project>/.qsoc/skills,
 *          ~/.config/qsoc/skills, and a platform-native system skills dir.
 *          Same-name skills in higher layers shadow lower ones.
 */
class QSocToolSkillFind : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolSkillFind(
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    ~QSocToolSkillFind() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setProjectManager(QSocProjectManager *projectManager);

    struct SkillInfo
    {
        QString name;
        QString description;
        QString argumentHint; /* e.g. "-m 'message'" for /commit */
        QString whenToUse;    /* trigger hint for the LLM */
        QString path;
        QString scope;                /* "project" or "user" */
        bool    userInvocable = true; /* register as /name slash command */
    };

    /* Scan all skill directories and return a merged, deduplicated list.
     * Project-scoped skills take priority over user-scoped ones with the
     * same name. Public so the REPL can use it for prompt injection and
     * slash command registration. */
    QList<SkillInfo> scanAllSkills() const;

    /* Read the full SKILL.md content (frontmatter + body). */
    QString readSkillContent(const QString &filePath) const;

private:
    QSocProjectManager *projectManager = nullptr;

    QStringList      allSkillsDirs() const;
    QList<SkillInfo> scanSkillsDir(const QString &dirPath, const QString &scope) const;
    SkillInfo        parseSkillFile(const QString &filePath, const QString &scope) const;
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
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    ~QSocToolSkillCreate() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setProjectManager(QSocProjectManager *projectManager);

private:
    QSocProjectManager *projectManager = nullptr;

    QString userSkillsPath() const;
    QString projectSkillsPath() const;
    bool    isValidSkillName(const QString &name) const;
};

#endif // QSOCTOOLSKILL_H
