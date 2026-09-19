// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmgenerator.h"
#include "qsoc_prcm_fixture.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {

YAML::Node declaration(int width, bool axi, bool activeHigh = false)
{
    auto node = YAML::Load(
        QString(qsocPrcmDeclaration()).replace("aon_clk", "prcm_clk").toStdString());
    node["prcm"]["mmio"]["data_width"]    = width;
    node["prcm"]["mmio"]["address_width"] = 8;
    node["prcm"]["mmio"]["bus"]           = axi ? "axi4_lite" : "apb4";
    auto domain                           = node["prcm"]["domain"]["periph"];
    domain["mode"]["RUN"]["code"]         = quint64(1) << (width - 4);
    domain["mode"]["RESET"]               = YAML::Clone(domain["mode"]["RUN"]);
    domain["mode"]["RESET"]["code"]       = 1;
    if (width == 16)
        domain["reset_mode"] = "RESET";
    domain["mode"]["RESET"]["reset"]     = "asserted";
    domain["mode"]["RESET"]["isolation"] = "enabled";
    domain["transition"]                 = YAML::Node(YAML::NodeType::Sequence);
    for (const auto &from : {"OFF", "RESET", "RUN"}) {
        for (const auto &to : {"OFF", "RESET", "RUN"}) {
            if (QString(from) == to)
                continue;
            YAML::Node edge;
            edge["from"] = from;
            edge["to"]   = to;
            domain["transition"].push_back(edge);
        }
    }
    auto reset                                    = node["reset"][0];
    reset["source"]["warm_n"]["active"]           = "low";
    reset["target"]["manage_n"]                   = YAML::Load(R"(
active: low
async: {clock: prcm_clk, stage: 2}
link: {por_n: {}, warm_n: {}}
)");
    node["prcm"]["controller"]["reset"]["target"] = "manage_n";
    if (activeHigh) {
        reset["target"]["manage_n"]["active"] = "high";
        reset["target"]["periph_n"]["active"] = "high";
        reset["source"]["hold_n"]["active"]   = "high";
    }
    return node;
}

QString commonBench()
{
    return R"(
module tb;
localparam D = @WIDTH@;
localparam M = D - 3;
localparam INITIAL = @INITIAL@;
localparam SAMPLE = @STAGE@;
localparam [D-1:0] RUN = (D'(1) << (D-4));
localparam [D-1:0] DONE = (D'(1) << M);
localparam [D-1:0] INVALID = (D'(1) << (M+1));
reg prcm_clk = 0, por_n = 0, warm_n = 1;
wire periph_clk, periph_n, manage_n, power_en, stop_req, iso_req;
reg [1:0] power_delay = 0, isolation_delay = 3, idle_delay = 3;
reg power_fail = 0;
wire pgood = power_delay[1] && !power_fail;
wire iso_active = isolation_delay[1];
wire idle = idle_delay[1];
wire reset_active = @RESET@;
integer clock_edge = 0;
always @(posedge periph_clk) clock_edge = clock_edge + 1;
always @(posedge prcm_clk) begin
    power_delay <= {power_delay[0], power_en};
    isolation_delay <= {isolation_delay[0], iso_req};
    idle_delay <= {idle_delay[0], stop_req};
end
reg old_power = 0;
always @(negedge prcm_clk) begin
    if (por_n && old_power && !power_en)
        check(iso_active && reset_active, "SAFE_POWER_OFF");
    old_power <= power_en;
end
task tick;
begin #5; prcm_clk = 1; #5; prcm_clk = 0; #1; end
endtask
task check(input bit predicate, input string reason);
begin if (!predicate) $fatal(1, "%s", reason); end
endtask
)";
}

QString axiBench()
{
    return R"(
reg [7:0] s_axi_awaddr = 0, s_axi_araddr = 0;
reg [2:0] s_axi_awprot = 0, s_axi_arprot = 0;
reg [D-1:0] s_axi_wdata = 0;
reg [D/8-1:0] s_axi_wstrb = 0;
reg s_axi_awvalid = 0, s_axi_wvalid = 0, s_axi_arvalid = 0;
reg s_axi_bready = 0, s_axi_rready = 0;
wire s_axi_awready, s_axi_wready, s_axi_arready, s_axi_bvalid, s_axi_rvalid;
wire [1:0] s_axi_bresp, s_axi_rresp;
wire [D-1:0] s_axi_rdata;
controller dut(.*);
wire register_ready = s_axi_arready;
task address(input [7:0] addr);
begin
    s_axi_awaddr = addr; s_axi_awvalid = 1;
    #1; check(s_axi_awready, "AW_ACCEPT"); tick;
    s_axi_awvalid = 0; s_axi_awaddr = '1;
end
endtask
task data(input [D-1:0] value, input [D/8-1:0] mask);
begin
    s_axi_wdata = value; s_axi_wstrb = mask; s_axi_wvalid = 1;
    #1; check(s_axi_wready, "W_ACCEPT"); tick;
    s_axi_wvalid = 0; s_axi_wdata = '1; s_axi_wstrb = 0;
end
endtask
task response;
integer n;
begin
    n = 0;
    while (!s_axi_bvalid && n < 40) begin tick; n = n + 1; end
    check(s_axi_bvalid && s_axi_bresp == 0, "WRITE_RESPONSE");
    s_axi_bready = 1; tick; s_axi_bready = 0;
end
endtask
task write(input [7:0] addr, input [D-1:0] value, input [D/8-1:0] mask);
begin address(addr); data(value, mask); response; end
endtask
task read(input [7:0] addr, output [D-1:0] value);
integer n;
begin
    s_axi_araddr = addr; s_axi_arvalid = 1; n = 0;
    #1;
    while (!s_axi_arready && n < 40) begin tick; n = n + 1; end
    check(s_axi_arready, "AR_ACCEPT"); tick; s_axi_arvalid = 0;
    check(s_axi_rvalid && s_axi_rresp == 0, "READ_RESPONSE");
    value = s_axi_rdata; s_axi_rready = 1; tick; s_axi_rready = 0;
end
endtask
task warm_reset;
reg [D-1:0] saved;
begin
    address(0); data(RUN, '1);
    s_axi_araddr = D/8; s_axi_arvalid = 1; tick; s_axi_arvalid = 0;
    saved = s_axi_rdata;
    check(s_axi_bvalid && s_axi_rvalid && saved == (RUN | DONE), "OLD_RESPONSE");
    #1; warm_n = 0; #1;
    check(power_en && !reset_active, "WARM_EDGE_HOLD");
    warm_n = 1;
    repeat (60) begin
        tick;
        check(s_axi_bvalid && s_axi_bresp == 0, "WARM_B_HOLD");
        check(s_axi_rvalid && s_axi_rresp == 0 && s_axi_rdata == saved, "WARM_R_HOLD");
    end
    check(power_en == (INITIAL != 0) && iso_active && reset_active, "WARM_TARGET");
    response; s_axi_rready = 1; tick; s_axi_rready = 0;
end
endtask
)";
}

QString apbBench()
{
    return R"(
reg [7:0] s_apb_paddr = 0;
reg [D-1:0] s_apb_pwdata = 0;
reg [D/8-1:0] s_apb_pstrb = 0;
reg [2:0] s_apb_pprot = 0;
reg s_apb_pselx = 0, s_apb_penable = 0, s_apb_pwrite = 0;
wire [D-1:0] s_apb_prdata;
wire s_apb_pready, s_apb_pslverr;
controller dut(.*);
wire register_ready = s_apb_pready;
task transfer(input bit writing, input [7:0] addr, input [D-1:0] value,
              input [D/8-1:0] mask, output [D-1:0] result);
integer n;
begin
    s_apb_paddr = addr; s_apb_pwdata = value; s_apb_pstrb = mask;
    s_apb_pwrite = writing; s_apb_pselx = 1; s_apb_penable = 0; tick;
    s_apb_penable = 1; n = 0; #1;
    while (!s_apb_pready && n < 40) begin tick; n = n + 1; end
    check(s_apb_pready && !s_apb_pslverr, "APB_COMPLETE");
    result = s_apb_prdata; tick; s_apb_pselx = 0; s_apb_penable = 0;
end
endtask
task write(input [7:0] addr, input [D-1:0] value, input [D/8-1:0] mask);
reg [D-1:0] unused;
begin transfer(1, addr, value, mask, unused); end
endtask
task read(input [7:0] addr, output [D-1:0] value);
begin transfer(0, addr, 0, 0, value); end
endtask
task warm_reset;
begin
    #1; warm_n = 0; #1;
    check(power_en && !reset_active, "WARM_EDGE_HOLD");
    warm_n = 1;
    repeat (60) tick;
    check(power_en == (INITIAL != 0) && iso_active && reset_active, "WARM_TARGET");
end
endtask
)";
}

QString operationBench()
{
    return R"(
task await_mode(input [D-1:0] mode);
reg [D-1:0] value;
integer n;
begin
    n = 0; read(D/8, value);
    while (value != (mode | DONE) && n < 80) begin read(D/8, value); n = n + 1; end
    check(value == (mode | DONE), "MODE_COMPLETION");
end
endtask
reg [D-1:0] value;
integer before_edge, startup_edge;
initial begin
    tick; por_n = 1;
    for (startup_edge = 1; startup_edge <= 2*SAMPLE; startup_edge = startup_edge + 1) begin
        tick; check(register_ready == (startup_edge == 2*SAMPLE), "COLD_RELEASE");
    end
    await_mode(INITIAL);
    write(0, RUN, '1); await_mode(RUN);
    check(power_en && !reset_active && !iso_active && !idle, "ACTUAL_RUN");
    before_edge = clock_edge; repeat (4) tick;
    check(clock_edge > before_edge, "CLOCK_RUN");
    write(0, 3, '1); repeat (5) tick;
    read(0, value); check(value == 3, "INVALID_READBACK");
    read(D/8, value); check(value == (3 | INVALID), "INVALID_STATUS");
    check(power_en && !reset_active && !iso_active && !idle, "INVALID_TARGET_HOLD");
    write(0, 0, 0); read(0, value); check(value == 3, "MASK_HOLD");
    write(0, 1, '1); await_mode(1);
    check(power_en && reset_active && iso_active && idle, "ACTUAL_RESET");
    write(0, RUN, '1); await_mode(RUN);
    warm_reset; await_mode(INITIAL);
    write(0, 0, '1); await_mode(0);
    before_edge = clock_edge; repeat (4) tick;
    check(clock_edge == before_edge, "CLOCK_OFF");
    write(0, RUN, '1); await_mode(RUN);
    power_fail = 1; repeat (12) tick;
    read(D/4, value); check(value == 1, "FAULT_HISTORY");
    read(D/8, value); check(value[D-1], "FAULT_STATUS");
    #1; warm_n = 0; #1; warm_n = 1;
    repeat (60) tick;
    read(D/4, value); check(value == 1, "WARM_EVENT_HOLD");
    write(0, 0, '1); await_mode(0);
    power_fail = 0; write(D/4, 1, '1);
    read(D/4, value); check(value == 0, "HISTORY_ACK");
    write(0, RUN, '1); await_mode(RUN);
    $display("PRCM_PASS"); $finish;
end
endmodule
)";
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
    void rejectInvalid()
    {
        const auto node  = declaration(32, false);
        const auto bound = QSocPrcmBinding::resolve(node, "controller.soc_net");
        QVERIFY(bound.plan);
        for (const auto &name : {"controller$unit", "controller$"})
            QVERIFY2(QSocPrcmGenerator::generate(*bound.plan, name, 2).circuit, name);
        for (const auto &name :
             {"clock_cell",
              "reset_cell",
              "qsoc_prcm_domain",
              "qsoc_rst_sync",
              "qsoc_tc_clk_gate",
              "bad-name"}) {
            const auto result = QSocPrcmGenerator::generate(*bound.plan, name, 2);
            QVERIFY2(!result.circuit, name);
            QCOMPARE(result.diagnostic.size(), 1);
        }
        QVERIFY(!QSocPrcmGenerator::generate(*bound.plan, "controller", 1).circuit);
        auto large                                    = *bound.plan;
        large.input.domain["periph"].mode["RUN"].code = quint64(1) << 29;
        const auto width = QSocPrcmGenerator::generate(large, "controller", 2);
        QVERIFY(!width.circuit);
        QVERIFY(width.diagnostic[0].message.contains("one MMIO data word"));
        QCOMPARE(width.diagnostic[0].source.size(), 2);
        QCOMPARE(width.diagnostic[0].source[0].path, "prcm.domain.periph.mode.RUN.code");
        QCOMPARE(width.diagnostic[0].source[1].path, "prcm.mmio.data_width");
        auto alias                                               = *bound.plan;
        alias.input.domain["periph"].isolation.completion.signal = "idle";
        const auto feedback = QSocPrcmGenerator::generate(alias, "controller", 2);
        QVERIFY(!feedback.circuit);
        QVERIFY(feedback.diagnostic[0].message.contains("distinct feedback"));
        QCOMPARE(feedback.diagnostic[0].source.size(), 2);
        auto collisionNode                                           = declaration(32, false);
        collisionNode["prcm"]["supply"]["periph"]["valid"]["signal"] = "s_apb_paddr";
        const auto collision = QSocPrcmBinding::resolve(collisionNode, "controller.soc_net");
        QVERIFY(collision.plan);
        const auto bus = QSocPrcmGenerator::generate(*collision.plan, "controller", 2);
        QVERIFY(!bus.circuit);
        QVERIFY(bus.diagnostic[0].message.contains("Bus port conflicts"));
    }

    void behavior_data()
    {
        QTest::addColumn<int>("width");
        QTest::addColumn<bool>("axi");
        QTest::addColumn<bool>("activeHigh");
        QTest::newRow("apb8") << 8 << false << false;
        QTest::newRow("apb16-high") << 16 << false << true;
        QTest::newRow("apb32") << 32 << false << false;
        QTest::newRow("axi32") << 32 << true << false;
        QTest::newRow("axi64-high") << 64 << true << true;
    }

    void behavior()
    {
        QFETCH(int, width);
        QFETCH(bool, axi);
        QFETCH(bool, activeHigh);
        const auto tool = QStandardPaths::findExecutable("verilator");
        if (tool.isEmpty())
            QSOC_TEST_MISSING_DEPENDENCY("verilator");
        const auto bound
            = QSocPrcmBinding::resolve(declaration(width, axi, activeHigh), "controller.soc_net");
        QVERIFY(bound.plan);
        const auto generated
            = QSocPrcmGenerator::generate(*bound.plan, "controller", activeHigh ? 3 : 2);
        QStringList error;
        for (const auto &item : generated.diagnostic)
            error.append(item.message);
        QVERIFY2(generated.circuit.has_value(), qPrintable(error.join('\n')));
        const auto &circuit = *generated.circuit;
        QCOMPARE(circuit.rtl.size(), 7);
        QCOMPARE(circuit.mmio.registers[0].byteOffset, quint64(0));
        QCOMPARE(circuit.mmio.registers[1].byteOffset, quint64(width / 8));
        QCOMPARE(circuit.mmio.registers[2].byteOffset, quint64(width / 4));
        QCOMPARE(circuit.mmio.registers[0].fields[0].width, quint32(width - 3));
        QCOMPARE(circuit.mmio.registers[1].fields.size(), 4);
        QCOMPARE(circuit.mmio.registers[1].fields[0].width, quint32(width - 3));
        QCOMPARE(circuit.mmio.registers[1].fields[1].lsb, quint32(width - 3));
        QCOMPARE(circuit.mmio.registers[1].fields[2].lsb, quint32(width - 2));
        QCOMPARE(circuit.mmio.registers[1].fields[3].lsb, quint32(width - 1));
        QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_prcm_circuit-XXXXXX");
        QVERIFY(directory.isValid());
        QStringList file;
        for (auto it = circuit.rtl.cbegin(); it != circuit.rtl.cend(); ++it) {
            QVERIFY(save(
                directory.filePath(it.key()),
                "`default_nettype none\n" + it.value() + "\n`default_nettype wire\n"));
            file.append(it.key());
        }
        auto bench = commonBench() + (axi ? axiBench() : apbBench()) + operationBench();
        bench.replace("@WIDTH@", QString::number(width));
        bench.replace("@INITIAL@", width == 16 ? "1" : "0");
        bench.replace("@STAGE@", activeHigh ? "3" : "2");
        bench.replace("@RESET@", activeHigh ? "periph_n" : "!periph_n");
        QVERIFY(save(directory.filePath("tb.sv"), bench));
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
        QVERIFY(run.contains("PRCM_PASS"));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocprcmgenerator.moc"
