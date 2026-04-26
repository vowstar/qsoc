// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/mcp/qsocmcpstdio.h"
#include "agent/mcp/qsocmcptransport.h"
#include "agent/mcp/qsocmcptypes.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QSignalSpy>
#include <QtCore>
#include <QtTest>

namespace {

McpServerConfig shellConfig(const QString &script)
{
    McpServerConfig cfg;
    cfg.name    = "fake";
    cfg.type    = "stdio";
    cfg.command = "/bin/sh";
    cfg.args    = QStringList{"-c", script};
    return cfg;
}

QString frameLiteral(const QString &payload)
{
    return QString("printf 'Content-Length: %1\\r\\n\\r\\n%2'").arg(payload.size()).arg(payload);
}

bool waitForState(const QSocMcpTransport *transport, QSocMcpTransport::State target, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (transport->state() != target) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    }
    return true;
}

bool waitForSignal(QSignalSpy &spy, int minCount, int timeoutMs)
{
    QElapsedTimer timer;
    timer.start();
    while (spy.size() < minCount) {
        if (timer.elapsed() > timeoutMs) {
            return false;
        }
        QCoreApplication::processEvents(QEventLoop::AllEvents, 25);
    }
    return true;
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        static auto                   argc      = 1;
        static char                   appName[] = "qsoc_test";
        static std::array<char *, 1>  argv      = {{appName}};
        static const QCoreApplication app(argc, argv.data());
    }

    void receivesSingleFrame()
    {
        const QString payload = R"({"jsonrpc":"2.0","id":1,"result":{"ok":true}})";
        const QString script  = frameLiteral(payload);

        QSocMcpStdioTransport transport(shellConfig(script));
        QSignalSpy            startedSpy(&transport, &QSocMcpTransport::started);
        QSignalSpy            messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        QVERIFY(waitForSignal(startedSpy, 1, 2000));
        QVERIFY(waitForSignal(messageSpy, 1, 2000));
        QVERIFY(waitForSignal(closedSpy, 1, 2000));

        const auto message = messageSpy.first().first().value<nlohmann::json>();
        QCOMPARE(QString::fromStdString(message["jsonrpc"].get<std::string>()), QStringLiteral("2.0"));
        QCOMPARE(message["id"].get<int>(), 1);
        QVERIFY(message["result"]["ok"].get<bool>());
    }

    void receivesMultipleFramesInOneFlush()
    {
        const QString p1     = R"({"id":1,"x":"a"})";
        const QString p2     = R"({"id":2,"x":"b"})";
        const QString p3     = R"({"id":3,"x":"c"})";
        const QString script = frameLiteral(p1) + " && " + frameLiteral(p2) + " && "
                               + frameLiteral(p3);

        QSocMcpStdioTransport transport(shellConfig(script));
        QSignalSpy            messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        QVERIFY(waitForSignal(closedSpy, 1, 2000));
        QCOMPARE(messageSpy.size(), qsizetype(3));

        QCOMPARE(messageSpy.at(0).first().value<nlohmann::json>()["id"].get<int>(), 1);
        QCOMPARE(messageSpy.at(1).first().value<nlohmann::json>()["id"].get<int>(), 2);
        QCOMPARE(messageSpy.at(2).first().value<nlohmann::json>()["id"].get<int>(), 3);
    }

    void reassemblesFramesAcrossReads()
    {
        /* Sleep mid-frame so the kernel pipe almost certainly fragments
         * the read into two ready-read chunks; the parser must hold
         * partial state across them. */
        const QString payload = R"({"id":42,"jsonrpc":"2.0"})";
        const QString script  = QString(
                                    "printf 'Content-Length: %1\\r\\n'; "
                                    "sleep 0.1; "
                                    "printf '\\r\\n'; "
                                    "sleep 0.1; "
                                    "printf '%2'")
                                    .arg(payload.size())
                                    .arg(payload);

        QSocMcpStdioTransport transport(shellConfig(script));
        QSignalSpy            messageSpy(&transport, &QSocMcpTransport::messageReceived);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        QVERIFY(waitForSignal(closedSpy, 1, 3000));
        QCOMPARE(messageSpy.size(), qsizetype(1));
        QCOMPARE(messageSpy.first().first().value<nlohmann::json>()["id"].get<int>(), 42);
    }

    void emitsErrorWhenContentLengthMissing()
    {
        const QString script = QStringLiteral(
            "printf 'Content-Type: application/json\\r\\n\\r\\n{\\\"id\\\":1}'");

        QSocMcpStdioTransport transport(shellConfig(script));
        QSignalSpy            errorSpy(&transport, &QSocMcpTransport::errorOccurred);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        QVERIFY(waitForSignal(errorSpy, 1, 2000));
        QVERIFY(waitForSignal(closedSpy, 1, 2000));
    }

    void stopTerminatesProcess()
    {
        QSocMcpStdioTransport transport(shellConfig("sleep 30"));
        QSignalSpy            startedSpy(&transport, &QSocMcpTransport::started);
        QSignalSpy            closedSpy(&transport, &QSocMcpTransport::closed);

        transport.start();
        QVERIFY(waitForSignal(startedSpy, 1, 2000));
        transport.stop();
        QVERIFY(waitForSignal(closedSpy, 1, 2000));
        QVERIFY(waitForState(&transport, QSocMcpTransport::State::Stopped, 1000));
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocmcpstdio.moc"
