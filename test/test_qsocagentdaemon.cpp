// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

/**
 * @file test_qsocagentdaemon.cpp
 * @brief Tests for the qsoc-agentd wire protocol.
 * @details Spawns the real daemon binary on a private socket and drives it
 *          like a frontend would: greeting, open, status, command, events,
 *          shutdown.
 */

#include "agent/protocol/qsocagentruntimeevent.h"
#include "common/qsoclocalendpoint.h"
#include "common/qsoclocalpeer.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

using json = nlohmann::json;

namespace {

constexpr int kHeaderBytes = 8;

QByteArray frame(const QJsonObject &object)
{
    const QByteArray payload = QJsonDocument(object).toJson(QJsonDocument::Compact);
    return QByteArray::number(payload.size(), 16).rightJustified(kHeaderBytes, '0') + payload;
}

class DaemonClient
{
public:
    explicit DaemonClient(const QString &socketPath)
        : m_socketPath(socketPath)
    {
        m_socket.connectToServer(QSocLocalEndpoint::resolve(socketPath));
        if (!m_socket.waitForConnected(5000)) {
            m_error = m_socket.errorString();
        }
    }

    ~DaemonClient() { m_socket.disconnectFromServer(); }

    [[nodiscard]] bool                connected() const { return m_error.isEmpty(); }
    [[nodiscard]] const QLocalSocket &socket() const { return m_socket; }
    [[nodiscard]] QString             error() const { return m_error; }

    void send(const QJsonObject &object)
    {
        m_socket.write(frame(object));
        m_socket.flush();
    }

    QJsonObject receive(int timeoutMs = 10000)
    {
        if (!m_error.isEmpty()) {
            return {};
        }
        QByteArray buffer;
        buffer.swap(m_buffer);
        while (buffer.size() < kHeaderBytes) {
            if (!m_socket.waitForReadyRead(timeoutMs)) {
                m_error = m_socket.errorString();
                return {};
            }
            buffer += m_socket.readAll();
        }
        const int length = buffer.left(kHeaderBytes).toInt(nullptr, 16);
        while (buffer.size() < kHeaderBytes + length) {
            if (!m_socket.waitForReadyRead(timeoutMs)) {
                m_error = m_socket.errorString();
                return {};
            }
            buffer += m_socket.readAll();
        }
        const QByteArray payload = buffer.mid(kHeaderBytes, length);
        m_buffer                 = buffer.mid(kHeaderBytes + length);
        const QJsonDocument doc  = QJsonDocument::fromJson(payload);
        if (!doc.isObject()) {
            m_error = QStringLiteral("malformed frame");
            return {};
        }
        return doc.object();
    }

    /** Receive frames until one carries a reply for @p id. */
    QJsonObject waitForReply(qint64 id, int timeoutMs = 10000)
    {
        for (int round = 0; round < 200; ++round) {
            const QJsonObject frameObject = receive(timeoutMs);
            if (frameObject.isEmpty()) {
                return {};
            }
            if (frameObject.contains(QStringLiteral("id"))) {
                if (static_cast<qint64>(frameObject.value(QStringLiteral("id")).toDouble()) == id) {
                    return frameObject;
                }
            }
        }
        m_error = QStringLiteral("reply never arrived");
        return {};
    }

    /** Receive one event frame. */
    QJsonObject receiveEvent(int timeoutMs = 10000)
    {
        for (int round = 0; round < 200; ++round) {
            const QJsonObject frameObject = receive(timeoutMs);
            if (frameObject.isEmpty()) {
                return {};
            }
            if (frameObject.contains(QStringLiteral("event"))) {
                return frameObject.value(QStringLiteral("event")).toObject();
            }
        }
        return {};
    }

private:
    QString      m_socketPath;
    QLocalSocket m_socket;
    QByteArray   m_buffer;
    QString      m_error;
};

} // namespace

class TestQSocAgentDaemon : public QObject
{
    Q_OBJECT

private slots:

    void initTestCase()
    {
        m_daemonPath = QStringLiteral(QSOC_AGENTD_PATH);
        QVERIFY2(QFile::exists(m_daemonPath), "qsoc-agentd was not built");
    }

    void greetingCarriesDaemonIdentity()
    {
        QTemporaryDir fixture(QDir::tempPath() + QStringLiteral("/agentd_greet_XXXXXX"));
        QVERIFY(fixture.isValid());
        const QString socketPath = QDir(fixture.path()).filePath(QStringLiteral("d.sock"));

        QProcess daemon;
        daemon.setProcessEnvironment(isolatedEnvironment(fixture.path()));
        daemon.start(m_daemonPath, {QStringLiteral("-s"), socketPath});
        QVERIFY2(daemon.waitForStarted(5000), qPrintable(daemon.errorString()));
        QVERIFY2(waitForSocket(socketPath, 5000), "daemon socket never appeared");

        DaemonClient client(socketPath);
        QVERIFY2(client.connected(), qPrintable(client.error()));
        const QJsonObject greeting = client.receive();
        QVERIFY(!greeting.isEmpty());
        QCOMPARE(greeting.value(QStringLiteral("daemon")).toString(), QStringLiteral("qsoc-agentd"));
        QVERIFY(!greeting.value(QStringLiteral("version")).toString().isEmpty());
        QVERIFY(greeting.value(QStringLiteral("pid")).toDouble() > 0);
        QVERIFY(QSocLocalPeer::sameUser(client.socket()));
        const qint64 servedBy = QSocLocalPeer::processId(client.socket());
        QVERIFY(servedBy == -1 || servedBy == daemon.processId());

        client.send({{"id", 1}, {"method", QStringLiteral("shutdown")}});
        const QJsonObject bye = client.waitForReply(1);
        QVERIFY(
            bye.value(QStringLiteral("result")).toObject().value(QStringLiteral("bye")).toBool());
        QVERIFY(daemon.waitForFinished(5000));
    }

    void peersReportBothEndsAndRejectDisconnectedSockets()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        QVERIFY(server.listen(QSocLocalEndpoint::resolve(directory.filePath("peer.sock"))));
        QLocalSocket client;
        QVERIFY(!QSocLocalPeer::sameUser(client));
        QCOMPARE(QSocLocalPeer::processId(client), qint64(-1));
        client.connectToServer(server.fullServerName());
        QVERIFY(client.waitForConnected(5000));
        QTRY_VERIFY(server.hasPendingConnections());
        std::unique_ptr<QLocalSocket> accepted(server.nextPendingConnection());
        QVERIFY(QSocLocalPeer::sameUser(client));
        QVERIFY(QSocLocalPeer::sameUser(*accepted));
        QCOMPARE(QSocLocalPeer::processId(client), QCoreApplication::applicationPid());
        QCOMPARE(QSocLocalPeer::processId(*accepted), QCoreApplication::applicationPid());
    }

    void rejectsSharedSocketDirectories()
    {
#ifdef Q_OS_UNIX
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QVERIFY(
            QFile::setPermissions(
                directory.path(),
                QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner | QFile::ReadOther
                    | QFile::ExeOther));
        QString error;
        QVERIFY(!QSocLocalEndpoint::prepareDirectory(directory.filePath("agent.sock"), &error));
        QVERIFY(error.contains("private"));
#endif
    }

    void longEndpointUsesTheSamePrivateAddressOnBothSides()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString requested = directory.filePath(QString(160, QLatin1Char('x')) + "/agent.sock");
        const QString resolved = QSocLocalEndpoint::resolve(requested);
#ifdef Q_OS_UNIX
        QVERIFY(QFile::encodeName(resolved).size() < 104);
        QVERIFY(resolved != requested);
#endif
        QProcess daemon;
        daemon.setProcessEnvironment(isolatedEnvironment(directory.path()));
        daemon.start(m_daemonPath, {"--socket", requested});
        QVERIFY(daemon.waitForStarted(5000));
        QVERIFY(waitForSocket(requested, 5000));
        DaemonClient client(requested);
        QVERIFY(client.connected());
        QCOMPARE(client.receive().value("protocol").toInt(), 1);
        client.send({{"id", 1}, {"method", "shutdown"}});
        QVERIFY(!client.waitForReply(1).isEmpty());
        QVERIFY(daemon.waitForFinished(5000));
    }

    void openTurnLifecycleOverTheSocket()
    {
        QTemporaryDir fixture(QDir::tempPath() + QStringLiteral("/agentd_life_XXXXXX"));
        QVERIFY(fixture.isValid());
        const QString socketPath = QDir(fixture.path()).filePath(QStringLiteral("d.sock"));
        const QString project    = QDir(fixture.path()).filePath(QStringLiteral("project"));
        QVERIFY(QDir().mkpath(project));

        QProcess daemon;
        daemon.setProcessEnvironment(isolatedEnvironment(fixture.path()));
        daemon.start(m_daemonPath, {QStringLiteral("-s"), socketPath});
        QVERIFY2(daemon.waitForStarted(5000), qPrintable(daemon.errorString()));
        QVERIFY2(waitForSocket(socketPath, 5000), "daemon socket never appeared");

        DaemonClient client(socketPath);
        QVERIFY2(client.connected(), qPrintable(client.error()));
        QVERIFY(!client.receive().isEmpty()); /* greeting */

        /* open: creates a session in the project. */
        client.send(
            {{"id", 1},
             {"method", QStringLiteral("open")},
             {"params", QJsonObject{{"project_directory", project}}}});
        QJsonObject reply = client.waitForReply(1);
        QVERIFY(reply.contains(QStringLiteral("result")));
        const QJsonObject openResult = reply.value(QStringLiteral("result")).toObject();
        QVERIFY(openResult.value(QStringLiteral("ok")).toBool());
        const QString sessionId = openResult.value(QStringLiteral("session_id")).toString();
        QVERIFY(!sessionId.isEmpty());
        /* Sessions are lazy: the JSONL appears only after the first durable
         * record, so the id (not the file) is the observable contract here. */
        QVERIFY(!sessionId.contains(QLatin1Char('/')));

        /* status reflects the session. */
        client.send({{"id", 2}, {"method", QStringLiteral("status")}});
        reply                    = client.waitForReply(2);
        const QJsonObject status = reply.value(QStringLiteral("result")).toObject();
        QCOMPARE(status.value(QStringLiteral("session_id")).toString(), sessionId);
        QCOMPARE(status.value(QStringLiteral("running")).toBool(), false);
        QVERIFY(status.contains(QStringLiteral("model")));

        /* command: /status output streams as events, then the reply. */
        client.send(
            {{"id", 3},
             {"method", QStringLiteral("command")},
             {"params", QJsonObject{{"input", QStringLiteral("/status")}}}});
        bool        sawModelLine = false;
        QJsonObject commandReply;
        for (int round = 0; round < 100; ++round) {
            const QJsonObject frameObject = client.receive();
            if (frameObject.isEmpty()) {
                break;
            }
            if (frameObject.contains(QStringLiteral("event"))) {
                const QJsonObject event = frameObject.value(QStringLiteral("event")).toObject();
                if (event.value(QStringLiteral("kind")).toString() == QStringLiteral("output")
                    && event.value(QStringLiteral("text"))
                           .toString()
                           .contains(QStringLiteral("Model:"))) {
                    sawModelLine = true;
                }
            } else {
                commandReply = frameObject;
                break;
            }
        }
        QVERIFY(sawModelLine);
        QVERIFY(commandReply.value(QStringLiteral("result"))
                    .toObject()
                    .value(QStringLiteral("handled"))
                    .toBool());

        /* sessions listing answers with a (possibly empty) array: sessions
         * are lazy, so nothing is listed until a durable record exists. */
        client.send({{"id", 4}, {"method", QStringLiteral("sessions")}});
        reply                 = client.waitForReply(4);
        const QJsonArray rows = reply.value(QStringLiteral("result"))
                                    .toObject()
                                    .value(QStringLiteral("sessions"))
                                    .toArray();
        QCOMPARE(rows.size(), 0);

        /* unknown method is an error, not a crash. */
        client.send({{"id", 5}, {"method", QStringLiteral("nope")}});
        reply = client.waitForReply(5);
        QVERIFY(reply.contains(QStringLiteral("error")));

        client.send({{"id", 6}, {"method", QStringLiteral("shutdown")}});
        QVERIFY(!client.waitForReply(6).isEmpty());
        QVERIFY(daemon.waitForFinished(5000));
        QCOMPARE(daemon.exitStatus(), QProcess::NormalExit);
    }

    void turnWithoutOpenIsRefused()
    {
        QTemporaryDir fixture(QDir::tempPath() + QStringLiteral("/agentd_noopen_XXXXXX"));
        QVERIFY(fixture.isValid());
        const QString socketPath = QDir(fixture.path()).filePath(QStringLiteral("d.sock"));

        QProcess daemon;
        daemon.setProcessEnvironment(isolatedEnvironment(fixture.path()));
        daemon.start(m_daemonPath, {QStringLiteral("-s"), socketPath});
        QVERIFY(daemon.waitForStarted(5000));
        QVERIFY(waitForSocket(socketPath, 5000));

        DaemonClient client(socketPath);
        QVERIFY(client.connected());
        QVERIFY(!client.receive().isEmpty()); /* greeting */
        client.send(
            {{"id", 1},
             {"method", QStringLiteral("turn")},
             {"params", QJsonObject{{"input", QStringLiteral("hi")}}}});
        const QJsonObject reply = client.waitForReply(1);
        QVERIFY(reply.contains(QStringLiteral("error")));
        QVERIFY(reply.value(QStringLiteral("error")).toString().contains(QStringLiteral("open")));

        client.send({{"id", 2}, {"method", QStringLiteral("shutdown")}});
        QVERIFY(!client.waitForReply(2).isEmpty());
        QVERIFY(daemon.waitForFinished(5000));
    }

private:
    static QProcessEnvironment isolatedEnvironment(const QString &home)
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("HOME"), home);
        env.insert(QStringLiteral("XDG_CONFIG_HOME"), QDir(home).filePath(QStringLiteral("config")));
        env.insert(QStringLiteral("XDG_RUNTIME_DIR"), home);
        env.insert(QStringLiteral("QSOC_HOME"), QDir(home).filePath(QStringLiteral("config/qsoc")));
        env.remove(QStringLiteral("http_proxy"));
        env.remove(QStringLiteral("https_proxy"));
        env.remove(QStringLiteral("HTTP_PROXY"));
        env.remove(QStringLiteral("HTTPS_PROXY"));
        return env;
    }

    static bool waitForSocket(const QString &socketPath, int timeoutMs)
    {
        QDeadlineTimer deadline(timeoutMs);
        while (!deadline.hasExpired()) {
            DaemonClient probe(socketPath);
            if (probe.connected()
                && probe.receive(static_cast<int>(deadline.remainingTime())).value("protocol").toInt()
                       == 1)
                return true;
            QTest::qWait(20);
        }
        return false;
    }

    QString m_daemonPath;
};

QSOC_TEST_MAIN(TestQSocAgentDaemon)
#include "test_qsocagentdaemon.moc"
