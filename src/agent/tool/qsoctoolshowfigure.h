// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLSHOWFIGURE_H
#define QSOCTOOLSHOWFIGURE_H

#include "agent/qsoctool.h"

#include <QPointer>

class QLLMService;

/**
 * @brief Show an image file to the user without sending it to the model.
 * @details Reads through the session's read_file tool, so a remote workspace
 *          works the same as a local one. The agent gate hides this tool when
 *          no terminal displays the result.
 */
class QSocToolShowFigure : public QSocTool
{
    Q_OBJECT

public:
    QSocToolShowFigure(QObject *parent, QSocTool *reader, QLLMService *llm = nullptr);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    bool    isReadOnly() const override { return true; }

private:
    QPointer<QSocTool> reader_;
    QLLMService       *llm_ = nullptr;
};

#endif // QSOCTOOLSHOWFIGURE_H
