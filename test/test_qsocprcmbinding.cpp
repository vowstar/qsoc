// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmbinding.h"
#include "qsoc_prcm_fixture.h"
#include "qsoc_test.h"

#include <QtTest>

#include <algorithm>

namespace {

class Test : public QObject
{
    Q_OBJECT

private slots:
    void managementReset_data()
    {
        QTest::addColumn<QString>("fault");
        QTest::addColumn<QString>("path");
        QTest::newRow("low") << QString("low") << QString();
        QTest::newRow("high") << QString("high") << QString();
        QTest::newRow("target") << QString("target") << QString("prcm.controller.reset.target");
        QTest::newRow("clock") << QString("clock")
                               << QString("reset[0].target.manage_n.async.clock");
        QTest::newRow("cold") << QString("cold") << QString("reset[0].target.manage_n.link");
        QTest::newRow("domain") << QString("domain")
                                << QString("reset[0].target.manage_n.link.restart_n");
        QTest::newRow("control") << QString("control")
                                 << QString("clock[0].target.periph_clk.icg.enable");
    }

    void managementReset()
    {
        QFETCH(QString, fault);
        QFETCH(QString, path);
        auto node                                     = YAML::Load(qsocPrcmDeclaration());
        node["prcm"]["controller"]["reset"]["target"] = "manage_n";
        node["reset"][0]["source"]["restart_n"]       = YAML::Load("{active: low}");
        node["reset"][0]["target"]["manage_n"]        = YAML::Load(
            "{active: low, async: {clock: aon_clk, stage: 2}, link: {por_n: {}, restart_n: {}}}");
        auto target = node["reset"][0]["target"]["manage_n"];
        if (fault == "high")
            target["active"] = "high";
        if (fault == "target")
            node["prcm"]["controller"]["reset"]["target"] = "missing";
        if (fault == "clock")
            target["async"]["clock"] = "periph_clk";
        if (fault == "cold")
            target["link"].remove("por_n");
        if (fault == "domain") {
            node["prcm"]["domain"]["periph"]["reset"]["source"] = "restart_n";
            auto link = node["reset"][0]["target"]["periph_n"]["link"];
            link.remove("hold_n");
            link["restart_n"] = YAML::Load("{}");
        }
        if (fault == "control") {
            node["reset"][0]["source"]["gate_en"] = YAML::Load("{active: high}");
            target["link"]["gate_en"]             = YAML::Load("{}");
        }
        const auto result = QSocPrcmBinding::resolve(YAML::Load(YAML::Dump(node)), "reset.soc_net");
        if (path.isEmpty()) {
            QVERIFY2(
                result.plan,
                result.diagnostic.isEmpty() ? "No plan" : qPrintable(result.diagnostic[0].message));
            QCOMPARE(result.plan->input.resetSource, "por_n");
            QCOMPARE(result.plan->input.resetTarget, "manage_n");
            const auto selected = std::find_if(
                result.plan->reset.targets.cbegin(),
                result.plan->reset.targets.cend(),
                [](const auto &item) { return item.name == "manage_n"; });
            QVERIFY(selected != result.plan->reset.targets.cend());
            QCOMPARE(selected->active, fault);
            return;
        }
        QVERIFY(!result.plan);
        QCOMPARE(result.diagnostic.size(), 1);
        QCOMPARE(result.diagnostic[0].source[0].path, path);
        QCOMPARE(
            result.diagnostic[0].code,
            fault == "domain" || fault == "control" ? "PRCM_RESOURCE_CONFLICT"
                                                    : "PRCM_RESOURCE_REFERENCE");
    }

    void bind()
    {
        const auto node   = YAML::Load(qsocPrcmDeclaration());
        const auto before = YAML::Dump(node);
        const auto result = QSocPrcmBinding::resolve(node, "controller.soc_net");
        QVERIFY2(
            result.plan.has_value(),
            result.diagnostic.isEmpty() ? "No plan" : qPrintable(result.diagnostic[0].message));
        QVERIFY(result.diagnostic.isEmpty());
        QCOMPARE(result.plan->clock.name, "clock");
        QCOMPARE(result.plan->reset.name, "reset");
        QCOMPARE(result.plan->domain.size(), 1);
        QCOMPARE(result.plan->domain["periph"].clockEnable, "gate_en");
        QVERIFY(result.plan->domain["periph"].resetSourceActiveLow);
        QVERIFY(result.plan->domain["periph"].resetTargetActiveLow);
        QCOMPARE(result.plan->clock.ports.size(), 4);
        QCOMPARE(QSocResetPrimitive::describePorts(result.plan->reset).size(), 4);
        const auto source = result.plan->input.source.value("reset[0].target.periph_n.async.clock");
        QCOMPARE(source.file, "controller.soc_net");
        QVERIFY(source.line > 0);
        QCOMPARE(YAML::Dump(node), before);
    }

    void polarity()
    {
        auto node                                        = YAML::Load(qsocPrcmDeclaration());
        node["reset"][0]["source"]["hold_n"]["active"]   = "high";
        node["reset"][0]["target"]["periph_n"]["active"] = "high";
        const auto result = QSocPrcmBinding::resolve(node, "controller.soc_net");
        QVERIFY(result.plan);
        QVERIFY(!result.plan->domain["periph"].resetSourceActiveLow);
        QVERIFY(!result.plan->domain["periph"].resetTargetActiveLow);
    }

    void sharedSupply()
    {
        auto node                               = YAML::Load(qsocPrcmDeclaration());
        node["clock"][0]["target"]["other_clk"] = YAML::Load(
            "{icg: {enable: other_gate, reset: por_n}, link: {aon_clk: {}}}");
        node["reset"][0]["source"]["other_hold_n"] = YAML::Load("{active: low}");
        node["reset"][0]["target"]["other_n"]      = YAML::Load(
            "{active: low, async: {clock: other_clk, stage: 3}, link: {por_n: {}, other_hold_n: "
            "{}}}");
        auto other               = YAML::Clone(node["prcm"]["domain"]["periph"]);
        other["clock"]["target"] = "other_clk";
        other["reset"]   = YAML::Load("{controller: reset, source: other_hold_n, target: other_n}");
        other["quiesce"] = YAML::Load(
            "{request: other_stop, ack: {signal: other_idle, sample_clock: aon_clk}}");
        other["isolation"] = YAML::Load(
            "{request: other_iso, active: {signal: other_isolated, sample_clock: aon_clk}}");
        node["prcm"]["domain"]["other"] = other;
        const auto result               = QSocPrcmBinding::resolve(node, "controller.soc_net");
        QVERIFY2(
            result.plan.has_value(),
            result.diagnostic.isEmpty() ? "No plan" : qPrintable(result.diagnostic[0].message));
        QCOMPARE(result.plan->domain.size(), 2);
        QCOMPARE(result.plan->domain["other"].clockEnable, "other_gate");
        QCOMPARE(result.plan->domain["periph"].clockEnable, "gate_en");
        other["clock"]       = YAML::Clone(node["prcm"]["domain"]["periph"]["clock"]);
        other["reset"]       = YAML::Clone(node["prcm"]["domain"]["periph"]["reset"]);
        const auto duplicate = QSocPrcmBinding::resolve(node, "controller.soc_net");
        QVERIFY(!duplicate.plan);
        QCOMPARE(duplicate.diagnostic[0].code, "PRCM_RESOURCE_CONFLICT");
        QCOMPARE(duplicate.diagnostic[0].source.size(), 2);
    }

    void unusedSource()
    {
        auto node                                = YAML::Load(qsocPrcmDeclaration());
        node["reset"][0]["source"]["periph_clk"] = YAML::Load("{active: low}");
        const auto result = QSocPrcmBinding::resolve(node, "controller.soc_net");
        QVERIFY(result.plan);
        QCOMPARE(QSocResetPrimitive::describePorts(result.plan->reset).size(), 4);
    }

    void roleSource_data()
    {
        QTest::addColumn<QString>("fault");
        QTest::addColumn<QStringList>("path");
        QTest::newRow("clock-input-target")
            << QString("clock-input-target")
            << QStringList{"clock[0].target.aon_clk", "clock[0].input.aon_clk"};
        QTest::newRow("clock-control-target")
            << QString("clock-control-target")
            << QStringList{"clock[0].target.periph_clk.icg.enable", "clock[0].target.periph_clk"};
        QTest::newRow("reset-clock-source")
            << QString("reset-clock-source")
            << QStringList{"reset[0].target.periph_n.async.clock", "reset[0].source.periph_clk"};
        QTest::newRow("reset-clock-target")
            << QString("reset-clock-target")
            << QStringList{"reset[0].target.periph_n.async.clock", "reset[0].target.periph_n"};
    }

    void roleSource()
    {
        QFETCH(QString, fault);
        QFETCH(QStringList, path);
        auto node   = YAML::Load(qsocPrcmDeclaration());
        auto target = node["reset"][0]["target"]["periph_n"];
        if (fault == "clock-input-target") {
            node["clock"][0]["target"]["aon_clk"] = YAML::Clone(
                node["clock"][0]["target"]["periph_clk"]);
            node["clock"][0]["target"].remove("periph_clk");
            node["prcm"]["domain"]["periph"]["clock"]["target"] = "aon_clk";
            target["async"]["clock"]                            = "aon_clk";
        }
        if (fault == "clock-control-target")
            node["clock"][0]["target"]["periph_clk"]["icg"]["enable"] = "periph_clk";
        if (fault == "reset-clock-source") {
            node["reset"][0]["source"]["periph_clk"] = YAML::Load("{active: low}");
            target["link"]["periph_clk"]             = YAML::Load("{}");
        }
        if (fault == "reset-clock-target")
            target["async"]["clock"] = "periph_n";
        const auto result = QSocPrcmBinding::resolve(YAML::Load(YAML::Dump(node)), "role.soc_net");
        QVERIFY(!result.plan);
        QCOMPARE(result.diagnostic.size(), 1);
        QCOMPARE(result.diagnostic[0].code, "PRCM_RESOURCE_CONFLICT");
        QCOMPARE(result.diagnostic[0].source.size(), path.size());
        for (qsizetype i = 0; i < path.size(); ++i) {
            const auto &source = result.diagnostic[0].source[i];
            QCOMPARE(source.file, "role.soc_net");
            QCOMPARE(source.path, path[i]);
            QVERIFY(source.line > 0);
            QVERIFY(source.column > 0);
        }
    }

    void reject_data()
    {
        QTest::addColumn<QString>("fault");
        QTest::addColumn<QString>("code");
        for (const auto &name :
             {"missing-clock",
              "missing-input",
              "missing-target",
              "missing-reset",
              "missing-link",
              "missing-por",
              "wrong-reset-clock",
              "wrong-gate-reset",
              "controller-supply",
              "domain-supply",
              "domain-controller"}) {
            QTest::newRow(name) << QString(name) << QString("PRCM_RESOURCE_REFERENCE");
        }
        for (const auto &name :
             {"stage", "divider", "test-port", "reset-stage", "feedback-clock", "power", "reset-mix"}) {
            QTest::newRow(name) << QString(name) << QString("PRCM_RESOURCE_UNSUPPORTED");
        }
        for (const auto &name :
             {"request-feedback",
              "power-gate",
              "feedback-gate",
              "request-clock",
              "request-reset",
              "request-reset-target",
              "cross-request-feedback",
              "root-control",
              "clock-control",
              "gate-fanout",
              "reset-fanout",
              "module-name",
              "duplicate-controller"}) {
            QTest::newRow(name) << QString(name) << QString("PRCM_RESOURCE_CONFLICT");
        }
        QTest::newRow("mmio-width") << QString("mmio-width") << QString("PRCM_MMIO");
        QTest::newRow("mmio-address") << QString("mmio-address") << QString("PRCM_MMIO");
        QTest::newRow("undeclared-source")
            << QString("undeclared-source") << QString("PRCM_RESOURCE_REFERENCE");
        QTest::newRow("reset-capacity")
            << QString("reset-capacity") << QString("PRCM_RESOURCE_VALUE");
        QTest::newRow("sanitized-name") << QString("sanitized-name") << QString("PRCM_NAME");
    }

    void reject()
    {
        QFETCH(QString, fault);
        QFETCH(QString, code);
        auto node   = YAML::Load(qsocPrcmDeclaration());
        auto domain = node["prcm"]["domain"]["periph"];
        auto gate   = node["clock"][0]["target"]["periph_clk"]["icg"];
        auto target = node["reset"][0]["target"]["periph_n"];
        if (fault == "controller-supply")
            node["prcm"]["controller"]["supply"] = "periph";
        if (fault == "domain-supply")
            domain["supply"] = "absent";
        if (fault == "domain-controller")
            domain["clock"]["controller"] = "absent";
        if (fault == "mmio-width")
            node["prcm"]["mmio"]["data_width"] = 64;
        if (fault == "mmio-address")
            node["prcm"]["mmio"]["address_width"] = 1;
        if (fault == "missing-clock")
            node["prcm"]["controller"]["clock"]["controller"] = "absent";
        if (fault == "missing-input")
            node["prcm"]["controller"]["clock"]["input"] = "absent";
        if (fault == "missing-target")
            domain["clock"]["target"] = "absent";
        if (fault == "missing-reset")
            domain["reset"]["source"] = "absent";
        if (fault == "missing-link")
            target["link"].remove("hold_n");
        if (fault == "missing-por")
            target["link"].remove("por_n");
        if (fault == "wrong-reset-clock")
            target["async"]["clock"] = "aon_clk";
        if (fault == "wrong-gate-reset")
            gate["reset"] = "other_reset";
        if (fault == "stage")
            domain["clock"]["stage"] = "link.icg";
        if (fault == "divider")
            node["clock"][0]["target"]["periph_clk"]["div"]["default"] = 2;
        if (fault == "test-port")
            node["clock"][0]["test_enable"] = "test_en";
        if (fault == "reset-stage")
            node["reset"][0]["test_enable"] = "test_en";
        if (fault == "feedback-clock")
            domain["quiesce"]["ack"]["sample_clock"] = "periph_clk";
        if (fault == "power")
            node["power"].push_back(YAML::Load("{name: legacy}"));
        if (fault == "reset-mix")
            target["count"] = YAML::Load("{clock: periph_clk, cycle: 8}");
        if (fault == "power-gate")
            node["prcm"]["supply"]["periph"]["request"] = "gate_en";
        if (fault == "feedback-gate")
            domain["quiesce"]["ack"]["signal"] = "gate_en";
        if (fault == "request-clock")
            domain["quiesce"]["request"] = "periph_clk";
        if (fault == "request-reset")
            domain["quiesce"]["request"] = "hold_n";
        if (fault == "request-reset-target")
            domain["quiesce"]["request"] = "periph_n";
        if (fault == "request-feedback")
            domain["quiesce"]["ack"]["signal"] = "stop_req";
        if (fault == "cross-request-feedback")
            domain["quiesce"]["ack"]["signal"] = "iso_req";
        if (fault == "root-control")
            domain["reset"]["source"] = "por_n";
        if (fault == "clock-control")
            gate["enable"] = "aon_clk";
        if (fault == "gate-fanout")
            node["clock"][0]["target"]["other_clk"] = YAML::Clone(
                node["clock"][0]["target"]["periph_clk"]);
        if (fault == "reset-fanout")
            node["reset"][0]["target"]["other_n"] = YAML::Clone(target);
        if (fault == "module-name") {
            node["reset"][0]["name"]                          = "clock";
            node["prcm"]["controller"]["reset"]["controller"] = "clock";
            domain["reset"]["controller"]                     = "clock";
        }
        if (fault == "duplicate-controller")
            node["clock"].push_back(YAML::Clone(node["clock"][0]));
        if (fault == "undeclared-source")
            target["link"]["other_n"] = YAML::Load("{}");
        if (fault == "reset-capacity")
            target["async"]["stage"] = 1;
        if (fault == "sanitized-name")
            node["reset"][0]["source"]["hold[0]"] = YAML::Load("{active: low}");
        const auto result = QSocPrcmBinding::resolve(node, "controller.soc_net");
        QVERIFY(!result.plan);
        QCOMPARE(result.diagnostic.size(), 1);
        QCOMPARE(result.diagnostic[0].code, code);
        QVERIFY(!result.diagnostic[0].source.isEmpty());
        if (code == "PRCM_RESOURCE_CONFLICT")
            QCOMPARE(result.diagnostic[0].source.size(), 2);
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocprcmbinding.moc"
