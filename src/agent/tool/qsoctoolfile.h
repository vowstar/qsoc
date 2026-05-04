// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLFILE_H
#define QSOCTOOLFILE_H

#include "agent/qsoctool.h"
#include "agent/tool/qsoctoolpath.h"
#include "common/qllmservice.h"

#include <QPointer>

class QSocFileHistory;

/**
 * @brief Tool to read files (unrestricted)
 */
class QSocToolFileRead : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolFileRead(
        QObject         *parent      = nullptr,
        QSocPathContext *pathContext = nullptr,
        QLLMService     *llm         = nullptr);
    ~QSocToolFileRead() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setPathContext(QSocPathContext *pathContext);

    /**
     * @brief Attach an LLM service for image-attachment capability gating.
     * @details When the file's leading bytes match a known raster image
     *          signature, read_file dispatches into the shared
     *          QSocImageAttach pipeline. The pipeline consults the
     *          active model's modalities.image flag to decide whether
     *          to inline the bytes or degrade to alt-text. Without an
     *          LLM pointer the read still works; image files get the
     *          alt-text fallback as if the model were text-only.
     */
    void setLLMService(QLLMService *llm);

private:
    QSocPathContext      *pathContext = nullptr;
    QPointer<QLLMService> llmService;
};

/**
 * @brief Tool to list files in a directory (unrestricted)
 */
class QSocToolFileList : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolFileList(QObject *parent = nullptr, QSocPathContext *pathContext = nullptr);
    ~QSocToolFileList() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setPathContext(QSocPathContext *pathContext);

private:
    QSocPathContext *pathContext = nullptr;
};

/**
 * @brief Tool to write files (restricted to allowed directories)
 */
class QSocToolFileWrite : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolFileWrite(QObject *parent = nullptr, QSocPathContext *pathContext = nullptr);
    ~QSocToolFileWrite() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setPathContext(QSocPathContext *pathContext);

    /**
     * @brief Attach a file history store for pre-edit backups.
     * @details When set, write_file captures the current content of the
     *          target file (or marks it absent) via trackEdit() before
     *          overwriting. The store can be nullptr to disable tracking.
     */
    void setFileHistory(QSocFileHistory *history);

private:
    QSocPathContext *pathContext = nullptr;
    QSocFileHistory *fileHistory = nullptr;
};

/**
 * @brief Tool to edit files with string replacement (restricted to allowed directories)
 */
class QSocToolFileEdit : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolFileEdit(QObject *parent = nullptr, QSocPathContext *pathContext = nullptr);
    ~QSocToolFileEdit() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setPathContext(QSocPathContext *pathContext);

    /**
     * @brief Attach a file history store for pre-edit backups.
     * @details When set, edit_file captures the current content of the
     *          target file via trackEdit() before applying the string
     *          replacement. The store can be nullptr to disable tracking.
     */
    void setFileHistory(QSocFileHistory *history);

private:
    QSocPathContext *pathContext = nullptr;
    QSocFileHistory *fileHistory = nullptr;
};

#endif // QSOCTOOLFILE_H
