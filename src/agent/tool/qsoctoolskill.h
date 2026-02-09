// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLSKILL_H
#define QSOCTOOLSKILL_H

#include "agent/qsoctool.h"
#include "common/qsocprojectmanager.h"

/**
 * @brief Tool to discover, search, and read user-defined skills (SKILL.md)
 * @details Skills are markdown prompt templates stored in project (.qsoc/skills/)
 *          or user (~/.config/qsoc/skills/) directories.
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

private:
    QSocProjectManager *projectManager = nullptr;

    struct SkillInfo
    {
        QString name;
        QString description;
        QString path;
        QString scope;
    };

    QString          userSkillsPath() const;
    QString          projectSkillsPath() const;
    QList<SkillInfo> scanSkillsDir(const QString &dirPath, const QString &scope) const;
    SkillInfo        parseSkillFile(const QString &filePath, const QString &scope) const;
    QString          readSkillContent(const QString &filePath) const;
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
