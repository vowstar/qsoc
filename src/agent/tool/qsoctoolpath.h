// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLPATH_H
#define QSOCTOOLPATH_H

#include "agent/qsoctool.h"
#include "common/qsocprojectmanager.h"

#include <QMutex>
#include <QSet>

/**
 * @brief Shared path context for agent tools
 * @details Maintains a lightweight list of commonly used paths:
 *          - Project directory (auto-set from project manager)
 *          - Current working directory (adjustable)
 *          - User-mentioned directories (dynamic list, max 10)
 */
class QSocPathContext : public QObject
{
    Q_OBJECT

public:
    explicit QSocPathContext(QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);

    /* Getters */
    QString     getProjectDir() const;
    QString     getWorkingDir() const;
    QStringList getUserDirs() const;

    /* Setters */
    void setWorkingDir(const QString &dir);
    void addUserDir(const QString &dir);
    void removeUserDir(const QString &dir);
    void clearUserDirs();

    /* Permission checks */
    bool isWriteAllowed(const QString &path) const;

    /* All writable directories for display */
    QStringList getWritableDirs() const;

    /* Get compact summary for injection into messages (max ~200 chars) */
    QString getSummary() const;

    /* Get full context as structured text */
    QString getFullContext() const;

private:
    QSocProjectManager *projectManager = nullptr;
    QString             workingDir;
    QStringList         userDirs;
    mutable QMutex      mutex;

    static constexpr int MaxUserDirs = 10;
};

/**
 * @brief Tool to query and manage path context
 */
class QSocToolPathContext : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolPathContext(QObject *parent = nullptr, QSocPathContext *pathContext = nullptr);
    ~QSocToolPathContext() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocPathContext *pathContext = nullptr;
};

#endif // QSOCTOOLPATH_H
