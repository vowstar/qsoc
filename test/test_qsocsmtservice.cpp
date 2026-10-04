// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocipc.h"
#include "common/qsoclocalendpoint.h"
#include "common/qsocprocesslimits.h"
#include "qsoc_test.h"
#include "smt/qsocsmtinput.h"
#include "smt/qsocsmtservice.h"

#include <future>
#include <limits>
#include <thread>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace {

bool processIsRunning(qint64 pid)
{
#ifdef Q_OS_WIN
    const HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr)
        return false;
    const bool running = ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    ::CloseHandle(process);
    return running;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
}

QJsonObject request(const QString &source, bool optimize = false)
{
    return {{"smtlib", source}, {"mode", optimize ? "optimize" : "check"}};
}

QJsonObject solve(const QString &source, bool optimize = false)
{
    return QSocSmtService::solve(request(source, optimize), {}, QStringLiteral(QSOC_SMT_WORKER_PATH));
}

QJsonObject probe(
    const QString &mode, int timeout = 100, std::stop_token stop = {}, bool optimize = false)
{
    auto input
        = request("; " + mode + "\n(assert true)" + (optimize ? "(maximize 1)" : ""), optimize);
    input.insert("timeout_ms", timeout);
    return QSocSmtService::solve(input, stop, QStringLiteral(QSOC_SMT_PROBE_PATH));
}

class Test final : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { QVERIFY(QSocSmtService::supported()); }

    void startupExitDiagnostics_data()
    {
        QTest::addColumn<QByteArray>("mode");
        QTest::addColumn<QString>("status");
        QTest::newRow("normal") << QByteArray("exit") << QStringLiteral("normal");
        QTest::newRow("crash") << QByteArray("crash") << QStringLiteral("crash");
    }

    void startupExitDiagnostics()
    {
        QFETCH(QByteArray, mode);
        QFETCH(QString, status);
        const auto previous = qgetenv("QSOC_TEST_SMT_STARTUP");
        const auto restore  = qScopeGuard([previous] {
            if (previous.isNull())
                qunsetenv("QSOC_TEST_SMT_STARTUP");
            else
                qputenv("QSOC_TEST_SMT_STARTUP", previous);
        });
        qputenv("QSOC_TEST_SMT_STARTUP", mode);
        const auto result  = probe("startup", 2000);
        const auto details = QJsonDocument(result).toJson(QJsonDocument::Compact);
        QVERIFY2(result.value("execution").toString() == "error", details.constData());
        const auto reason = result.value("reason").toString();
        QVERIFY2(reason.startsWith("Worker exited before connecting"), details.constData());
        QVERIFY2(reason.contains("exit status: " + status), details.constData());
        if (mode == "exit")
            QVERIFY2(reason.contains("exit code: 23"), details.constData());
        else
            QVERIFY2(reason.contains("exit code: "), details.constData());
        QVERIFY(QSocSmtService::isValidResponse(result));
    }

#ifdef Q_OS_MACOS
    void finiteInheritedAddressLimit()
    {
        const auto previous = qgetenv("QSOC_TEST_SMT_STARTUP");
        const auto restore  = qScopeGuard([previous] {
            if (previous.isNull())
                qunsetenv("QSOC_TEST_SMT_STARTUP");
            else
                qputenv("QSOC_TEST_SMT_STARTUP", previous);
        });
        qputenv("QSOC_TEST_SMT_STARTUP", "finite-cap");
        const auto result  = probe("probe-inherited-limit", 5000);
        const auto details = QJsonDocument(result).toJson(QJsonDocument::Compact);
        QVERIFY2(result.value("execution").toString() == "completed", details.constData());
        QVERIFY2(result.value("solver_status").toString() == "sat", details.constData());
        QVERIFY(QSocSmtService::isValidResponse(result));
        const quint64     initial  = result.value("probe_initial_virtual").toString().toULongLong();
        const quint64     original = result.value("probe_original_hard").toString().toULongLong();
        const quint64     hard     = result.value("probe_inherited_hard").toString().toULongLong();
        const quint64     soft     = result.value("probe_installed_soft").toString().toULongLong();
        const quint64     installed = result.value("probe_installed_hard").toString().toULongLong();
        constexpr quint64 budget    = QSocSmtService::memoryLimitMiB * 1024ULL * 1024ULL;
        QVERIFY2(initial >= 2, details.constData());
        QVERIFY2(initial <= (std::numeric_limits<quint64>::max() - budget) / 2, details.constData());
        const quint64 target = initial + budget;
        QVERIFY2(hard > target, details.constData());
        QVERIFY2(hard < 2 * initial + budget, details.constData());
        QVERIFY2(hard <= original, details.constData());
        QVERIFY2(soft > 0, details.constData());
        QCOMPARE(soft, installed);
        QVERIFY2(installed < hard, details.constData());
        const auto exhausted = probe("probe-memory", 5000);
        const auto failure   = QJsonDocument(exhausted).toJson(QJsonDocument::Compact);
        QVERIFY2(exhausted.value("execution").toString() == "resource_limit", failure.constData());
    }
#endif

    void repeatedWorkerStartup()
    {
        auto input = request("(assert true)");
        input.insert("timeout_ms", 2000);
        for (int iteration = 0; iteration < 64; ++iteration) {
            const auto result
                = QSocSmtService::solve(input, {}, QStringLiteral(QSOC_SMT_WORKER_PATH));
            const auto details = QByteArray::number(iteration) + ": "
                                 + QJsonDocument(result).toJson(QJsonDocument::Compact);
            QVERIFY2(result.value("execution").toString() == "completed", details.constData());
            QVERIFY2(result.value("solver_status").toString() == "sat", details.constData());
        }
    }

    void lexicalRejections_data()
    {
        QTest::addColumn<QByteArray>("source");
        QTest::newRow("nul") << QByteArray("(assert true)\0(assert false)", 28);
        QTest::newRow("control") << QByteArray("(assert\x01 true)");
        QTest::newRow("trailing") << QByteArray("(assert true) trailing");
        QTest::newRow("extra-close") << QByteArray("(assert true))");
        QTest::newRow("unterminated")
            << QByteArray("(assert (= \"value \"\"inside\"\" \" \"x\"))(");
        QTest::newRow("quoted-command") << QByteArray("(|assert| true)");
        QTest::newRow("include") << QByteArray("(include \"formula.smt2\")");
        QTest::newRow("recursive") << QByteArray("(define-fun-rec f ((x Int)) Int (f x))");
        QTest::newRow("check") << QByteArray("(check-sat)");
        QTest::newRow("logic-twice") << QByteArray("(set-logic QF_LIA)(set-logic QF_LIA)");
        QTest::newRow("logic-late") << QByteArray("(assert true)(set-logic QF_LIA)");
        QTest::newRow("nested-name") << QByteArray("(assert (and (! true :named a) true))");
        QTest::newRow("duplicate-name")
            << QByteArray("(assert (! true :named a))(assert (! false :named |a|))");
        QTest::newRow("depth") << QByteArray(65, '(') + QByteArray(65, ')');
        QTest::newRow("utf8") << QByteArray("(assert |\xff|)");
    }

    void lexicalRejections()
    {
        QFETCH(QByteArray, source);
        QVERIFY(!QSocSmtInput::scan(source, false).error.isEmpty());
        QVERIFY(!QSocSmtInput::scan(source, true).error.isEmpty());
    }

    void lexicalStringsAndComments()
    {
        const QByteArray source
            = "; (set-option :regular-output-channel \"file\")\n"
              "(declare-const |x;y()| Int)"
              "(assert (= |x;y()| 1))"
              "(assert (= \"quoted \"\"value\"\" ; ()\" \"quoted \"\"value\"\" ; ()\"))";
        QVERIFY(QSocSmtInput::scan(source, false).error.isEmpty());
        QCOMPARE(
            solve(QString::fromUtf8(source)).value("solver_status").toString(),
            QStringLiteral("sat"));
    }

    void unsafeCommandsCannotWrite()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto path = directory.filePath(QStringLiteral("solver-output.txt"));
        const auto source
            = QStringLiteral("(set-option :regular-output-channel \"%1\")(assert true)").arg(path);
        for (const bool optimize : {false, true}) {
            const auto result = solve(source + (optimize ? "(maximize 1)" : ""), optimize);
            QCOMPARE(result.value("execution").toString(), QStringLiteral("error"));
            QVERIFY(!QFile::exists(path));
        }
    }

    void validationAndModes()
    {
        auto input = request("(assert true)");
        input.insert("priority", "lex");
        QVERIFY(!QSocSmtService::validateRequest(input).isEmpty());
        input = request("(maximize 1)", true);
        input.insert("priority", "pareto");
        QVERIFY(!QSocSmtService::validateRequest(input).isEmpty());
        input.insert("priority", "box");
        QVERIFY(!QSocSmtService::validateRequest(input).isEmpty());
        input.remove("priority");
        input.insert("timeout_ms", 1.5);
        QVERIFY(!QSocSmtService::validateRequest(input).isEmpty());
        input.insert("timeout_ms", 120001);
        QVERIFY(!QSocSmtService::validateRequest(input).isEmpty());
        QCOMPARE(solve("(maximize 1)").value("execution").toString(), QStringLiteral("error"));
        QCOMPARE(solve("(assert true)", true).value("execution").toString(), QStringLiteral("error"));
        QCOMPARE(
            solve(QString("(maximize 1)").repeated(17), true).value("execution").toString(),
            QStringLiteral("error"));
        QCOMPARE(
            solve("(assert (forall ((x Int)) (= x x)))(maximize 1)", true)
                .value("execution")
                .toString(),
            QStringLiteral("error"));
    }

    void coreIncludesBackground_data()
    {
        QTest::addColumn<bool>("optimize");
        QTest::newRow("solver") << false;
        QTest::newRow("optimizer") << true;
    }

    void coreIncludesBackground()
    {
        QFETCH(bool, optimize);
        const QString source
            = "(declare-const x Int)(assert (>= x 1))"
              "(assert (! (<= x 0) :named upper))(assert (! (< x 100) :named unrelated))";
        const auto result = solve(source + (optimize ? "(minimize x)" : ""), optimize);
        QCOMPARE(result.value("solver_status").toString(), QStringLiteral("unsat"));
        QCOMPARE(result.value("unsat_core").toArray(), QJsonArray({"upper"}));
        QVERIFY(result.value("model_smtlib").isNull());
        QCOMPARE(
            solve("(declare-const x Int)(assert (<= x 0))").value("solver_status").toString(),
            QStringLiteral("sat"));
    }

    void lexicographicOrderAndExactBounds()
    {
        const QString background = "(declare-const x Int)(declare-const y Int)"
                                   "(assert (and (>= x 0) (>= y 0) (<= (+ x y) 10)))";
        for (const bool reversed : {false, true}) {
            const QString objectives = reversed ? "(maximize y)(maximize x)"
                                                : "(maximize x)(maximize y)";
            const auto    result     = solve(background + objectives, true);
            QCOMPARE(result.value("optimality").toString(), QStringLiteral("optimal"));
            const auto rows = result.value("objectives").toArray();
            QCOMPARE(rows.size(), 2);
            QCOMPARE(rows[0].toObject().value("model_value").toString(), QStringLiteral("10"));
            QCOMPARE(rows[1].toObject().value("model_value").toString(), QStringLiteral("0"));
            const QString first  = reversed ? "y" : "x";
            const QString second = reversed ? "x" : "y";
            QCOMPARE(
                solve(background + QString("(assert (> %1 10))").arg(first))
                    .value("solver_status")
                    .toString(),
                QStringLiteral("unsat"));
            QCOMPARE(
                solve(background + QString("(assert (= %1 10))(assert (> %2 0))").arg(first, second))
                    .value("solver_status")
                    .toString(),
                QStringLiteral("unsat"));
        }
        const auto mixed   = solve(background + "(minimize x)(maximize y)(maximize 7)", true);
        const auto details = QJsonDocument(mixed).toJson(QJsonDocument::Compact);
        QVERIFY2(mixed.value("execution").toString() == "completed", details.constData());
        QVERIFY2(mixed.value("optimality").toString() == "optimal", details.constData());
        const auto values = mixed.value("objectives").toArray();
        QCOMPARE(values[0].toObject().value("model_value").toString(), QStringLiteral("0"));
        QCOMPARE(values[1].toObject().value("model_value").toString(), QStringLiteral("10"));
        QCOMPARE(values[2].toObject().value("model_value").toString(), QStringLiteral("7"));
    }

    void objectiveKinds_data()
    {
        QTest::addColumn<QString>("source");
        QTest::addColumn<QString>("value");
        QTest::addColumn<QString>("optimality");
        QTest::newRow("negative") << QStringLiteral(
            "(declare-const x Int)(assert (<= x (- 3)))(maximize x)")
                                  << QStringLiteral("-3") << QStringLiteral("optimal");
        QTest::newRow("rational") << QStringLiteral(
            "(declare-const x Real)(assert (<= x (/ 3 2)))(maximize x)")
                                  << QStringLiteral("3/2") << QStringLiteral("optimal");
        QTest::newRow("bitvector") << QStringLiteral("(declare-const x (_ BitVec 8))(maximize x)")
                                   << QStringLiteral("255") << QStringLiteral("optimal");
        QTest::newRow("strict-limit")
            << QStringLiteral("(declare-const x Real)(assert (< x 1))(maximize x)")
            << QStringLiteral("1") << QStringLiteral("limit");
        QTest::newRow("minimum") << QStringLiteral(
            "(declare-const x Int)(assert (>= x (- 3)))(minimize x)")
                                 << QStringLiteral("-3") << QStringLiteral("optimal");
    }

    void objectiveKinds()
    {
        QFETCH(QString, source);
        QFETCH(QString, value);
        QFETCH(QString, optimality);
        const auto result  = solve(source, true);
        const auto details = QJsonDocument(result).toJson(QJsonDocument::Compact);
        QVERIFY2(result.value("optimality").toString() == optimality, details.constData());
        const auto row = result.value("objectives").toArray()[0].toObject();
        QCOMPARE(row.value("lower").toObject().value("rational").toString(), value);
        QCOMPARE(row.value("upper").toObject().value("rational").toString(), value);
        QCOMPARE(
            row.value("upper").toObject().value("epsilon").toString(),
            optimality == "limit" ? QStringLiteral("-1") : QStringLiteral("0"));
    }

    void unboundedDoesNotCompleteLaterObjectives()
    {
        const auto result = solve(
            "(declare-const x Int)(declare-const y Int)"
            "(assert (and (>= y 0) (<= y 10)))(maximize x)(maximize y)",
            true);
        QCOMPARE(result.value("optimality").toString(), QStringLiteral("unbounded"));
        const auto values = result.value("objectives").toArray();
        QCOMPARE(
            values[0].toObject().value("upper").toObject().value("infinity").toString(),
            QStringLiteral("1"));
        QCOMPARE(
            values[1].toObject().value("classification").toString(),
            QStringLiteral("not_classified"));
    }

    void nonlinearBoundsAreNotCertified()
    {
        const auto result = solve("(declare-const x Real)(assert (= (* x x) 2))(maximize x)", true);
        QCOMPARE(result.value("optimality").toString(), QStringLiteral("not_proven"));
        const auto values = result.value("objectives").toArray();
        QCOMPARE(values.size(), 1);
        QVERIFY(!values[0].toObject().value("bounds_proven").toBool());
    }

    void realMemoryLimitAndOutputTruncation()
    {
        const auto exhausted = solve(
            "(declare-const x (_ BitVec 1000000000))"
            "(assert (= x (_ bv0 1000000000)))");
        QCOMPARE(exhausted.value("execution").toString(), QStringLiteral("resource_limit"));
        const auto large = solve(
            "(declare-const x (_ BitVec 5000000))"
            "(assert (= x (_ bv0 5000000)))");
        QCOMPARE(large.value("solver_status").toString(), QStringLiteral("sat"));
        QVERIFY(large.value("truncated").toBool());
        QVERIFY(large.value("model_smtlib").isNull());
        QVERIFY(
            QJsonDocument(large).toJson(QJsonDocument::Compact).size()
            <= QSocSmtService::outputLimit);
        QCOMPARE(solve("(assert true)").value("solver_status").toString(), QStringLiteral("sat"));
    }

    void nulAndResultOptions()
    {
        QString source = QStringLiteral("(assert true)");
        source.append(QChar(0));
        source.append(QStringLiteral("(assert false)"));
        QCOMPARE(solve(source).value("execution").toString(), QStringLiteral("error"));
        auto input = request(QString("(maximize 1)").repeated(16), true);
        input.insert("return_model", false);
        const auto result = QSocSmtService::solve(input, {}, QStringLiteral(QSOC_SMT_WORKER_PATH));
        QCOMPARE(result.value("optimality").toString(), QStringLiteral("optimal"));
        QCOMPARE(result.value("objectives").toArray().size(), 16);
        QVERIFY(result.value("model_smtlib").isNull());
    }

    void isolationFailures_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("execution");
        QTest::newRow("parse-stall") << QStringLiteral("probe-parse") << QStringLiteral("timeout");
        QTest::newRow("solve-stall") << QStringLiteral("probe-solve") << QStringLiteral("timeout");
        QTest::newRow("verification-stall")
            << QStringLiteral("probe-verify") << QStringLiteral("timeout");
        QTest::newRow("serialization-stall")
            << QStringLiteral("probe-serialize") << QStringLiteral("timeout");
        QTest::newRow("memory") << QStringLiteral("probe-memory")
                                << QStringLiteral("resource_limit");
        QTest::newRow("crash") << QStringLiteral("probe-crash") << QStringLiteral("error");
        QTest::newRow("output-limit") << QStringLiteral("probe-output") << QStringLiteral("error");
        QTest::newRow("partial-response")
            << QStringLiteral("probe-partial") << QStringLiteral("error");
    }

    void isolationFailures()
    {
        QFETCH(QString, mode);
        QFETCH(QString, execution);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto    marker = directory.filePath("phase.pid");
        QElapsedTimer elapsed;
        elapsed.start();
        const auto result
            = probe(mode + "\n; probe-ready: " + marker, mode == "probe-memory" ? 5000 : 2000);
        QVERIFY2(
            result.value("execution").toString() == execution,
            QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
        QVERIFY(elapsed.elapsed() < 6000);
        if (execution == "timeout") {
            QFile ready(marker);
            QVERIFY2(ready.open(QIODevice::ReadOnly), "Worker did not reach the stalled phase");
            const auto pid = ready.readAll().toLongLong();
            QVERIFY(pid > 0);
            QVERIFY(!processIsRunning(pid));
        }
        QVERIFY(result.value("solver_status").isNull());
        QCOMPARE(solve("(assert true)").value("solver_status").toString(), QStringLiteral("sat"));
    }

    void optimizePhaseDeadlines()
    {
        for (const auto *phase : {"probe-parse", "probe-solve", "probe-verify", "probe-serialize"}) {
            QTemporaryDir directory;
            QVERIFY(directory.isValid());
            const auto marker = directory.filePath("phase.pid");
            const auto result
                = probe(QString::fromLatin1(phase) + "\n; probe-ready: " + marker, 2000, {}, true);
            QVERIFY2(
                result.value("execution").toString() == "timeout",
                QJsonDocument(result).toJson(QJsonDocument::Compact).constData());
            QFile ready(marker);
            QVERIFY2(ready.open(QIODevice::ReadOnly), "Worker did not reach the stalled phase");
            const auto pid = ready.readAll().toLongLong();
            QVERIFY(pid > 0);
            QVERIFY(!processIsRunning(pid));
        }
    }

    void cancellationAndQueuedCancellation()
    {
        std::stop_source firstStop;
        std::stop_source secondStop;
        auto             first  = std::async(std::launch::async, [&] {
            return probe("probe-stall", 10000, firstStop.get_token());
        });
        auto             second = std::async(std::launch::async, [&] {
            return probe("probe-stall", 10000, secondStop.get_token());
        });
        QTest::qWait(200);
        std::stop_source queuedStop;
        auto             queued = std::async(std::launch::async, [&] {
            return probe("probe-stall", 10000, queuedStop.get_token());
        });
        QTest::qWait(50);
        queuedStop.request_stop();
        QVERIFY(queued.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
        QCOMPARE(queued.get().value("execution").toString(), QStringLiteral("cancelled"));
        firstStop.request_stop();
        secondStop.request_stop();
        QCOMPARE(first.get().value("execution").toString(), QStringLiteral("cancelled"));
        QCOMPARE(second.get().value("execution").toString(), QStringLiteral("cancelled"));
        QCOMPARE(solve("(assert true)").value("solver_status").toString(), QStringLiteral("sat"));
    }

    void runningCancellationReclaimsBothWorkers()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        for (int round = 0; round < 2; ++round) {
            const auto firstMarker = directory.filePath(QStringLiteral("first-%1.pid").arg(round));
            const auto secondMarker = directory.filePath(QStringLiteral("second-%1.pid").arg(round));
            std::stop_source firstStop;
            std::stop_source secondStop;
            auto             first   = std::async(std::launch::async, [&] {
                return probe(
                    "probe-solve\n; probe-ready: " + firstMarker, 20000, firstStop.get_token());
            });
            auto             second  = std::async(std::launch::async, [&] {
                return probe(
                    "probe-solve\n; probe-ready: " + secondMarker, 20000, secondStop.get_token());
            });
            const auto       cleanup = qScopeGuard([&] {
                firstStop.request_stop();
                secondStop.request_stop();
            });
            QTRY_VERIFY_WITH_TIMEOUT(QFile::exists(firstMarker) && QFile::exists(secondMarker), 5000);
            QFile firstFile(firstMarker);
            QFile secondFile(secondMarker);
            QVERIFY(firstFile.open(QIODevice::ReadOnly));
            QVERIFY(secondFile.open(QIODevice::ReadOnly));
            const auto firstPid  = firstFile.readAll().toLongLong();
            const auto secondPid = secondFile.readAll().toLongLong();
            QVERIFY(firstPid > 1 && secondPid > 1 && firstPid != secondPid);
            QVERIFY(processIsRunning(firstPid));
            QVERIFY(processIsRunning(secondPid));
            QElapsedTimer elapsed;
            elapsed.start();
            firstStop.request_stop();
            secondStop.request_stop();
            QVERIFY(first.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
            QVERIFY(second.wait_for(std::chrono::seconds(3)) == std::future_status::ready);
            QVERIFY(elapsed.elapsed() < 4000);
            QCOMPARE(first.get().value("execution").toString(), QStringLiteral("cancelled"));
            QCOMPARE(second.get().value("execution").toString(), QStringLiteral("cancelled"));
            QVERIFY(!processIsRunning(firstPid));
            QVERIFY(!processIsRunning(secondPid));
        }
        QCOMPARE(solve("(assert true)").value("solver_status").toString(), QStringLiteral("sat"));
    }

    void queuedBytesAreBounded()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        std::stop_source                      stop;
        std::vector<std::future<QJsonObject>> calls;
        std::vector<std::jthread>             threads;
        const auto                            cleanup = qScopeGuard([&] { stop.request_stop(); });
        QJsonArray                            results;
        bool                                  busy   = false;
        const auto                            launch = [&](auto function) {
            std::packaged_task<QJsonObject()> task(std::move(function));
            calls.push_back(task.get_future());
            threads.emplace_back(std::move(task));
        };
        const auto collect = [&](bool wait) {
            for (size_t index = 0; index < calls.size(); ++index) {
                auto &call = calls[index];
                if (call.valid()
                    && (wait
                        || call.wait_for(std::chrono::milliseconds(0))
                               == std::future_status::ready)) {
                    const auto result = call.get();
                    busy              = busy || result.value("execution").toString() == "busy";
                    results.append(QJsonObject{{"call", int(index)}, {"result", result}});
                }
            }
        };
        QStringList markers;
        for (int index = 0; index < 2; ++index) {
            const auto marker = directory.filePath(QString::number(index));
            markers.append(marker);
            launch([marker, &stop] {
                return probe("probe-solve\n; probe-ready: " + marker, 20000, stop.get_token());
            });
        }
        QElapsedTimer elapsed;
        elapsed.start();
        while ((!QFile::exists(markers[0]) || !QFile::exists(markers[1]))
               && elapsed.elapsed() < 5000) {
            collect(false);
            if (!results.isEmpty())
                break;
            QTest::qWait(10);
        }
        const bool started = QFile::exists(markers[0]) && QFile::exists(markers[1]);
        if (!started) {
            stop.request_stop();
            collect(true);
        }
        QVERIFY2(started, QJsonDocument(results).toJson(QJsonDocument::Compact).constData());
        QList<qint64> pids;
        for (const auto &marker : markers) {
            QFile file(marker);
            QVERIFY(file.open(QIODevice::ReadOnly));
            const auto pid = file.readAll().toLongLong();
            QVERIFY(pid > 1);
            QVERIFY(processIsRunning(pid));
            pids.append(pid);
        }
        QVERIFY(pids[0] != pids[1]);
        const auto    input = request("; probe-solve\n" + QString(260000, '\n') + "(assert true)");
        constexpr int queuedCalls = 36;
        const auto    queuedBytes = QJsonDocument(input).toJson(QJsonDocument::Compact).size();
        QVERIFY(queuedBytes < 2 * 1024 * 1024);
        QVERIFY(queuedBytes * queuedCalls > 16 * 1024 * 1024);
        for (int i = 0; i < queuedCalls; ++i) {
            launch([input, &stop] {
                return QSocSmtService::solve(
                    input, stop.get_token(), QStringLiteral(QSOC_SMT_PROBE_PATH));
            });
        }
        elapsed.restart();
        while (!busy && elapsed.elapsed() < 5000) {
            collect(false);
            QTest::qWait(10);
        }
        collect(false);
        const bool observedBusy = busy;
        const bool workersHeld  = calls[0].valid() && calls[1].valid();
        stop.request_stop();
        collect(true);
        const auto details = QJsonDocument(results).toJson(QJsonDocument::Compact);
        QVERIFY2(workersHeld, details.constData());
        QVERIFY2(observedBusy, details.constData());
        for (const auto &entry : results) {
            const auto completed = entry.toObject();
            if (completed.value("call").toInt() < 2)
                QVERIFY2(
                    completed.value("result").toObject().value("execution") == "cancelled",
                    details.constData());
        }
        for (const auto pid : pids)
            QVERIFY(!processIsRunning(pid));
        QCOMPARE(solve("(assert true)").value("solver_status").toString(), QStringLiteral("sat"));
    }

    void concurrentWorkerFailureIsContained()
    {
        std::stop_source stop;
        auto             active = std::async(std::launch::async, [&] {
            return probe("probe-stall", 10000, stop.get_token());
        });
        QTest::qWait(100);
        QCOMPARE(probe("probe-crash", 2000).value("execution").toString(), QStringLiteral("error"));
        QCOMPARE(solve("(assert true)").value("solver_status").toString(), QStringLiteral("sat"));
        QVERIFY(active.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
        stop.request_stop();
        QCOMPARE(active.get().value("execution").toString(), QStringLiteral("cancelled"));
    }

    void remoteProtocol_data()
    {
        QTest::addColumn<QString>("mode");
        for (const auto *mode :
             {"success", "cancel", "handshake-cancel", "version", "response", "partial"})
            QTest::newRow(mode) << QString::fromLatin1(mode);
    }

    void remoteProtocol()
    {
        QFETCH(QString, mode);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const auto endpoint = QSocLocalEndpoint::resolve(directory.filePath("remote.sock"));
        QString    error;
        QVERIFY(QSocLocalEndpoint::prepareDirectory(endpoint, &error));
        QLocalServer server;
        server.setSocketOptions(QLocalServer::UserAccessOption);
        QVERIFY(server.listen(endpoint));
        std::stop_source stop;
        auto             pending = std::async(std::launch::async, [&] {
            return QSocSmtService::solveRemote(request("(assert true)"), endpoint, stop.get_token());
        });
        QTRY_VERIFY_WITH_TIMEOUT(server.hasPendingConnections(), 2000);
        std::unique_ptr<QLocalSocket> socket(server.nextPendingConnection());
        if (mode == "handshake-cancel") {
            stop.request_stop();
        } else {
            const auto hello = QSocIpc::frame(
                QJsonObject{
                    {"daemon", "qsoc-agentd"},
                    {"protocol", mode == "version" ? 2 : 1},
                    {"capabilities", QJsonArray{"smt"}}});
            socket->write(hello.first(5));
            socket->flush();
            QTest::qWait(20);
            socket->write(hello.mid(5));
            socket->flush();
        }
        if (mode != "version" && mode != "handshake-cancel") {
            QByteArray  buffer;
            QJsonObject message;
            auto        decoded = QSocIpc::DecodeResult::Incomplete;
            QTRY_VERIFY_WITH_TIMEOUT(
                ([&] {
                    if (decoded == QSocIpc::DecodeResult::Incomplete) {
                        buffer += socket->readAll();
                        decoded = QSocIpc::decode(buffer, message, 2 * 1024 * 1024);
                    }
                    return decoded != QSocIpc::DecodeResult::Incomplete;
                })(),
                2000);
            QCOMPARE(decoded, QSocIpc::DecodeResult::Complete);
            QCOMPARE(message.value("method").toString(), QStringLiteral("smt.solve"));
            QCOMPARE(message.value("params").toObject(), request("(assert true)"));
            if (mode == "cancel") {
                stop.request_stop();
            } else {
                auto result = QSocSmtService::failure("completed", {});
                result.insert("solver_status", "sat");
                result.insert("feasibility", "feasible");
                result.insert("optimality", "not_applicable");
                const auto frame = QSocIpc::frame(
                    QJsonObject{{"id", mode == "response" ? 2 : 1}, {"result", result}});
                socket->write(mode == "partial" ? frame.chopped(1) : frame);
                if (mode == "partial")
                    socket->disconnectFromServer();
                else
                    socket->flush();
            }
        }
        QTRY_VERIFY_WITH_TIMEOUT(
            pending.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready, 2000);
        const auto result = pending.get();
        QCOMPARE(
            result.value("execution").toString(),
            mode == "success"         ? QStringLiteral("completed")
            : mode.endsWith("cancel") ? QStringLiteral("cancelled")
                                      : QStringLiteral("error"));
        QTRY_COMPARE(socket->state(), QLocalSocket::UnconnectedState);
    }

    void stdinTransportRemainsCompatible()
    {
        QSocProcessLimits limits;
        QProcess          process;
        QVERIFY(limits.configure(process, QSocSmtService::memoryLimitMiB * 1024ULL * 1024ULL));
        process.start(QStringLiteral(QSOC_SMT_WORKER_PATH), {});
        QVERIFY(process.waitForStarted(2000));
        process.write(QJsonDocument(request("(assert true)")).toJson(QJsonDocument::Compact));
        process.closeWriteChannel();
        QVERIFY(process.waitForFinished(5000));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        const auto result = QJsonDocument::fromJson(process.readAllStandardOutput()).object();
        QCOMPARE(result.value("solver_status").toString(), QStringLiteral("sat"));
    }

    void hardMemoryLimitIsInstalled()
    {
#ifdef Q_OS_LINUX
        QProcess process;
        process.start(QStringLiteral(QSOC_SMT_WORKER_PATH), {});
        QVERIFY(process.waitForStarted());
        const auto bytes = static_cast<quint64>(QSocSmtService::memoryLimitMiB) * 1024 * 1024;
        const QRegularExpression expected(
            QStringLiteral("Max address space\\s+%1\\s+%1").arg(bytes));
        QElapsedTimer elapsed;
        elapsed.start();
        bool installed = false;
        while (!installed && elapsed.elapsed() < 2000) {
            QFile limits(QStringLiteral("/proc/%1/limits").arg(process.processId()));
            if (limits.open(QIODevice::ReadOnly)) {
                installed = expected.match(QString::fromLatin1(limits.readAll())).hasMatch();
            }
            QTest::qWait(5);
        }
        process.kill();
        QVERIFY(process.waitForFinished(2000));
        QVERIFY(installed);
#endif
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocsmtservice.moc"
