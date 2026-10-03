// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCDAEMONRESOURCES_H
#define QSOCDAEMONRESOURCES_H

#include <functional>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QStringList>
#include <QTimer>

class QSocDaemonResources : public QObject
{
public:
    using Callback = std::function<void(const QJsonObject &)>;

    explicit QSocDaemonResources(
        QObject *parent = nullptr, QString program = {}, QStringList arguments = {});
    ~QSocDaemonResources() override;

    static bool validatePaths(const QJsonObject &params, QJsonArray &paths);
    static int  runProbe(qint64 supervisorPid);

    void request(QObject *requester, const QJsonArray &paths, Callback callback);
    void cancel(QObject *requester);
    void shutdown();

private:
    void readOutput();
    void complete(const QJsonObject &result);
    void stop(const QString &status, const QString &reason);

    QString                 program_;
    QStringList             arguments_;
    QProcess               *process_ = nullptr;
    QPointer<QObject>       requester_;
    QMetaObject::Connection requesterConnection_;
    QByteArray              output_;
    Callback                callback_;
    QTimer                  deadline_;
    bool                    stopped_ = false;
};

#endif
