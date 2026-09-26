// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolsmt.h"
#include "qsoc_test.h"

#include <QJsonDocument>
#include <QTimer>
#include <QtTest>

namespace {

QJsonObject decode(const QString &text)
{
    return QJsonDocument::fromJson(text.toUtf8()).object();
}

json checkRequest(const QString &suffix = {})
{
    return {
        {"smtlib", (QStringLiteral("(declare-const x Int)(assert (= x 7))") + suffix).toStdString()},
        {"timeout_ms", 2000}};
}

class ImmediateTool final : public QSocTool
{
public:
    QString getName() const override { return QStringLiteral("ordinary"); }
    QString getDescription() const override { return QStringLiteral("Return a result"); }
    json    getParametersSchema() const override { return {{"type", "object"}}; }
    QString execute(const json &) override { return QStringLiteral("ordinary result"); }
};

class Test : public QObject
{
    Q_OBJECT
private slots:
    void initTestCase();
    void schemaAndImmediateErrors();
    void checkAndOptimize();
    void deferredCompletesOnce();
    void cancelOnlyOneOwner();
    void ownerDestroyed();
    void toolDestroyed();
    void registryDestroyed();
    void synchronousToolDestroyed();
    void workerFailureLeavesRegistryUsable();
    void queueAdmissionIsBounded();
};

void Test::initTestCase()
{
    if (!QSocToolSmt::supported())
        QSKIP("Worker resource limits require Linux");
}

void Test::schemaAndImmediateErrors()
{
    QSocToolSmt tool(nullptr, QStringLiteral(QSOC_SMT_WORKER_PATH));
    const auto  schema = tool.getParametersSchema();
    QCOMPARE(schema.at("properties").at("priority").at("enum"), json::array({"lex"}));
    QVERIFY(!schema.at("additionalProperties").get<bool>());
    QVERIFY(tool.isReadOnly());
    QSocToolRegistry registry;
    registry.registerTool(&tool);
    QObject owner;
    for (const auto &request :
         {json::array(),
          json{{"smtlib", ""}, {"mode", "invalid"}},
          json{{"smtlib", ""}, {"priority", "pareto"}}}) {
        auto       outcome     = QSocToolResultStatus::Ok;
        int        completions = 0;
        const auto result      = registry.executeToolDeferred(
            "z3_solve",
            request,
            &owner,
            [&](const QString &) { ++completions; },
            {},
            [&](QSocToolResultStatus value) { outcome = value; });
        QVERIFY(result.has_value());
        QCOMPARE(decode(*result).value("execution").toString(), QStringLiteral("error"));
        QCOMPARE(outcome, QSocToolResultStatus::Failed);
        QCOMPARE(completions, 0);
        QCOMPARE(registry.children().size(), 0);
    }
}

void Test::checkAndOptimize()
{
    QSocToolSmt tool(nullptr, QStringLiteral(QSOC_SMT_WORKER_PATH));
    const auto  result = decode(tool.execute(checkRequest()));
    QCOMPARE(result.value("execution").toString(), QStringLiteral("completed"));
    QCOMPARE(result.value("solver_status").toString(), QStringLiteral("sat"));
    const json request{
        {"mode", "optimize"},
        {"priority", "lex"},
        {"smtlib",
         "(declare-const x Int)(declare-const y Int)(assert (and (>= x 0) (>= y 0) (>= (+ x y) "
         "7)))(minimize x)(minimize y)"}};
    const auto optimized = decode(tool.execute(request));
    QCOMPARE(optimized.value("solver_status").toString(), QStringLiteral("sat"));
    QCOMPARE(optimized.value("objectives").toArray().size(), 2);
}

void Test::deferredCompletesOnce()
{
    QSocToolSmt      tool(nullptr, QStringLiteral(QSOC_SMT_WORKER_PATH));
    QSocToolRegistry registry;
    registry.registerTool(&tool);
    QObject    owner;
    int        completions = 0;
    int        outcomes    = 0;
    QString    value;
    const auto result = registry.executeToolDeferred(
        "z3_solve",
        checkRequest(),
        &owner,
        [&](const QString &text) {
            value = text;
            ++completions;
        },
        {},
        [&](QSocToolResultStatus status) {
            QCOMPARE(status, QSocToolResultStatus::Ok);
            ++outcomes;
        });
    QVERIFY(!result.has_value());
    QCOMPARE(registry.children().size(), 1);
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 5000);
    QCOMPARE(outcomes, 1);
    QCOMPARE(decode(value).value("solver_status").toString(), QStringLiteral("sat"));
    QTRY_COMPARE(registry.children().size(), 0);
    QTest::qWait(50);
    QCOMPARE(completions, 1);
}

void Test::cancelOnlyOneOwner()
{
    QSocToolSmt      tool(nullptr, QStringLiteral(QSOC_SMT_PROBE_PATH));
    QSocToolRegistry registry;
    ImmediateTool    ordinary;
    registry.registerTool(&tool);
    registry.registerTool(&ordinary);
    QObject first, second;
    int     firstDone = 0, secondDone = 0;
    QString firstResult, secondResult;
    registry.executeToolDeferred(
        "z3_solve", checkRequest("; probe-parse"), &first, [&](const QString &text) {
            firstResult = text;
            ++firstDone;
        });
    auto secondRequest          = checkRequest("; probe-parse");
    secondRequest["timeout_ms"] = 500;
    registry.executeToolDeferred("z3_solve", secondRequest, &second, [&](const QString &text) {
        secondResult = text;
        ++secondDone;
    });
    registry.abortCalls(&first);
    QTRY_COMPARE_WITH_TIMEOUT(firstDone, 1, 5000);
    QTRY_COMPARE_WITH_TIMEOUT(secondDone, 1, 5000);
    QCOMPARE(decode(firstResult).value("execution").toString(), QStringLiteral("cancelled"));
    QCOMPARE(decode(secondResult).value("execution").toString(), QStringLiteral("timeout"));
    QTest::qWait(50);
    QCOMPARE(firstDone, 1);
    QCOMPARE(secondDone, 1);
    QCOMPARE(
        registry.executeTool("ordinary", json::object(), &first), QStringLiteral("ordinary result"));
}

void Test::ownerDestroyed()
{
    QSocToolSmt      tool(nullptr, QStringLiteral(QSOC_SMT_PROBE_PATH));
    QSocToolRegistry registry;
    registry.registerTool(&tool);
    auto owner       = std::make_unique<QObject>();
    int  completions = 0;
    registry.executeToolDeferred(
        "z3_solve", checkRequest("; probe-parse"), owner.get(), [&](const QString &) {
            ++completions;
        });
    owner.reset();
    QTRY_COMPARE_WITH_TIMEOUT(registry.children().size(), 0, 5000);
    QTest::qWait(150);
    QCOMPARE(completions, 0);
}

void Test::toolDestroyed()
{
    auto tool = std::make_unique<QSocToolSmt>(nullptr, QStringLiteral(QSOC_SMT_PROBE_PATH));
    QSocToolRegistry registry;
    registry.registerTool(tool.get());
    QObject owner;
    int     completions = 0;
    registry.executeToolDeferred(
        "z3_solve", checkRequest("; probe-parse"), &owner, [&](const QString &) { ++completions; });
    tool.reset();
    QTRY_COMPARE(completions, 1);
    QTest::qWait(150);
    QCOMPARE(completions, 1);
    QCOMPARE(registry.count(), 0);
}

void Test::registryDestroyed()
{
    QSocToolSmt tool(nullptr, QStringLiteral(QSOC_SMT_PROBE_PATH));
    auto        registry = std::make_unique<QSocToolRegistry>();
    registry->registerTool(&tool);
    QObject owner;
    int     completions = 0;
    registry->executeToolDeferred(
        "z3_solve", checkRequest("; probe-parse"), &owner, [&](const QString &) { ++completions; });
    registry.reset();
    QTRY_COMPARE(completions, 1);
    QTest::qWait(150);
    QCOMPARE(completions, 1);
}

void Test::synchronousToolDestroyed()
{
    auto tool = std::make_unique<QSocToolSmt>(nullptr, QStringLiteral(QSOC_SMT_PROBE_PATH));
    QSocToolRegistry registry;
    registry.registerTool(tool.get());
    QObject owner;
    QTimer::singleShot(20, &owner, [&] { tool.reset(); });
    const auto result = registry.executeTool("z3_solve", checkRequest("; probe-parse"), &owner);
    QCOMPARE(decode(result).value("execution").toString(), QStringLiteral("cancelled"));
    QCOMPARE(registry.count(), 0);
}

void Test::workerFailureLeavesRegistryUsable()
{
    QSocToolSmt      tool(nullptr, QStringLiteral(QSOC_SMT_PROBE_PATH));
    ImmediateTool    ordinary;
    QSocToolRegistry registry;
    registry.registerTool(&tool);
    registry.registerTool(&ordinary);
    QObject owner;
    auto    status      = QSocToolResultStatus::Ok;
    int     completions = 0;
    QString result;
    registry.executeToolDeferred(
        "z3_solve",
        checkRequest("; probe-memory"),
        &owner,
        [&](const QString &text) {
            result = text;
            ++completions;
        },
        {},
        [&](QSocToolResultStatus value) { status = value; });
    QTRY_COMPARE_WITH_TIMEOUT(completions, 1, 5000);
    QCOMPARE(decode(result).value("execution").toString(), QStringLiteral("resource_limit"));
    QCOMPARE(status, QSocToolResultStatus::Failed);
    QCOMPARE(
        registry.executeTool("ordinary", json::object(), &owner), QStringLiteral("ordinary result"));
}

void Test::queueAdmissionIsBounded()
{
    QSocToolSmt      tool(nullptr, QStringLiteral(QSOC_SMT_PROBE_PATH));
    QSocToolRegistry registry;
    registry.registerTool(&tool);
    QObject owner;
    int     completions = 0;
    for (int i = 0; i < 66; ++i) {
        const auto result = registry.executeToolDeferred(
            "z3_solve", checkRequest("; probe-parse"), &owner, [&](const QString &) {
                ++completions;
            });
        QVERIFY(!result.has_value());
    }
    const auto rejected
        = registry.executeToolDeferred("z3_solve", checkRequest(), &owner, [&](const QString &) {
              ++completions;
          });
    QVERIFY(rejected.has_value());
    QCOMPARE(decode(*rejected).value("execution").toString(), QStringLiteral("busy"));
    registry.abortCalls(&owner);
    QTRY_COMPARE_WITH_TIMEOUT(completions, 66, 5000);
    QTest::qWait(50);
    QCOMPARE(completions, 66);
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoctoolsmt.moc"
