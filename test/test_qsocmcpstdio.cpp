// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcpstdio.h"
#include "agent/mcp/qsocmcpstdioparser_p.h"
#include "agent/mcp/qsocmcptransport.h"
#include "agent/mcp/qsocmcptypes.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QDir>
#include <QFileInfo>
#include <QPointer>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QString peerProgram()
{
    QString name = QStringLiteral("qsoc_mcp_stdio_peer");
#ifdef Q_OS_WIN
    name += QStringLiteral(".exe");
#endif
    return QDir(QCoreApplication::applicationDirPath()).filePath(name);
}

QString unicodeText()
{
    QString text = QStringLiteral("caf");
    text.append(QChar(0x00e9));
    return text;
}

McpServerConfig defaultFramingPeerConfig(const QString &mode, const QStringList &extraArgs = {})
{
    McpServerConfig config;
    config.name    = QStringLiteral("peer");
    config.type    = QStringLiteral("stdio");
    config.command = peerProgram();
    config.args    = QStringList{mode} + extraArgs;
    return config;
}

McpServerConfig peerConfig(const QString &mode, const QStringList &extraArgs = {})
{
    McpServerConfig config = defaultFramingPeerConfig(mode, extraArgs);
    config.stdioFraming    = McpStdioFraming::Newline;
    return config;
}

bool waitForCount(QSignalSpy &spy, qsizetype count, int timeoutMs = 10000)
{
    const bool reached = QTest::qWaitFor([&spy, count]() { return spy.size() >= count; }, timeoutMs);
    if (!reached) {
        qWarning().noquote() << QStringLiteral(
                                    "Timed out waiting for %1 emission(s) of %2; observed %3")
                                    .arg(count)
                                    .arg(QString::fromLatin1(spy.signal()))
                                    .arg(spy.size());
    }
    return reached;
}

bool waitForNoProcesses(QSocMcpStdioTransport &transport, int timeoutMs = 10000)
{
    const bool stopped = QTest::qWaitFor(
        [&transport]() { return transport.findChildren<QProcess *>().isEmpty(); }, timeoutMs);
    if (!stopped) {
        qWarning() << "Timed out waiting for child process cleanup; transport state"
                   << static_cast<int>(transport.state());
    }
    return stopped;
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        const QFileInfo peer(peerProgram());
        QVERIFY2(peer.isFile(), qPrintable(QStringLiteral("Missing stdio peer: %1").arg(peer.path())));
    }

    void newlineParserEnforcesInboundLimit()
    {
        QByteArray oversized(9, 'x');
        qsizetype  scanOffset = 0;
        auto result = QSocMcpStdioInternal::takeLine(oversized, scanOffset, qsizetype(8), false);
        QCOMPARE(result.status, QSocMcpStdioInternal::LineStatus::TooLarge);

        oversized.append('\n');
        scanOffset = 0;
        result     = QSocMcpStdioInternal::takeLine(oversized, scanOffset, qsizetype(8), false);
        QCOMPARE(result.status, QSocMcpStdioInternal::LineStatus::TooLarge);

        QByteArray exact(8, 'x');
        exact.append('\r');
        scanOffset = 0;
        result     = QSocMcpStdioInternal::takeLine(exact, scanOffset, qsizetype(8), false);
        QCOMPARE(result.status, QSocMcpStdioInternal::LineStatus::NeedMore);
        exact.append('\n');
        result = QSocMcpStdioInternal::takeLine(exact, scanOffset, qsizetype(8), false);
        QCOMPARE(result.status, QSocMcpStdioInternal::LineStatus::Message);
        QCOMPARE(result.message, QByteArray(8, 'x'));
        QVERIFY(exact.isEmpty());
    }

    void newlineParserReportsUnterminatedEnd()
    {
        QByteArray buffer     = QByteArrayLiteral("{\"jsonrpc\":\"2.0\"}");
        qsizetype  scanOffset = 0;
        const auto result = QSocMcpStdioInternal::takeLine(buffer, scanOffset, qsizetype(64), true);
        QCOMPARE(result.status, QSocMcpStdioInternal::LineStatus::Unterminated);
    }

    void newlineWireRoundTripsUtf8()
    {
        QSocMcpStdioTransport transport(peerConfig(QStringLiteral("echo")));
        QSignalSpy            startedSpy(&transport, &QSocMcpTransport::started);
        QSignalSpy            messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy            errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);

        nlohmann::json sent;
        sent["jsonrpc"]        = "2.0";
        sent["id"]             = 7;
        sent["method"]         = "echo";
        sent["params"]["text"] = unicodeText().toUtf8().toStdString();

        transport.start();
        QVERIFY(waitForCount(startedSpy, 1));
        transport.sendMessage(sent);
        QVERIFY(waitForCount(messageSpy, 1));
        QVERIFY(waitForCount(closedSpy, 1));
        QVERIFY(waitForNoProcesses(transport));

        QCOMPARE(messageSpy.first().first().value<nlohmann::json>(), sent);
        QCOMPARE(startedSpy.size(), qsizetype(1));
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QCOMPARE(closedSpy.size(), qsizetype(1));
        QCOMPARE(transport.state(), QSocMcpTransport::State::Stopped);
    }

    void legacyContentLengthWireRemainsCompatible()
    {
        const McpServerConfig config = defaultFramingPeerConfig(QStringLiteral("legacy-echo"));
        QCOMPARE(config.stdioFraming, McpStdioFraming::ContentLength);

        QSocMcpStdioTransport transport(config);
        QSignalSpy            startedSpy(&transport, &QSocMcpTransport::started);
        QSignalSpy            messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy            errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);
        const nlohmann::json  sent = {{"jsonrpc", "2.0"}, {"id", 9}, {"method", "echo"}};

        transport.start();
        QVERIFY(waitForCount(startedSpy, 1));
        transport.sendMessage(sent);
        QVERIFY(waitForCount(messageSpy, 1));
        QVERIFY(waitForCount(closedSpy, 1));
        QVERIFY(waitForNoProcesses(transport));

        QCOMPARE(messageSpy.first().first().value<nlohmann::json>(), sent);
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QCOMPARE(closedSpy.size(), qsizetype(1));
    }

    void legacyContentLengthReceivesMultipleMessages()
    {
        QSocMcpStdioTransport transport(defaultFramingPeerConfig(QStringLiteral("legacy-multiple")));
        QSignalSpy messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        QVERIFY(waitForCount(closedSpy, 1));
        QVERIFY(waitForNoProcesses(transport));

        QCOMPARE(messageSpy.size(), qsizetype(3));
        QCOMPARE(messageSpy.at(0).first().value<nlohmann::json>()["id"].get<int>(), 1);
        QCOMPARE(messageSpy.at(1).first().value<nlohmann::json>()["id"].get<int>(), 2);
        QCOMPARE(messageSpy.at(2).first().value<nlohmann::json>()["id"].get<int>(), 3);
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QCOMPARE(closedSpy.size(), qsizetype(1));
    }

    void legacyContentLengthReassemblesSplitFrame()
    {
        QSocMcpStdioTransport transport(defaultFramingPeerConfig(QStringLiteral("legacy-split")));
        QSignalSpy            messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy            errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);
        bool                  released = false;

        transport.start();
        QProcess *process = transport.findChild<QProcess *>();
        QVERIFY(process != nullptr);
        connect(process, &QProcess::readyReadStandardOutput, &transport, [&]() {
            if (released) {
                return;
            }
            QCOMPARE(process->bytesAvailable(), qint64(0));
            released = true;
            transport.sendMessage({{"gate", "open"}});
        });

        QVERIFY(waitForCount(closedSpy, 1));
        QVERIFY(waitForNoProcesses(transport));
        QVERIFY(released);
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QCOMPARE(messageSpy.size(), qsizetype(1));
        QCOMPARE(messageSpy.first().first().value<nlohmann::json>()["id"].get<int>(), 42);
        QCOMPARE(closedSpy.size(), qsizetype(1));
    }

    void legacyContentLengthRejectsMissingHeader()
    {
        QSocMcpStdioTransport transport(defaultFramingPeerConfig(QStringLiteral("legacy-invalid")));
        QSignalSpy            messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy            errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        QVERIFY(waitForCount(closedSpy, 1));
        QVERIFY(waitForNoProcesses(transport));

        QCOMPARE(messageSpy.size(), qsizetype(0));
        QCOMPARE(errorSpy.size(), qsizetype(1));
        QVERIFY(errorSpy.first().first().toString().contains(QStringLiteral("framing")));
        QCOMPARE(closedSpy.size(), qsizetype(1));
    }

    void acceptsCrLfDelimiter()
    {
        QSocMcpStdioTransport transport(peerConfig(QStringLiteral("single-crlf")));
        QSignalSpy            messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy            errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        QVERIFY(waitForCount(closedSpy, 1));
        QVERIFY(waitForNoProcesses(transport));

        QCOMPARE(messageSpy.size(), qsizetype(1));
        QCOMPARE(
            QString::fromStdString(messageSpy.first()
                                       .first()
                                       .value<nlohmann::json>()["result"]["text"]
                                       .get<std::string>()),
            unicodeText());
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QCOMPARE(closedSpy.size(), qsizetype(1));
    }

    void receivesMultipleMessages()
    {
        QSocMcpStdioTransport transport(peerConfig(QStringLiteral("multiple")));
        QSignalSpy            messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy            errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        QVERIFY(waitForCount(closedSpy, 1));
        QVERIFY(waitForNoProcesses(transport));

        QCOMPARE(messageSpy.size(), qsizetype(3));
        QCOMPARE(messageSpy.at(0).first().value<nlohmann::json>()["id"].get<int>(), 1);
        QCOMPARE(messageSpy.at(1).first().value<nlohmann::json>()["id"].get<int>(), 2);
        QCOMPARE(messageSpy.at(2).first().value<nlohmann::json>()["id"].get<int>(), 3);
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QCOMPARE(closedSpy.size(), qsizetype(1));
    }

    void reassemblesSplitUtf8Deterministically()
    {
        QSocMcpStdioTransport transport(peerConfig(QStringLiteral("split")));
        QSignalSpy            messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy            errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);
        bool                  released = false;

        transport.start();
        QProcess *process = transport.findChild<QProcess *>();
        QVERIFY(process != nullptr);
        connect(process, &QProcess::readyReadStandardOutput, &transport, [&]() {
            if (released) {
                return;
            }
            QCOMPARE(process->bytesAvailable(), qint64(0));
            released = true;
            transport.sendMessage({{"gate", "open"}});
        });

        QVERIFY(waitForCount(closedSpy, 1));
        QVERIFY(waitForNoProcesses(transport));
        QVERIFY(released);
        QCOMPARE(messageSpy.size(), qsizetype(1));
        QCOMPARE(messageSpy.first().first().value<nlohmann::json>()["id"].get<int>(), 42);
        QCOMPARE(
            QString::fromStdString(messageSpy.first()
                                       .first()
                                       .value<nlohmann::json>()["result"]["text"]
                                       .get<std::string>()),
            unicodeText());
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QCOMPARE(closedSpy.size(), qsizetype(1));
    }

    void invalidJsonTerminatesExactlyOnce()
    {
        QSocMcpStdioTransport transport(peerConfig(QStringLiteral("invalid")));
        QSignalSpy            errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        QVERIFY(waitForCount(closedSpy, 1));
        QVERIFY(waitForNoProcesses(transport));

        QCOMPARE(errorSpy.size(), qsizetype(1));
        QVERIFY(errorSpy.first().first().toString().contains(QStringLiteral("invalid JSON")));
        QCOMPARE(closedSpy.size(), qsizetype(1));
        QCOMPARE(transport.state(), QSocMcpTransport::State::Stopped);
    }

    void partialMessageAtExitIsRejected()
    {
        QSocMcpStdioTransport transport(peerConfig(QStringLiteral("partial")));
        QSignalSpy            messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy            errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        QVERIFY(waitForCount(closedSpy, 1));
        QVERIFY(waitForNoProcesses(transport));

        QCOMPARE(messageSpy.size(), qsizetype(0));
        QCOMPARE(errorSpy.size(), qsizetype(1));
        QVERIFY(errorSpy.first().first().toString().contains(QStringLiteral("unterminated")));
        QCOMPARE(closedSpy.size(), qsizetype(1));
    }

    void failedStartTerminatesExactlyOnce()
    {
        QTemporaryDir temporaryDir;
        QVERIFY(temporaryDir.isValid());
        McpServerConfig config = peerConfig(QStringLiteral("unused"));
        config.command         = QDir(temporaryDir.path()).filePath(QStringLiteral("missing-peer"));
        config.args.clear();

        QSocMcpStdioTransport transport(config);
        QSignalSpy            startedSpy(&transport, &QSocMcpTransport::started);
        QSignalSpy            errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        QVERIFY(waitForCount(closedSpy, 1));
        QVERIFY(waitForNoProcesses(transport));

        QCOMPARE(startedSpy.size(), qsizetype(0));
        QCOMPARE(errorSpy.size(), qsizetype(1));
        QVERIFY(errorSpy.first().first().toString().contains(QStringLiteral("'peer'")));
        QCOMPARE(closedSpy.size(), qsizetype(1));
        QCOMPARE(transport.state(), QSocMcpTransport::State::Stopped);
    }

    void crashTerminatesExactlyOnce()
    {
        QSocMcpStdioTransport transport(peerConfig(QStringLiteral("crash")));
        QSignalSpy            startedSpy(&transport, &QSocMcpTransport::started);
        QSignalSpy            errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        QVERIFY(waitForCount(closedSpy, 1));
        QVERIFY(waitForNoProcesses(transport));

        QCOMPARE(startedSpy.size(), qsizetype(1));
        QCOMPARE(errorSpy.size(), qsizetype(1));
        QVERIFY(errorSpy.first().first().toString().contains(QStringLiteral("crashed")));
        QCOMPARE(closedSpy.size(), qsizetype(1));
        QCOMPARE(transport.state(), QSocMcpTransport::State::Stopped);
    }

    void nonzeroExitTerminatesExactlyOnce()
    {
        QSocMcpStdioTransport transport(peerConfig(QStringLiteral("nonzero")));
        QSignalSpy            errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        QVERIFY(waitForCount(closedSpy, 1));
        QVERIFY(waitForNoProcesses(transport));

        QCOMPARE(errorSpy.size(), qsizetype(1));
        QVERIFY(errorSpy.first().first().toString().contains(QStringLiteral("code 23")));
        QCOMPARE(closedSpy.size(), qsizetype(1));
    }

    void writeFailureTerminatesExactlyOnce()
    {
        QSocMcpStdioTransport transport(peerConfig(QStringLiteral("close-input")));
        QSignalSpy            messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy            errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);
        connect(&transport, &QSocMcpTransport::messageReceived, &transport, [&](const auto &) {
            transport.sendMessage({{"jsonrpc", "2.0"}, {"id", 9}, {"method", "ping"}});
        });

        transport.start();
        QVERIFY(waitForCount(closedSpy, 1));
        QVERIFY(waitForNoProcesses(transport));

        QCOMPARE(messageSpy.size(), qsizetype(1));
        QCOMPARE(errorSpy.size(), qsizetype(1));
        QCOMPARE(closedSpy.size(), qsizetype(1));
        QCOMPARE(transport.state(), QSocMcpTransport::State::Stopped);
    }

    void stopIsAsynchronousAndExactOnce()
    {
        QSocMcpStdioTransport transport(peerConfig(QStringLiteral("ignore-term")));
        QSignalSpy            startedSpy(&transport, &QSocMcpTransport::started);
        QSignalSpy            errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        QVERIFY(waitForCount(startedSpy, 1));
        transport.stop();
        QCOMPARE(closedSpy.size(), qsizetype(0));
        QCOMPARE(transport.state(), QSocMcpTransport::State::Stopping);
        QVERIFY(waitForCount(closedSpy, 1));
        QVERIFY(waitForNoProcesses(transport));

        QCOMPARE(errorSpy.size(), qsizetype(0));
        QCOMPARE(closedSpy.size(), qsizetype(1));
        QCOMPARE(transport.state(), QSocMcpTransport::State::Stopped);
    }

    void stopDuringStartNeverReturnsToRunning()
    {
        QSocMcpStdioTransport transport(peerConfig(QStringLiteral("ignore")));
        QSignalSpy            startedSpy(&transport, &QSocMcpTransport::started);
        QSignalSpy            errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        const qsizetype startedBeforeStop = startedSpy.size();
        transport.stop();
        QCOMPARE(transport.state(), QSocMcpTransport::State::Stopping);
        QVERIFY(waitForCount(closedSpy, 1));
        QVERIFY(waitForNoProcesses(transport));

        QCOMPARE(startedSpy.size(), startedBeforeStop);
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QCOMPARE(closedSpy.size(), qsizetype(1));
        QCOMPARE(transport.state(), QSocMcpTransport::State::Stopped);
    }

    void restartDropsPreviousPartialMessage()
    {
        QTemporaryDir temporaryDir;
        QVERIFY(temporaryDir.isValid());
        const QString marker = QDir(temporaryDir.path()).filePath(QStringLiteral("stage"));

        QSocMcpStdioTransport transport(peerConfig(QStringLiteral("restart"), {marker}));
        QSignalSpy            startedSpy(&transport, &QSocMcpTransport::started);
        QSignalSpy            messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy            errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);
        int                   closedCount = 0;
        connect(
            &transport,
            &QSocMcpTransport::closed,
            &transport,
            [&]() {
                closedCount++;
                if (closedCount == 1) {
                    transport.start();
                }
            },
            Qt::DirectConnection);

        transport.start();
        QVERIFY(waitForCount(closedSpy, 2, 5000));
        QVERIFY(waitForNoProcesses(transport));

        QCOMPARE(startedSpy.size(), qsizetype(2));
        QCOMPARE(messageSpy.size(), qsizetype(1));
        QCOMPARE(messageSpy.first().first().value<nlohmann::json>()["id"].get<int>(), 99);
        QCOMPARE(errorSpy.size(), qsizetype(1));
        QCOMPARE(closedSpy.size(), qsizetype(2));
        QCOMPARE(transport.state(), QSocMcpTransport::State::Stopped);
    }

    void stopFromMessageDropsBufferedMessages()
    {
        QSocMcpStdioTransport transport(peerConfig(QStringLiteral("two-hold")));
        QSignalSpy            messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy            errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);
        connect(&transport, &QSocMcpTransport::messageReceived, &transport, [&](const auto &) {
            if (messageSpy.size() == 1) {
                transport.stop();
            }
        });

        transport.start();
        QVERIFY(waitForCount(closedSpy, 1));
        QVERIFY(waitForNoProcesses(transport));

        QCOMPARE(messageSpy.size(), qsizetype(1));
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QCOMPARE(closedSpy.size(), qsizetype(1));
    }

    void deleteFromMessageIsSafe()
    {
        auto *transport = new QSocMcpStdioTransport(peerConfig(QStringLiteral("two-hold")));
        QPointer<QSocMcpStdioTransport> guard(transport);
        QSignalSpy                      messageSpy(transport, &QSocMcpTransport::messageReceived);
        QSignalSpy                      errorSpy(transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy                      closedSpy(transport, &QSocMcpTransport::closed);
        connect(transport, &QSocMcpTransport::messageReceived, transport, [&](const auto &) {
            delete transport;
            transport = nullptr;
        });

        transport->start();
        QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(), 3000);
        QCOMPARE(messageSpy.size(), qsizetype(1));
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QCOMPARE(closedSpy.size(), qsizetype(0));
    }

    void deleteFromFinishedTailIsSafe()
    {
        auto *transport = new QSocMcpStdioTransport(peerConfig(QStringLiteral("single-crlf")));
        QPointer<QSocMcpStdioTransport> guard(transport);
        QSignalSpy                      messageSpy(transport, &QSocMcpTransport::messageReceived);
        QSignalSpy                      errorSpy(transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy                      closedSpy(transport, &QSocMcpTransport::closed);
        connect(transport, &QSocMcpTransport::messageReceived, transport, [&](const auto &) {
            delete transport;
            transport = nullptr;
        });

        transport->start();
        QProcess *process = transport->findChild<QProcess *>();
        QVERIFY(process != nullptr);
        QVERIFY(disconnect(process, &QProcess::readyReadStandardOutput, transport, nullptr));

        QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(), 3000);
        QCOMPARE(messageSpy.size(), qsizetype(1));
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QCOMPARE(closedSpy.size(), qsizetype(0));
    }

    void deleteFromErrorIsSafe()
    {
        auto *transport = new QSocMcpStdioTransport(peerConfig(QStringLiteral("invalid")));
        QPointer<QSocMcpStdioTransport> guard(transport);
        QSignalSpy                      errorSpy(transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy                      closedSpy(transport, &QSocMcpTransport::closed);
        connect(transport, &QSocMcpTransport::errorOccurred, transport, [&](const QString &) {
            delete transport;
            transport = nullptr;
        });

        transport->start();
        QTRY_VERIFY_WITH_TIMEOUT(guard.isNull(), 3000);
        QCOMPARE(errorSpy.size(), qsizetype(1));
        QCOMPARE(closedSpy.size(), qsizetype(0));
    }

    void destructorIsSilent()
    {
        auto      *transport = new QSocMcpStdioTransport(peerConfig(QStringLiteral("ignore")));
        QSignalSpy startedSpy(transport, &QSocMcpTransport::started);
        QSignalSpy errorSpy(transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy closedSpy(transport, &QSocMcpTransport::closed);

        transport->start();
        QVERIFY(waitForCount(startedSpy, 1));
        QPointer<QProcess> process(transport->findChild<QProcess *>());
        QVERIFY(!process.isNull());
        delete transport;

        QVERIFY(process.isNull());
        QCOMPARE(errorSpy.size(), qsizetype(0));
        QCOMPARE(closedSpy.size(), qsizetype(0));
    }

    void oversizedOutboundMessageIsRejected()
    {
        QSocMcpStdioTransport transport(peerConfig(QStringLiteral("ignore")));
        QSignalSpy            startedSpy(&transport, &QSocMcpTransport::started);
        QSignalSpy            errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        QVERIFY(waitForCount(startedSpy, 1));
        transport.sendMessage({{"payload", std::string(64 * 1024 * 1024, 'x')}});
        QVERIFY(waitForCount(closedSpy, 1));
        QVERIFY(waitForNoProcesses(transport));

        QCOMPARE(errorSpy.size(), qsizetype(1));
        QVERIFY(errorSpy.first().first().toString().contains(QStringLiteral("64 MiB")));
        QCOMPARE(closedSpy.size(), qsizetype(1));
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocmcpstdio.moc"
