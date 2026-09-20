// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmcomposition.h"
#include "common/qsocprcmformal.h"
#include "common/qsocprcmshared.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {

YAML::Node declaration(bool axi = false, bool shared = false)
{
    auto node                          = YAML::Load(R"(
clock:
  - name: clock
    input: {aon_clk: {}}
    target:
      client_clk:
        icg: {enable: client_gate, reset: por_n}
        link: {aon_clk: {}}
      provider_clk:
        icg: {enable: provider_gate, reset: por_n}
        link: {aon_clk: {}}
reset:
  - name: reset
    source: {por_n: {active: low}, warm_n: {active: low}, client_hold_n: {active: low}, provider_hold_n: {active: low}}
    target:
      manage_n:
        active: low
        async: {clock: aon_clk, stage: 2}
        link: {por_n: {}, warm_n: {}}
      client_n:
        active: low
        async: {clock: client_clk, stage: 2}
        link: {por_n: {}, client_hold_n: {}}
      provider_n:
        active: low
        async: {clock: provider_clk, stage: 2}
        link: {por_n: {}, provider_hold_n: {}}
prcm:
  version: 1
  controller:
    clock: {controller: clock, input: aon_clk}
    reset: {controller: reset, source: por_n, target: manage_n, stage: 2}
    supply: aon
  mmio: {bus: apb4, data_width: 8, address_width: 12}
  supply:
    aon: {always_on: true}
    client: {request: client_power, valid: {signal: client_pgood, sample_clock: aon_clk}}
    provider: {request: provider_power, valid: {signal: provider_pgood, sample_clock: aon_clk}}
  domain:
    client:
      supply: client
      clock: {controller: clock, target: client_clk, stage: target.icg}
      reset: {controller: reset, source: client_hold_n, target: client_n}
      quiesce: {request: client_stop, ack: {signal: client_idle, sample_clock: aon_clk}}
      isolation: {request: client_iso, active: {signal: client_isolated, sample_clock: aon_clk}}
      reset_mode: 'OFF'
      mode:
        'OFF': {code: 0, power: 'off', clock: stopped, reset: asserted, isolation: enabled}
        'RESET': {code: 1, power: 'on', clock: running, reset: asserted, isolation: enabled}
        'RUN': {code: 2, power: 'on', clock: running, reset: released, isolation: disabled}
      transition: [{from: 'OFF', to: 'RESET'}, {from: 'OFF', to: 'RUN'}, {from: 'RESET', to: 'OFF'}, {from: 'RESET', to: 'RUN'}, {from: 'RUN', to: 'OFF'}, {from: 'RUN', to: 'RESET'}]
      require:
        access: {service: provider.online, mode: ['RUN']}
    provider:
      supply: provider
      clock: {controller: clock, target: provider_clk, stage: target.icg}
      reset: {controller: reset, source: provider_hold_n, target: provider_n}
      quiesce: {request: provider_stop, ack: {signal: provider_idle, sample_clock: aon_clk}}
      isolation: {request: provider_iso, active: {signal: provider_isolated, sample_clock: aon_clk}}
      reset_mode: 'OFF'
      mode:
        'OFF': {code: 0, power: 'off', clock: stopped, reset: asserted, isolation: enabled}
        'RESET': {code: 1, power: 'on', clock: running, reset: asserted, isolation: enabled}
        'RUN': {code: 2, power: 'on', clock: running, reset: released, isolation: disabled}
      transition: [{from: 'OFF', to: 'RESET'}, {from: 'OFF', to: 'RUN'}, {from: 'RESET', to: 'OFF'}, {from: 'RESET', to: 'RUN'}, {from: 'RUN', to: 'OFF'}, {from: 'RUN', to: 'RESET'}]
      service:
        online: {mode: 'RUN'}
  chip:
    reset_mode: 'SLEEP'
    mode:
      'NORMAL':
        code: 0
        domain: {client: {allow: ['OFF', 'RESET', 'RUN']}, provider: {allow: ['OFF', 'RESET', 'RUN']}}
      'SLEEP':
        code: 1
        domain: {client: {target: 'OFF'}, provider: {target: 'OFF'}}
)");
    node["prcm"]["mmio"]["bus"]        = axi ? "axi4_lite" : "apb4";
    node["prcm"]["mmio"]["data_width"] = axi ? 32 : 8;
    if (shared) {
        const auto clone = [](const YAML::Node &source) {
            return YAML::Load(
                QString::fromStdString(YAML::Dump(source)).replace("client", "peer").toStdString());
        };
        node["clock"][0]["target"]["peer_clk"]    = clone(node["clock"][0]["target"]["client_clk"]);
        node["reset"][0]["source"]["peer_hold_n"] = clone(
            node["reset"][0]["source"]["client_hold_n"]);
        node["reset"][0]["target"]["peer_n"] = clone(node["reset"][0]["target"]["client_n"]);
        node["prcm"]["supply"]["peer"]       = clone(node["prcm"]["supply"]["client"]);
        node["prcm"]["domain"]["peer"]       = clone(node["prcm"]["domain"]["client"]);
        for (const auto &mode : {"NORMAL", "SLEEP"})
            node["prcm"]["chip"]["mode"][mode]["domain"]["peer"] = clone(
                node["prcm"]["chip"]["mode"][mode]["domain"]["client"]);
    }
    return node;
}

QString bench(const QSocPrcmCircuit &circuit, bool axi, bool shared, bool pending = false)
{
    QString text = "module tb;\n";
    for (const auto &port : circuit.port) {
        const bool feedback = port.name.endsWith("_pgood") || port.name.endsWith("_idle")
                              || port.name.endsWith("_isolated");
        const bool variable = port.direction == "input" && !feedback;
        const auto width    = port.width == 1 ? QString() : QString("[%1:0] ").arg(port.width - 1);
        text += QString(variable ? "reg " : "wire ") + width + port.name;
        text += variable ? (port.name == "warm_n" ? "=1;\n" : "=0;\n") : ";\n";
    }
    text += R"(
control dut(.*);
always #5 aon_clk=!aon_clk;
reg fail_provider=0, stall_provider=0, stall_client=0;
reg [1:0] client_power_delay=0,client_iso_delay=3,client_idle_delay=3;
always @(posedge aon_clk) begin
 client_power_delay<={client_power_delay[0],client_power};
 client_iso_delay<={client_iso_delay[0],client_iso};
 client_idle_delay<={client_idle_delay[0],client_stop};
end
assign client_pgood=client_power_delay[1];
assign client_isolated=client_iso_delay[1];
assign client_idle=client_idle_delay[1] && !stall_client;
reg [1:0] provider_power_delay=0,provider_iso_delay=3,provider_idle_delay=3;
always @(posedge aon_clk) begin
 provider_power_delay<={provider_power_delay[0],provider_power};
 provider_iso_delay<={provider_iso_delay[0],provider_iso};
 provider_idle_delay<={provider_idle_delay[0],provider_stop};
end
assign provider_pgood=provider_power_delay[1] && !fail_provider && !stall_provider;
assign provider_isolated=provider_iso_delay[1];
assign provider_idle=provider_idle_delay[1];
integer transactions=0, observed=0;
reg old_client_power=0,old_provider_power=0;
always @(negedge aon_clk) begin
 #1;
 if(por_n) begin
  observed=observed+1;
  if(old_client_power && !client_power && (!client_isolated || client_n)) $fatal(1,"CLIENT_SAFE_OFF");
  if(old_provider_power && !provider_power && (!provider_isolated || provider_n)) $fatal(1,"PROVIDER_SAFE_OFF");
  if(!fail_provider && !dut.prcm_d1_fault && dut.prcm_s0_request && dut.prcm_s0_grant && provider_stop)
   $fatal(1,"SERVICE_ACCEPT");
  if(!fail_provider && client_n && !client_isolated) begin
   if(!provider_n || provider_isolated || provider_idle || !provider_pgood || !dut.prcm_s0_request || !dut.prcm_s0_grant)
    $fatal(1,"SERVICE_AVAILABLE");
  end
 end
 old_client_power=client_power;old_provider_power=provider_power;
end
integer ticks=0;
always @(posedge aon_clk) begin
 ticks=ticks+1;if(ticks>5000)$fatal(1,"TIMEOUT");
end
)";
    text += axi ? R"(
task access(input bit wr,input integer address,input integer value,output integer result);
 bit aw,w;
 begin
  if(wr) begin
   aw=0;w=0;
   @(negedge aon_clk);s_axi_awvalid=1;s_axi_wvalid=1;s_axi_awaddr=address*4;s_axi_wdata=value;s_axi_wstrb=15;
   while(!aw || !w)begin
    @(posedge aon_clk);if(s_axi_awready)aw=1;if(s_axi_wready)w=1;
    @(negedge aon_clk);if(aw)s_axi_awvalid=0;if(w)s_axi_wvalid=0;
   end
   s_axi_bready=1;
   do @(posedge aon_clk);while(!s_axi_bvalid);
   if(s_axi_bresp!=0)$fatal(1,"BUS_WRITE_ERROR");
   result=0;
   @(negedge aon_clk);s_axi_bready=0;
  end else begin
   @(negedge aon_clk);s_axi_arvalid=1;s_axi_araddr=address*4;
   do @(posedge aon_clk);while(!s_axi_arready);
   @(negedge aon_clk);s_axi_arvalid=0;s_axi_rready=1;
   do @(posedge aon_clk);while(!s_axi_rvalid);
   if(s_axi_rresp!=0)$fatal(1,"BUS_READ_ERROR");
   result=s_axi_rdata;
   @(negedge aon_clk);s_axi_rready=0;
  end
  transactions=transactions+1;
 end
endtask
)"
                : R"(
task access(input bit wr,input integer address,input integer value,output integer result);
 begin
  @(negedge aon_clk);s_apb_pselx=1;s_apb_penable=0;s_apb_pwrite=wr;s_apb_paddr=address;s_apb_pwdata=value;s_apb_pstrb=1;
  @(negedge aon_clk);s_apb_penable=1;
  do @(posedge aon_clk); while(!s_apb_pready);
  result=s_apb_prdata;
  if(s_apb_pslverr)$fatal(1,"BUS_ERROR %0d",address);
  transactions=transactions+1;
  @(negedge aon_clk);s_apb_pselx=0;s_apb_penable=0;s_apb_pwrite=0;s_apb_pstrb=0;
 end
endtask
)";
    auto operation = QString(R"(
integer value;
task write_reg(input integer address,input integer data);
 begin access(1,address,data,value);end
endtask
task expect_reg(input integer address,input integer mask,input integer expected);
 integer data;
 begin
  access(0,address,0,data);
  if((data & mask)!=expected)$fatal(1,"READBACK %0d actual=%0h expected=%0h mask=%0h",address,data,expected,mask);
 end
endtask
task poll(input integer address,input integer mask,input integer expected);
 integer data,count;
 begin
  count=0;
  do begin
   access(0,address,0,data);count=count+1;
   if(count>150)$fatal(1,"PROGRESS %0d actual=%0h expected=%0h",address,data,expected);
  end while((data & mask)!=expected);
 end
endtask
task run_client;
 begin
  write_reg(0,0);write_reg(2,2);poll(3,31,6);
  if(!client_n || !provider_n || client_isolated || provider_isolated)$fatal(1,"RUN_PHYSICAL");
  expect_reg(6,127,64);
 end
endtask
initial begin
 repeat(4)@(negedge aon_clk);por_n=1;
 poll(1,7,3);
 run_client;
 write_reg(2,3);expect_reg(3,15,11);
 if(!client_n || !provider_n)$fatal(1,"INVALID_RETAINS_MODE");
 write_reg(2,2);poll(3,31,6);
 write_reg(0,1);poll(1,7,3);
 expect_reg(3,63,34);
 write_reg(0,0);poll(3,31,6);
 stall_client=1;warm_n=0;
 repeat(15)@(negedge aon_clk);
 if(!dut.prcm_s0_request || !dut.prcm_s0_grant || !provider_power || provider_stop || !provider_n)
  $fatal(1,"WARM_KEEPS_SERVICE");
 fork
  begin expect_reg(2,3,0); end
  begin
   repeat(8)@(negedge aon_clk);
   if(@BUS_WAIT@)$fatal(1,"WARM_BUS_WAIT");
   warm_n=1;
  end
 join
 expect_reg(0,1,1);
 stall_client=0;poll(1,7,3);
 stall_provider=1;
 write_reg(0,0);write_reg(2,2);
 wait(dut.prcm_s0_request && !dut.prcm_s0_grant);
 write_reg(0,1);
 repeat(10)@(negedge aon_clk);
 if(!dut.prcm_s0_request || dut.prcm_s0_grant || client_n)$fatal(1,"CANCEL_WAITS_GRANT");
 stall_provider=0;poll(1,7,3);
 write_reg(2,0);run_client;
@AXI_HOLD@
 fail_provider=1;
 repeat(4)@(negedge aon_clk);
 if(!dut.prcm_d0_fault || client_n || dut.client_gate || !client_iso)$fatal(1,"SERVICE_FAILURE_PROTECTS");
 expect_reg(4,2,2);
 write_reg(0,1);write_reg(2,0);fail_provider=0;poll(1,7,3);
 expect_reg(4,2,2);write_reg(4,2);expect_reg(4,2,0);
 $display("SHARED_CIRCUIT_PASS transactions=%0d observations=%0d",transactions,observed);$finish;
end
endmodule
)");
    operation.replace(
        "@BUS_WAIT@",
        axi ? "!s_axi_arvalid || s_axi_arready" : "!s_apb_pselx || !s_apb_penable || s_apb_pready");
    operation.replace(
        "@AXI_HOLD@",
        axi ? R"(
 @(negedge aon_clk);s_axi_arvalid=1;s_axi_araddr=12;
 do @(posedge aon_clk);while(!s_axi_arready);
 @(negedge aon_clk);s_axi_arvalid=0;
 wait(s_axi_rvalid);
 @(negedge aon_clk);s_axi_awvalid=1;s_axi_awaddr=8;s_axi_wvalid=1;s_axi_wdata=2;s_axi_wstrb=15;
 do @(posedge aon_clk);while(!s_axi_awready || !s_axi_wready);
 @(negedge aon_clk);s_axi_awvalid=0;s_axi_wvalid=0;
 wait(s_axi_bvalid);
 stall_client=1;warm_n=0;
 repeat(15)begin
  @(negedge aon_clk);
  if(!s_axi_rvalid || s_axi_rdata!=6 || s_axi_rresp!=0 || !s_axi_bvalid || s_axi_bresp!=0)
   $fatal(1,"WARM_RESPONSE_HOLD");
 end
 if(!dut.prcm_s0_request || !provider_n || provider_stop)$fatal(1,"WARM_RESPONSE_SERVICE");
 warm_n=1;s_axi_rready=1;s_axi_bready=1;
 @(posedge aon_clk);
 @(negedge aon_clk);s_axi_rready=0;s_axi_bready=0;stall_client=0;
 transactions=transactions+2;
 poll(1,7,3);run_client;
)"
            : "");
    if (shared) {
        text.replace("dut.prcm_d1_fault", "dut.prcm_d2_fault");
        text += R"(
reg [1:0] peer_power_delay=0, peer_iso_delay=3, peer_idle_delay=3;
always @(posedge aon_clk) begin
    peer_power_delay <= {peer_power_delay[0], peer_power};
    peer_iso_delay <= {peer_iso_delay[0], peer_iso};
    peer_idle_delay <= {peer_idle_delay[0], peer_stop};
end
assign peer_pgood=peer_power_delay[1];
assign peer_isolated=peer_iso_delay[1];
assign peer_idle=peer_idle_delay[1];
always @(negedge aon_clk) begin
    #1;
    if (por_n && peer_n && !peer_isolated
        && (!provider_n || provider_isolated || provider_idle || !provider_pgood))
        $fatal(1, "PEER_SERVICE_AVAILABLE");
end
)";
        operation.replace("expect_reg(6,127,64)", "expect_reg(9,127,64)");
        operation.replace(" run_client;\n write_reg(2,3);", R"(
 run_client;
 write_reg(5,2);poll(6,31,6);
 write_reg(2,0);poll(3,31,4);
 repeat(12)@(negedge aon_clk);
 expect_reg(9,127,64);
 if(!peer_n || peer_isolated || !provider_n || provider_stop)
  $fatal(1,"LAST_CONSUMER_HOLD");
 write_reg(0,1);poll(1,7,3);
 if(peer_power || provider_power)$fatal(1,"SHARED_SLEEP");
 write_reg(5,0);run_client;
 write_reg(2,3);
)");
    }
    if (pending) {
        operation = operation.left(operation.indexOf("initial begin")) + R"(
initial begin
 repeat(4)@(negedge aon_clk);por_n=1;
 poll(1,7,3);
 write_reg(0,0);write_reg(5,2);
 wait(!dut.prcm_d1_reset_request);
 write_reg(5,1);
 if(!dut.prcm_d1_reset)$fatal(1,"RESET_WAIT_NOT_REACHED");
 while(dut.prcm_d1_reset)begin
  @(negedge aon_clk);#1;
  if(dut.prcm_d1_reset && dut.prcm_d1_reset_request)
   $fatal(1,"RESET_RELEASE_PENDING");
 end
 poll(6,31,5);
 write_reg(2,2);poll(3,31,6);
 repeat(20)@(negedge aon_clk);
 if(dut.prcm_d0_fault || dut.prcm_d1_fault)$fatal(1,"NO_SERVICE_FAULT");
 write_reg(0,1);poll(1,7,3);
 $display("SHARED_CIRCUIT_PASS transactions=%0d observations=%0d",transactions,observed);$finish;
end
endmodule
)";
    }
    return text + operation;
}

bool save(const QString &path, const QString &text)
{
    QFile      file(path);
    const auto data = text.toUtf8();
    return file.open(QIODevice::WriteOnly) && file.write(data) == data.size();
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void composition()
    {
        const auto bound = QSocPrcmBinding::resolve(declaration(), "controller.soc_net");
        QVERIFY(bound.plan);
        auto  input  = bound.plan->input;
        auto &client = input.domain["client"];
        input.domain["provider"].service.insert("other", "RUN");
        client.require.insert("second", {"provider", "other", {"RUN"}});
        const auto plan = QSocPrcmComposition::build(input);
        QVERIFY(plan.plan);
        QCOMPARE(plan.plan->domain.size(), 2);
        QCOMPARE(plan.plan->service.size(), 1);
        QCOMPARE(plan.plan->service[0].consumer, "client");
        QCOMPARE(plan.plan->service[0].provider, "provider");
        QCOMPARE(
            plan.plan->service[0].source,
            QStringList({"prcm.domain.client.require.access", "prcm.domain.client.require.second"}));
        QCOMPARE(plan.plan->resetCode, quint64(1));
        QVERIFY(!plan.plan->chip[0]["client"]);
        QCOMPARE(*plan.plan->chip[1]["provider"], QSocPrcmTarget::Off);
        client.require.clear();
        input.chipMode.clear();
        const auto unused = QSocPrcmComposition::build(input);
        QVERIFY(unused.plan);
        QVERIFY(unused.plan->service.isEmpty());
        QVERIFY(unused.plan->chip.isEmpty());
    }

    void reject_data()
    {
        QTest::addColumn<QString>("fault");
        QTest::addColumn<QString>("path");
        QTest::newRow("supply") << "supply" << "prcm.domain.provider.supply";
        QTest::newRow("unknown") << "unknown" << "prcm.domain.client.require.access.service";
        QTest::newRow("layer") << "layer" << "prcm.domain.client.require.access.service";
        QTest::newRow("service-mode")
            << "service-mode" << "prcm.domain.provider.service.online.mode";
        QTest::newRow("require-mode") << "require-mode" << "prcm.domain.client.require.access.mode";
        QTest::newRow("partial-allow")
            << "partial-allow" << "prcm.chip.mode.NORMAL.domain.client.allow";
        QTest::newRow("service-policy")
            << "service-policy" << "prcm.chip.mode.NORMAL.domain.client.allow";
        QTest::newRow("missing-domain") << "missing-domain" << "prcm.chip.mode.NORMAL.domain";
        QTest::newRow("chip-code") << "chip-code" << "prcm.chip.mode.SLEEP.code";
        QTest::newRow("power-feedback") << "power-feedback" << "prcm.supply.provider.valid.signal";
        QTest::newRow("drain-feedback")
            << "drain-feedback" << "prcm.domain.provider.quiesce.ack.signal";
        QTest::newRow("isolation-feedback")
            << "isolation-feedback" << "prcm.domain.provider.isolation.active.signal";
        QTest::newRow("local-feedback")
            << "local-feedback" << "prcm.domain.client.quiesce.ack.signal";
    }

    void reject()
    {
        QFETCH(QString, fault);
        QFETCH(QString, path);
        const auto bound = QSocPrcmBinding::resolve(declaration(), "controller.soc_net");
        QVERIFY(bound.plan);
        auto input = bound.plan->input;
        if (fault == "supply")
            input.domain["provider"].supply = "client";
        if (fault == "unknown")
            input.domain["client"].require["access"].service = "missing";
        if (fault == "layer")
            input.domain["provider"].require.insert("self", {"provider", "online", {"RUN"}});
        if (fault == "service-mode")
            input.domain["provider"].service["online"] = "RESET";
        if (fault == "require-mode")
            input.domain["client"].require["access"].mode = {"RESET"};
        if (fault == "partial-allow")
            input.chipMode["NORMAL"].domain["client"].allow.removeLast();
        if (fault == "service-policy") {
            auto &policy  = input.chipMode["NORMAL"].domain["provider"];
            policy.allow  = {};
            policy.target = "OFF";
        }
        if (fault == "missing-domain")
            input.chipMode["NORMAL"].domain.remove("client");
        if (fault == "chip-code")
            input.chipMode["SLEEP"].code = 0;
        if (fault == "power-feedback")
            input.supplyTable["provider"].valid.signal = "client_pgood";
        if (fault == "drain-feedback")
            input.domain["provider"].quiesce.completion.signal = "client_idle";
        if (fault == "isolation-feedback")
            input.domain["provider"].isolation.completion.signal = "client_isolated";
        if (fault == "local-feedback")
            input.domain["client"].quiesce.completion.signal = "client_pgood";
        const auto result = QSocPrcmComposition::build(input);
        QVERIFY(!result.plan);
        QCOMPARE(result.diagnostic.size(), 1);
        QCOMPARE(result.diagnostic[0].code, "PRCM_COMPOSITION_UNSUPPORTED");
        QCOMPARE(result.diagnostic[0].source[0].path, path);
        if (fault == "service-policy") {
            QCOMPARE(result.diagnostic[0].source.size(), 2);
            QCOMPARE(
                result.diagnostic[0].source[1].path, "prcm.chip.mode.NORMAL.domain.provider.target");
        }
        if (fault.endsWith("feedback")) {
            const auto expected = fault == "drain-feedback"
                                      ? "prcm.domain.client.quiesce.ack.signal"
                                  : fault == "isolation-feedback"
                                      ? "prcm.domain.client.isolation.active.signal"
                                      : "prcm.supply.client.valid.signal";
            QCOMPARE(result.diagnostic[0].source.size(), 2);
            QCOMPARE(result.diagnostic[0].source[1].path, expected);
        }
    }

    void structure()
    {
        const auto bound = QSocPrcmBinding::resolve(declaration(), "controller.soc_net");
        QVERIFY(bound.plan);
        const auto result = QSocPrcmShared::generate(*bound.plan, "control", 2);
        QVERIFY(result.circuit);
        const auto &circuit = *result.circuit;
        QCOMPARE(circuit.rtl.size(), 8);
        const QStringList names{
            "CHIP_REQUEST",
            "CHIP_STATUS",
            "DOMAIN_client_REQUEST",
            "DOMAIN_client_STATUS",
            "DOMAIN_client_EVENT",
            "DOMAIN_provider_REQUEST",
            "DOMAIN_provider_STATUS",
            "DOMAIN_provider_EVENT"};
        QCOMPARE(circuit.mmio.registers.size(), names.size());
        for (qsizetype i = 0; i < names.size(); ++i) {
            QCOMPARE(circuit.mmio.registers[i].name, names[i]);
            QCOMPARE(circuit.mmio.registers[i].byteOffset, quint64(i));
        }
        QCOMPARE(circuit.mmio.registers[4].fields.size(), 2);
        QCOMPARE(circuit.mmio.registers[7].fields.size(), 1);
        const auto instance = circuit.binding["instance"].toObject();
        QCOMPARE(instance.size(), 7);
        const auto bus = instance["prcm_register_inst"].toObject()["port"].toObject();
        QCOMPARE(bus["rst_ni"].toString(), "prcm_cold_n");
        QCOMPARE(bus["clear_i"].toString(), "prcm_clear");
        const auto receiver = circuit.binding["receiver"].toObject();
        QCOMPARE(receiver.size(), 3);
        QCOMPARE(receiver["prcm_d0_reset_sample"].toObject()["input"].toString(), "!client_n");
        const auto service = circuit.binding["service"].toArray();
        QCOMPARE(service.size(), 1);
        QCOMPARE(service[0].toObject()["consumer"].toString(), "client");
        QCOMPARE(service[0].toObject()["provider"].toString(), "provider");
        const auto noStage = QSocPrcmShared::generate(*bound.plan, "control", 1);
        QVERIFY(!noStage.circuit);
        QCOMPARE(noStage.diagnostic[0].source[0].path, "prcm.controller");
        QVERIFY(!QSocPrcmShared::generate(*bound.plan, "qsoc_prcm_service", 2).circuit);
        auto unused = *bound.plan;
        unused.input.domain["client"].require.clear();
        unused.input.chipMode.clear();
        const auto standalone = QSocPrcmShared::generate(unused, "control", 2);
        QVERIFY(standalone.circuit);
        QVERIFY(!standalone.circuit->rtl.contains("qsoc_prcm_service.v"));
        QCOMPARE(standalone.circuit->mmio.registers.size(), 6);
        auto named = *bound.plan;
        named.input.domain.insert("CHIP", named.input.domain.take("client"));
        named.domain.insert("CHIP", named.domain.take("client"));
        for (auto &mode : named.input.chipMode)
            mode.domain.insert("CHIP", mode.domain.take("client"));
        const auto distinct = QSocPrcmShared::generate(named, "control", 2);
        QVERIFY(distinct.circuit);
        QCOMPARE(distinct.circuit->mmio.registers[0].name, "CHIP_REQUEST");
        QCOMPARE(distinct.circuit->mmio.registers[2].name, "DOMAIN_CHIP_REQUEST");
    }

    void formal_data()
    {
        QTest::addColumn<bool>("axi");
        QTest::addColumn<bool>("reach");
        QTest::addColumn<bool>("hasRun");
        QTest::newRow("apb8-shared-names") << false << false << true;
        QTest::newRow("axi32") << true << false << true;
        QTest::newRow("cover-apb8-shared-names") << false << true << true;
        QTest::newRow("cover-axi32") << true << true << true;
        QTest::newRow("cover-reset-only") << false << true << false;
    }

    void formal()
    {
        QFETCH(bool, axi);
        QFETCH(bool, reach);
        QFETCH(bool, hasRun);
        QList<const char *> tools{"sby", "yosys"};
        tools.append(reach ? QList<const char *>{"btormc", "btorsim"} : QList<const char *>{"z3"});
        for (const auto &tool : tools) {
            if (QStandardPaths::findExecutable(tool).isEmpty())
                QSOC_TEST_MISSING_DEPENDENCY(tool);
        }
        auto input = declaration(axi, !axi);
        if (!hasRun) {
            input["prcm"].remove("chip");
            for (const auto &entry : input["prcm"]["domain"]) {
                auto domain = entry.second;
                domain.remove("require");
                domain.remove("service");
                domain["mode"].remove("RUN");
                domain["mode"]["OFF"]["code"]   = 5;
                domain["mode"]["RESET"]["code"] = 2;
                domain["transition"]            = YAML::Load(
                    "[{from: OFF, to: RESET}, {from: RESET, to: OFF}]");
            }
        }
        if (!axi) {
            input = YAML::Load(
                QString::fromStdString(YAML::Dump(input))
                    .replace("aon_clk", "D")
                    .replace("warm_n", "FAULT")
                    .replace("client_power", "endmodule_power")
                    .replace("por_n", "proof_cold_n")
                    .toStdString());
        }
        const auto bound = QSocPrcmBinding::resolve(input, "control.soc_net");
        QVERIFY(bound.plan);
        const auto composition = QSocPrcmComposition::build(bound.plan->input);
        QVERIFY(composition.plan);
        const auto generated = QSocPrcmShared::generate(*bound.plan, "control", 2);
        QVERIFY(generated.circuit);
        const auto formal = QSocPrcmFormal::generateShared(
            *bound.plan, *composition.plan, *generated.circuit, "control", 2);
        QVERIFY(!formal.systemVerilog.contains(QRegularExpression("@[A-Za-z_]+@")));
        QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_prcm_shared_formal-XXXXXX");
        QVERIFY(directory.isValid());
        for (auto file = generated.circuit->rtl.cbegin(); file != generated.circuit->rtl.cend();
             ++file)
            QVERIFY(save(directory.filePath(file.key()), file.value()));
        QVERIFY(save(directory.filePath("control_formal.sv"), formal.systemVerilog));
        QVERIFY(save(directory.filePath("control.sby"), formal.sby));
        QProcess process;
        process.setWorkingDirectory(directory.path());
        process.setProcessChannelMode(QProcess::MergedChannels);
        const QStringList taskName = reach ? QStringList{"cover", "cover_fault"}
                                           : QStringList{"normal", "fault"};
        for (const auto &task : taskName) {
            process.start(QStandardPaths::findExecutable("sby"), {"-f", "control.sby", task});
            QVERIFY(process.waitForStarted());
            QVERIFY(process.waitForFinished(210000));
            const auto output = process.readAll();
            QCOMPARE(process.exitStatus(), QProcess::NormalExit);
            QVERIFY2(process.exitCode() == 0, output.right(3000).constData());
            QFile status(directory.filePath(QString("control_%1/status").arg(task)));
            QVERIFY(status.open(QIODevice::ReadOnly));
            QCOMPARE(status.readAll().simplified().split(' ').first(), QByteArray("PASS"));
        }
    }

    void behavior_data()
    {
        QTest::addColumn<bool>("axi");
        QTest::addColumn<bool>("shared");
        QTest::addColumn<int>("stage");
        QTest::newRow("apb8") << false << false << 2;
        QTest::newRow("axi32") << true << false << 2;
        QTest::newRow("shared-apb8") << false << true << 3;
        QTest::newRow("reset-pending") << false << false << 8;
    }

    void behavior()
    {
        QFETCH(bool, axi);
        QFETCH(bool, shared);
        QFETCH(int, stage);
        const auto tool = QStandardPaths::findExecutable("verilator");
        if (tool.isEmpty())
            QSOC_TEST_MISSING_DEPENDENCY("verilator");
        auto input                                    = declaration(axi, shared);
        input["prcm"]["controller"]["reset"]["stage"] = stage;
        const auto bound = QSocPrcmBinding::resolve(input, "controller.soc_net");
        QVERIFY(bound.plan);
        const auto generated = QSocPrcmShared::generate(*bound.plan, "control", stage);
        QVERIFY(generated.circuit);
        QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_prcm_shared-XXXXXX");
        QVERIFY(directory.isValid());
        QStringList file;
        for (auto it = generated.circuit->rtl.cbegin(); it != generated.circuit->rtl.cend(); ++it) {
            QVERIFY(save(
                directory.filePath(it.key()),
                "`default_nettype none\n" + it.value() + "\n`default_nettype wire\n"));
            file.append(it.key());
        }
        QVERIFY(
            save(directory.filePath("tb.sv"), bench(*generated.circuit, axi, shared, stage == 8)));
        QProcess process;
        process.setWorkingDirectory(directory.path());
        process.setProcessChannelMode(QProcess::MergedChannels);
        QStringList argument{
            "--binary", "--timing", "--top-module", "tb", "-Wno-fatal", "-j", "16", "tb.sv"};
        argument.append(file);
        process.start(tool, argument);
        QVERIFY(process.waitForStarted());
        QVERIFY(process.waitForFinished(180000));
        const auto build = process.readAll();
        QVERIFY2(process.exitCode() == 0, build.constData());
        process.start(directory.filePath("obj_dir/Vtb"), QStringList{});
        QVERIFY(process.waitForStarted());
        QVERIFY(process.waitForFinished(30000));
        const auto run = process.readAll();
        QVERIFY2(process.exitCode() == 0, run.constData());
        QVERIFY(run.contains("SHARED_CIRCUIT_PASS"));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocprcmshared.moc"
