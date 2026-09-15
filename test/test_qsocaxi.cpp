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
    plan.bus            = QSocMmioBus::Axi4;
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
        ports.append(QString("%1 %2%3%4;")
                         .arg(
                             port.direction == "input" ? "reg" : "wire",
                             port.width == 1 ? QString() : QString("[%1:0] ").arg(port.width - 1),
                             port.name,
                             port.direction == "input" ? " = 0" : ""));
    }
    QString source = R"(module tb;
localparam integer D = @D@;
localparam integer I = @I@;
localparam integer A = @A@;
localparam [A-1:0] BASE = @BASE@;
localparam integer B = D / 8;
@PORTS@
always #5 clk_i = !clk_i;
dut u_dut (.*);
reg [7:0] memory [0:65535];
integer transactions = 0;
integer observations = 0;
integer i, sz, kind, start, n;
reg [D-1:0] pattern;
reg [B-1:0] strobes;

function integer beat_address(input integer address, length, size, burst, beat);
    integer step, base, span;
    begin
        step = 2 ** size;
        span = step * length;
        base = address / step * step;
        if (beat == 0 || burst == 0)
            beat_address = address;
        else if (burst == 1)
            beat_address = base + beat * step;
        else
            beat_address = address / span * span + (address % span + beat * step) % span;
    end
endfunction

function integer active_byte(input integer address, size, lane);
    integer first, last;
    begin
        first = address % B;
        last = address / (2 ** size) * (2 ** size) + (2 ** size) - 1;
        active_byte = lane >= first && address / B * B + lane <= last;
    end
endfunction

function integer mapped(input integer address);
    mapped = address >= 'h100 && address < 'h200;
endfunction

task send_aw(input integer address, length, size, burst, ident);
    @(negedge clk_i);
    s_axi_awaddr = A'(address) + BASE;
    s_axi_awlen = 8'(length - 1);
    s_axi_awsize = 3'(size);
    s_axi_awburst = 2'(burst);
    s_axi_awid = I'(ident);
    s_axi_awvalid = 1;
    do @(posedge clk_i); while (!s_axi_awready);
    @(negedge clk_i);
    s_axi_awvalid = 0;
    s_axi_awaddr = '1;
    s_axi_awid = 0;
    s_axi_awlen = 0;
endtask

task send_w(input reg [D-1:0] data, input reg [B-1:0] mask, input bit last);
    @(negedge clk_i);
    s_axi_wdata = data;
    s_axi_wstrb = mask;
    s_axi_wlast = last;
    s_axi_wvalid = 1;
    do @(posedge clk_i); while (!s_axi_wready);
    @(negedge clk_i);
    s_axi_wvalid = 0;
    s_axi_wdata = ~data;
    s_axi_wstrb = ~mask;
    s_axi_wlast = !last;
endtask

task receive_b(input bit error, input integer ident, stalls);
    s_axi_bready = 0;
    while (!s_axi_bvalid) @(negedge clk_i);
    repeat (stalls + 1) begin
        if (!s_axi_bvalid || s_axi_bresp !== (error ? 2'b10 : 2'b00)
            || s_axi_bid !== I'(ident)) $fatal(1, "WRITE_RESPONSE");
        observations++;
        @(negedge clk_i);
    end
    s_axi_bready = 1;
    @(negedge clk_i);
    s_axi_bready = 0;
    transactions++;
endtask

task write_burst(input integer address, length, size, burst, ident, order, mask_mode,
                 input bit descriptor_error);
    integer beat, lane, current, byte_address;
    reg [D-1:0] data;
    reg [B-1:0] mask;
    bit error;
    error = descriptor_error;
    if (order == 0) send_aw(address, length, size, burst, ident);
    for (beat = 0; beat < length; beat++) begin
        current = beat_address(address, length, size, burst, beat);
        data = 0;
        mask = 0;
        for (lane = 0; lane < B; lane++) begin
            data[lane*8 +: 8] = 8'(beat * 17 + lane * 13 + ident);
            mask[lane] = active_byte(current, size, lane)
                         && (mask_mode == 0 || (mask_mode == 1 && (lane + beat) % 2 == 0));
            byte_address = current / B * B + lane;
            if (mask[lane] && mapped(current) && !descriptor_error)
                memory[byte_address] = data[lane*8 +: 8];
        end
        if (!mapped(current)) error = 1;
        send_w(data, mask, beat == length - 1);
        if (order != 0 && beat == 0) begin
            repeat (3) @(negedge clk_i);
            if (s_axi_bvalid) $fatal(1, "PREMATURE_WRITE_RESPONSE");
            send_aw(address, length, size, burst, ident);
        end
    end
    receive_b(error, ident, 3);
endtask

task read_burst(input integer address, length, size, burst, ident, stalls,
                input bit descriptor_error);
    integer beat, lane, current, byte_address;
    reg [D-1:0] expected;
    bit error;
    @(negedge clk_i);
    s_axi_araddr = A'(address) + BASE;
    s_axi_arlen = 8'(length - 1);
    s_axi_arsize = 3'(size);
    s_axi_arburst = 2'(burst);
    s_axi_arid = I'(ident);
    s_axi_arvalid = 1;
    do @(posedge clk_i); while (!s_axi_arready);
    @(negedge clk_i);
    s_axi_arvalid = 0;
    s_axi_araddr = '1;
    s_axi_arid = 0;
    for (beat = 0; beat < length; beat++) begin
        current = beat_address(address, length, size, burst, beat);
        expected = 0;
        error = descriptor_error || !mapped(current);
        for (lane = 0; lane < B; lane++) begin
            byte_address = current / B * B + lane;
            if (!error && active_byte(current, size, lane))
                expected[lane*8 +: 8] = memory[byte_address];
        end
        while (!s_axi_rvalid) @(negedge clk_i);
        repeat (stalls + 1) begin
            if (!s_axi_rvalid || s_axi_rdata !== expected)
                $fatal(1, "READ_DATA address=%h beat=%0d expected=%h actual=%h", current, beat, expected, s_axi_rdata);
            if (s_axi_rresp !== (error ? 2'b10 : 2'b00)) $fatal(1, "READ_ERROR");
            if (s_axi_rid !== I'(ident)) $fatal(1, "READ_ID");
            if (s_axi_rlast !== (beat == length - 1)) $fatal(1, "READ_LAST");
            observations++;
            @(negedge clk_i);
        end
        s_axi_rready = 1;
        @(negedge clk_i);
        s_axi_rready = 0;
    end
    transactions++;
endtask

initial begin
    for (i = 0; i < 65536; i++) memory[i] = 0;
    repeat (3) @(negedge clk_i);
    rst_ni = 1;
    for (sz = 0; sz <= $clog2(B); sz++) begin
        for (kind = 0; kind < 3; kind++) begin
            start = 'h100 + (kind == 2 ? 3 * (2 ** sz) : 0);
            write_burst(start, 4, sz, kind, 15, kind % 2, 0, 0);
            read_burst(start, 4, sz, kind, 9, 2, 0);
            write_burst(start, 4, sz, kind, 3, 1, 1, 0);
            read_burst(start, 4, sz, kind, 7, 0, 0);
            write_burst(start, 4, sz, kind, 0, 0, 2, 0);
            read_burst(start, 4, sz, kind, 1, 0, 0);
        end
        if (sz > 0) begin
            write_burst('h101, 4, sz, 1, 14, 1, 0, 0);
            read_burst('h101, 4, sz, 1, 13, 1, 0);
            write_burst('h101, 4, sz, 0, 12, 0, 1, 0);
            read_burst('h101, 4, sz, 0, 11, 1, 0);
        end
    end
    foreach (memory[i]) begin
        if (i >= 'h100 && i < 'h200 && i % B == 0)
            read_burst(i, 1, $clog2(B), 1, 15, 0, 0);
    end
    write_burst('h100, 256, 0, 1, 15, 1, 1, 0);
    read_burst('h100, 256, 0, 1, 14, 1, 0);
    write_burst('h1fc, 16, 0, 1, 13, 0, 0, 0);
    read_burst('h1fc, 16, 0, 1, 12, 1, 0);
    write_burst('hffc, 16, 0, 1, 11, 0, 0, 1);
    read_burst('hffc, 16, 0, 1, 10, 0, 1);
    for (n = 2; n <= 16; n *= 2) begin
        write_burst('h100 + n - 1, n, 0, 2, n % 16, 0, 0, 0);
        read_burst('h100 + n - 1, n, 0, 2, n % 16, 1, 0);
    end
    // A blocked write response must not prevent independent reads.
    send_aw('h100, 1, 0, 1, 5);
    send_w('0, '0, 1);
    for (n = 0; n < 3; n++) read_burst('h100, 1, 0, 1, n, 1, 0);
    receive_b(0, 5, 1);
    // A held read response must not prevent independent writes.
    @(negedge clk_i);
    s_axi_araddr = BASE + 'h100;
    s_axi_arlen = 0;
    s_axi_arsize = 0;
    s_axi_arburst = 1;
    s_axi_arid = 6;
    s_axi_arvalid = 1;
    do @(posedge clk_i); while (!s_axi_arready);
    @(negedge clk_i); s_axi_arvalid = 0;
    while (!s_axi_rvalid) @(negedge clk_i);
    pattern = s_axi_rdata;
    for (n = 0; n < 3; n++) begin
        write_burst('h180, 1, 0, 1, n, 1, 2, 0);
        if (!s_axi_rvalid || s_axi_rdata !== pattern || s_axi_rid !== I'(6))
            $fatal(1, "INDEPENDENT_READ_HOLD");
    end
    @(negedge clk_i); rst_ni = 0;
    repeat (2) @(negedge clk_i);
    if (s_axi_rvalid || s_axi_bvalid) $fatal(1, "RESET_RESPONSE");
    rst_ni = 1;
    for (i = 0; i < 65536; i++) memory[i] = 0;
    read_burst('h100, 1, 0, 1, 0, 0, 0);
    // Cancel an orphan write-data beat before pairing a new address.
    send_w('1, '1, 1);
    @(negedge clk_i); rst_ni = 0;
    repeat (2) @(negedge clk_i); rst_ni = 1;
    write_burst('h100, 1, 0, 1, 4, 0, 2, 0);
    read_burst('h100, 1, 0, 1, 3, 0, 0);
    if (I == 32) begin
        write_burst('h100, 1, 0, 1, 32'h80000001, 0, 0, 0);
        read_burst('h100, 1, 0, 1, 32'hffffffff, 3, 0);
    end
    $display("AXI_PASS transactions=%0d observations=%0d", transactions, observations);
    $finish;
end
initial begin #1000000; $fatal(1, "TIMEOUT"); end
endmodule
)";
    source.replace("@D@", QString::number(plan.dataWidth));
    source.replace("@I@", QString::number(plan.idWidth));
    source.replace("@A@", QString::number(plan.addressWidth));
    source.replace("@BASE@", QString("64'h%1").arg(plan.registers.first().byteOffset - 0x100, 0, 16));
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
    for (int width : {8, 16, 32, 64, 128, 256, 512, 1024}) {
        QTest::newRow(qPrintable(QString::number(width))) << width;
    }
}

void Test::behavior()
{
    QFETCH(int, width);
    const QString simulator = QStandardPaths::findExecutable("verilator");
    if (simulator.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY("verilator");
    }
    QSocMmioPlan plan = memoryPlan(quint32(width));
    if (width == 16)
        plan.idWidth = 1;
    if (width == 128)
        plan.idWidth = 32;
    if (width == 1024) {
        plan.addressWidth = 64;
        for (auto &reg : plan.registers)
            reg.byteOffset += Q_UINT64_C(0x100000000);
    }
    QStringList errors;
    QVERIFY2(QSocMmioGenerator::canonicalizePlan(&plan, &errors), qPrintable(errors.join('\n')));
    QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_axi_XXXXXX");
    QVERIFY(directory.isValid());
    const QString evidence = qEnvironmentVariable("QSOC_AXI_EVIDENCE");
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
        qInfo().noquote() << width << directory.path();
    QVERIFY2(process.exitCode() == 0, output.constData());
    QVERIFY2(output.contains("AXI_PASS"), output.constData());
}

void Test::configuration()
{
    QSocMmioPlan plan = memoryPlan(128);
    QStringList  errors;
    QVERIFY(QSocMmioGenerator::canonicalizePlan(&plan, &errors));
    plan.dataWidth = 96;
    QVERIFY(!QSocMmioGenerator::canonicalizePlan(&plan, &errors));
    plan     = memoryPlan(128);
    plan.bus = QSocMmioBus::Axi4Lite;
    QVERIFY(!QSocMmioGenerator::canonicalizePlan(&plan, &errors));
    plan         = memoryPlan(8);
    plan.idWidth = 0;
    QVERIFY(!QSocMmioGenerator::canonicalizePlan(&plan, &errors));
    for (int width : {8, 16, 32, 64, 128, 256, 512, 1024}) {
        const QString     yaml = QString(R"(generator:
  kind: mmio
  bus: axi4
  data_width: %1
  address_width: 16
  id_width: 32
  identity: {version: 129.2.255, type: 0xfedcba98}
  register:
    data:
      offset: 0x100
      field:
        value: {lsb: 0, width: 8, access: rw, reset: 0}
)")
                                     .arg(width);
        QSocModuleManager manager;
        QSocMmioPlan      parsed;
        errors.clear();
        QVERIFY2(
            QSocMmioGenerator::buildPlan(
                manager.moduleYamlToDefinition("peripheral", "dut", YAML::Load(yaml.toStdString())),
                &parsed,
                &errors),
            qPrintable(errors.join('\n')));
        QCOMPARE(parsed.idWidth, quint32(32));
        quint64 identity = 0;
        for (const auto &reg : parsed.registers) {
            if (reg.byteOffset >= 8)
                continue;
            for (const auto &field : reg.fields) {
                QVERIFY(field.constantValue.has_value());
                identity |= *field.constantValue << (reg.byteOffset * 8 + field.lsb);
            }
        }
        QCOMPARE(identity, Q_UINT64_C(0xfedcba988102ff00));
    }
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
    for (int width : {8, 32, 128, 1024}) {
        QTest::newRow(qPrintable(QString("formal%1").arg(width))) << width << true;
        QTest::newRow(qPrintable(QString("uvm%1").arg(width))) << width << false;
    }
}

void Test::collateral()
{
    QFETCH(int, width);
    QFETCH(bool, formal);
    const QString tool = QStandardPaths::findExecutable(formal ? "sby" : "verilator");
    if (tool.isEmpty())
        QSOC_TEST_MISSING_DEPENDENCY("verification tool");
    const QString uvmSource = qEnvironmentVariable("UVM_HOME") + "/src";
    if (!formal && !QFile::exists(uvmSource + "/uvm_pkg.sv"))
        QSOC_TEST_MISSING_DEPENDENCY("UVM_HOME/src/uvm_pkg.sv");
    QSocMmioPlan plan = smallPlan(width);
    QStringList  errors;
    QVERIFY2(QSocMmioGenerator::canonicalizePlan(&plan, &errors), qPrintable(errors.join('\n')));
    QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_axi_collateral_XXXXXX");
    QVERIFY(directory.isValid());
    if (!qEnvironmentVariableIsEmpty("QSOC_AXI_EVIDENCE")) {
        directory.setAutoRemove(false);
        qInfo().noquote() << "collateral" << width << formal << directory.path();
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
    for (int width : {8, 16, 32, 64, 128, 256, 512, 1024})
        QTest::newRow(qPrintable(QString::number(width))) << width;
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
reg [B-1:0] mask;

task write_access(input integer address, size, input reg [D-1:0] value,
                  input reg [B-1:0] strobe, input bit error);
    @(negedge clk_i);
    s_axi_awaddr = 16'(address); s_axi_awlen = 0; s_axi_awsize = 3'(size);
    s_axi_awburst = 1; s_axi_awid = 127; s_axi_awvalid = 1;
    do @(posedge clk_i); while (!s_axi_awready);
    @(negedge clk_i); s_axi_awvalid = 0;
    s_axi_wdata = value; s_axi_wstrb = strobe; s_axi_wlast = 1; s_axi_wvalid = 1;
    do @(posedge clk_i); while (!s_axi_wready);
    @(negedge clk_i); s_axi_wvalid = 0;
    while (!s_axi_bvalid) @(negedge clk_i);
    repeat (3) begin
        if (s_axi_bresp !== (error ? 2'b10 : 2'b00) || s_axi_bid !== 127)
            $fatal(1, "IOMUX_WRITE_RESPONSE address=%h", address);
        @(negedge clk_i);
    end
    s_axi_bready = 1;
    @(negedge clk_i); s_axi_bready = 0;
    transactions++;
endtask

task write_byte(input integer address, input reg [7:0] value, input bit error);
    reg [D-1:0] word_data;
    reg [B-1:0] byte_strobe;
    word_data = 0; byte_strobe = 0;
    word_data[(address % B)*8 +: 8] = value;
    byte_strobe[address % B] = 1;
    write_access(address, 0, word_data, byte_strobe, error);
endtask

task read_byte(input integer address, input reg [7:0] expected, input bit error);
    reg [D-1:0] expected_word;
    expected_word = 0;
    expected_word[(address % B)*8 +: 8] = expected;
    @(negedge clk_i);
    s_axi_araddr = 16'(address); s_axi_arlen = 0; s_axi_arsize = 0;
    s_axi_arburst = 1; s_axi_arid = 126; s_axi_arvalid = 1;
    do @(posedge clk_i); while (!s_axi_arready);
    @(negedge clk_i); s_axi_arvalid = 0;
    while (!s_axi_rvalid) @(negedge clk_i);
    repeat (3) begin
        if (s_axi_rdata !== expected_word) $fatal(1, "IOMUX_BYTE_READ address=%h expected=%h actual=%h", address, expected_word, s_axi_rdata);
        if (s_axi_rresp !== (error ? 2'b10 : 2'b00) || s_axi_rid !== 126 || !s_axi_rlast)
            $fatal(1, "IOMUX_READ_RESPONSE address=%h", address);
        @(negedge clk_i);
    end
    s_axi_rready = 1;
    @(negedge clk_i); s_axi_rready = 0;
    transactions++;
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
        data = 0; mask = 0;
        data[8 +: 8] = 1; mask[1] = 1;
        data[(2 % B)*8 +: 8] = 1; mask[2 % B] = 1;
        data[(24 % B)*8 +: 8] = 1; mask[24 % B] = 1;
        write_access('h100, $clog2(B), data, mask, 0);
        read_byte('h118, 1, 0);
        read_byte('h101, 1, 0);
        read_byte('h102, 1, 0);
    end
    // The last word contains both implemented and out-of-window bytes.
    read_byte('h119f, 0, 0);
    read_byte('h11a0, 0, 1);
    write_byte('h11a0, 'hff, 1);
    if (D > 256) write_access('h1180, $clog2(B), '1, '1, 1);
    read_byte('h101, 1, 0);
    @(negedge clk_i); rst_ni = 0;
    repeat (2) @(negedge clk_i); rst_ni = 1;
    read_byte('h101, 0, 0);
    if (pad_output_value_o !== 0) $fatal(1, "IOMUX_RESET_ROUTE");
    $display("IOMUX_AXI_PASS width=%0d transactions=%0d", D, transactions);
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
    const QString     source = QString(R"(generator:
  kind: iomux
  bus: axi4
  data_width: %1
  address_width: 16
  id_width: 7
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
    QSocModuleManager manager;
    QSocIomuxPlan     plan;
    QStringList       errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(
            manager.moduleYamlToDefinition("peripheral", "dut", YAML::Load(source.toStdString())),
            &plan,
            &errors),
        qPrintable(errors.join('\n')));
    QCOMPARE(plan.mmio.idWidth, quint32(7));
    const QString header = QSocIomuxGenerator::generateSoftwareHeader(plan);
    QVERIFY(header.contains("IMPID_VALUE"));
    QVERIFY(QSocIomuxGenerator::generateReport(plan).contains("impid: 0x8123456789abcdef"));
    const auto ports = QSocMmioGenerator::describePorts(plan.mmio);
    QVERIFY(std::any_of(ports.cbegin(), ports.cend(), [](const auto &port) {
        return port.name == "s_axi_awid" && port.width == 7;
    }));
    QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_axi_iomux_XXXXXX");
    QVERIFY(directory.isValid());
    if (!qEnvironmentVariableIsEmpty("QSOC_AXI_EVIDENCE")) {
        directory.setAutoRemove(false);
        qInfo().noquote() << "iomux" << width << directory.path();
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
    QVERIFY2(process.exitCode() == 0 && output.contains("IOMUX_AXI_PASS"), output.constData());
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocaxi.moc"
