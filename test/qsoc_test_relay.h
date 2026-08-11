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
#include <QtGlobal>

#include <atomic>
#include <limits>

#ifndef Q_OS_WIN
#include <sys/socket.h>
#endif

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
        m_blackholeThrough.store(std::numeric_limits<int>::max(), std::memory_order_relaxed);
    }

    /** @brief Stop forwarding until another client connection is accepted. */
    void blackholeUntilNextConnection()
    {
        m_blackholeThrough.store(acceptedConnectionCount(), std::memory_order_relaxed);
        m_stopReading.store(false, std::memory_order_relaxed);
    }

    /** @brief Resume forwarding on the established pair. */
    void heal()
    {
        m_stopReading.store(false, std::memory_order_relaxed);
        m_blackholeThrough.store(-1, std::memory_order_relaxed);
    }

    /**
     * @brief Stop draining the sockets, so the client's own sends stall.
     * @details Stronger than blackhole(), which reads and discards and so
     *          always lets the local kernel accept a small write. Here the
     *          receive window closes instead, which is the only way to leave a
     *          peer blocked mid-packet with unsent bytes still buffered on its
     *          side. Cleared by heal().
     */
    void stopReading() { m_stopReading.store(true, std::memory_order_relaxed); }

    /**
     * @brief Close the live pair gracefully, which the client sees as FIN.
     * @details The polite counterpart to hangUp(): the peer says it is done
     *          instead of vanishing. libssh2 reports that as a clean channel
     *          close, so it is the case that must not be mistaken for a
     *          process that reported an exit status.
     */
    void finish()
    {
        const QMutexLocker locker(&m_socketsLock);
        for (QTcpSocket *sock : m_sockets) {
            /* Queued onto the relay thread, where the sockets live. */
            QMetaObject::invokeMethod(
                sock, [sock] { sock->disconnectFromHost(); }, Qt::QueuedConnection);
        }
    }

    /**
     * @brief Take away only the direction the client reads from.
     * @details The client reaches EOF on recv while its own sends keep being
     *          accepted, which is what a TCP half-close looks like and what
     *          neither finish() nor hangUp() produces: both of those take the
     *          client's send direction away as well. Forwarding stops in the
     *          same breath, because a write to a socket whose send side is gone
     *          fails and Qt then closes the whole socket, which the client
     *          would read as a reset instead.
     *
     *          No-op before the pair exists, and on platforms without the
     *          POSIX call, so a build there stays green even though the mode is
     *          unavailable.
     */
    void halfClose()
    {
        const QMutexLocker locker(&m_socketsLock);
        m_blackholeThrough.store(std::numeric_limits<int>::max(), std::memory_order_relaxed);
        if (m_sockets.isEmpty()) {
            return;
        }
        /* Index 0 is the accepted socket, the one facing the client. */
        QTcpSocket *sock = m_sockets.at(0);
        /* Queued onto the relay thread, where the sockets live. */
        QMetaObject::invokeMethod(sock, [sock] { shutdownSend(sock); }, Qt::QueuedConnection);
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
            QTcpSocket *downstream   = server.nextPendingConnection();
            const int   connectionId = m_acceptedConnections.fetch_add(1, std::memory_order_relaxed)
                                       + 1;
            auto       *upstream     = new QTcpSocket(downstream);
            upstream->connectToHost(QHostAddress::LocalHost, m_upstreamPort);
            {
                const QMutexLocker locker(&m_socketsLock);
                m_sockets = {downstream, upstream};
            }

            auto pump = [this, connectionId](QTcpSocket *from, QTcpSocket *to) {
                /* Leaving the bytes unread is what distinguishes stopReading()
                 * from blackhole(): Qt stops draining the OS socket once its
                 * own buffer is full, the receive window closes, and the peer's
                 * sends begin to block partway through a packet. */
                if (m_stopReading.load(std::memory_order_relaxed)) {
                    return;
                }
                const QByteArray bytes = from->readAll();
                if (!isBlackholed(connectionId)) {
                    to->write(bytes);
                }
            };
            downstream->setReadBufferSize(kStalledBufferBytes);
            upstream->setReadBufferSize(kStalledBufferBytes);
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
    bool isBlackholed(int connectionId) const
    {
        const int through = m_blackholeThrough.load(std::memory_order_relaxed);
        return through == std::numeric_limits<int>::max()
               || (through >= 0 && connectionId <= through);
    }

    static void shutdownSend(QTcpSocket *sock)
    {
#ifndef Q_OS_WIN
        ::shutdown(static_cast<int>(sock->socketDescriptor()), SHUT_WR);
#else
        Q_UNUSED(sock)
#endif
    }

    /* Small enough that a stalled relay closes its receive window after very
     * little data, and irrelevant while forwarding, because pump() drains on
     * every readyRead. */
    static constexpr int kStalledBufferBytes = 4096;

    std::atomic<bool>   m_stopReading{false};
    std::atomic<int>    m_acceptedConnections{0};
    std::atomic<int>    m_blackholeThrough{-1};
    quint16             m_upstreamPort;
    quint16             m_port = 0;
    QSemaphore          m_listening;
    mutable QMutex      m_socketsLock;
    QList<QTcpSocket *> m_sockets;
};

#endif // QSOC_TEST_RELAY_H
