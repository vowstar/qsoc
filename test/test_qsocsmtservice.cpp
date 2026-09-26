// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocsmtinput.h"
#include "common/qsocsmtservice.h"
#include "qsoc_test.h"

#include <future>
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QRegularExpression>
#include <QTemporaryDir>
#include <QtTest>

namespace {

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
    void initTestCase()
    {
        if (!QSocSmtService::supported()) {
            QSKIP("Hard memory isolation is unavailable on this platform");
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
        const auto mixed = solve(background + "(minimize x)(maximize y)(maximize 7)", true);
        QCOMPARE(mixed.value("optimality").toString(), QStringLiteral("optimal"));
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
        const auto result = solve(source, true);
        QCOMPARE(result.value("optimality").toString(), optimality);
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
        QElapsedTimer elapsed;
        elapsed.start();
        const auto result = probe(mode, mode == "probe-memory" ? 5000 : 100);
        QCOMPARE(result.value("execution").toString(), execution);
        QVERIFY(elapsed.elapsed() < 6000);
        QVERIFY(result.value("solver_status").isNull());
        QCOMPARE(solve("(assert true)").value("solver_status").toString(), QStringLiteral("sat"));
    }

    void optimizePhaseDeadlines()
    {
        for (const auto *phase : {"probe-parse", "probe-solve", "probe-verify", "probe-serialize"}) {
            QCOMPARE(
                probe(phase, 100, {}, true).value("execution").toString(),
                QStringLiteral("timeout"));
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

    void queuedBytesAreBounded()
    {
        std::stop_source stop;
        auto input = request("; probe-solve\n" + QString(260000, '\n') + "(assert true)");
        std::vector<std::future<QJsonObject>> calls;
        for (int i = 0; i < 36; ++i) {
            calls.push_back(std::async(std::launch::async, [input, &stop] {
                return QSocSmtService::solve(
                    input, stop.get_token(), QStringLiteral(QSOC_SMT_PROBE_PATH));
            }));
        }
        bool          busy = false;
        QElapsedTimer elapsed;
        elapsed.start();
        while (!busy && elapsed.elapsed() < 5000) {
            for (auto &call : calls) {
                if (call.valid()
                    && call.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) {
                    busy = call.get().value("execution").toString() == "busy" || busy;
                }
            }
            QTest::qWait(10);
        }
        stop.request_stop();
        for (auto &call : calls) {
            if (call.valid()) {
                call.get();
            }
        }
        QVERIFY(busy);
        QCOMPARE(solve("(assert true)").value("solver_status").toString(), QStringLiteral("sat"));
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
