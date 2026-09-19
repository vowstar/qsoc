// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcminput.h"
#include "qsoc_test.h"

#include <limits>
#include <QtTest>

namespace {

QByteArray declaration()
{
    return R"(prcm:
  version: 1
  controller:
    clock: {controller: clock, input: aon_clk}
    reset: {controller: reset, source: por_n}
    supply: aon
  mmio: {bus: apb4, data_width: 32, address_width: 12}
  supply:
    aon: {always_on: true}
    peripheral:
      request: power_en
      valid: {signal: power_valid, sample_clock: aon_clk}
  domain:
    peripheral:
      supply: peripheral
      clock: {controller: clock, target: peripheral_clk, stage: target.icg}
      reset: {controller: reset, source: hold_n, target: peripheral_rst_n}
      quiesce:
        request: stop_req
        ack: {signal: idle, sample_clock: aon_clk}
      isolation:
        request: isolate_req
        active: {signal: isolated, sample_clock: aon_clk}
      reset_mode: 'OFF'
      mode:
        'OFF': {code: 0, power: 'off', clock: stopped, reset: asserted, isolation: enabled}
        'RUN': {code: 1, power: 'on', clock: running, reset: released, isolation: disabled}
      transition:
        - {from: 'OFF', to: 'RUN'}
        - {from: 'RUN', to: 'OFF'}
)";
}

YAML::Node field(YAML::Node root, const QString &path)
{
    for (const auto &name : path.split('.')) {
        root.reset(root[name.toStdString()]);
    }
    return root;
}

QSocPrcmParseResult parse(const YAML::Node &root)
{
    return QSocPrcmParser::parse(root, "controller.soc_net");
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void typedDeclaration()
    {
        const auto root   = YAML::Load(declaration().constData());
        const auto before = YAML::Dump(root);
        const auto result = parse(root);
        QVERIFY(result.diagnostic.isEmpty());
        QVERIFY(result.input.has_value());
        QCOMPARE(YAML::Dump(root), before);
        const auto &input = *result.input;
        QCOMPARE(input.clockController, "clock");
        QCOMPARE(input.clockInput, "aon_clk");
        QCOMPARE(input.resetController, "reset");
        QCOMPARE(input.resetSource, "por_n");
        QCOMPARE(input.supply, "aon");
        QCOMPARE(input.bus, QSocMmioBus::Apb4);
        QCOMPARE(input.dataWidth, 32U);
        QCOMPARE(input.addressWidth, 12U);
        QVERIFY(input.supplyTable.value("aon").alwaysOn);
        const auto physical = input.supplyTable.value("peripheral");
        QCOMPARE(physical.request, "power_en");
        QCOMPARE(physical.valid.signal, "power_valid");
        QCOMPARE(physical.valid.sampleClock, "aon_clk");
        const auto domain = input.domain.value("peripheral");
        QCOMPARE(domain.supply, "peripheral");
        QCOMPARE(domain.clock.target, "peripheral_clk");
        QCOMPARE(domain.clock.stage, "target.icg");
        QCOMPARE(domain.reset.source, "hold_n");
        QCOMPARE(domain.reset.target, "peripheral_rst_n");
        QCOMPARE(domain.quiesce.completion.signal, "idle");
        QCOMPARE(domain.isolation.completion.signal, "isolated");
        QCOMPARE(domain.resetMode, "OFF");
        const auto run = domain.mode.value("RUN");
        QCOMPARE(run.code, 1ULL);
        QVERIFY(run.power && run.clock && !run.reset && !run.isolation);
        const auto off = domain.mode.value("OFF");
        QVERIFY(!off.power && !off.clock && off.reset && off.isolation);
        QCOMPARE(domain.transition.size(), 2);
        QCOMPARE(domain.transition[0].from, "OFF");
        QCOMPARE(domain.transition[1].to, "OFF");
        const auto source = input.source.value("prcm.domain.peripheral.mode.RUN.clock");
        QCOMPARE(source.file, "controller.soc_net");
        QCOMPARE(source.line, 27);
        QCOMPARE(source.column, 46);
    }

    void domainAndChip()
    {
        auto root            = YAML::Load(declaration().constData());
        auto domain          = root["prcm"]["domain"]["peripheral"];
        domain["service"]    = YAML::Load("{online: {mode: RUN}}");
        domain["require"]    = YAML::Load("{memory: {service: memory.online, mode: [RUN]}}");
        root["prcm"]["chip"] = YAML::Load(R"(
reset_mode: SLEEP
mode:
  NORMAL: {code: 0, domain: {peripheral: {allow: ['OFF', 'RUN']}}}
  SLEEP: {code: 1, domain: {peripheral: {target: 'OFF'}}}
)");
        const auto result    = parse(root);
        QVERIFY(result.input);
        const auto parsed = result.input->domain.value("peripheral");
        QCOMPARE(parsed.service.value("online"), "RUN");
        const auto dependency = parsed.require.value("memory");
        QCOMPARE(dependency.domain, "memory");
        QCOMPARE(dependency.service, "online");
        QCOMPARE(dependency.mode, QStringList{"RUN"});
        QCOMPARE(result.input->chipResetMode, "SLEEP");
        QCOMPARE(
            result.input->chipMode.value("NORMAL").domain.value("peripheral").allow,
            (QStringList{"OFF", "RUN"}));
        QCOMPARE(result.input->chipMode.value("SLEEP").domain.value("peripheral").target, "OFF");
    }

    void malformed_data()
    {
        QTest::addColumn<QString>("path");
        QTest::addColumn<QString>("value");
        QTest::addColumn<QString>("code");
        QTest::newRow("version") << "prcm.version" << "2" << "PRCM_VERSION";
        QTest::newRow("null") << "prcm.domain" << "null" << "PRCM_TYPE";
        QTest::newRow("list") << "prcm.domain" << "[]" << "PRCM_TYPE";
        QTest::newRow("empty") << "prcm.domain" << "{}" << "PRCM_REQUIRED";
        QTest::newRow("signed") << "prcm.mmio.data_width" << "-32" << "PRCM_NUMBER";
        QTest::newRow("fraction") << "prcm.mmio.data_width" << "32.0" << "PRCM_NUMBER";
        QTest::newRow("overflow") << "prcm.mmio.data_width" << "4294967296" << "PRCM_NUMBER";
        QTest::newRow("bus") << "prcm.mmio.bus" << "unknown" << "PRCM_BUS";
        QTest::newRow("signal") << "prcm.supply.peripheral.request" << "'request; endmodule'"
                                << "PRCM_NAME";
        QTest::newRow("typo") << "prcm.domain.peripheral.mode.RUN.clcok" << "running"
                              << "PRCM_FIELD";
        QTest::newRow("state") << "prcm.domain.peripheral.mode.RUN.power" << "true" << "PRCM_VALUE";
        QTest::newRow("width64") << "prcm.domain.peripheral.mode.RUN.code" << "18446744073709551616"
                                 << "PRCM_NUMBER";
        QTest::newRow("missing-feedback")
            << "prcm.domain.peripheral.quiesce.ack" << "{signal: idle}" << "PRCM_REQUIRED";
        QTest::newRow("aon-control") << "prcm.supply.aon.request" << "aon_req" << "PRCM_FIELD";
        QTest::newRow("service-path")
            << "prcm.domain.peripheral.require"
            << "{memory: {service: memory.online.extra, mode: [RUN]}}" << "PRCM_REFERENCE";
        QTest::newRow("service-mode")
            << "prcm.domain.peripheral.require" << "{memory: {service: memory.online, mode: []}}"
            << "PRCM_REQUIRED";
        QTest::newRow("mode-twice")
            << "prcm.domain.peripheral.require"
            << "{memory: {service: memory.online, mode: [RUN, RUN]}}" << "PRCM_DUPLICATE";
        QTest::newRow("two-policy") << "prcm.chip"
                                    << "{reset_mode: NORMAL, mode: {NORMAL: {code: 0, domain: "
                                       "{peripheral: {allow: [RUN], target: RUN}}}}}"
                                    << "PRCM_POLICY";
        QTest::newRow("no-policy")
            << "prcm.chip"
            << "{reset_mode: NORMAL, mode: {NORMAL: {code: 0, domain: {peripheral: {}}}}}"
            << "PRCM_POLICY";
    }

    void malformed()
    {
        QFETCH(QString, path);
        QFETCH(QString, value);
        QFETCH(QString, code);
        auto root         = YAML::Load(declaration().constData());
        field(root, path) = YAML::Load(value.toStdString());
        const auto result = parse(root);
        QVERIFY(!result.input);
        QCOMPARE(result.diagnostic.size(), 1);
        QCOMPARE(result.diagnostic[0].code, code);
        QVERIFY(!result.diagnostic[0].message.isEmpty());
        QVERIFY(!result.diagnostic[0].source.isEmpty());
    }

    void duplicateLocation()
    {
        auto text = declaration();
        text.replace("  version: 1\n", "  version: 1\n  version: 1\n");
        const auto result = parse(YAML::Load(text.constData()));
        QVERIFY(!result.input);
        QCOMPARE(result.diagnostic[0].code, "PRCM_DUPLICATE");
        const auto source = result.diagnostic[0].source;
        QCOMPARE(source.size(), 2);
        QCOMPARE(source[0].path, "prcm.version");
        QCOMPARE(source[0].line, 2);
        QCOMPARE(source[1].line, 3);
        QCOMPARE(source[0].column, 3);
        QCOMPARE(source[1].column, 3);
        QVERIFY(parse(YAML::Load(declaration().constData())).input);
    }

    void numericBoundary()
    {
        auto root                                           = YAML::Load(declaration().constData());
        field(root, "prcm.domain.peripheral.mode.RUN.code") = "0xffffffffffffffff";
        const auto result                                   = parse(root);
        QVERIFY(result.input);
        QCOMPARE(
            result.input->domain.value("peripheral").mode.value("RUN").code,
            std::numeric_limits<quint64>::max());
    }

    void emptyOptionalTable()
    {
        auto root         = YAML::Load(declaration().constData());
        auto domain       = root["prcm"]["domain"]["peripheral"];
        domain["service"] = YAML::Load("{}");
        domain["require"] = YAML::Load("{}");
        const auto result = parse(root);
        QVERIFY(result.input);
        QVERIFY(result.input->domain.value("peripheral").service.isEmpty());
        QVERIFY(result.input->domain.value("peripheral").require.isEmpty());
    }

    void absentAndRecursive()
    {
        QVERIFY(!parse(YAML::Load("null")).input);
        const auto absent = parse(YAML::Load("clock: []"));
        QCOMPARE(absent.diagnostic[0].code, "PRCM_REQUIRED");
        const auto recursive = parse(YAML::Load("prcm: &p {version: 1, controller: *p}"));
        QVERIFY(!recursive.input);
        QVERIFY(!recursive.diagnostic.isEmpty());
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocprcminput.moc"
