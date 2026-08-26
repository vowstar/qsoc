// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocmmiogenerator.h"
#include "common/qsocmodulemanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

namespace {

QSocModuleDefinition makeDefinition(const QString &body, const QString &moduleName = "timer_ctrl")
{
    QSocModuleManager manager;
    return manager.moduleYamlToDefinition("peripheral", moduleName, YAML::Load(body.toStdString()));
}

QSocModuleDefinition makeValidDefinition()
{
    return makeDefinition(R"(
generator:
  kind: mmio
  bus: axi4_lite
  register:
    identification:
      offset: 0x00
      field:
        device_id:
          lsb: 0
          width: 8
          access: ro
          value: 0x2a
    control:
      offset: 0x04
      field:
        enable:
          lsb: 0
          access: rw
          reset: 0
          output: enable_o
        mode:
          lsb: 8
          width: 2
          access: rw
          reset: 1
          output: mode_o
    scratch:
      offset: 0x10
      field:
        data:
          lsb: 0
          width: 32
          access: rw
          reset: 0
          output: scratch_o
    status:
      offset: 0x08
      field:
        busy:
          lsb: 0
          access: ro
          input: busy_i
)");
}

QSocModuleDefinition makeReverseOrderedDefinition()
{
    return makeDefinition(R"(
generator:
  register:
    scratch:
      field:
        data:
          output: scratch_o
          reset: 0
          access: rw
          width: 32
          lsb: 0
      offset: 0x10
    status:
      field:
        busy:
          input: busy_i
          access: ro
          lsb: 0
      offset: 0x08
    control:
      field:
        mode:
          output: mode_o
          reset: 1
          access: rw
          width: 2
          lsb: 8
        enable:
          output: enable_o
          reset: 0
          access: rw
          lsb: 0
      offset: 0x04
    identification:
      field:
        device_id:
          value: 0x2a
          access: ro
          width: 8
          lsb: 0
      offset: 0x00
  bus: axi4_lite
  kind: mmio
)");
}

bool hasErrorPath(const QStringList &errors, const QString &path)
{
    for (const QString &error : errors) {
        if (error.startsWith(path + ':') || error.contains(QStringLiteral(" ") + path + ':')) {
            return true;
        }
    }
    return false;
}

void writeTextFile(const QString &path, const QString &text)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream(&file) << text;
}

QString protocolTestbench()
{
    return R"(`timescale 1ns/1ps
module tb;
reg         clk_i;
reg         rst_ni;
reg  [31:0] s_axi_awaddr;
reg  [2:0]  s_axi_awprot;
reg         s_axi_awvalid;
wire        s_axi_awready;
reg  [31:0] s_axi_wdata;
reg  [3:0]  s_axi_wstrb;
reg         s_axi_wvalid;
wire        s_axi_wready;
wire [1:0]  s_axi_bresp;
wire        s_axi_bvalid;
reg         s_axi_bready;
reg  [31:0] s_axi_araddr;
reg  [2:0]  s_axi_arprot;
reg         s_axi_arvalid;
wire        s_axi_arready;
wire [31:0] s_axi_rdata;
wire [1:0]  s_axi_rresp;
wire        s_axi_rvalid;
reg         s_axi_rready;
wire        enable_o;
wire [1:0]  mode_o;
wire [31:0] scratch_o;
reg         busy_i;
integer     failures;

timer_ctrl dut (
    .clk_i(clk_i),
    .rst_ni(rst_ni),
    .s_axi_awaddr(s_axi_awaddr),
    .s_axi_awprot(s_axi_awprot),
    .s_axi_awvalid(s_axi_awvalid),
    .s_axi_awready(s_axi_awready),
    .s_axi_wdata(s_axi_wdata),
    .s_axi_wstrb(s_axi_wstrb),
    .s_axi_wvalid(s_axi_wvalid),
    .s_axi_wready(s_axi_wready),
    .s_axi_bresp(s_axi_bresp),
    .s_axi_bvalid(s_axi_bvalid),
    .s_axi_bready(s_axi_bready),
    .s_axi_araddr(s_axi_araddr),
    .s_axi_arprot(s_axi_arprot),
    .s_axi_arvalid(s_axi_arvalid),
    .s_axi_arready(s_axi_arready),
    .s_axi_rdata(s_axi_rdata),
    .s_axi_rresp(s_axi_rresp),
    .s_axi_rvalid(s_axi_rvalid),
    .s_axi_rready(s_axi_rready),
    .enable_o(enable_o),
    .mode_o(mode_o),
    .scratch_o(scratch_o),
    .busy_i(busy_i)
);

task check_value;
    input condition;
    input [8*64-1:0] label;
    begin
        if (condition !== 1'b1) begin
            failures = failures + 1;
            $display("TEST_FAIL %0s", label);
        end
    end
endtask

task send_aw;
    input [31:0] address;
    begin
        @(negedge clk_i);
        s_axi_awaddr  = address;
        s_axi_awvalid = 1'b1;
        while (s_axi_awready !== 1'b1)
            @(negedge clk_i);
        @(posedge clk_i);
        #1 s_axi_awvalid = 1'b0;
    end
endtask

task capture_aw_then_perturb;
    input [31:0] accepted_address;
    input [31:0] changed_address;
    begin
        @(negedge clk_i);
        s_axi_awaddr  = accepted_address;
        s_axi_awvalid = 1'b1;
        check_value(s_axi_awready === 1'b1, "AW slot initially ready");
        @(posedge clk_i);
        #1;
        s_axi_awaddr = changed_address;
        check_value(s_axi_awready === 1'b0, "AW slot closes after handshake");
        @(negedge clk_i);
        s_axi_awvalid = 1'b0;
    end
endtask

task send_w;
    input [31:0] data;
    input [3:0] strobe;
    begin
        @(negedge clk_i);
        s_axi_wdata  = data;
        s_axi_wstrb  = strobe;
        s_axi_wvalid = 1'b1;
        while (s_axi_wready !== 1'b1)
            @(negedge clk_i);
        @(posedge clk_i);
        #1 s_axi_wvalid = 1'b0;
    end
endtask

task capture_w_then_perturb;
    input [31:0] accepted_data;
    input [3:0] accepted_strobe;
    input [31:0] changed_data;
    input [3:0] changed_strobe;
    begin
        @(negedge clk_i);
        s_axi_wdata  = accepted_data;
        s_axi_wstrb  = accepted_strobe;
        s_axi_wvalid = 1'b1;
        check_value(s_axi_wready === 1'b1, "W slot initially ready");
        @(posedge clk_i);
        #1;
        s_axi_wdata = changed_data;
        s_axi_wstrb = changed_strobe;
        check_value(s_axi_wready === 1'b0, "W slot closes after handshake");
        @(negedge clk_i);
        s_axi_wvalid = 1'b0;
    end
endtask

task start_write_together;
    input [31:0] address;
    input [31:0] data;
    input [3:0] strobe;
    begin
        @(negedge clk_i);
        s_axi_awaddr  = address;
        s_axi_awvalid = 1'b1;
        s_axi_wdata   = data;
        s_axi_wstrb   = strobe;
        s_axi_wvalid  = 1'b1;
        check_value(
            s_axi_awready === 1'b1 && s_axi_wready === 1'b1,
            "idle AW and W channels ready");
        @(posedge clk_i);
        #1;
        s_axi_awvalid = 1'b0;
        s_axi_wvalid  = 1'b0;
    end
endtask

task check_write_blocked_while_b_pending;
    begin
        @(negedge clk_i);
        #1 check_value(
            s_axi_awready === 1'b0 && s_axi_wready === 1'b0,
            "write channels blocked by B response");
        repeat (2) begin
            @(posedge clk_i);
            #1 check_value(
                s_axi_bvalid === 1'b1 && s_axi_awready === 1'b0
                    && s_axi_wready === 1'b0,
                "write channels stay blocked by B response");
        end
    end
endtask

task finish_b;
    input [1:0] expected_response;
    reg [1:0] held_response;
    begin
        while (s_axi_bvalid !== 1'b1)
            @(posedge clk_i);
        #1 held_response = s_axi_bresp;
        check_value(held_response === expected_response, "write response");
        repeat (2) begin
            @(posedge clk_i);
            #1 check_value(
                s_axi_bvalid === 1'b1 && s_axi_bresp === held_response,
                "write response backpressure");
        end
        @(negedge clk_i);
        s_axi_bready = 1'b1;
        @(posedge clk_i);
        #1 s_axi_bready = 1'b0;
        check_value(s_axi_bvalid === 1'b0, "write response accepted");
    end
endtask

task start_read;
    input [31:0] address;
    begin
        @(negedge clk_i);
        s_axi_araddr  = address;
        s_axi_arvalid = 1'b1;
        while (s_axi_arready !== 1'b1)
            @(negedge clk_i);
        @(posedge clk_i);
        #1 s_axi_arvalid = 1'b0;
    end
endtask

task finish_read;
    input [31:0] expected_data;
    input [1:0] expected_response;
    reg [31:0] held_data;
    reg [1:0] held_response;
    begin
        while (s_axi_rvalid !== 1'b1)
            @(posedge clk_i);
        #1 held_data = s_axi_rdata;
        held_response = s_axi_rresp;
        check_value(held_data === expected_data, "read data");
        check_value(held_response === expected_response, "read response");
        repeat (2) begin
            @(posedge clk_i);
            #1 check_value(
                s_axi_rvalid === 1'b1 && s_axi_rdata === held_data
                    && s_axi_rresp === held_response,
                "read response backpressure");
        end
        @(negedge clk_i);
        s_axi_rready = 1'b1;
        @(posedge clk_i);
        #1 s_axi_rready = 1'b0;
        check_value(s_axi_rvalid === 1'b0, "read response accepted");
    end
endtask

initial begin
    clk_i = 1'b0;
    forever #5 clk_i = ~clk_i;
end

initial begin
    #10000;
    $display("TEST_FAIL timeout");
    $finish;
end

initial begin
    failures       = 0;
    rst_ni         = 1'b0;
    s_axi_awaddr   = 32'b0;
    s_axi_awprot   = 3'b0;
    s_axi_awvalid  = 1'b0;
    s_axi_wdata    = 32'b0;
    s_axi_wstrb    = 4'b0;
    s_axi_wvalid   = 1'b0;
    s_axi_bready   = 1'b0;
    s_axi_araddr   = 32'b0;
    s_axi_arprot   = 3'b0;
    s_axi_arvalid  = 1'b0;
    s_axi_rready   = 1'b0;
    busy_i         = 1'b0;

    repeat (3) @(posedge clk_i);
    #1;
    check_value(
        !s_axi_awready && !s_axi_wready && !s_axi_arready,
        "ready low during reset");
    check_value(!s_axi_bvalid && !s_axi_rvalid, "valid low during reset");
    check_value(
        enable_o === 1'b0 && mode_o === 2'b01 && scratch_o === 32'b0,
        "outputs reset during reset");
    @(negedge clk_i);
    rst_ni = 1'b1;
    @(posedge clk_i);
    #1;
    check_value(s_axi_awready && s_axi_wready && s_axi_arready, "ready after reset");
    check_value(
        enable_o === 1'b0 && mode_o === 2'b01 && scratch_o === 32'b0,
        "field reset values");

    capture_aw_then_perturb(32'h00000004, 32'h0000000c);
    repeat (2) @(posedge clk_i);
    check_value(s_axi_bvalid === 1'b0, "address waits for data");
    send_w(32'h00000001, 4'b0001);
    #1;
    check_value(enable_o === 1'b1 && mode_o === 2'b01, "byte zero write strobe");
    finish_b(2'b00);

    capture_w_then_perturb(32'h00000200, 4'b0010, 32'hffffffff, 4'b1111);
    repeat (2) @(posedge clk_i);
    check_value(s_axi_bvalid === 1'b0, "data waits for address");
    send_aw(32'h00000004);
    #1;
    check_value(enable_o === 1'b1 && mode_o === 2'b10, "byte one write strobe");
    finish_b(2'b00);

    start_write_together(32'h00000004, 32'h00000100, 4'b0011);
    check_value(s_axi_bvalid && s_axi_bresp === 2'b00, "same-cycle write response");
    check_value(enable_o === 1'b0 && mode_o === 2'b01, "same-cycle write commits");
    check_write_blocked_while_b_pending();
    finish_b(2'b00);

    start_read(32'h00000004);
    finish_read(32'h00000100, 2'b00);

    start_write_together(32'h00000010, 32'h00ab0000, 4'b0100);
    check_value(scratch_o === 32'h00ab0000, "byte two write strobe");
    finish_b(2'b00);
    start_read(32'h00000010);
    finish_read(32'h00ab0000, 2'b00);

    start_write_together(32'h00000010, 32'hcd000000, 4'b1000);
    check_value(scratch_o === 32'hcdab0000, "byte three write strobe");
    finish_b(2'b00);
    start_read(32'h00000010);
    finish_read(32'hcdab0000, 2'b00);

    start_write_together(32'h00000004, 32'hffffffff, 4'b0000);
    check_value(s_axi_bvalid && s_axi_bresp === 2'b00, "zero strobe returns OKAY");
    check_value(enable_o === 1'b0 && mode_o === 2'b01, "zero strobe changes nothing");
    finish_b(2'b00);

    start_write_together(32'h0000000c, 32'hffffffff, 4'b1111);
    finish_b(2'b10);
    check_value(enable_o === 1'b0 && mode_o === 2'b01, "unmapped write ignored");
    start_read(32'h0000000c);
    finish_read(32'h00000000, 2'b10);

    start_write_together(32'h10000004, 32'hffffffff, 4'b1111);
    finish_b(2'b10);
    check_value(enable_o === 1'b0 && mode_o === 2'b01, "high alias write ignored");
    start_read(32'h10000004);
    finish_read(32'h00000000, 2'b10);

    start_read(32'h00000004);
    finish_read(32'h00000100, 2'b00);

    send_w(32'hffffffff, 4'b1111);
    send_aw(32'h00000006);
    finish_b(2'b10);
    check_value(enable_o === 1'b0 && mode_o === 2'b01, "misaligned write ignored");

    start_read(32'h00000006);
    finish_read(32'h00000000, 2'b10);

    busy_i = 1'b1;
    start_read(32'h00000000);
    finish_read(32'h0000002a, 2'b00);
    start_read(32'h00000008);
    @(negedge clk_i);
    busy_i = 1'b0;
    #1 check_value(
        s_axi_rvalid === 1'b1 && s_axi_rdata === 32'h00000001,
        "live input captured in R response");
    repeat (2) begin
        @(posedge clk_i);
        #1 check_value(
            s_axi_rvalid === 1'b1 && s_axi_rdata === 32'h00000001
                && s_axi_arready === 1'b0,
            "captured R response stays stable");
    end
    finish_read(32'h00000001, 2'b00);
    start_read(32'h00000008);
    finish_read(32'h00000000, 2'b00);

    send_aw(32'h00000004);
    @(negedge clk_i);
    s_axi_wdata   = 32'h00000000;
    s_axi_wstrb   = 4'b0011;
    s_axi_wvalid  = 1'b1;
    s_axi_araddr  = 32'h00000004;
    s_axi_arvalid = 1'b1;
    check_value(s_axi_wready && s_axi_arready, "concurrent channels ready");
    @(posedge clk_i);
    #1;
    s_axi_wvalid  = 1'b0;
    s_axi_arvalid = 1'b0;
    check_value(s_axi_bvalid && s_axi_bresp === 2'b00, "concurrent write response");
    check_value(s_axi_rvalid && s_axi_rresp === 2'b00, "concurrent read response");
    check_value(s_axi_rdata === 32'h00000100, "concurrent read returns old value");
    check_value(enable_o === 1'b0 && mode_o === 2'b00, "concurrent write commits");
    finish_b(2'b00);
    finish_read(32'h00000100, 2'b00);

    if (failures == 0)
        $display("TEST_PASS");
    else
        $display("TEST_FAIL count=%0d", failures);
    $finish;
end
endmodule
)";
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void draftIsRecognizedAndIncomplete();
    void validMapGeneratesAxiLiteSlave();
    void sourceOrderDoesNotChangeGeneratedVerilog();
    void duplicateModuleNameIsRejected();
    void invalidMap_data();
    void invalidMap();
    void generatedVerilogPassesProtocolSmokeTestWhenIverilogIsAvailable();
};

void Test::draftIsRecognizedAndIncomplete()
{
    QSocModuleDefinition definition;
    definition.libraryName                  = "peripheral";
    definition.moduleName                   = "timer_ctrl";
    definition.extraAttributes["generator"] = QSocMmioGenerator::createDraftGenerator();

    QVERIFY(QSocMmioGenerator::isMmio(definition));
    const QStringList errors = QSocMmioGenerator::validate(definition);
    QVERIFY(!errors.isEmpty());
    QVERIFY(hasErrorPath(errors, "generator.register"));

    const YAML::Node generator = definition.extraAttributes["generator"];
    QCOMPARE(QString::fromStdString(generator["kind"].as<std::string>()), "mmio");
    QCOMPARE(QString::fromStdString(generator["bus"].as<std::string>()), "axi4_lite");
    QVERIFY(generator["register"].IsMap());
    QCOMPARE(generator["register"].size(), std::size_t(0));
}

void Test::sourceOrderDoesNotChangeGeneratedVerilog()
{
    QString     expected;
    QString     actual;
    QStringList errors;

    QVERIFY(QSocMmioGenerator::generateVerilog(makeValidDefinition(), &expected, &errors));
    QVERIFY(errors.isEmpty());
    QVERIFY(QSocMmioGenerator::generateVerilog(makeReverseOrderedDefinition(), &actual, &errors));
    QVERIFY(errors.isEmpty());
    QCOMPARE(actual, expected);
}

void Test::validMapGeneratesAxiLiteSlave()
{
    const QSocModuleDefinition definition = makeValidDefinition();
    QVERIFY(QSocMmioGenerator::isMmio(definition));
    QVERIFY(QSocMmioGenerator::validate(definition).isEmpty());

    QString     verilog;
    QStringList errors;
    QVERIFY(QSocMmioGenerator::generateVerilog(definition, &verilog, &errors));
    QVERIFY(errors.isEmpty());
    QVERIFY(verilog.contains("module timer_ctrl"));
    QVERIFY(verilog.contains(QRegularExpression("input\\s+wire\\s+clk_i")));
    QVERIFY(verilog.contains(QRegularExpression("input\\s+wire\\s+rst_ni")));
    QVERIFY(verilog.contains("s_axi_awaddr"));
    QVERIFY(verilog.contains("s_axi_awprot"));
    QVERIFY(verilog.contains("s_axi_wdata"));
    QVERIFY(verilog.contains("s_axi_wstrb"));
    QVERIFY(verilog.contains("s_axi_bvalid"));
    QVERIFY(verilog.contains("s_axi_araddr"));
    QVERIFY(verilog.contains("s_axi_arprot"));
    QVERIFY(verilog.contains("s_axi_rdata"));
    QVERIFY(verilog.contains("s_axi_rvalid"));
    QVERIFY(verilog.contains("enable_o"));
    QVERIFY(verilog.contains("mode_o"));
    QVERIFY(verilog.contains("scratch_o"));
    QVERIFY(verilog.contains("busy_i"));
    QVERIFY(verilog.contains("8'h2a", Qt::CaseInsensitive));

    /* Address and data channels must be held independently. */
    QVERIFY(verilog.contains("reg        aw_pending_q;"));
    QVERIFY(verilog.contains("reg        w_pending_q;"));
    QVERIFY(verilog.contains("reg [31:0] awaddr_q;"));
    QVERIFY(verilog.contains("reg [31:0] wdata_q;"));
    QVERIFY(verilog.contains("reg [3:0]  wstrb_q;"));
    QVERIFY(verilog.contains("if (aw_take) begin"));
    QVERIFY(verilog.contains("if (w_take) begin"));

    /* Invalid and misaligned accesses return SLVERR, not silent OKAY. */
    QVERIFY(verilog.contains("2'b10"));
    QVERIFY(verilog.contains("s_axi_bresp"));
    QVERIFY(verilog.contains("s_axi_rresp"));
}

void Test::duplicateModuleNameIsRejected()
{
    QSocModuleDefinition definition   = makeValidDefinition();
    definition.hasDuplicateModuleName = true;

    const QStringList errors = QSocMmioGenerator::validate(definition);
    QVERIFY2(hasErrorPath(errors, "module.name"), qPrintable(errors.join('\n')));

    QString verilog;
    QVERIFY(!QSocMmioGenerator::generateVerilog(definition, &verilog));
    QVERIFY(verilog.isEmpty());
}

void Test::invalidMap_data()
{
    QTest::addColumn<QString>("body");
    QTest::addColumn<QString>("path");
    QTest::addColumn<QString>("moduleName");

    const QString head           = R"(
generator:
  kind: mmio
  bus: axi4_lite
)";
    const QString validGenerator = head + R"(  register:
    control:
      offset: 0
      field:
        enable: {lsb: 0, access: rw, reset: 0}
)";

    QTest::newRow("unknown-generator-key") << head + "  extra: true\n  register: {}\n"
                                           << QString("generator.extra") << QString("timer_ctrl");
    QTest::newRow("unknown-register-key") << head + R"(  register:
    control:
      offset: 0
      extra: true
      field:
        enable: {lsb: 0, access: rw, reset: 0}
)" << QString("generator.register.control.extra")
                                          << QString("timer_ctrl");
    QTest::newRow("unknown-field-key") << head + R"(  register:
    control:
      offset: 0
      field:
        enable: {lsb: 0, access: rw, reset: 0, extra: true}
)" << QString("generator.register.control.field.enable.extra")
                                       << QString("timer_ctrl");
    QTest::newRow("register-not-map") << head + "  register: bad\n"
                                      << QString("generator.register") << QString("timer_ctrl");
    QTest::newRow("field-not-map") << head + R"(  register:
    control:
      offset: 0
      field: bad
)" << QString("generator.register.control.field")
                                   << QString("timer_ctrl");
    QTest::newRow("duplicate-generator")
        << validGenerator + validGenerator << QString("module.generator") << QString("timer_ctrl");
    QTest::newRow("duplicate-field-property") << head + R"(  register:
    control:
      offset: 0
      field:
        enable:
          lsb: 0
          access: rw
          reset: 0
          reset: 1
)" << QString("generator.register.control.field.enable.reset")
                                              << QString("timer_ctrl");
    QTest::newRow("unaligned-offset") << head + R"(  register:
    control:
      offset: 0x02
      field:
        enable: {lsb: 0, access: rw, reset: 0}
)" << QString("generator.register.control.offset")
                                      << QString("timer_ctrl");
    QTest::newRow("duplicate-offset") << head + R"(  register:
    control:
      offset: 0
      field:
        enable: {lsb: 0, access: rw, reset: 0}
    status:
      offset: 0
      field:
        busy: {lsb: 0, access: ro, input: busy_i}
)" << QString("generator.register.status.offset")
                                      << QString("timer_ctrl");
    QTest::newRow("overlapping-fields") << head + R"(  register:
    control:
      offset: 0
      field:
        low: {lsb: 0, width: 4, access: rw, reset: 0}
        high: {lsb: 3, width: 2, access: rw, reset: 0}
)" << QString("generator.register.control.field.high")
                                        << QString("timer_ctrl");
    QTest::newRow("field-out-of-range") << head + R"(  register:
    control:
      offset: 0
      field:
        enable: {lsb: 31, width: 2, access: rw, reset: 0}
)" << QString("generator.register.control.field.enable")
                                        << QString("timer_ctrl");
    QTest::newRow("zero-field-width") << head + R"(  register:
    control:
      offset: 0
      field:
        enable: {lsb: 0, width: 0, access: rw, reset: 0}
)" << QString("generator.register.control.field.enable.width")
                                      << QString("timer_ctrl");
    QTest::newRow("negative-offset") << head + R"(  register:
    control:
      offset: -4
      field:
        enable: {lsb: 0, access: rw, reset: 0}
)" << QString("generator.register.control.offset")
                                     << QString("timer_ctrl");
    QTest::newRow("value-does-not-fit") << head + R"(  register:
    status:
      offset: 0
      field:
        code: {lsb: 0, width: 2, access: ro, value: 4}
)" << QString("generator.register.status.field.code.value")
                                        << QString("timer_ctrl");
    QTest::newRow("rw-missing-reset") << head + R"(  register:
    control:
      offset: 0
      field:
        enable: {lsb: 0, access: rw}
)" << QString("generator.register.control.field.enable.reset")
                                      << QString("timer_ctrl");
    QTest::newRow("ro-has-two-sources") << head + R"(  register:
    status:
      offset: 0
      field:
        busy: {lsb: 0, access: ro, input: busy_i, value: 0}
)" << QString("generator.register.status.field.busy")
                                        << QString("timer_ctrl");
    QTest::newRow("ro-has-no-source") << head + R"(  register:
    status:
      offset: 0
      field:
        busy: {lsb: 0, access: ro}
)" << QString("generator.register.status.field.busy")
                                      << QString("timer_ctrl");
    QTest::newRow("unsupported-access") << head + R"(  register:
    control:
      offset: 0
      field:
        enable: {lsb: 0, access: wo, reset: 0}
)" << QString("generator.register.control.field.enable.access")
                                        << QString("timer_ctrl");
    QTest::newRow("invalid-sideband-identifier") << head + R"(  register:
    status:
      offset: 0
      field:
        busy: {lsb: 0, access: ro, input: 9bad}
)" << QString("generator.register.status.field.busy.input")
                                                 << QString("timer_ctrl");
    QTest::newRow("fixed-port-collision") << head + R"(  register:
    status:
      offset: 0
      field:
        busy: {lsb: 0, access: ro, input: clk_i}
)" << QString("generator.register.status.field.busy.input")
                                          << QString("timer_ctrl");
    QTest::newRow("internal-name-collision") << head + R"(  register:
    status:
      offset: 0
      field:
        busy: {lsb: 0, access: ro, input: write_fire}
)" << QString("generator.register.status.field.busy.input")
                                             << QString("timer_ctrl");
    QTest::newRow("duplicate-sideband") << head + R"(  register:
    status:
      offset: 0
      field:
        busy: {lsb: 0, access: ro, input: status_i}
        ready: {lsb: 1, access: ro, input: status_i}
)" << QString("generator.register.status.field.ready.input")
                                        << QString("timer_ctrl");
    QTest::newRow("manual-empty-parameter") << QString("parameter: {}\n") + validGenerator
                                            << QString("module.parameter") << QString("timer_ctrl");
    QTest::newRow("manual-invalid-port") << QString("port: bad\n") + validGenerator
                                         << QString("module.port") << QString("timer_ctrl");
    QTest::newRow("manual-invalid-bus")
        << QString("bus: []\n") + validGenerator << QString("module.bus") << QString("timer_ctrl");
    QTest::newRow("invalid-register-identifier") << head + R"(  register:
    "bad-name":
      offset: 0
      field:
        enable: {lsb: 0, access: rw, reset: 0}
)" << QString("generator.register.bad-name") << QString("timer_ctrl");
    QTest::newRow("invalid-module-identifier") << head + R"(  register:
    control:
      offset: 0
      field:
        enable: {lsb: 0, access: rw, reset: 0}
)" << QString("module.name") << QString("bad-module");
}

void Test::invalidMap()
{
    QFETCH(QString, body);
    QFETCH(QString, path);
    QFETCH(QString, moduleName);

    const QSocModuleDefinition definition = makeDefinition(body, moduleName);
    const QStringList          errors     = QSocMmioGenerator::validate(definition);
    QVERIFY2(!errors.isEmpty(), qPrintable(body));
    QVERIFY2(hasErrorPath(errors, path), qPrintable(errors.join('\n')));

    QString     verilog = "unchanged";
    QStringList generationErrors;
    QVERIFY(!QSocMmioGenerator::generateVerilog(definition, &verilog, &generationErrors));
    QVERIFY(verilog.isEmpty());
    QVERIFY2(hasErrorPath(generationErrors, path), qPrintable(generationErrors.join('\n')));
}

void Test::generatedVerilogPassesProtocolSmokeTestWhenIverilogIsAvailable()
{
    const QString compiler = QStandardPaths::findExecutable("iverilog");
    const QString runtime  = QStandardPaths::findExecutable("vvp");
    if (compiler.isEmpty() || runtime.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("iverilog and vvp"));
    }

    QString     verilog;
    QStringList errors;
    QVERIFY(QSocMmioGenerator::generateVerilog(makeValidDefinition(), &verilog, &errors));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString sourcePath = QDir(directory.path()).filePath("timer_ctrl.v");
    const QString benchPath  = QDir(directory.path()).filePath("tb.v");
    const QString outputPath = QDir(directory.path()).filePath("timer_ctrl.out");
    writeTextFile(sourcePath, verilog);
    writeTextFile(benchPath, protocolTestbench());

    QProcess process;
    process.setWorkingDirectory(directory.path());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(compiler, {"-g2001", "-s", "tb", "-o", outputPath, sourcePath, benchPath});
    QVERIFY(process.waitForStarted());
    QVERIFY(process.waitForFinished());
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    const QByteArray compilerOutput = process.readAll();
    QVERIFY2(process.exitCode() == 0, compilerOutput.constData());
    QVERIFY(QFile::exists(outputPath));

    QProcess simulation;
    simulation.setWorkingDirectory(directory.path());
    simulation.setProcessChannelMode(QProcess::MergedChannels);
    simulation.start(runtime, {outputPath});
    QVERIFY(simulation.waitForStarted());
    QVERIFY(simulation.waitForFinished());
    QCOMPARE(simulation.exitStatus(), QProcess::NormalExit);
    const QByteArray simulationOutput = simulation.readAll();
    QCOMPARE(simulation.exitCode(), 0);
    QVERIFY2(!simulationOutput.contains("TEST_FAIL"), simulationOutput.constData());
    QVERIFY2(simulationOutput.contains("TEST_PASS"), simulationOutput.constData());
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocmmiogenerator.moc"
