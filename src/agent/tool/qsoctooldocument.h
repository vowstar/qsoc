// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLDOCUMENT_H
#define QSOCTOOLDOCUMENT_H

#include "agent/qsoctool.h"

class QLLMService;
class QSocPathContext;

class QSocToolDocumentRead : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolDocumentRead(
        QObject *parent = nullptr, QSocPathContext *paths = nullptr, QLLMService *llm = nullptr);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    bool    isReadOnly() const override { return true; }

private:
    QSocPathContext      *paths_ = nullptr;
    QPointer<QLLMService> llm_;
};

#endif
