// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLMEMORY_H
#define QSOCTOOLMEMORY_H

#include "agent/qsoctool.h"
#include "common/qsocprojectmanager.h"

/**
 * @brief Tool to read agent memory (persistent context across sessions)
 * @details Reads from both user-level (~/.config/qsoc/memory.md) and
 *          project-level (<project>/.qsoc/memory.md) memory files.
 */
class QSocToolMemoryRead : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolMemoryRead(
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    ~QSocToolMemoryRead() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setProjectManager(QSocProjectManager *projectManager);

private:
    QSocProjectManager *projectManager = nullptr;

    /**
     * @brief Get the user-level memory file path
     * @return Path to ~/.config/qsoc/memory.md
     */
    QString userMemoryPath() const;

    /**
     * @brief Get the project-level memory file path
     * @return Path to <project>/.qsoc/memory.md
     */
    QString projectMemoryPath() const;

    /**
     * @brief Read content from a memory file
     * @param filePath Path to the memory file
     * @return File content or empty string if not found
     */
    QString readMemoryFile(const QString &filePath) const;
};

/**
 * @brief Tool to write agent memory (persistent context across sessions)
 * @details Writes to either user-level or project-level memory file.
 */
class QSocToolMemoryWrite : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolMemoryWrite(
        QObject *parent = nullptr, QSocProjectManager *projectManager = nullptr);
    ~QSocToolMemoryWrite() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setProjectManager(QSocProjectManager *projectManager);

private:
    QSocProjectManager *projectManager = nullptr;

    /**
     * @brief Get the user-level memory file path
     * @return Path to ~/.config/qsoc/memory.md
     */
    QString userMemoryPath() const;

    /**
     * @brief Get the project-level memory file path
     * @return Path to <project>/.qsoc/memory.md
     */
    QString projectMemoryPath() const;

    /**
     * @brief Write content to a memory file
     * @param filePath Path to the memory file
     * @param content Content to write
     * @return True if successful
     */
    bool writeMemoryFile(const QString &filePath, const QString &content) const;
};

#endif // QSOCTOOLMEMORY_H
