// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocmmioformal.h"
#include "common/qsocmmiogenerator.h"
#include "common/qsocmmiouvm.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QSocMmioPlan makePlan(QSocMmioBus bus, quint32 width)
{
    QSocMmioPlan plan;
    plan.moduleName   = "registers";
    plan.bus          = bus;
    plan.dataWidth    = width;
    plan.addressWidth = 8;
    plan.clearPort    = "clear_i";
    QSocMmioFieldPlan field;
    field.name       = "mode";
    field.width      = 2;
    field.access     = QSocMmioAccess::ReadWrite;
    field.resetValue = 1;
    field.outputPort = "mode_o";
    plan.registers.append({"request", {}, 0, {field}});
    field               = {};
    field.name          = "value";
    field.constantValue = 1;
    plan.registers.append({"status", {}, 8, {field}});
    field            = {};
    field.name       = "power_lost";
    field.access     = QSocMmioAccess::WriteOneClear;
    field.resetValue = 0;
    field.inputPort  = "event_i";
    field.outputPort = "event_o";
    plan.registers.append({"history", {}, 16, {field}});
    field            = {};
    field.name       = "byte";
    field.width      = 8;
    field.lsb        = width - 8;
    field.access     = QSocMmioAccess::ReadWrite;
    field.resetValue = 0x5a;
    field.outputPort = "byte_o";
    plan.registers.append({"aux", {}, 24, {field}});
    return plan;
}

QString commonBench()
{
    return R"(
module tb;
localparam D = @WIDTH@;
reg clk_i = 0, rst_ni = 0, clear_i = 0, event_i = 0;
wire [1:0] mode_o;
wire event_o;
wire [7:0] byte_o;
task tick;
begin
    #5; clk_i = 1; #5; clk_i = 0;
end
endtask
task check(input bit predicate, input string reason);
begin
    if (!predicate) $fatal(1, "%s", reason);
end
endtask
)";
}

QString axiBench()
{
    return commonBench() + R"(
reg [7:0] s_axi_awaddr = 0, s_axi_araddr = 0;
reg [2:0] s_axi_awprot = 0, s_axi_arprot = 0;
reg [D-1:0] s_axi_wdata = 0;
reg [D/8-1:0] s_axi_wstrb = 0;
reg s_axi_awvalid = 0, s_axi_wvalid = 0, s_axi_arvalid = 0;
reg s_axi_bready = 0, s_axi_rready = 0;
wire s_axi_awready, s_axi_wready, s_axi_arready, s_axi_bvalid, s_axi_rvalid;
wire [1:0] s_axi_bresp, s_axi_rresp;
wire [D-1:0] s_axi_rdata;
registers dut(.*);
task address(input [7:0] value);
begin
    s_axi_awaddr = value; s_axi_awvalid = 1;
    #1; check(s_axi_awready, "AW_ACCEPT"); tick;
    s_axi_awvalid = 0; s_axi_awaddr = 8'hff;
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
begin
    #1; check(s_axi_bvalid && s_axi_bresp == 0, "WRITE_RESPONSE");
    s_axi_bready = 1; tick; s_axi_bready = 0;
end
endtask
task write(input [7:0] addr, input [D-1:0] value, input [D/8-1:0] mask);
begin
    address(addr); data(value, mask); response;
end
endtask
initial begin
    tick; rst_ni = 1; tick;
    write(24, {D{1'b1}}, '1);
    check(byte_o == 8'hff, "AUX_WRITE");
    address(0); data(3, '1);
    event_i = 1;
    s_axi_arvalid = 1; s_axi_araddr = 0;
    tick; s_axi_arvalid = 0; event_i = 0;
    check(s_axi_rvalid && s_axi_rdata == 3, "READ_BEFORE_CLEAR");
    clear_i = 1;
    repeat (3) begin
        tick;
        check(mode_o == 1 && byte_o == 8'h5a, "RW_CLEAR");
        check(event_o, "EVENT_RETAIN");
        check(s_axi_bvalid && s_axi_bresp == 0, "B_RETAIN");
        check(s_axi_rvalid && s_axi_rdata == 3 && s_axi_rresp == 0, "R_RETAIN");
        check(!s_axi_arready, "READ_CLEAR_ORDER");
    end
    response;
    s_axi_rready = 1; tick; s_axi_rready = 0;
    check(!s_axi_rvalid && !s_axi_bvalid, "RESPONSE_CONSUME");
    s_axi_arvalid = 1; s_axi_araddr = 0;
    address(0); data(2, '1);
    repeat (2) begin
        tick;
        check(!s_axi_bvalid && mode_o == 1, "NO_WRITE_DURING_CLEAR");
        check(!s_axi_arready && !s_axi_rvalid, "NO_READ_DURING_CLEAR");
    end
    clear_i = 0; tick;
    s_axi_arvalid = 0;
    check(s_axi_rvalid && s_axi_rdata == 1, "QUEUED_READ");
    check(mode_o == 2, "QUEUED_WRITE");
    s_axi_rready = 1; response; s_axi_rready = 0;
    address(0);
    clear_i = 1; tick;
    data(3, '1);
    check(!s_axi_bvalid && mode_o == 1, "AW_CLEAR_HOLD");
    clear_i = 0; tick;
    check(mode_o == 3, "AW_CLEAR_COMMIT"); response;
    data(2, '1);
    clear_i = 1; tick;
    address(0);
    check(!s_axi_bvalid && mode_o == 1, "W_CLEAR_HOLD");
    clear_i = 0; tick;
    check(mode_o == 2, "W_CLEAR_COMMIT"); response;
    address(16);
    clear_i = 1; data(1, '1); tick;
    check(event_o && !s_axi_bvalid, "EVENT_CLEAR_HOLD");
    event_i = 1; clear_i = 0; tick;
    check(event_o, "EVENT_SET_PRIORITY"); response;
    event_i = 0; write(16, 1, '1);
    check(!event_o, "EVENT_W1C");
    clear_i = 1; event_i = 1; tick;
    check(event_o && mode_o == 1, "EVENT_SET_DURING_CLEAR");
    $display("CLEAR_PASS"); $finish;
end
endmodule
)";
}

QString apbBench()
{
    return commonBench() + R"(
reg [7:0] s_apb_paddr = 0;
reg [D-1:0] s_apb_pwdata = 0;
reg [D/8-1:0] s_apb_pstrb = 0;
reg [2:0] s_apb_pprot = 0;
reg s_apb_pselx = 0, s_apb_penable = 0, s_apb_pwrite = 0;
wire [D-1:0] s_apb_prdata;
wire s_apb_pready, s_apb_pslverr;
registers dut(.*);
task setup(input [7:0] addr, input [D-1:0] value);
begin
    s_apb_paddr = addr; s_apb_pwdata = value; s_apb_pstrb = '1;
    s_apb_pselx = 1; s_apb_penable = 0; s_apb_pwrite = 1; tick;
    s_apb_penable = 1;
end
endtask
task finish;
begin
    #1; check(s_apb_pready && !s_apb_pslverr, "APB_COMPLETE"); tick;
    s_apb_pselx = 0; s_apb_penable = 0;
end
endtask
initial begin
    tick; rst_ni = 1; tick;
    setup(0, 3); finish;
    setup(24, '1); finish;
    check(mode_o == 3 && byte_o == 8'hff, "APB_WRITE");
    event_i = 1; tick; event_i = 0;
    setup(0, 2);
    clear_i = 1;
    repeat (3) begin
        #1; check(!s_apb_pready, "APB_CLEAR_WAIT"); tick;
        check(mode_o == 1 && byte_o == 8'h5a, "RW_CLEAR");
        check(event_o, "EVENT_RETAIN");
    end
    clear_i = 0; finish;
    check(mode_o == 2, "APB_QUEUED_WRITE");
    setup(16, 1); event_i = 1; finish;
    check(event_o, "EVENT_SET_PRIORITY");
    event_i = 0; setup(16, 1); finish;
    check(!event_o, "EVENT_W1C");
    clear_i = 1; event_i = 1; tick;
    check(event_o && mode_o == 1, "EVENT_SET_DURING_CLEAR");
    $display("CLEAR_PASS"); $finish;
end
endmodule
)";
}

bool save(const QString &path, const QString &text)
{
    QFile      file(path);
    const auto bytes = text.toUtf8();
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

class Test : public QObject
{
    Q_OBJECT
private slots:
    void plan()
    {
        auto        plan = makePlan(QSocMmioBus::Apb4, 32);
        QStringList error;
        QVERIFY(QSocMmioGenerator::canonicalizePlan(&plan, &error));
        QVERIFY(QSocMmioGenerator::describePorts(plan).contains({"clear_i", "input", 1}));
        QVERIFY(QSocMmioFormal::generate(plan).systemVerilog.isEmpty());
        QVERIFY(QSocMmioUvm::generate(plan).testbenchSource.isEmpty());
        for (const auto &name :
             {"clk_i", "write_fire", "mmio_field_0_q", "mode_o", "event_o", "bad-name"}) {
            auto invalid      = plan;
            invalid.clearPort = name;
            QVERIFY2(!QSocMmioGenerator::canonicalizePlan(&invalid, &error), name);
        }
        for (auto bus : {QSocMmioBus::Axi4, QSocMmioBus::AhbLite, QSocMmioBus::Ahb}) {
            auto invalid = plan;
            invalid.bus  = bus;
            QVERIFY(!QSocMmioGenerator::canonicalizePlan(&invalid, &error));
            QVERIFY(error.join('\n').contains("MMIO_BUS plan.clear_port"));
            QVERIFY(QSocMmioGenerator::generateVerilog(invalid).isEmpty());
        }
    }

    void transaction_data()
    {
        QTest::addColumn<bool>("apb");
        QTest::addColumn<quint32>("width");
        QTest::newRow("apb8") << true << quint32(8);
        QTest::newRow("apb32") << true << quint32(32);
        QTest::newRow("axil32") << false << quint32(32);
        QTest::newRow("axil64") << false << quint32(64);
    }

    void transaction()
    {
        QFETCH(bool, apb);
        QFETCH(quint32, width);
        const auto tool = QStandardPaths::findExecutable("verilator");
        if (tool.isEmpty())
            QSOC_TEST_MISSING_DEPENDENCY("verilator");
        auto        plan = makePlan(apb ? QSocMmioBus::Apb4 : QSocMmioBus::Axi4Lite, width);
        QStringList error;
        QVERIFY2(QSocMmioGenerator::canonicalizePlan(&plan, &error), qPrintable(error.join('\n')));
        QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_mmio_clear-XXXXXX");
        QVERIFY(directory.isValid());
        QVERIFY(save(directory.filePath("dut.v"), QSocMmioGenerator::generateVerilog(plan)));
        auto bench = apb ? apbBench() : axiBench();
        bench.replace("@WIDTH@", QString::number(width));
        QVERIFY(save(directory.filePath("tb.sv"), bench));
        QProcess process;
        process.setWorkingDirectory(directory.path());
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(
            tool,
            {"--binary", "--timing", "--top-module", "tb", "-Wno-fatal", "-j", "16", "tb.sv", "dut.v"});
        QVERIFY(process.waitForStarted());
        QVERIFY(process.waitForFinished(180000));
        const auto build = process.readAll();
        QVERIFY2(process.exitCode() == 0, build.constData());
        process.start(directory.filePath("obj_dir/Vtb"), QStringList{});
        QVERIFY(process.waitForStarted());
        QVERIFY(process.waitForFinished(30000));
        const auto run = process.readAll();
        QVERIFY2(process.exitCode() == 0, run.constData());
        QVERIFY(run.contains("CLEAR_PASS"));
    }
};
} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocmmioclear.moc"
