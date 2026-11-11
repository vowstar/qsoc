// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLWAVEFORM_H
#define QSOCTOOLWAVEFORM_H

#include "agent/qsoctool.h"

class QSocPathContext;

class QSocToolWaveformRead : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolWaveformRead(QObject *parent = nullptr, QSocPathContext *paths = nullptr);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    bool    isReadOnly() const override { return true; }

private:
    QSocPathContext *paths_ = nullptr;
};

#endif
