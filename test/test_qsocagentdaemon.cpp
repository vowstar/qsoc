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
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QSet>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_WIN
#include <windows.h>

#include <tlhelp32.h>
#endif

#ifdef Q_OS_LINUX
#include <pwd.h>
#include <unistd.h>
#endif

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

#ifdef Q_OS_LINUX
    void defaultEndpointUsesPrivateRuntime_data()
    {
        QTest::addColumn<QString>("runtimeState");
        QTest::addColumn<bool>("explicitEndpoint");
        QTest::newRow("valid-runtime") << QStringLiteral("valid") << false;
        QTest::newRow("missing-runtime") << QStringLiteral("missing") << false;
        QTest::newRow("invalid-runtime-and-fallback") << QStringLiteral("invalid") << false;
        QTest::newRow("explicit-endpoint") << QStringLiteral("invalid") << true;
    }

    void defaultEndpointUsesPrivateRuntime()
    {
        QFETCH(QString, runtimeState);
        QFETCH(bool, explicitEndpoint);
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto *user = ::getpwuid(::geteuid());
        QVERIFY(user != nullptr);
        const QString qtFallback = fixture.filePath(
            QStringLiteral("runtime-") + QString::fromLocal8Bit(user->pw_name));
        QProcessEnvironment env = isolatedEnvironment(fixture.path());
        env.insert(QStringLiteral("TMPDIR"), fixture.path());
        QString socketPath = fixture.filePath(QStringLiteral("qsoc/agentd.sock"));
        if (runtimeState == QStringLiteral("missing")) {
            env.remove(QStringLiteral("XDG_RUNTIME_DIR"));
            env.remove(QStringLiteral("QT_FATAL_WARNINGS"));
            socketPath = QDir(qtFallback).filePath(QStringLiteral("qsoc/agentd.sock"));
        } else if (runtimeState == QStringLiteral("invalid")) {
            QFile invalidRuntime(fixture.filePath(QStringLiteral("invalid-runtime")));
            QFile invalidFallback(qtFallback);
            QVERIFY(invalidRuntime.open(QIODevice::WriteOnly));
            QVERIFY(invalidFallback.open(QIODevice::WriteOnly));
            env.insert(QStringLiteral("XDG_RUNTIME_DIR"), invalidRuntime.fileName());
            env.remove(QStringLiteral("QT_FATAL_WARNINGS"));
            socketPath = fixture.filePath(
                QStringLiteral("qsoc-%1/agentd.sock").arg(static_cast<qulonglong>(::geteuid())));
        }
        QStringList arguments;
        if (explicitEndpoint) {
            socketPath = fixture.filePath(QStringLiteral("explicit.sock"));
            arguments << QStringLiteral("--socket") << socketPath;
        }
        QProcess daemon;
        daemon.setProcessEnvironment(env);
        daemon.setWorkingDirectory(fixture.path());
        daemon.start(m_daemonPath, arguments);
        QVERIFY(daemon.waitForStarted(5000));
        const bool       listening     = waitForSocket(socketPath, 5000);
        const QByteArray startupErrors = daemon.readAllStandardError();
        QVERIFY2(listening, startupErrors.constData());
        DaemonClient client(socketPath);
        QVERIFY(client.connected());
        QCOMPARE(client.receive().value("protocol").toInt(), 1);
        client.send({{"id", 1}, {"method", "shutdown"}});
        QVERIFY(client.waitForReply(1).value("result").toObject().value("bye").toBool());
        QVERIFY(daemon.waitForFinished(5000));
        QCOMPARE(daemon.exitStatus(), QProcess::NormalExit);
        QCOMPARE(daemon.exitCode(), 0);
        QVERIFY(daemon.readAllStandardOutput().contains(
            QSocLocalEndpoint::resolve(socketPath).toLocal8Bit()));
        const QByteArray errors = startupErrors + daemon.readAllStandardError();
        if (runtimeState == QStringLiteral("valid") || explicitEndpoint) {
            QVERIFY2(errors.isEmpty(), errors.constData());
        } else {
            QVERIFY(errors.contains("QStandardPaths:"));
            for (const QByteArray &line : errors.split('\n')) {
                QVERIFY2(line.isEmpty() || line.startsWith("QStandardPaths:"), errors.constData());
            }
        }
    }
#endif

    void resourcesDoNotStartASession_data()
    {
        QTest::addColumn<bool>("customPolicy");
        QTest::newRow("default-policy") << false;
        QTest::newRow("owner-policy") << true;
    }

    void resourcesDoNotStartASession()
    {
        QFETCH(bool, customPolicy);
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto socketPath = fixture.filePath("resources.sock");
        QProcess   daemon;
        daemon.setProcessEnvironment(isolatedEnvironment(fixture.path()));
        daemon.setWorkingDirectory(fixture.path());
        QStringList arguments{"--socket", socketPath};
        if (customPolicy)
            arguments << "--smt-memory-reserve-mib" << "256" << "--smt-memory-strict";
        daemon.start(m_daemonPath, arguments);
        QVERIFY(daemon.waitForStarted(5000));
        QVERIFY(waitForSocket(socketPath, 5000));
        DaemonClient client(socketPath);
        QVERIFY(client.connected());
        const auto greeting = client.receive();
        QVERIFY(greeting.value("capabilities").toArray().contains("resources"));
        client.send(
            {{"id", 1},
             {"method", "resources"},
             {"params", QJsonObject{{"paths", QJsonArray{"relative"}}}}});
        QVERIFY(client.waitForReply(1).contains("error"));
        client.send({{"id", 2}, {"method", "resources"}, {"params", QJsonArray{}}});
        QVERIFY(client.waitForReply(2).contains("error"));
        client.send(
            {{"id", 3},
             {"method", "resources"},
             {"params", QJsonObject{{"paths", QJsonArray{fixture.path()}}}}});
        const auto result = client.waitForReply(3).value("result").toObject();
        QCOMPARE(result.value("scope").toString(), QStringLiteral("local_daemon"));
        QVERIFY(!result.value("system").toObject().isEmpty());
        const auto smt = result.value("smt").toObject();
        QCOMPARE(
            smt.value("host_reserve_bytes").toInteger(),
            qint64(customPolicy ? 256 : 512) * 1024 * 1024);
        QCOMPARE(smt.value("strict_sampling").toBool(), customPolicy);
        const auto processes = result.value("processes").toArray();
        QVERIFY(!processes.isEmpty());
        QCOMPARE(processes.first().toObject().value("pid").toInteger(), daemon.processId());
        QSet<qint64> pids;
        for (const auto &entry : processes) {
            const auto pid = entry.toObject().value("pid").toInteger();
            QVERIFY(pid > 0);
            QVERIFY(!pids.contains(pid));
            pids.insert(pid);
        }
#ifdef Q_OS_WIN
        const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        QVERIFY(snapshot != INVALID_HANDLE_VALUE);
        QHash<qint64, QString> images;
        PROCESSENTRY32W        entry{};
        entry.dwSize          = sizeof(entry);
        const bool enumerated = ::Process32FirstW(snapshot, &entry);
        if (enumerated) {
            do {
                images.insert(entry.th32ProcessID, QString::fromWCharArray(entry.szExeFile));
            } while (::Process32NextW(snapshot, &entry));
        }
        const DWORD error = ::GetLastError();
        ::CloseHandle(snapshot);
        QVERIFY(enumerated);
        QCOMPARE(error, DWORD(ERROR_NO_MORE_FILES));
        QCOMPARE(images.value(daemon.processId()), QFileInfo(m_daemonPath).fileName());
        for (const auto pid : pids) {
            if (pid == daemon.processId())
                continue;
            const auto image = images.value(pid);
            qInfo().noquote() << "Resource descendant" << pid
                              << (image.isEmpty() ? QStringLiteral("already exited") : image);
            QVERIFY2(
                image.compare(QFileInfo(m_daemonPath).fileName(), Qt::CaseInsensitive) != 0,
                "A resources query started a session or retained its sampling process");
        }
#else
        QCOMPARE(processes.size(), 1);
#endif
        const auto storage = result.value("storage").toArray();
        QCOMPARE(storage.size(), 1);
        QVERIFY(storage.first().toObject().value("valid").toBool());
        client.send({{"id", 4}, {"method", "shutdown"}});
        QVERIFY(client.waitForReply(4).value("result").toObject().value("bye").toBool());
        QVERIFY(daemon.waitForFinished(5000));
        QCOMPARE(daemon.exitCode(), 0);
        const auto errors = daemon.readAllStandardError();
        QVERIFY2(errors.isEmpty(), errors.constData());
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

    void shutdownSurvivesSessionDisconnects()
    {
        for (int round = 0; round < 32; ++round) {
            QTemporaryDir fixture;
            QVERIFY(fixture.isValid());
            const QString socketPath = fixture.filePath("daemon.sock");
            QProcess      daemon;
            daemon.setProcessEnvironment(isolatedEnvironment(fixture.path()));
            daemon.start(m_daemonPath, {"--socket", socketPath});
            QVERIFY(daemon.waitForStarted(5000));
            QVERIFY(waitForSocket(socketPath, 5000));
            {
                DaemonClient disconnected(socketPath);
                QVERIFY(disconnected.connected());
                QVERIFY(!disconnected.receive().isEmpty());
                disconnected.send({{"id", 1}, {"method", "status"}});
                QVERIFY(!disconnected.waitForReply(1).isEmpty());
            }
            DaemonClient control(socketPath);
            QVERIFY(control.connected());
            QVERIFY(!control.receive().isEmpty());
            if (round % 2) {
                control.send({{"id", 1}, {"method", "status"}});
                QVERIFY(!control.waitForReply(1).isEmpty());
            }
            control.send({{"id", 2}, {"method", "shutdown"}});
            QVERIFY(control.waitForReply(2).value("result").toObject().value("bye").toBool());
            QVERIFY(daemon.waitForFinished(5000));
            QCOMPARE(daemon.exitStatus(), QProcess::NormalExit);
            QCOMPARE(daemon.exitCode(), 0);
        }
    }

    void shutdownClosesOtherLiveSessions()
    {
        for (int round = 0; round < 5; ++round) {
            QTemporaryDir fixture;
            QVERIFY(fixture.isValid());
            const QString socketPath = fixture.filePath("daemon.sock");
            QProcess      daemon;
            daemon.setProcessEnvironment(isolatedEnvironment(fixture.path()));
            daemon.start(m_daemonPath, {"--socket", socketPath});
            QVERIFY(daemon.waitForStarted(5000));
            QVERIFY(waitForSocket(socketPath, 5000));
            DaemonClient active(socketPath);
            QVERIFY(active.connected());
            QVERIFY(!active.receive().isEmpty());
            active.send({{"id", 1}, {"method", "status"}});
            QVERIFY(!active.waitForReply(1).isEmpty());
            DaemonClient control(socketPath);
            QVERIFY(control.connected());
            QVERIFY(!control.receive().isEmpty());
            control.send({{"id", 1}, {"method", "shutdown"}});
            QVERIFY(control.waitForReply(1).value("result").toObject().value("bye").toBool());
            QVERIFY(daemon.waitForFinished(5000));
            QCOMPARE(daemon.exitStatus(), QProcess::NormalExit);
            QCOMPARE(daemon.exitCode(), 0);
        }
    }

    void incompleteFramesShareABoundedBudget()
    {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const QString socketPath = fixture.filePath("daemon.sock");
        QProcess      daemon;
        daemon.setProcessEnvironment(isolatedEnvironment(fixture.path()));
        daemon.start(m_daemonPath, {"--socket", socketPath});
        QVERIFY(daemon.waitForStarted(5000));
        QVERIFY(waitForSocket(socketPath, 5000));
        QList<std::shared_ptr<QLocalSocket>> sockets;
        for (int i = 0; i < 5; ++i) {
            auto socket = std::make_shared<QLocalSocket>();
            socket->connectToServer(QSocLocalEndpoint::resolve(socketPath));
            QVERIFY(socket->waitForConnected(5000));
            QVERIFY(socket->waitForReadyRead(5000));
            socket->readAll();
            const int bytes = (i < 4 ? 15 : 5) * 1024 * 1024;
            socket->write(QByteArray("01000000") + QByteArray(bytes, ' '));
            QDeadlineTimer deadline(10000);
            while (socket->bytesToWrite() && !deadline.hasExpired()
                   && socket->state() == QLocalSocket::ConnectedState)
                socket->waitForBytesWritten(100);
            if (i < 4) {
                QCOMPARE(socket->bytesToWrite(), qint64(0));
                QCOMPARE(socket->state(), QLocalSocket::ConnectedState);
            } else if (socket->state() == QLocalSocket::ConnectedState) {
                QVERIFY(socket->waitForDisconnected(5000));
            }
            sockets.append(socket);
        }
        QCOMPARE(sockets.last()->state(), QLocalSocket::UnconnectedState);
        for (const auto &socket : sockets)
            socket->abort();
        QTest::qWait(100);
        DaemonClient client(socketPath);
        QVERIFY(client.connected());
        QVERIFY(!client.receive().isEmpty());
        client.send({{"id", 1}, {"method", "shutdown"}});
        QVERIFY(!client.waitForReply(1).isEmpty());
        QVERIFY(daemon.waitForFinished(5000));
    }

    void smtRequestsDoNotRequireAnAgentSession()
    {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const QString socketPath = fixture.filePath("daemon.sock");
        QProcess      daemon;
        daemon.setProcessEnvironment(isolatedEnvironment(fixture.path()));
        daemon.start(m_daemonPath, {"--socket", socketPath});
        QVERIFY(daemon.waitForStarted(5000));
        QVERIFY(waitForSocket(socketPath, 5000));
        DaemonClient client(socketPath);
        QVERIFY(client.connected());
        const auto hello = client.receive();
        QVERIFY(hello.value("capabilities").toArray().contains("smt"));
        QCOMPARE(hello.value("pid").toDouble(), static_cast<double>(daemon.processId()));
        client.send(
            {{"id", 1},
             {"method", "smt.solve"},
             {"params", QJsonObject{{"smtlib", "(assert true)"}, {"mode", "check"}}}});
        client.send(
            {{"id", 2},
             {"method", "smt.solve"},
             {"params", QJsonObject{{"smtlib", "(assert false)"}, {"mode", "check"}}}});
        QHash<int, QJsonObject> replies;
        for (int i = 0; i < 2; ++i) {
            const auto response = client.receive();
            QVERIFY2(!response.isEmpty(), qPrintable(client.error()));
            replies.insert(response.value("id").toInt(), response.value("result").toObject());
        }
        QCOMPARE(replies.value(1).value("execution").toString(), "completed");
        QCOMPARE(replies.value(1).value("solver_status").toString(), "sat");
        QCOMPARE(replies.value(2).value("execution").toString(), "completed");
        QCOMPARE(replies.value(2).value("solver_status").toString(), "unsat");
        client.send(
            {{"id", 3}, {"method", "smt.cancel"}, {"params", QJsonObject{{"request_id", 1}}}});
        QCOMPARE(client.waitForReply(3).value("result").toObject().value("canceled").toBool(), false);
        client.send({{"id", 4}, {"method", "shutdown"}});
        QVERIFY(!client.waitForReply(4).isEmpty());
        QVERIFY(daemon.waitForFinished(5000));
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
