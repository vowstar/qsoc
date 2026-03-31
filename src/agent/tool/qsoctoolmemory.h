// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLMEMORY_H
#define QSOCTOOLMEMORY_H

#include "agent/qsocmemorymanager.h"
#include "agent/qsoctool.h"

/**
 * @brief Tool to read agent memory (persistent context across sessions)
 * @details Reads from both user-level and project-level memory directories.
 *          Supports filtering by scope, type, or specific topic name.
 */
class QSocToolMemoryRead : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolMemoryRead(
        QObject *parent = nullptr, QSocMemoryManager *memoryManager = nullptr);
    ~QSocToolMemoryRead() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setMemoryManager(QSocMemoryManager *memoryManager);

private:
    QSocMemoryManager *memoryManager = nullptr;
};

/**
 * @brief Tool to write agent memory (persistent context across sessions)
 * @details Writes topic files with YAML frontmatter and auto-rebuilds the index.
 */
class QSocToolMemoryWrite : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolMemoryWrite(
        QObject *parent = nullptr, QSocMemoryManager *memoryManager = nullptr);
    ~QSocToolMemoryWrite() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setMemoryManager(QSocMemoryManager *memoryManager);

private:
    QSocMemoryManager *memoryManager = nullptr;
};

#endif // QSOCTOOLMEMORY_H
