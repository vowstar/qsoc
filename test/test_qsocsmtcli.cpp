// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/client/qsocagentdaemonclient.h"
#include "cli/qsoccliworker.h"
#include "common/qsocconsole.h"
#include "common/qsocinterrupt.h"
#include "common/qsocipc.h"
#include "common/qsoclocalendpoint.h"
#include "qsoc_test.h"
#include "smt/qsocsmtservice.h"
#include <QBuffer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QScopeGuard>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>
#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <csignal>
#endif

namespace {
bool alive(qint64 pid)
{
    if (pid <= 0)
        return false;
#ifdef Q_OS_WIN
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process)
        return false;
    const bool running = WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    CloseHandle(process);
    return running;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0;
#endif
}

qint64 markerPid(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().toLongLong() : 0;
}

class Test : public QObject
{
    Q_OBJECT
private:
    QTemporaryDir temporary;
    QBuffer       output;
    QByteArray    oldBinDirectory;

    QString formula(const QByteArray &source, const QString &name = "input.smt2")
    {
        const QString path = temporary.filePath(name);
        QFile         file(path);
        if (!file.open(QIODevice::WriteOnly) || file.write(source) != source.size())
            return {};
        return path;
    }

    QJsonObject result() const { return QJsonDocument::fromJson(output.data()).object(); }

    void configureStdinProcess(QProcess &process)
    {
        auto environment = QProcessEnvironment::systemEnvironment();
        for (const auto *key :
             {"HOME", "USERPROFILE", "QSOC_HOME", "XDG_CONFIG_HOME", "TMPDIR", "TEMP", "TMP"})
            environment.insert(key, temporary.path());
        environment.remove("QSOC_SMT_SOCKET");
        process.setProcessEnvironment(environment);
        process.setWorkingDirectory(temporary.path());
        process.setProgram(QStringLiteral(QSOC_BINARY_PATH));
    }

private slots:
    void init()
    {
        QVERIFY(temporary.isValid());
        output.setData({});
        QVERIFY(output.open(QIODevice::ReadWrite));
        QSocConsole::setOutputDevice(&output);
        oldBinDirectory = qgetenv("QSOC_BIN_DIR");
        qputenv("QSOC_BIN_DIR", QFileInfo(QStringLiteral(QSOC_AGENTD_PATH)).absolutePath().toUtf8());
    }
    void cleanup()
    {
        QSocConsole::setOutputDevice(nullptr);
        output.close();
        if (oldBinDirectory.isNull())
            qunsetenv("QSOC_BIN_DIR");
        else
            qputenv("QSOC_BIN_DIR", oldBinDirectory);
        QSocInterrupt::finishForegroundHandoff();
    }
    void ownedSolve_data()
    {
        QTest::addColumn<QByteArray>("source");
        QTest::addColumn<QString>("status");
        QTest::newRow("sat") << QByteArray("(declare-const x Int)(assert (= x 1))")
                             << QString("sat");
        QTest::newRow("unsat") << QByteArray("(assert false)") << QString("unsat");
    }
    void ownedSolve()
    {
        QFETCH(QByteArray, source);
        QFETCH(QString, status);
        QSocCliWorker cli;
        QSignalSpy    done(&cli, &QSocCliWorker::exit);
        const QString input = formula(source);
        QVERIFY(!input.isEmpty());
        cli.setup({"qsoc", "smt", input}, true);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
        QCOMPARE(done.first().first().toInt(), 0);
        QCOMPARE(result().value("execution").toString(), QString("completed"));
        QCOMPARE(result().value("solver_status").toString(), status);
    }
    void stdinClosedPipe_data()
    {
        QTest::addColumn<QByteArray>("source");
        QTest::addColumn<int>("expectedExit");
        QTest::addColumn<QString>("expectedStatus");
        QTest::addColumn<QString>("expectedReason");
        const QByteArray tail         = "(assert false)";
        const QString    invalidBytes = "SMT-LIB input exceeds 256 KiB or is not valid UTF-8.";
        QTest::newRow("delayed-blocks")
            << QByteArray("(assert true)") + QByteArray(32768, ' ') + tail << 0 << QString("unsat")
            << QString();
        QTest::newRow("exact-limit")
            << QByteArray(QSocSmtService::inputLimit - tail.size(), ' ') + tail << 0
            << QString("unsat") << QString();
        QTest::newRow("over-limit")
            << QByteArray(QSocSmtService::inputLimit + 1, ' ') << 2 << QString() << invalidBytes;
        QTest::newRow("invalid-utf8")
            << QByteArray(1, char(0xff)) << 2 << QString() << invalidBytes;
        QTest::newRow("empty") << QByteArray() << 2 << QString()
                               << QString("Invalid SMT input size or NUL byte");
        QTest::newRow("ctrl-z-prefix") << QByteArray("(assert true)") + char(0x1a) + tail << 2
                                       << QString() << QString("Unsupported control character");
        QTest::newRow("crlf-byte-limit")
            << QByteArray("(assert true)")
                   + QByteArray("\r\n").repeated(QSocSmtService::inputLimit / 2)
            << 2 << QString() << invalidBytes;
    }
    void stdinClosedPipe()
    {
        QFETCH(QByteArray, source);
        QFETCH(int, expectedExit);
        QFETCH(QString, expectedStatus);
        QFETCH(QString, expectedReason);
        QProcess process;
        configureStdinProcess(process);
        const auto cleanup = qScopeGuard([&] {
            if (process.state() != QProcess::NotRunning) {
                process.kill();
                process.waitForFinished(5000);
            }
        });
        process.setArguments({"smt", "-"});
        process.start();
        QVERIFY(process.waitForStarted(5000));
        for (qsizetype offset = 0; offset < source.size(); offset += 4096) {
            const QByteArray block = source.mid(offset, 4096);
            QCOMPARE(process.write(block), block.size());
            while (process.bytesToWrite() > 0)
                QVERIFY(process.waitForBytesWritten(5000));
            if (offset == 0 && source.size() > 4096)
                QVERIFY(!process.waitForFinished(50));
        }
        process.closeWriteChannel();
        QVERIFY(process.waitForFinished(15000));
        const QByteArray stdoutBytes = process.readAllStandardOutput();
        const QByteArray stderrBytes = process.readAllStandardError();
        const QByteArray diagnostic  = stdoutBytes + '\n' + stderrBytes;
        QVERIFY2(process.exitStatus() == QProcess::NormalExit, diagnostic.constData());
        QVERIFY2(process.exitCode() == expectedExit, diagnostic.constData());
        const auto value = QJsonDocument::fromJson(stdoutBytes).object();
        QVERIFY2(
            value.value("execution").toString() == (expectedExit == 0 ? "completed" : "error"),
            diagnostic.constData());
        QCOMPARE(value.value("solver_status").toString(), expectedStatus);
        if (!expectedReason.isEmpty())
            QCOMPARE(value.value("reason").toString(), expectedReason);
        QVERIFY2(value.value("reason").toString() != "Unknown error", diagnostic.constData());
    }
    void invalidModeLeavesStdinOpen()
    {
        QProcess process;
        configureStdinProcess(process);
        const auto cleanup = qScopeGuard([&] {
            if (process.state() != QProcess::NotRunning) {
                process.kill();
                process.waitForFinished(5000);
            }
        });
        process.setArguments({"smt", "--mode", "unsupported", "-"});
        process.start();
        QVERIFY(process.waitForStarted(5000));
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitCode(), 2);
        const auto value = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        QCOMPARE(value.value("reason").toString(), QString("mode must be check or optimize."));
    }
    void invalidModePrecedesInput()
    {
        QSocCliWorker cli;
        QSignalSpy    done(&cli, &QSocCliWorker::exit);
        cli.setup({"qsoc", "smt", "--mode", "unsupported", temporary.filePath("absent.smt2")}, true);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
        QCOMPARE(done.first().first().toInt(), 2);
        QCOMPARE(result().value("reason").toString(), QString("mode must be check or optimize."));
    }
    void invalidInput_data()
    {
        QTest::addColumn<QByteArray>("source");
        QTest::newRow("invalid-utf8") << QByteArray(1, char(0xff));
        QTest::newRow("oversize") << QByteArray(256 * 1024 + 1, 'x');
        QTest::newRow("unsafe-command") << QByteArray("(set-option :timeout 1)");
    }
    void invalidInput()
    {
        QFETCH(QByteArray, source);
        QSocCliWorker cli;
        QSignalSpy    done(&cli, &QSocCliWorker::exit);
        const QString input = formula(source);
        QVERIFY(!input.isEmpty());
        cli.setup({"qsoc", "smt", input}, true);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
        QCOMPARE(done.first().first().toInt(), 2);
        QCOMPARE(result().value("execution").toString(), QString("error"));
    }
    void daemonFailure_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<int>("expectedExit");
        QTest::newRow("disconnected") << QString("disconnected") << 2;
        QTest::newRow("malformed-result") << QString("malformed") << 2;
        QTest::newRow("worker-timeout") << QString("timeout") << 124;
    }
    void daemonFailure()
    {
        QFETCH(QString, kind);
        QFETCH(int, expectedExit);
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        const QString endpoint = QSocLocalEndpoint::resolve(temporary.filePath("failure.sock"));
        QVERIFY(server.listen(endpoint));
        int        requests = 0;
        QByteArray received;
        connect(&server, &QLocalServer::newConnection, &server, [&] {
            auto *socket = server.nextPendingConnection();
            socket->write(
                QSocIpc::frame(
                    QJsonObject{
                        {"daemon", "qsoc-agentd"},
                        {"protocol", 1},
                        {"capabilities", QJsonArray{"smt"}}}));
            connect(socket, &QLocalSocket::readyRead, &server, [&, socket] {
                received += socket->readAll();
                QJsonObject request;
                if (QSocIpc::decode(received, request) != QSocIpc::DecodeResult::Complete)
                    return;
                ++requests;
                if (kind == "disconnected") {
                    socket->disconnectFromServer();
                    return;
                }
                const QJsonObject value
                    = kind == "timeout"
                          ? QSocSmtService::failure("timeout", "Worker deadline exceeded.")
                          : QJsonObject{{"execution", "completed"}};
                socket->write(
                    QSocIpc::frame(QJsonObject{{"id", request.value("id")}, {"result", value}}));
            });
        });
        QSocCliWorker cli;
        QSignalSpy    done(&cli, &QSocCliWorker::exit);
        const QString input = formula("(assert true)");
        QVERIFY(!input.isEmpty());
        cli.setup({"qsoc", "smt", "--connect", endpoint, input}, true);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 5000);
        QCOMPARE(done.first().first().toInt(), expectedExit);
        QCOMPARE(requests, 1);
        QCOMPARE(
            result().value("execution").toString(),
            kind == "timeout" ? QString("timeout") : QString("error"));
        QVERIFY(server.isListening());
    }
    void cancelledOwnerLeavesOtherTaskRunning()
    {
        QTemporaryDir binaries;
        QVERIFY(binaries.isValid());
        const QString workerName = QFileInfo(QStringLiteral(QSOC_SMT_WORKER_PATH)).fileName();
        const QString workerPath = binaries.filePath(workerName);
        QVERIFY(QFile::copy(QStringLiteral(QSOC_SMT_PROBE_PATH), workerPath));
        QVERIFY(
            QFile::setPermissions(
                workerPath, QFile::permissions(QStringLiteral(QSOC_SMT_PROBE_PATH))));
        QProcess daemon;
        auto     environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QSOC_BIN_DIR", binaries.path());
        daemon.setProcessEnvironment(environment);
        daemon.setWorkingDirectory(temporary.path());
        const QString endpoint = temporary.filePath("daemon.sock");
        daemon.start(
            QStringLiteral(QSOC_AGENTD_PATH),
            {"--socket", endpoint, "--smt-memory-reserve-mib", "0"});
        const auto stop = qScopeGuard([&] {
            daemon.kill();
            daemon.waitForFinished(5000);
        });
        QVERIFY(daemon.waitForStarted());
        QSocAgentDaemonClient other(endpoint);
        QTRY_VERIFY_WITH_TIMEOUT(other.connectToDaemon(100), 5000);
        const QString ownMarker   = temporary.filePath("own.ready");
        const QString otherMarker = temporary.filePath("other.ready");
        const auto    source      = [](const QString &marker) {
            return QString("; probe-ready: %1\n(assert true)").arg(marker);
        };
        QSignalSpy   otherReplies(&other, &QSocAgentDaemonClient::replyReceived);
        const qint64 otherId = other.nextId();
        other.send(
            {{"id", otherId},
             {"method", "smt.solve"},
             {"params", QJsonObject{{"smtlib", source(otherMarker)}, {"timeout_ms", 20000}}}});
        QTRY_VERIFY_WITH_TIMEOUT(markerPid(otherMarker) > 0, 5000);
        QTimer cancel;
        bool   interrupted = false;
        connect(&cancel, &QTimer::timeout, this, [&] {
            if (markerPid(ownMarker) > 0 && !interrupted) {
                interrupted = true;
                QSocInterrupt::request();
            }
        });
        cancel.start(10);
        QSocCliWorker cli;
        QSignalSpy    done(&cli, &QSocCliWorker::exit);
        const QString input = formula(source(ownMarker).toUtf8());
        QVERIFY(!input.isEmpty());
        cli.setup({"qsoc", "smt", "--connect", endpoint, "--timeout-ms", "20000", input}, true);
        QTRY_COMPARE_WITH_TIMEOUT(done.size(), 1, 15000);
        cancel.stop();
        QVERIFY2(interrupted, output.data().constData());
        QCOMPARE(done.first().first().toInt(), 130);
        QCOMPARE(result().value("execution").toString(), QString("cancelled"));
        QVERIFY(!alive(markerPid(ownMarker)));
        QVERIFY(alive(markerPid(otherMarker)));
        QCOMPARE(daemon.state(), QProcess::Running);
        QCOMPARE(otherReplies.size(), 0);
        other.send(
            {{"id", other.nextId()},
             {"method", "smt.cancel"},
             {"params", QJsonObject{{"request_id", otherId}}}});
        QTRY_VERIFY_WITH_TIMEOUT(!alive(markerPid(otherMarker)), 5000);
        QTRY_VERIFY_WITH_TIMEOUT(otherReplies.size() >= 2, 5000);
    }
};
} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocsmtcli.moc"
