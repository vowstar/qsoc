// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "cli/qsocresourceformat.h"
#include "common/config.h"
#include "common/qsocconsole.h"
#include "common/qsocipc.h"
#include "common/qsoclocalendpoint.h"
#include "qsoc_test.h"
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>

#include <QBuffer>
#include <QStringList>
#include <QtCore>
#include <QtTest>

#include <array>
#include <cstdio>

namespace {

class Test : public QObject
{
    Q_OBJECT

private:
    QBuffer outBuffer;
    QBuffer errBuffer;

    void resetCapture()
    {
        outBuffer.close();
        errBuffer.close();
        outBuffer.setData(QByteArray());
        errBuffer.setData(QByteArray());
        outBuffer.open(QIODevice::ReadWrite);
        errBuffer.open(QIODevice::ReadWrite);
        QSocConsole::setOutputDevice(&outBuffer);
        QSocConsole::setErrorDevice(&errBuffer);
    }

    QString captured()
    {
        QSocConsole::out().flush();
        QSocConsole::err().flush();
        return QString::fromUtf8(outBuffer.data()) + QString::fromUtf8(errBuffer.data());
    }

private slots:
    void cleanupTestCase()
    {
        QSocConsole::setOutputDevice(nullptr);
        QSocConsole::setErrorDevice(nullptr);
    }

    void resourceSummaryKeepsMetricsSeparate()
    {
        const QJsonObject report{
            {"status", "partial"},
            {"system",
             QJsonObject{
                 {"memory_available_bytes", qint64(2) * 1024 * 1024 * 1024},
                 {"memory_total_bytes", qint64(8) * 1024 * 1024 * 1024},
                 {"memory_available_kind", "estimate"},
                 {"memory_effective_available_bytes", 1}}},
            {"processes",
             QJsonArray{
                 QJsonObject{
                     {"pid", 42},
                     {"resident_bytes", 32 * 1024 * 1024},
                     {"private_commit_bytes", 99 * 1024 * 1024},
                     {"cpu_time_ns", qint64(1500000000)}},
                 QJsonObject{{"pid", 43}, {"resident_bytes", 64 * 1024 * 1024}}}},
            {"smt",
             QJsonObject{{"active_count", 1}, {"queued_count", 2}, {"admission", "waiting_memory"}}},
            {"storage",
             QJsonArray{QJsonObject{
                 {"path", "/workspace"},
                 {"valid", true},
                 {"available_bytes", 0},
                 {"read_only", true}}}}};
        const auto result = QSocResourceFormat::summary(report, 42);
        QVERIFY(result.contains("2.0 GiB available (estimate) / 8.0 GiB total"));
        QVERIFY(result.contains("Daemon RSS: 32.0 MiB | CPU time: 1.50 s"));
        QVERIFY(result.contains("Observed descendants: 1"));
        QVERIFY(result.contains("SMT: 1 active, 2 queued, waiting_memory"));
        QVERIFY(result.contains("Disk /workspace: 0 B available (read-only)"));
        QVERIFY(!result.contains("99.0 MiB"));
        QVERIFY(!result.contains('%'));
    }

    void resourceSummaryPreservesUnknownAndBoundsOutput()
    {
        QJsonArray volumes;
        for (int i = 0; i < 12; ++i)
            volumes.append(
                QJsonObject{
                    {"path", QString(100, 'x') + '\n'}, {"valid", false}, {"available_bytes", 100}});
        const auto result = QSocResourceFormat::summary(
            {{"status", "unknown"}, {"storage", volumes}, {"reason", "first\nsecond"}}, 42);
        QVERIFY(result.contains("Host RAM: unknown available / unknown total"));
        QVERIFY(result.contains("Daemon RSS: unknown | CPU time: unknown"));
        QVERIFY(result.contains("Observed descendants: unknown"));
        QVERIFY(!result.contains("0 B"));
        QVERIFY(result.contains("Reason: first second"));
        QCOMPARE(result.count('\n'), 14);
    }

    void resourceQueryUsesOnlyLocalDaemonProtocol_data()
    {
        QTest::addColumn<bool>("supported");
        QTest::addColumn<bool>("remote");
        QTest::addColumn<QString>("status");
        QTest::newRow("local") << true << false << QStringLiteral("ok");
        QTest::newRow("remote") << true << true << QStringLiteral("partial");
        QTest::newRow("unsupported") << false << false << QStringLiteral("ok");
        QTest::newRow("busy") << true << false << QStringLiteral("busy");
    }

    void resourceQueryUsesOnlyLocalDaemonProtocol()
    {
        QFETCH(bool, supported);
        QFETCH(bool, remote);
        QFETCH(QString, status);
        resetCapture();
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = QSocLocalEndpoint::resolve(directory.filePath("resources.sock"));
        QString    error;
        QVERIFY(QSocLocalEndpoint::prepareDirectory(path, &error));
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        QVERIFY(server.listen(path));
        QStringList methods;
        QJsonArray  paths;
        connect(&server, &QLocalServer::newConnection, &server, [&] {
            auto *socket = server.nextPendingConnection();
            socket->write(
                QSocIpc::frame(
                    QJsonObject{
                        {"daemon", "qsoc-agentd"},
                        {"protocol", 1},
                        {"capabilities", supported ? QJsonArray{"resources"} : QJsonArray{}}}));
            connect(
                socket,
                &QLocalSocket::readyRead,
                socket,
                [&, socket, buffer = QByteArray{}]() mutable {
                    buffer += socket->readAll();
                    QJsonObject request;
                    while (QSocIpc::decode(buffer, request) == QSocIpc::DecodeResult::Complete) {
                        methods.append(request.value("method").toString());
                        paths = request.value("params").toObject().value("paths").toArray();
                        socket->write(
                            QSocIpc::frame(
                                QJsonObject{
                                    {"id", request.value("id")},
                                    {"result",
                                     QJsonObject{
                                         {"scope", "local_daemon"},
                                         {"status", status},
                                         {"system", QJsonObject{}},
                                         {"processes", QJsonArray{}},
                                         {"storage", QJsonArray{}},
                                         {"smt", QJsonObject{}}}}}));
                    }
                });
        });
        QSocCliWorker worker;
        QSignalSpy    exitSpy(&worker, &QSocCliWorker::exit);
        QStringList   arguments{
            "qsoc",
            "agent",
            "--resources",
            "--connect",
            path,
            "-d",
            directory.path(),
            "--workspace",
            remote ? QStringLiteral("/workspace/remote") : directory.path()};
        if (remote)
            arguments << "--ssh" << "resource-test";
        worker.setup(arguments, true);
        QTRY_COMPARE(exitSpy.size(), 1);
        QCOMPARE(exitSpy.first().first().toInt(), !supported || status == "busy" ? 1 : 0);
        if (!supported) {
            QVERIFY(methods.isEmpty());
            QVERIFY(captured().contains("unsupported"));
            return;
        }
        QCOMPARE(methods, QStringList{"resources"});
        QCOMPARE(paths, remote ? QJsonArray{} : QJsonArray{QDir::cleanPath(directory.path())});
        QSocConsole::out().flush();
        const auto result = QJsonDocument::fromJson(outBuffer.data()).object();
        QCOMPARE(result.value("scope").toString(), "local_daemon");
        QCOMPARE(result.value("status").toString(), status);
    }

    void resourceQueryRejectsModelQueries()
    {
        resetCapture();
        QSocCliWorker worker;
        QSignalSpy    exitSpy(&worker, &QSocCliWorker::exit);
        worker.setup({"qsoc", "agent", "--resources", "--query", "do not run"}, true);
        QTRY_COMPARE(exitSpy.size(), 1);
        QCOMPARE(exitSpy.first().first().toInt(), 2);
        QVERIFY(captured().contains("cannot be combined"));
    }

    void explicitArgumentsIgnoreHostArguments()
    {
        resetCapture();
        QSocCliWorker socCliWorker;
        QSignalSpy    exitSpy(&socCliWorker, &QSocCliWorker::exit);
        socCliWorker.setup({"qsoc", "gui"}, true);

        QTRY_COMPARE(exitSpy.count(), 1);
        QCOMPARE(exitSpy.takeFirst().at(0).toInt(), 0);
        QVERIFY(!captured().contains("host-only"));
    }

    void invalidGuiOptionIsReported()
    {
        resetCapture();
        QSocCliWorker socCliWorker;
        QSignalSpy    exitSpy(&socCliWorker, &QSocCliWorker::exit);
        socCliWorker.setup({"qsoc", "gui", "--bogus"}, true);

        QTRY_COMPARE(exitSpy.count(), 1);
        QCOMPARE(exitSpy.takeFirst().at(0).toInt(), 1);
        QVERIFY(captured().contains("Unknown option 'bogus'"));
    }

    void optionH()
    {
        resetCapture();
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments = {
                "qsoc",
                "-h",
            };
            socCliWorker.setup(appArguments, true);
            socCliWorker.run();
        }
        const QString text = captured();
        QVERIFY2(text.contains("Usage: qsoc [options]"), qPrintable(text));
    }

    void optionHelp()
    {
        resetCapture();
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments = {
                "qsoc",
                "--help",
            };
            socCliWorker.setup(appArguments, true);
            socCliWorker.run();
        }
        QVERIFY(captured().contains("Usage: qsoc [options]"));
    }

    void optionVerbose()
    {
        resetCapture();
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments = {
                "qsoc",
                "--verbose=10",
            };
            socCliWorker.setup(appArguments, true);
            socCliWorker.run();
        }
        const QString text = captured();
        QVERIFY(text.contains("Error: invalid log level: 10"));
        QVERIFY(text.contains("QSoC " QSOC_VERSION));
        QVERIFY(text.contains("Usage: qsoc [options]"));
    }

    void optionV()
    {
        resetCapture();
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments = {
                "qsoc",
                "-v",
            };
            socCliWorker.setup(appArguments, true);
            socCliWorker.run();
        }
        QVERIFY(captured().contains("QSoC " QSOC_VERSION));
    }

    void optionVersion()
    {
        resetCapture();
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments = {
                "qsoc",
                "--version",
            };
            socCliWorker.setup(appArguments, true);
            socCliWorker.run();
        }
        QVERIFY(captured().contains("QSoC " QSOC_VERSION));
    }
};

} // namespace

int main(int argc, char *argv[])
{
    int                    hostArgc     = 2;
    char                   hostName[]   = "qsoc";
    char                   hostOption[] = "--host-only";
    std::array<char *, 2>  hostArgv{{hostName, hostOption}};
    const QCoreApplication application(hostArgc, hostArgv.data());
    Test                   testCase;
    const int              result = QTest::qExec(&testCase, argc, argv);
    fprintf(stderr, "Tests completed with result: %d\n", result);
    _exit(result ? 1 : 0);
    return result;
}

#include "test_qsoccliworker.moc"
