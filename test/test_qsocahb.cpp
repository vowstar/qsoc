// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsociomuxgenerator.h"
#include "common/qsocmmioformal.h"
#include "common/qsocmmiogenerator.h"
#include "common/qsocmmiouvm.h"
#include "common/qsocmodulemanager.h"
#include "qsoc_test.h"

#include <algorithm>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {
bool save(const QString &path, const QString &text)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(text.toUtf8()) == text.toUtf8().size();
}

QSocMmioPlan memoryPlan(quint32 width)
{
    QSocMmioPlan plan;
    plan.moduleName     = "dut";
    plan.bus            = QSocMmioBus::AhbLite;
    plan.dataWidth      = width;
    plan.addressWidth   = 16;
    plan.idWidth        = 4;
    const quint32 bytes = width / 8;
    for (quint32 address = 0x100; address < 0x200; address += bytes) {
        QSocMmioRegisterPlan reg;
        reg.name       = QString("word_%1").arg(address);
        reg.byteOffset = address;
        for (quint32 lane = 0; lane < bytes; ++lane) {
            QSocMmioFieldPlan field;
            field.name       = QString("byte_%1").arg(lane);
            field.lsb        = lane * 8;
            field.width      = 8;
            field.access     = QSocMmioAccess::ReadWrite;
            field.resetValue = 0;
            if (address == 0x100 && lane == 0)
                field.outputPort = "byte0_o";
            reg.fields.append(field);
        }
        plan.registers.append(reg);
    }
    return plan;
}

QString bench(const QSocMmioPlan &plan)
{
    QStringList ports;
    for (const auto &port : QSocMmioGenerator::describePorts(plan)) {
        if (port.name == "s_ahb_hready")
            continue;
        ports.append(QString("%1 %2%3%4;")
                         .arg(
                             port.direction == "input" ? "reg" : "wire",
                             port.width == 1 ? QString() : QString("[%1:0] ").arg(port.width - 1),
                             port.name,
                             port.direction == "input" ? " = 0" : ""));
    }
    QString source = R"(module tb;
localparam integer D = @D@;
localparam integer B = D/8;
@PORTS@
reg external_wait = 0;
wire s_ahb_hready = s_ahb_hreadyout && !external_wait;
always #5 clk_i = !clk_i;
dut u_dut(.*);
reg [7:0] memory [0:65535];
reg active = 0, writing = 0, bad = 0;
integer addr = 0, size = 0, delay = 0;
integer transactions = 0, observations = 0;
integer i, lane, sz, kind, n;
reg [D-1:0] expected;

function bit invalid(input integer address, amount);
    return amount > B || address % amount != 0
        || address < 'h100 || address + amount > 'h200;
endfunction

always @(posedge clk_i) begin
    if (!rst_ni) begin
        if (s_ahb_hreadyout !== 1 || s_ahb_hresp !== 0) $fatal(1, "RESET_RESPONSE");
        if (byte0_o !== 0) $fatal(1, "RESET_VALUE");
        active = 0; delay = 0;
        for (integer k = 0; k < 65536; k++) memory[k] = 0;
    end else begin
        if (byte0_o !== memory['h100]) $fatal(1, "WRITE_TIMING");
        if (s_ahb_hreadyout !== (!active || delay == 0)) $fatal(1, "READY_PHASE");
        if (s_ahb_hresp !== (active && bad)) $fatal(1, "ERROR_PHASE");
        if (active && s_ahb_hready) begin
            transactions++;
            if (!bad) begin
                if (writing) begin
                    for (integer k = 0; k < (1 << size); k++)
                        memory[addr+k] = s_ahb_hwdata[((addr+k)%B)*8 +: 8];
                end else begin
                    expected = 0;
                    for (integer k = 0; k < B; k++) expected[k*8 +: 8] = memory[addr-addr%B+k];
                    if (s_ahb_hrdata !== expected) $fatal(1, "READBACK addr=%h want=%h got=%h", addr, expected, s_ahb_hrdata);
                    observations++;
                end
            end
        end
        if (delay != 0) delay--;
        if (s_ahb_hready) begin
            active = s_ahb_hsel && s_ahb_htrans[1];
            if (active) begin
                addr = s_ahb_haddr; size = s_ahb_hsize; writing = s_ahb_hwrite;
                bad = invalid(addr, 1 << size);
                delay = bad ? 1 : 0;
            end
        end
    end
end

function [D-1:0] pattern(input integer beat);
    for (integer k = 0; k < B; k++) pattern[k*8 +: 8] = 8'(beat*37 + k*13 + 7);
endfunction

// The address belongs to the next transfer, data to the preceding transfer.
task cycle(input bit sel, input integer trans, address, input bit wr,
           input integer size, burst, input reg [D-1:0] data);
    @(negedge clk_i);
    s_ahb_hsel = sel; s_ahb_htrans = 2'(trans); s_ahb_haddr = 16'(address);
    s_ahb_hwrite = wr; s_ahb_hsize = 3'(size); s_ahb_hburst = 3'(burst);
    s_ahb_hwdata = data; s_ahb_hprot = 4'b0011;
    do @(posedge clk_i); while (!s_ahb_hready);
    #1;
endtask

task access(input bit wr, input integer address, size, input reg [D-1:0] data);
    cycle(1, 2, address, wr, size, 0, ~data);
    cycle(0, 0, 0, 0, 0, 0, data);
endtask

function integer burst_address(input integer start, beat, count, size, kind);
    integer next, span;
    next = start + beat * (1 << size);
    span = count * (1 << size);
    if (kind != 0 && kind % 2 == 0) next = start - start % span + next % span;
    return next;
endfunction

task burst(input bit wr, input integer kind, count, size);
    integer start, a;
    bit busy;
    start = 'h100;
    if (kind != 0 && kind % 2 == 0) start += (count-1) * (1 << size);
    cycle(1, 2, start, wr, size, kind, '1);
    for (integer beat = 1; beat <= count; beat++) begin
        busy = beat < count && beat % 3 == 0;
        a = burst_address(start, beat, count, size, kind);
        if (busy) cycle(1, 1, a, wr, size, kind, pattern(beat-1));
        cycle(beat < count, beat < count ? 3 : 0, a, wr, size, kind, busy ? '0 : pattern(beat-1));
    end
endtask

initial begin
    repeat (3) @(negedge clk_i); rst_ni = 1;
    for (sz = 0; sz <= $clog2(B); sz++) begin
        access(1, 'h100, sz, pattern(sz));
        access(0, 'h100, sz, 0);
        burst(1, 1, 2, sz); burst(0, 1, 2, sz);
    end
    for (kind = 0; kind < 8; kind++) begin
        n = kind == 0 ? 1 : kind == 1 ? 9 : (1 << (kind/2 + 1));
        burst(1, kind, n, 0); burst(0, kind, n, 0);
    end
    @(negedge clk_i); s_ahb_hmastlock = 1;
    burst(1, 3, 4, 0); burst(0, 3, 4, 0);
    @(negedge clk_i); s_ahb_hmastlock = 0;
    for (i = 0; i < B; i++) access(1, 'h100+i, 0, pattern(i));
    for (i = 0; i < 256; i += B) access(0, 'h100+i, $clog2(B), 0);
    // IDLE and BUSY carry distracting addresses but never access the register bank.
    cycle(1, 0, 'h100, 1, 0, 0, '1);
    cycle(1, 1, 'h100, 1, 0, 0, '1);
    cycle(0, 2, 'h100, 1, 0, 0, '1);
    cycle(0, 0, 0, 0, 0, 0, '1);
    access(0, 'h100, 0, 0);
    // A pending address waits for the preceding slave's data phase to finish.
    @(negedge clk_i); external_wait = 1; s_ahb_hsel = 1; s_ahb_htrans = 2;
    s_ahb_hwrite = 1; s_ahb_haddr = 'h100; s_ahb_hwdata = '1;
    repeat (3) @(negedge clk_i);
    external_wait = 0;
    @(posedge clk_i); #1;
    cycle(0, 0, 0, 0, 0, 0, '0);
    access(0, 'h100, 0, 0);
    // Continuous address phases pair each write with its own data phase.
    cycle(1, 2, 'h130, 1, 0, 0, '0);
    cycle(1, 2, 'h131, 1, 0, 0, pattern(10));
    cycle(1, 2, 'h130, 0, 0, 0, pattern(11));
    cycle(0, 0, 0, 0, 0, 0, '1);
    access(0, 'h131, 0, 0);
    access(1, 'h200, 0, '1); access(0, 'h200, 0, 0);
    if (D > 8) access(1, 'h101, 1, '1);
    if (D < 1024) access(1, 'h100, $clog2(B)+1, '1);
    access(1, 'hffff, 7, '1);
    // Cancel the speculative next address during the first ERROR cycle.
    cycle(1, 2, 'h200, 1, 0, 0, '0);
    @(negedge clk_i); s_ahb_hsel = 1; s_ahb_htrans = 2; s_ahb_haddr = 'h140; s_ahb_hwdata = '1;
    @(posedge clk_i); #1;
    cycle(0, 0, 0, 0, 0, 0, '1);
    access(0, 'h140, 0, 0);
    cycle(1, 2, 'h200, 1, 0, 0, '0);
    cycle(1, 2, 'h208, 0, 0, 0, '1);
    cycle(1, 2, 'h150, 1, 0, 0, '1);
    cycle(0, 0, 0, 0, 0, 0, pattern(20));
    access(0, 'h150, 0, 0);
    // Reset cancels a captured write before its data phase can commit.
    cycle(1, 2, 'h100, 1, 0, 0, '0);
    @(negedge clk_i); rst_ni = 0; s_ahb_hwdata = '1; s_ahb_hsel = 0; s_ahb_htrans = 0;
    repeat (2) @(negedge clk_i); rst_ni = 1;
    access(0, 'h100, 0, 0);
    // Reset also clears both ERROR phases.
    for (i = 0; i < 2; i++) begin
        cycle(1, 2, 'h200, 1, 0, 0, '0);
        if (i != 0) begin @(posedge clk_i); #1; end
        @(negedge clk_i); rst_ni = 0; s_ahb_hsel = 0; s_ahb_htrans = 0;
        repeat (2) @(negedge clk_i); rst_ni = 1;
        access(0, 'h100, 0, 0);
    end
    $display("AHB_PASS transactions=%0d observations=%0d", transactions, observations);
    $finish;
end
initial begin #1000000; $fatal(1, "TIMEOUT"); end
endmodule
)";
    source.replace("@D@", QString::number(plan.dataWidth));
    source.replace("@PORTS@", ports.join('\n'));
    return source;
}
class Test : public QObject
{
    Q_OBJECT
private slots:
    void behavior_data();
    void behavior();
    void configuration();
    void collateral_data();
    void collateral();
    void iomux_data();
    void iomux();
};

void Test::behavior_data()
{
    QTest::addColumn<int>("width");
    QTest::addColumn<bool>("full");
    for (int width : {8, 16, 32, 64, 128, 256, 512, 1024}) {
        QTest::newRow(qPrintable(QString("lite%1").arg(width))) << width << false;
        QTest::newRow(qPrintable(QString("full%1").arg(width))) << width << true;
    }
}

void Test::behavior()
{
    QFETCH(int, width);
    QFETCH(bool, full);
    const QString simulator = QStandardPaths::findExecutable("verilator");
    if (simulator.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY("verilator");
    }
    QSocMmioPlan plan = memoryPlan(quint32(width));
    plan.bus          = full ? QSocMmioBus::Ahb : QSocMmioBus::AhbLite;
    QStringList errors;
    QVERIFY2(QSocMmioGenerator::canonicalizePlan(&plan, &errors), qPrintable(errors.join('\n')));
    QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_ahb_XXXXXX");
    QVERIFY(directory.isValid());
    const QString evidence = qEnvironmentVariable("QSOC_AHB_EVIDENCE");
    if (!evidence.isEmpty())
        directory.setAutoRemove(false);
    QVERIFY(save(directory.filePath("dut.v"), QSocMmioGenerator::generateVerilog(plan)));
    QVERIFY(save(directory.filePath("tb.sv"), bench(plan)));
    QProcess process;
    process.setWorkingDirectory(directory.path());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(
        simulator,
        {"--binary", "--timing", "-j", "16", "-Wno-fatal", "--top-module", "tb", "dut.v", "tb.sv"});
    QVERIFY(process.waitForStarted());
    QVERIFY(process.waitForFinished(300000));
    const QByteArray buildOutput = process.readAll();
    QVERIFY(save(directory.filePath("build.log"), QString::fromUtf8(buildOutput)));
    QVERIFY2(process.exitCode() == 0, buildOutput.constData());
    process.start(directory.filePath("obj_dir/Vtb"), QStringList{});
    QVERIFY(process.waitForStarted());
    QVERIFY(process.waitForFinished(60000));
    const QByteArray output = process.readAll();
    QVERIFY(save(directory.filePath("run.log"), QString::fromUtf8(output)));
    if (!evidence.isEmpty())
        qInfo().noquote() << width << full << directory.path();
    QVERIFY2(process.exitCode() == 0, output.constData());
    QVERIFY2(output.contains("AHB_PASS"), output.constData());
}

void Test::configuration()
{
    QSocModuleManager manager;
    for (int width : {8, 16, 32, 64, 128, 256, 512, 1024}) {
        const auto   source = QString(
                                  "generator:\n  kind: mmio\n  bus: ahb_lite\n  data_width: %1\n  "
                                  "address_width: 16\n  register:\n    value:\n      offset: 0\n     "
                                  " field:\n        data: {lsb: 0, width: 8, access: rw, reset: 0}\n")
                                  .arg(width);
        QSocMmioPlan plan;
        QStringList  errors;
        QVERIFY2(
            QSocMmioGenerator::buildPlan(
                manager.moduleYamlToDefinition("peripheral", "dut", YAML::Load(source.toStdString())),
                &plan,
                &errors),
            qPrintable(errors.join('\n')));
        QCOMPARE(plan.bus, QSocMmioBus::AhbLite);
        const auto yaml = QSocMmioGenerator::describeModuleYaml(plan);
        QVERIFY(YAML::Dump(yaml).find("ahb_lite") != std::string::npos);
        QString fullSource = source;
        fullSource.replace("bus: ahb_lite", "bus: ahb");
        QVERIFY2(
            QSocMmioGenerator::buildPlan(
                manager.moduleYamlToDefinition(
                    "peripheral", "dut", YAML::Load(fullSource.toStdString())),
                &plan,
                &errors),
            qPrintable(errors.join('\n')));
        QCOMPARE(plan.bus, QSocMmioBus::Ahb);
        QVERIFY(
            YAML::Dump(QSocMmioGenerator::describeModuleYaml(plan)).find("bus: ahb")
            != std::string::npos);
    }
    for (int width : {0, 7, 24, 96, 2048}) {
        auto plan      = memoryPlan(32);
        plan.dataWidth = width;
        QVERIFY(!QSocMmioGenerator::canonicalizePlan(&plan));
    }
    auto plan         = memoryPlan(32);
    plan.addressWidth = 33;
    QVERIFY(!QSocMmioGenerator::canonicalizePlan(&plan));
    plan                                   = memoryPlan(32);
    plan.registers[0].fields[0].outputPort = "ahb_active_q";
    QVERIFY(!QSocMmioGenerator::canonicalizePlan(&plan));
}

QSocMmioPlan smallPlan(int width)
{
    QSocMmioPlan plan = memoryPlan(quint32(width));
    plan.registers.clear();
    QSocMmioRegisterPlan identity;
    identity.name       = "identity";
    identity.byteOffset = 0;
    QSocMmioFieldPlan value;
    value.name          = "impid";
    value.width         = 64;
    value.access        = QSocMmioAccess::ReadOnly;
    value.constantValue = Q_UINT64_C(0x8123456789abcdef);
    identity.fields.append(value);
    plan.registers.append(identity);
    QSocMmioRegisterPlan data;
    data.name        = "data";
    data.byteOffset  = 0x100;
    value            = {};
    value.name       = "selector";
    value.width      = 17;
    value.resetValue = 0;
    value.access     = QSocMmioAccess::ReadWrite;
    value.outputPort = "selector_o";
    data.fields.append(value);
    value            = {};
    value.name       = "pending";
    value.lsb        = 17;
    value.width      = 1;
    value.access     = QSocMmioAccess::WriteOneClear;
    value.resetValue = 0;
    value.inputPort  = "pending_set_i";
    value.outputPort = "pending_o";
    data.fields.append(value);
    value           = {};
    value.name      = "status";
    value.lsb       = 24;
    value.width     = 8;
    value.access    = QSocMmioAccess::ReadOnly;
    value.inputPort = "status_i";
    data.fields.append(value);
    for (auto field : data.fields) {
        const quint64 offset = data.byteOffset + (field.lsb / quint32(width)) * quint32(width / 8);
        if (plan.registers.last().byteOffset != offset) {
            QSocMmioRegisterPlan reg;
            reg.name       = field.name;
            reg.byteOffset = offset;
            plan.registers.append(reg);
        }
        field.lsb %= quint32(width);
        plan.registers.last().fields.append(field);
    }
    return plan;
}

void Test::collateral_data()
{
    QTest::addColumn<int>("width");
    QTest::addColumn<bool>("formal");
    QTest::addColumn<bool>("full");
    for (int width : {8, 32, 128, 1024}) {
        QTest::newRow(qPrintable(QString("formal%1").arg(width))) << width << true << false;
        QTest::newRow(qPrintable(QString("uvm%1").arg(width))) << width << false << false;
    }
    QTest::newRow("full-formal32") << 32 << true << true;
    QTest::newRow("full-uvm32") << 32 << false << true;
}

void Test::collateral()
{
    QFETCH(int, width);
    QFETCH(bool, formal);
    QFETCH(bool, full);
    const QString tool = QStandardPaths::findExecutable(formal ? "sby" : "verilator");
    if (tool.isEmpty())
        QSOC_TEST_MISSING_DEPENDENCY("verification tool");
    const QString uvmSource = qEnvironmentVariable("UVM_HOME") + "/src";
    if (!formal && !QFile::exists(uvmSource + "/uvm_pkg.sv"))
        QSOC_TEST_MISSING_DEPENDENCY("UVM_HOME/src/uvm_pkg.sv");
    QSocMmioPlan plan = smallPlan(width);
    plan.bus          = full ? QSocMmioBus::Ahb : QSocMmioBus::AhbLite;
    QStringList errors;
    QVERIFY2(QSocMmioGenerator::canonicalizePlan(&plan, &errors), qPrintable(errors.join('\n')));
    QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_ahb_collateral_XXXXXX");
    QVERIFY(directory.isValid());
    if (!qEnvironmentVariableIsEmpty("QSOC_AHB_EVIDENCE")) {
        directory.setAutoRemove(false);
        qInfo().noquote() << "collateral" << width << formal << full << directory.path();
    }
    QVERIFY(save(directory.filePath("dut.v"), QSocMmioGenerator::generateVerilog(plan)));
    QProcess process;
    process.setWorkingDirectory(directory.path());
    process.setProcessChannelMode(QProcess::MergedChannels);
    if (formal) {
        const auto collateral = QSocMmioFormal::generate(plan);
        QVERIFY(save(directory.filePath("dut_formal.sv"), collateral.systemVerilog));
        QVERIFY(save(directory.filePath("dut.sby"), collateral.sby));
        for (const QString &task : {"prove", "bmc", "cover"}) {
            process.start(tool, {"-f", "dut.sby", task});
            QVERIFY(process.waitForStarted());
            QVERIFY(process.waitForFinished(600000));
            const QByteArray output = process.readAll();
            QVERIFY(save(directory.filePath(task + ".log"), QString::fromUtf8(output)));
            QVERIFY2(process.exitCode() == 0 && output.contains("PASS"), output.constData());
        }
    } else {
        const auto collateral = QSocMmioUvm::generate(plan);
        QVERIFY(save(directory.filePath("dut_uvm_if.sv"), collateral.interfaceSource));
        QVERIFY(save(directory.filePath("dut_uvm_pkg.sv"), collateral.packageSource));
        QVERIFY(save(directory.filePath("dut_uvm_tb.sv"), collateral.testbenchSource));
        QVERIFY(save(directory.filePath("dut_uvm.fl"), collateral.fileList));
        process.start(
            tool,
            {"--binary",
             "--timing",
             "-j",
             "16",
             "-Wno-fatal",
             "--top-module",
             "dut_uvm_tb",
             "+define+UVM_NO_DPI",
             "-I" + uvmSource,
             uvmSource + "/uvm_pkg.sv",
             "-f",
             "dut_uvm.fl"});
        QVERIFY(process.waitForStarted());
        QVERIFY(process.waitForFinished(300000));
        QByteArray output = process.readAll();
        QVERIFY(save(directory.filePath("build.log"), QString::fromUtf8(output)));
        QVERIFY2(process.exitCode() == 0, output.constData());
        process.start(directory.filePath("obj_dir/Vdut_uvm_tb"), QStringList{});
        QVERIFY(process.waitForStarted());
        QVERIFY(process.waitForFinished(60000));
        output = process.readAll();
        QVERIFY(save(directory.filePath("run.log"), QString::fromUtf8(output)));
        QVERIFY2(process.exitCode() == 0 && output.contains("UVM Report Summary"), output.constData());
    }
}

void Test::iomux_data()
{
    QTest::addColumn<int>("width");
    QTest::addColumn<bool>("full");
    for (int width : {8, 16, 32, 64, 128, 256, 512, 1024})
        QTest::newRow(qPrintable(QString::number(width))) << width << false;
    QTest::newRow("full32") << 32 << true;
}

QString iomuxBench(int width, const QString &top)
{
    QStringList              declarations;
    const QRegularExpression portPattern(
        "(input|output)\\s+wire\\s+(\\[[0-9]+:0\\]\\s+)?([A-Za-z0-9_]+)");
    const QString module  = top.mid(top.lastIndexOf("module dut ("));
    auto          matches = portPattern.globalMatch(module.left(module.indexOf(");")));
    while (matches.hasNext()) {
        const auto match = matches.next();
        if (match.captured(3) == "s_ahb_hready") {
            declarations.append("wire s_ahb_hready = s_ahb_hreadyout;");
            continue;
        }
        declarations.append(
            (match.captured(1) == "input" ? "reg " : "wire ") + match.captured(2)
            + match.captured(3) + (match.captured(1) == "input" ? " = 0;" : ";"));
    }
    QString source = R"(module tb;
localparam integer D = @D@;
localparam integer B = D/8;
@PORTS@
always #5 clk_i = !clk_i;
dut u_dut(.*);
integer transactions = 0;
integer i, lane;
reg [63:0] identity = 64'h8123456789abcdef;
reg [D-1:0] data;


task transfer(input bit wr, input integer address, size, input reg [D-1:0] value,
              input bit error, output reg [D-1:0] result);
    integer waits;
    @(negedge clk_i);
    s_ahb_hsel = 1; s_ahb_htrans = 2; s_ahb_haddr = 16'(address);
    s_ahb_hwrite = wr; s_ahb_hsize = 3'(size);
    @(posedge clk_i);
    @(negedge clk_i);
    s_ahb_hsel = 0; s_ahb_htrans = 0; s_ahb_hwdata = value;
    waits = 0;
    do begin
        @(posedge clk_i);
        if (s_ahb_hresp !== error) $fatal(1, "IOMUX_RESPONSE address=%h", address);
        if (!s_ahb_hready) waits++;
    end while (!s_ahb_hready);
    if (waits != (error ? 1 : 0)) $fatal(1, "IOMUX_ERROR_PHASE");
    result = s_ahb_hrdata;
    @(negedge clk_i);
    transactions++;
endtask

task write_access(input integer address, size, input reg [D-1:0] value, input bit error);
    reg [D-1:0] result;
    transfer(1, address, size, value, error, result);
endtask

task write_byte(input integer address, input reg [7:0] value, input bit error);
    reg [D-1:0] word_data;
    word_data = 0;
    word_data[(address % B)*8 +: 8] = value;
    write_access(address, 0, word_data, error);
endtask

task read_byte(input integer address, input reg [7:0] expected, input bit error);
    reg [D-1:0] result;
    transfer(0, address, 0, '0, error, result);
    if (result[(address % B)*8 +: 8] !== expected)
        $fatal(1, "IOMUX_BYTE_READ address=%h expected=%h actual=%h", address, expected, result);
endtask

initial begin
    repeat (3) @(negedge clk_i); rst_ni = 1;
    pad_input_value_i = 2;
    for (i = 0; i < 8; i++) begin
        read_byte(i, identity[i*8 +: 8], 0);
        write_byte(i, 0, 0);
        read_byte(i, identity[i*8 +: 8], 0);
    end
    write_byte('h80, 'hff, 0);
    read_byte('h80, 0, 0);
    read_byte(0, 'hef, 0);
    // The upper selector byte takes effect before its lower byte is changed.
    write_byte('h101, 1, 0);
    if (pad_output_value_o[0] !== 1 || pad_output_enable_o[0] !== 1) $fatal(1, "IOMUX_HIGH_ROUTE");
    write_byte('h100, 1, 0);
    if (pad_output_value_o[0] !== 0 || pad_output_enable_o[0] !== 0) $fatal(1, "IOMUX_INVALID_ROUTE");
    read_byte('h101, 1, 0);
    write_byte('h100, 0, 0);
    if (pad_output_value_o[0] !== 1) $fatal(1, "IOMUX_BYTE_PRESERVATION");
    write_byte('h102, 1, 0);
    if (pad_output_value_o !== 3) $fatal(1, "IOMUX_NEIGHBOR_ROUTE");
    read_byte('h108, 2, 0);
    if (D >= 256) begin
        data = 0;
        data[8 +: 8] = 1;
        data[(2 % B)*8 +: 8] = 1;
        data[(24 % B)*8 +: 8] = 1;
        write_access('h100, $clog2(B), data, 0);
        read_byte('h118, 1, 0);
        read_byte('h101, 1, 0);
        read_byte('h102, 1, 0);
    end
    // The last word contains both implemented and out-of-window bytes.
    read_byte('h119f, 0, 0);
    read_byte('h11a0, 0, 1);
    write_byte('h11a0, 'hff, 1);
    if (D > 256) write_access('h1180, $clog2(B), '1, 1);
    read_byte('h101, 1, 0);
    @(negedge clk_i); rst_ni = 0;
    repeat (2) @(negedge clk_i); rst_ni = 1;
    read_byte('h101, 0, 0);
    if (pad_output_value_o !== 0) $fatal(1, "IOMUX_RESET_ROUTE");
    $display("IOMUX_AHB_PASS width=%0d transactions=%0d", D, transactions);
    $finish;
end
initial begin #1000000; $fatal(1, "TIMEOUT"); end
endmodule
)";
    source.replace("@D@", QString::number(width));
    source.replace("@PORTS@", declarations.join('\n'));
    return source;
}

void Test::iomux()
{
    QFETCH(int, width);
    QFETCH(bool, full);
    QString source = QString(R"(generator:
  kind: iomux
  bus: ahb_lite
  data_width: %1
  address_width: 16
  pin_count: 2
  hs_slots: 257
  impid: 0x8123456789abcdef
  option: {gpio: true, invert: true, rx_override: true}
  integration:
    instance: u_mux
    clock: clk
    reset: rst_n
    control: control
    pad: {input_value: pad_in, input_enable: pad_ie, output_value: pad_out, output_enable: pad_oe}
  route:
    - {pin: 0, slot: 256, function: serial, signal: tx, output_value: 1, output_enable: 1}
    - {pin: 1, slot: 1, function: gpio, signal: out, output_value: 1, output_enable: 1}
)")
                         .arg(width);
    if (full)
        source.replace("bus: ahb_lite", "bus: ahb");
    QSocModuleManager manager;
    QSocIomuxPlan     plan;
    QStringList       errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(
            manager.moduleYamlToDefinition("peripheral", "dut", YAML::Load(source.toStdString())),
            &plan,
            &errors),
        qPrintable(errors.join('\n')));
    const QString header = QSocIomuxGenerator::generateSoftwareHeader(plan);
    QVERIFY(header.contains("IMPID_VALUE"));
    QVERIFY(QSocIomuxGenerator::generateReport(plan).contains("impid: 0x8123456789abcdef"));
    const auto ports = QSocMmioGenerator::describePorts(plan.mmio);
    QVERIFY(std::any_of(ports.cbegin(), ports.cend(), [full](const auto &port) {
        return port.name == "s_ahb_hresp" && port.width == (full ? 2u : 1u);
    }));
    QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_ahb_iomux_XXXXXX");
    QVERIFY(directory.isValid());
    if (!qEnvironmentVariableIsEmpty("QSOC_AHB_EVIDENCE")) {
        directory.setAutoRemove(false);
        qInfo().noquote() << "iomux" << width << full << directory.path();
    }
    const QString top = QSocIomuxGenerator::generateTopVerilog(plan);
    QVERIFY(save(
        directory.filePath("dut.v"),
        QSocIomuxGenerator::generateRegsVerilog(plan)
            + QSocIomuxGenerator::generateConnVerilog(plan) + top));
    QVERIFY(save(directory.filePath("config.yaml"), source));
    QVERIFY(save(directory.filePath("regs.h"), header));
    QVERIFY(save(directory.filePath("tb.sv"), iomuxBench(width, top)));
    const QString tool = QStandardPaths::findExecutable("verilator");
    if (tool.isEmpty())
        QSOC_TEST_MISSING_DEPENDENCY("verilator");
    QProcess process;
    process.setWorkingDirectory(directory.path());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(
        tool,
        {"--binary", "--timing", "-j", "16", "-Wno-fatal", "--top-module", "tb", "dut.v", "tb.sv"});
    QVERIFY(process.waitForStarted());
    QVERIFY(process.waitForFinished(300000));
    QByteArray output = process.readAll();
    QVERIFY(save(directory.filePath("build.log"), QString::fromUtf8(output)));
    QVERIFY2(process.exitCode() == 0, output.constData());
    process.start(directory.filePath("obj_dir/Vtb"), QStringList{});
    QVERIFY(process.waitForStarted());
    QVERIFY(process.waitForFinished(60000));
    output = process.readAll();
    QVERIFY(save(directory.filePath("run.log"), QString::fromUtf8(output)));
    QVERIFY2(process.exitCode() == 0 && output.contains("IOMUX_AHB_PASS"), output.constData());
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocahb.moc"
