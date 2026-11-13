// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCFSTCLIENT_H
#define QSOCFSTCLIENT_H

#include <QByteArray>
#include <QJsonObject>
#include <QString>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>

class QSocFstClient
{
public:
    using ReadAt = std::function<QByteArray(quint64, quint32, QString *)>;
    struct Companion
    {
        quint64 size = 0;
        ReadAt  readAt;
    };
    enum class Next { Message, Paused, Failed };
    static constexpr quint64 memoryLimit    = 768ULL * 1024 * 1024;
    static constexpr quint64 temporaryLimit = 64ULL * 1024 * 1024;

    QSocFstClient();
    ~QSocFstClient();
    QSocFstClient(const QSocFstClient &)            = delete;
    QSocFstClient &operator=(const QSocFstClient &) = delete;

    bool start(
        quint64                           size,
        ReadAt                            readAt,
        std::shared_ptr<std::atomic_bool> cancelled,
        QString                          *error,
        const QString                    &executable = {});
    bool start(
        quint64                           size,
        ReadAt                            readAt,
        std::shared_ptr<std::atomic_bool> cancelled,
        QString                          *error,
        const QString                    &executable,
        std::optional<Companion>          companion);
    void beginQuantum(int milliseconds = 5000, quint64 bytes = 128ULL * 1024 * 1024);
    void keepAlive();
    Next next(QJsonObject *message, QString *error);
    bool select(
        const QJsonObject                &selection,
        QString                          *error,
        std::shared_ptr<std::atomic_bool> cancelled = {});
    bool    reusable() const;
    bool    onOwnerThread() const;
    void    setEvictionHandler(std::function<void()> handler);
    void    cancel();
    quint64 scanBytes() const;
    quint64 scanOffset() const;
    qint64  processId() const;
    QString limitReason() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

#endif
