// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCAGENTMAILBOX_H
#define QSOCAGENTMAILBOX_H

#include <functional>
#include <nlohmann/json.hpp>
#include <QMap>
#include <QObject>
#include <QPointer>
#include <QStringList>

class QSocAgent;

class QSocAgentMailbox : public QObject
{
    Q_OBJECT

public:
    using json = nlohmann::json;
    explicit QSocAgentMailbox(QObject *parent = nullptr);

    QString    registerAgent(QSocAgent *agent, const QString &name, const QString &taskId = {});
    QString    idFor(const QSocAgent *agent) const;
    QString    resolve(const QString &target) const;
    QSocAgent *agentFor(const QString &id) const;
    void       setWakeHandler(std::function<QString(QSocAgent *)> handler);
    void       setState(const QString &id, const QString &state);
    QString    stateFor(const QString &id) const;
    json       list(const QString &caller) const;
    json       send(
        const QString &sender,
        const QString &target,
        const QString &messageId,
        const QString &body,
        const QString &replyTo,
        bool           wake);
    json take(
        const QString &recipient,
        const QString &from    = {},
        const QString &replyTo = {},
        bool           peek    = false);
    int            pendingCount(const QString &recipient) const;
    void           cancel(const QString &id);
    void           reset(QSocAgent *root);
    void           finish(const QString &id, const QString &result);
    static QString render(const json &message);

signals:
    void changed();

private:
    struct AgentEntry
    {
        QPointer<QSocAgent> agent;
        QString             name;
        QString             taskId;
        QString             state = QStringLiteral("idle");
        QStringList         pending;
    };
    struct Message
    {
        QString id;
        QString sender;
        QString recipient;
        QString body;
        QString replyTo;
        QString delivery = QStringLiteral("accepted");
        bool    wake     = false;
        bool    replied  = false;
    };
    static QString                      keyFor(const QString &sender, const QString &id);
    static json                         receipt(const Message &message, bool duplicate);
    static json                         error(const char *code);
    QMap<QString, AgentEntry>           agents_;
    QMap<QString, QString>              aliases_;
    QMap<QString, Message>              messages_;
    std::function<QString(QSocAgent *)> wakeHandler_;
};

#endif
