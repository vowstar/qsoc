// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLFILE_H
#define QSOCTOOLFILE_H

#include "agent/qsoctool.h"
#include "agent/tool/qsoctoolpath.h"

/**
 * @brief Tool to read files (unrestricted)
 */
class QSocToolFileRead : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolFileRead(QObject *parent = nullptr, QSocPathContext *pathContext = nullptr);
    ~QSocToolFileRead() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

    void setPathContext(QSocPathContext *pathContext);

private:
    QSocPathContext *pathContext = nullptr;
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

private:
    QSocPathContext *pathContext = nullptr;
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

private:
    QSocPathContext *pathContext = nullptr;
};

#endif // QSOCTOOLFILE_H
