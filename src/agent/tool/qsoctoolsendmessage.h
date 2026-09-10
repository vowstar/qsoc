// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLSENDMESSAGE_H
#define QSOCTOOLSENDMESSAGE_H

#include "agent/tool/qsoctoolagentmessage.h"

class QSocSubAgentTaskSource;

/** @brief Peer messaging with compatibility for task_id callers. */
class QSocToolSendMessage : public QSocToolAgentMessage
{
    Q_OBJECT

public:
    QSocToolSendMessage(QObject *parent, QSocSubAgentTaskSource *taskSource);
    ~QSocToolSendMessage() override = default;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    QSocSubAgentTaskSource *taskSource_ = nullptr;
};

#endif /* QSOCTOOLSENDMESSAGE_H */
