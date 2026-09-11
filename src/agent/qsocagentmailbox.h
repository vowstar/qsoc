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

class QSocAgentInbox : public QObject
{
    Q_OBJECT
public:
    using QObject::QObject;
signals:
    void changed();
};

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
    QSocAgentInbox *inboxFor(const QString &id) const;
    QString         createGroup(const QStringList &members);
    void            setWakeHandler(std::function<QString(QSocAgent *)> handler);
    void            setState(const QString &id, const QString &state);
    QString         stateFor(const QString &id) const;
    json            list(const QString &caller) const;
    json            send(
        const QString &sender,
        const QString &target,
        const QString &messageId,
        const QString &body,
        const QString &replyTo,
        bool           wake);
    json sendSelected(
        const QString &sender,
        const json    &target,
        const QString &messageId,
        const QString &body,
        const QString &replyTo,
        bool           wake          = false,
        int            receiptOffset = 0);
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
        QPointer<QSocAgent>      agent;
        QPointer<QSocAgentInbox> inbox;
        QString                  name;
        QString                  taskId;
        QString                  state = QStringLiteral("idle");
        QStringList              pending;
    };
    struct Delivery
    {
        QString state = QStringLiteral("accepted");
        QString error;
        bool    replied = false;
    };
    struct Message
    {
        QString                 id;
        QString                 sender;
        QString                 body;
        QString                 replyTo;
        json                    selector;
        json                    excluded = json::array();
        QMap<QString, Delivery> deliveries;
        bool                    wake  = false;
        bool                    multi = false;
    };
    static QString keyFor(const QString &sender, const QString &id);
    static json    receipt(const Message &message, bool duplicate, int offset = 0);
    static json    error(const char *code);
    json           normalizeTarget(const json &target) const;
    QStringList    recipientsFor(const QString &sender, const json &selector, json *excluded) const;
    QString        deliveryError(
        const QString &sender, const QString &recipient, const QString &replyTo) const;
    void                                publish(const QStringList &recipients);
    QMap<QString, AgentEntry>           agents_;
    QMap<QString, QString>              aliases_;
    QMap<QString, Message>              messages_;
    QMap<QString, QStringList>          groups_;
    int                                 recordCount_ = 0;
    quint64                             generation_  = 0;
    std::function<QString(QSocAgent *)> wakeHandler_;
};

#endif
