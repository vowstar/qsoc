// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLAGENTMESSAGE_H
#define QSOCTOOLAGENTMESSAGE_H

#include "agent/qsoctool.h"

class QSocAgentMailbox;

class QSocToolAgentMessage : public QSocTool
{
    Q_OBJECT
public:
    QSocToolAgentMessage(QObject *parent, QSocAgentMailbox *mailbox, const QString &operation);
    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    bool    supportsDeferred() const override { return operation_ == QStringLiteral("wait_agent"); }
    bool    isReadOnly() const override { return true; }

private:
    QPointer<QSocAgentMailbox> mailbox_;
    QString                    operation_;
};

#endif
