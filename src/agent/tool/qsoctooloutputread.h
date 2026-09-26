// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLOUTPUTREAD_H
#define QSOCTOOLOUTPUTREAD_H

#include "agent/qsoctool.h"

class QSocToolOutputRead : public QSocTool
{
public:
    explicit QSocToolOutputRead(QObject *parent)
        : QSocTool(parent)
    {}
    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    bool    isReadOnly() const override { return true; }
};

#endif // QSOCTOOLOUTPUTREAD_H
