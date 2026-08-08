// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOC_TEST_RELAY_H
#define QSOC_TEST_RELAY_H

#include <QHostAddress>
#include <QList>
#include <QMetaObject>
#include <QMutex>
#include <QMutexLocker>
#include <QSemaphore>
#include <QTcpServer>
#include <QTcpSocket>
#include <QThread>

#include <atomic>

/**
 * @brief TCP relay that can stop forwarding while holding its sockets open.
 * @details From the client's side that is exactly a host which lost power:
 *          the connection is established, writes are accepted by the local
 *          kernel, and no reply ever comes. Closing a socket or killing the
 *          server would send FIN or RST instead and exercise a different,
 *          much easier path, so the blackhole is the case worth simulating.
 *
 *          Runs in its own thread because the code under test blocks in
 *          poll() and never returns to the main event loop. Deliberately not
 *          a QObject: nothing here declares signals or slots, and a plain
 *          class keeps this usable from a header by more than one test.
 */
class QSocTestRelay : public QThread
{
public:
    explicit QSocTestRelay(quint16 upstreamPort)
        : m_upstreamPort(upstreamPort)
    {}

    /** @brief Block until the relay is listening. False on failure. */
    bool waitUntilListening(int timeoutMs) { return m_listening.tryAcquire(1, timeoutMs); }

    quint16 port() const { return m_port; }

    int acceptedConnectionCount() const
    {
        return m_acceptedConnections.load(std::memory_order_relaxed);
    }

    /** @brief Stop forwarding; both sockets stay open. */
    void blackhole()
    {
        const QMutexLocker locker(&m_controlLock);
        m_healOnNextConnection = false;
        m_blackhole.store(true, std::memory_order_relaxed);
    }

    /** @brief Stop forwarding until another client connection is accepted. */
    void blackholeUntilNextConnection()
    {
        const QMutexLocker locker(&m_controlLock);
        m_blackhole.store(true, std::memory_order_relaxed);
        m_healOnNextConnection = true;
    }

    /** @brief Resume forwarding on the established pair. */
    void heal()
    {
        const QMutexLocker locker(&m_controlLock);
        m_healOnNextConnection = false;
        m_blackhole.store(false, std::memory_order_relaxed);
    }

    /** @brief Abort the live pair, which the client sees as RST. */
    void hangUp()
    {
        const QMutexLocker locker(&m_socketsLock);
        for (QTcpSocket *sock : m_sockets) {
            /* Queued onto the relay thread, where the sockets live. abort()
             * is not an invokable slot, so it goes through a functor. */
            QMetaObject::invokeMethod(sock, [sock] { sock->abort(); }, Qt::QueuedConnection);
        }
    }

    void stop()
    {
        quit();
        wait(5000);
    }

protected:
    void run() override
    {
        QTcpServer server;
        if (!server.listen(QHostAddress::LocalHost, 0)) {
            m_listening.release();
            return;
        }
        m_port = server.serverPort();

        QObject::connect(&server, &QTcpServer::newConnection, &server, [this, &server] {
            QTcpSocket *downstream = server.nextPendingConnection();
            {
                const QMutexLocker locker(&m_controlLock);
                if (m_healOnNextConnection) {
                    m_healOnNextConnection = false;
                    m_blackhole.store(false, std::memory_order_relaxed);
                }
            }
            m_acceptedConnections.fetch_add(1, std::memory_order_relaxed);
            auto *upstream = new QTcpSocket(downstream);
            upstream->connectToHost(QHostAddress::LocalHost, m_upstreamPort);
            {
                const QMutexLocker locker(&m_socketsLock);
                m_sockets = {downstream, upstream};
            }

            auto pump = [this](QTcpSocket *from, QTcpSocket *to) {
                const QByteArray bytes = from->readAll();
                if (!m_blackhole.load(std::memory_order_relaxed)) {
                    to->write(bytes);
                }
            };
            QObject::connect(
                downstream, &QTcpSocket::readyRead, downstream, [pump, downstream, upstream] {
                    pump(downstream, upstream);
                });
            QObject::connect(upstream, &QTcpSocket::readyRead, upstream, [pump, downstream, upstream] {
                pump(upstream, downstream);
            });
        });

        m_listening.release();
        exec();
    }

private:
    std::atomic<int>    m_acceptedConnections{0};
    quint16             m_upstreamPort;
    quint16             m_port = 0;
    QSemaphore          m_listening;
    std::atomic<bool>   m_blackhole{false};
    mutable QMutex      m_controlLock;
    bool                m_healOnNextConnection = false;
    mutable QMutex      m_socketsLock;
    QList<QTcpSocket *> m_sockets;
};

#endif // QSOC_TEST_RELAY_H
