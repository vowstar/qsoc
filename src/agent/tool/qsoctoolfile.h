// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLFILE_H
#define QSOCTOOLFILE_H

#include "agent/qsoctool.h"
#include "common/qsocprojectmanager.h"

/**
 * @brief Tool to read files (restricted to project directory)
 */
class QSocToolFileRead : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolFileRead(
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    ~QSocToolFileRead() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setProjectManager(QSocProjectManager *projectManager);

private:
    QSocProjectManager *projectManager_ = nullptr;

    /**
     * @brief Check if a path is within the project directory
     * @param filePath The path to check
     * @return true if path is within project directory, false otherwise
     */
    bool isPathAllowed(const QString &filePath) const;
};

/**
 * @brief Tool to list files in a directory (restricted to project directory)
 */
class QSocToolFileList : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolFileList(
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    ~QSocToolFileList() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setProjectManager(QSocProjectManager *projectManager);

private:
    QSocProjectManager *projectManager_ = nullptr;

    /**
     * @brief Check if a path is within the project directory
     * @param dirPath The path to check
     * @return true if path is within project directory, false otherwise
     */
    bool isPathAllowed(const QString &dirPath) const;
};

/**
 * @brief Tool to write files (restricted to project directory)
 */
class QSocToolFileWrite : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolFileWrite(
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    ~QSocToolFileWrite() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setProjectManager(QSocProjectManager *projectManager);

private:
    QSocProjectManager *projectManager_ = nullptr;
    bool                isPathAllowed(const QString &filePath) const;
};

/**
 * @brief Tool to edit files with string replacement (restricted to project directory)
 */
class QSocToolFileEdit : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolFileEdit(
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    ~QSocToolFileEdit() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setProjectManager(QSocProjectManager *projectManager);

private:
    QSocProjectManager *projectManager_ = nullptr;
    bool                isPathAllowed(const QString &filePath) const;
};

#endif // QSOCTOOLFILE_H
