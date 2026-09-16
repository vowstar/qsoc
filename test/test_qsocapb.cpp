// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsociomuxgenerator.h"
#include "common/qsocmmioformal.h"
#include "common/qsocmmiogenerator.h"
#include "common/qsocmmiouvm.h"
#include "common/qsocmodulemanager.h"
#include "qsoc_test.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {
QSocModuleDefinition definition(const QString &text, const QString &name)
{
    QSocModuleManager manager;
    return manager.moduleYamlToDefinition("peripheral", name, YAML::Load(text.toStdString()));
}

QString mmioSource(int width)
{
    return QString(R"(generator:
  kind: mmio
  bus: apb4
  data_width: %1
  address_width: 16
  register:
    impid:
      offset: 0
      field:
        value: {lsb: 0, width: 64, access: ro, value: 0x8123456789abcdef}
    selector:
      offset: 0x10
      field:
        value: {lsb: 0, width: 17, access: rw, reset: 0, output: selector_o}
    neighbor:
      offset: 0x14
      field:
        value: {lsb: 0, width: 8, access: rw, reset: 0x5a, output: neighbor_o}
    status:
      offset: 0x18
      field:
        pending: {lsb: 0, access: w1c, reset: 0, input: event_i, output: event_o}
    shifted:
      offset: 0x20
      field:
        value: {lsb: 7, width: 64, access: ro, value: 0x8123456789abcdef}
)")
        .arg(width);
}

QString iomuxSource(int width)
{
    return QString(R"(generator:
  kind: iomux
  bus: apb4
  data_width: %1
  address_width: 16
  pin_count: 2
  hs_slots: 257
  impid: 0x8123456789abcdef
  integration:
    instance: u_mux
    clock: clk
    reset: rst_n
    control: control
    pad:
      input_value: pad_in
      input_enable: pad_ie
      output_value: pad_out
      output_enable: pad_oe
  route:
    - {pin: 0, slot: 256, function: serial, signal: tx, output_value: 1, output_enable: 1}
    - {pin: 1, slot: 1, function: gpio, signal: out, output_value: 1, output_enable: 1}
)")
        .arg(width);
}

QString lowSpeedSource(int width)
{
    auto node              = YAML::Load(iomuxSource(width).toStdString());
    auto generator         = node["generator"];
    generator["pin_count"] = 257;
    generator["hs_slots"]  = 1;
    generator["route"]     = YAML::Load("[]");
    generator["ls"]        = YAML::Load(R"(
main:
  pins: [0, 256]
  channel:
    - {channel: 0, function: gpio, signal: low, output_value: 0, output_enable: 1}
    - {channel: 256, function: serial, signal: high, input_value: {link: rx}, input_enable: 1, output_value: 1, output_enable: 1}
other:
  pins: [1]
  reset: 1
  channel:
    - {channel: 1, function: other, signal: tx, output_value: 1, output_enable: 1}
)");
    return QString::fromStdString(YAML::Dump(node));
}

bool save(const QString &path, const QString &text)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(text.toUtf8()) == text.toUtf8().size();
}

QString bench(int width, bool iomux, bool lowSpeed)
{
    QString source = QString(R"(module tb;
localparam integer D = %1;
localparam integer B = D / 8;
reg clk_i = 0;
always #5 clk_i = !clk_i;
reg rst_ni = 0;
reg [15:0] s_apb_paddr = 0;
reg s_apb_pselx = 0, s_apb_penable = 0, s_apb_pwrite = 0;
reg [D-1:0] s_apb_pwdata = 0;
reg [B-1:0] s_apb_pstrb = 0;
reg [2:0] s_apb_pprot = 0;
wire [D-1:0] s_apb_prdata;
wire s_apb_pready, s_apb_pslverr;
%2
dut u_dut (.*);
integer transactions = 0;
reg [7:0] memory [0:2047];
reg [63:0] identity;
integer i;

task transfer(input bit wr, input integer address, input reg [31:0] data,
              input reg [3:0] mask, input reg [31:0] expected, input bit error);
    @(negedge clk_i);
    s_apb_pselx = 1;
    s_apb_penable = 0;
    s_apb_paddr = 16'(address);
    s_apb_pwrite = wr;
    s_apb_pwdata = D'(data);
    s_apb_pstrb = wr ? B'(mask) : 0;
    s_apb_pprot = 3'(transactions);
    @(posedge clk_i);
    @(negedge clk_i);
    s_apb_penable = 1;
    #1;
    if (!s_apb_pready || s_apb_pslverr !== error)
        $fatal(1, "RESPONSE address=%h", address);
    if (!wr && s_apb_prdata !== D'(expected))
        $fatal(1, "READBACK address=%h got=%h expected=%h", address, s_apb_prdata, D'(expected));
    @(posedge clk_i);
    @(negedge clk_i);
    s_apb_pselx = 0;
    s_apb_penable = 0;
    transactions = transactions + 1;
endtask

task write_byte(input integer address, input reg [7:0] value);
    transfer(1, address - address % B, 32'(value) << (8 * (address % B)),
             4'(1 << (address % B)), 0, 0);
endtask

task read_bytes(input integer start_address, input integer byte_count);
    reg [31:0] expected;
    for (integer address = start_address; address < start_address + byte_count; address += B) begin
        expected = 0;
        for (integer lane = 0; lane < B; lane++)
            expected = expected | (32'(memory[address + lane]) << (lane * 8));
        transfer(0, address, 0, 0, expected, 0);
    end
endtask

task consecutive_reads;
    @(negedge clk_i);
    s_apb_pselx = 1; s_apb_penable = 0; s_apb_pwrite = 0;
    s_apb_pstrb = 0; s_apb_paddr = 0;
    @(negedge clk_i); s_apb_penable = 1;
    #1;
    if (!s_apb_pready || s_apb_pslverr || s_apb_prdata !== D'(identity))
        $fatal(1, "READBACK consecutive first");
    @(negedge clk_i);
    s_apb_penable = 0; s_apb_paddr = 16'(B);
    @(negedge clk_i); s_apb_penable = 1;
    #1;
    if (!s_apb_pready || s_apb_pslverr || s_apb_prdata !== D'(identity >> D))
        $fatal(1, "READBACK consecutive second");
    @(negedge clk_i); s_apb_pselx = 0; s_apb_penable = 0;
    transactions = transactions + 2;
endtask

initial begin
    identity = 64'h8123456789abcdef;
    for (i = 0; i < 2048; i++) memory[i] = 0;
    for (i = 0; i < 8; i++) memory[i] = 8'(identity >> (8*i));
    repeat (2) @(negedge clk_i);
    rst_ni = 1;
    read_bytes(0, 8);
    consecutive_reads();
    for (i = 0; i < 8; i++) write_byte(i, 0);
    read_bytes(0, 8);
%3
    $display("APB_PASS transactions=%d", transactions);
    $finish;
end
initial begin
    #100000;
    $fatal(1, "TIMEOUT");
end
endmodule
)")
                         .arg(width);
    if (lowSpeed) {
        return source.arg(
            R"(reg [256:0] pad_input_value_i = 0;
wire [256:0] pad_input_enable_o, pad_output_value_o, pad_output_enable_o;
wire ls_c256_input_value_o;)",
            R"(
    memory['h18a] = 1;
    pad_input_value_i[256] = 1;
    write_byte('h188, 1);
    if (pad_output_enable_o[0]) $fatal(1, "LS_CROSS_POOL_CHANNEL");
    write_byte('h189, 1);
    if (pad_output_enable_o[0]) $fatal(1, "LS_INVALID_CHANNEL");
    write_byte('h188, 0);
    memory['h189] = 1;
    read_bytes('h188, 8);
    if (!pad_output_value_o[0] || !pad_output_enable_o[0]) $fatal(1, "LS256_TX");
    if (pad_output_value_o[256]) $fatal(1, "LS_NEIGHBOR");
    write_byte('h389, 1);
    memory['h389] = 1;
    read_bytes('h388, 8);
    if (!pad_output_value_o[256]) $fatal(1, "LS_PIN256_TX");
    write_byte('h591, 1);
    memory['h591] = 1;
    read_bytes('h590, 8);
    if (!ls_c256_input_value_o) $fatal(1, "LS_PIN256_RX");
    write_byte('h590, 1);
    if (ls_c256_input_value_o) $fatal(1, "LS_INVALID_RX");
    write_byte('h591, 0);
    if (ls_c256_input_value_o) $fatal(1, "LS_CROSS_POOL_RX");
    write_byte('h590, 0);
    if (ls_c256_input_value_o) $fatal(1, "LS_RX_PIN0");
)");
    }
    if (iomux) {
        return source.arg(
            R"(reg [1:0] pad_input_value_i = 0;
wire [1:0] pad_input_enable_o, pad_output_value_o, pad_output_enable_o;)",
            R"(
    read_bytes(8, 248);
    write_byte('h100, 1);
    if (pad_output_value_o !== 0) $fatal(1, "UNDECLARED_SLOT");
    write_byte('h101, 1);
    memory['h100] = 1; memory['h101] = 1;
    read_bytes('h100, 8);
    if (pad_output_value_o !== 0) $fatal(1, "INVALID_SLOT");
    write_byte('h100, 0);
    memory['h100] = 0;
    read_bytes('h100, 8);
    if (pad_output_value_o !== 1 || pad_output_enable_o !== 1) $fatal(1, "HS256_PIN");
    write_byte('h102, 1);
    memory['h102] = 1;
    read_bytes('h100, 8);
    if (pad_output_value_o !== 3) $fatal(1, "NEIGHBOR_PIN");
    transfer(1, 'h100, 0, 0, 0, 0);
    read_bytes('h100, 8);
    transfer(1, 'h80, '1, '1, 0, 0);
    read_bytes('h80, 8);
    transfer(0, 'h108, 0, 0, 0, 1);
    if (B > 1) transfer(0, 'h101, 0, 0, 0, 0);
    @(negedge clk_i); rst_ni = 0;
    @(negedge clk_i); rst_ni = 1;
    for (i = 'h100; i < 'h108; i++) memory[i] = 0;
    read_bytes('h100, 8);
    if (pad_output_value_o !== 0) $fatal(1, "RESET_ROUTE");
)");
    }
    return source.arg(
        R"(wire [16:0] selector_o;
wire [7:0] neighbor_o;
reg event_i = 0;
wire event_o;)",
        R"(
    for (i = 0; i < 64; i++) memory['h20 + (i+7)/8][(i+7)%8] = identity[i];
    read_bytes('h20, (9+B-1)/B*B);
    for (i = 'h20; i < 'h29; i++) write_byte(i, 'hff);
    read_bytes('h20, (9+B-1)/B*B);
    memory['h14] = 'h5a;
    read_bytes('h14, B);
    write_byte('h10, 'ha5); memory['h10] = 'ha5;
    write_byte('h11, 'h5a); memory['h11] = 'h5a;
    write_byte('h12, 1); memory['h12] = 1;
    read_bytes('h10, (3+B-1)/B*B);
    if (selector_o !== 17'h15aa5 || neighbor_o !== 'h5a) $fatal(1, "FIELD_OUTPUT");
    transfer(1, 'h10, 0, 0, 0, 0);
    read_bytes('h10, (3+B-1)/B*B);
    @(negedge clk_i);
    s_apb_pselx = 1; s_apb_penable = 0; s_apb_pwrite = 1;
    s_apb_paddr = 'h10; s_apb_pwdata = 0; s_apb_pstrb = '1;
    @(posedge clk_i); #1;
    if (selector_o !== 17'h15aa5) $fatal(1, "SETUP_WRITE");
    @(negedge clk_i); rst_ni = 0; s_apb_pselx = 0;
    @(negedge clk_i); rst_ni = 1;
    if (selector_o !== 0 || neighbor_o !== 'h5a) $fatal(1, "RESET_FIELD");
    event_i = 1;
    transfer(1, 'h18, 1, 1, 0, 0);
    if (!event_o) $fatal(1, "SET_WINS_CLEAR");
    event_i = 0;
    transfer(1, 'h18, 1, 0, 0, 0);
    if (!event_o) $fatal(1, "W1C_MASK");
    transfer(1, 'h18, 1, 1, 0, 0);
    if (event_o) $fatal(1, "W1C_CLEAR");
    transfer(0, 'h40, 0, 0, 0, 1);
    transfer(1, 'h40, '1, '1, 0, 1);
    if (B > 1) transfer(0, 'h11, 0, 0, 0, 1);
)");
}
} // namespace

class Test : public QObject
{
    Q_OBJECT
private slots:
    void validation();
    void behavior_data();
    void behavior();
    void collateral_data();
    void collateral();
};

void Test::validation()
{
    QStringList  errors;
    QSocMmioPlan plan;
    for (int width : {8, 16, 32}) {
        QVERIFY2(
            QSocMmioGenerator::buildPlan(definition(mmioSource(width), "dut"), &plan, &errors),
            qPrintable(errors.join('\n')));
        QCOMPARE(plan.bus, QSocMmioBus::Apb4);
        QCOMPARE(
            QString::fromStdString(
                QSocMmioGenerator::describeModuleYaml(plan)["bus"]["control"]["bus"]
                    .as<std::string>()),
            QString("apb4"));
        auto overlap                    = plan;
        overlap.registers[1].byteOffset = 4;
        QVERIFY(!QSocMmioGenerator::canonicalizePlan(&overlap, &errors));
    }
    for (int width : {0, 7, 24, 64, 128}) {
        QVERIFY(!QSocMmioGenerator::buildPlan(definition(mmioSource(width), "dut"), &plan, &errors));
    }
    QVERIFY(!QSocMmioGenerator::buildPlan(
        definition(mmioSource(8).replace("bus: apb4", "bus: axi4_lite"), "dut"), &plan, &errors));
    QVERIFY(!QSocMmioGenerator::buildPlan(
        definition(mmioSource(32).replace("address_width: 16", "address_width: 33"), "dut"),
        &plan,
        &errors));
}

void Test::behavior_data()
{
    QTest::addColumn<int>("width");
    QTest::addColumn<bool>("iomux");
    QTest::addColumn<bool>("lowSpeed");
    for (int width : {8, 16, 32}) {
        QTest::newRow(qPrintable(QString("mmio%1").arg(width))) << width << false << false;
        QTest::newRow(qPrintable(QString("iomux%1").arg(width))) << width << true << false;
    }
    QTest::newRow("ls8") << 8 << true << true;
    QTest::newRow("ls16") << 16 << true << true;
}

void Test::behavior()
{
    QFETCH(int, width);
    QFETCH(bool, iomux);
    QFETCH(bool, lowSpeed);
    const QString simulator = QStandardPaths::findExecutable("verilator");
    if (simulator.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY("verilator");
    }
    QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_apb_XXXXXX");
    QVERIFY(directory.isValid());
    QStringList errors;
    QString     rtl;
    if (iomux) {
        QSocIomuxPlan plan;
        QVERIFY2(
            QSocIomuxGenerator::buildPlan(
                definition(lowSpeed ? lowSpeedSource(width) : iomuxSource(width), "dut"),
                &plan,
                &errors),
            qPrintable(errors.join('\n')));
        rtl = QSocIomuxGenerator::generateRegsVerilog(plan)
              + QSocIomuxGenerator::generateConnVerilog(plan)
              + QSocIomuxGenerator::generateTopVerilog(plan);
        QVERIFY(QSocIomuxGenerator::generateReport(plan).contains("impid: 0x8123456789abcdef"));
    } else {
        QSocMmioPlan plan;
        QVERIFY2(
            QSocMmioGenerator::buildPlan(definition(mmioSource(width), "dut"), &plan, &errors),
            qPrintable(errors.join('\n')));
        rtl = QSocMmioGenerator::generateVerilog(plan);
    }
    QVERIFY(save(directory.filePath("dut.v"), rtl));
    QVERIFY(save(directory.filePath("tb.sv"), bench(width, iomux, lowSpeed)));
    QProcess process;
    process.setWorkingDirectory(directory.path());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(
        simulator,
        {"--binary", "--timing", "-j", "16", "-Wno-fatal", "--top-module", "tb", "dut.v", "tb.sv"});
    QVERIFY(process.waitForStarted());
    QVERIFY(process.waitForFinished(300000));
    QByteArray output = process.readAll();
    QVERIFY2(process.exitCode() == 0, output.constData());
    process.start(directory.filePath("obj_dir/Vtb"), QStringList{});
    QVERIFY(process.waitForStarted());
    QVERIFY(process.waitForFinished(60000));
    output = process.readAll();
    QVERIFY2(process.exitCode() == 0, output.constData());
    QVERIFY2(output.contains("APB_PASS"), output.constData());
}

void Test::collateral_data()
{
    QTest::addColumn<int>("width");
    QTest::addColumn<bool>("formal");
    for (int width : {8, 16, 32}) {
        QTest::newRow(qPrintable(QString("formal%1").arg(width))) << width << true;
        QTest::newRow(qPrintable(QString("uvm%1").arg(width))) << width << false;
    }
}

void Test::collateral()
{
    QFETCH(int, width);
    QFETCH(bool, formal);
    const QString tool = QStandardPaths::findExecutable(formal ? "sby" : "verilator");
    if (tool.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(formal ? "sby" : "verilator");
    }
    const QString uvmSource = qEnvironmentVariable("UVM_HOME") + "/src";
    if (!formal && !QFile::exists(uvmSource + "/uvm_pkg.sv")) {
        QSOC_TEST_MISSING_DEPENDENCY("UVM_HOME/src/uvm_pkg.sv");
    }
    QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_apb_collateral_XXXXXX");
    QVERIFY(directory.isValid());
    QSocMmioPlan plan;
    QStringList  errors;
    QVERIFY2(
        QSocMmioGenerator::buildPlan(definition(mmioSource(width), "dut"), &plan, &errors),
        qPrintable(errors.join('\n')));
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
        QVERIFY2(process.exitCode() == 0, output.constData());
        process.start(directory.filePath("obj_dir/Vdut_uvm_tb"), QStringList{});
        QVERIFY(process.waitForStarted());
        QVERIFY(process.waitForFinished(60000));
        output = process.readAll();
        QVERIFY2(process.exitCode() == 0 && output.contains("UVM Report Summary"), output.constData());
    }
}

QSOC_TEST_MAIN(Test)
#include "test_qsocapb.moc"
