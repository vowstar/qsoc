// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsociomuxgenerator.h"
#include "common/qsocmodulemanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QMap>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

namespace {

QSocModuleDefinition makeDefinition(const QString &body, const QString &moduleName = "iomux0")
{
    QSocModuleManager manager;
    return manager.moduleYamlToDefinition("peripheral", moduleName, YAML::Load(body.toStdString()));
}

QString integrationBlock()
{
    return QString(R"(    integration:
      instance: u_iomux0
      clock: clk_iomux
      reset: rst_iomux_n
      control: iomux_control
      pad:
        input_value: pad_input_value
        input_enable: pad_input_enable
        output_value: pad_output_value
        output_enable: pad_output_enable
)");
}

QString sourceForConfig(quint32 pinCount, quint32 hsSlots, quint32 dataWidth, quint32 addressWidth)
{
    return QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: %1
    address_width: %2
    pin_count: %3
    hs_slots: %4
%5    route: []
)")
        .arg(dataWidth)
        .arg(addressWidth)
        .arg(pinCount)
        .arg(hsSlots)
        .arg(integrationBlock());
}

QSocModuleDefinition makeValidDefinition()
{
    return makeDefinition(QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 12
    pin_count: 2
    hs_slots: 2
%1    route:
      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
        input_value: {link: gpio0_input, bit: 0}
        input_enable: 1
        output_value: {link: gpio0_output, bit: 0}
        output_enable: {link: gpio0_enable, bit: 0}
      - pin: 0
        slot: 1
        function: uart0
        signal: tx
        output_value: {link: uart0_tx}
        output_enable: 1
      - pin: 1
        slot: 1
        function: uart0
        signal: rx
        input_value: {link: uart0_rx}
        input_enable: 1
)")
                              .arg(integrationBlock()));
}

QSocModuleDefinition makeSimulationDefinition()
{
    return makeDefinition(QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 8
    pin_count: 2
    hs_slots: 3
%1    route:
      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
        input_value: {link: gpio0_in, bit: 0}
        input_enable: 1
        output_value: {link: gpio0_out, bit: 0}
        output_enable: {link: gpio0_oe, bit: 0}
      - pin: 0
        slot: 1
        function: uart0
        signal: tx
        input_value: {link: uart0_loop}
        output_value: {link: uart0_tx}
        output_enable: 1
      - pin: 1
        slot: 0
        function: spi0
        signal: mode
        output_value: {link: spi_mode, invert: true}
        output_enable: 1
      - pin: 1
        slot: 1
        function: uart0
        signal: rx
        input_value: {link: uart0_rx}
        input_enable: 1
)")
                              .arg(integrationBlock()));
}

void writeTextFile(const QString &path, const QString &text)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream(&file) << text;
}

QString routingTestbench()
{
    return QString(R"VERILOG(module tb;
reg  [1:0] pad_in;
wire [1:0] pad_ie;
wire [1:0] pad_ov;
wire [1:0] pad_oe;
wire [5:0] tx_ie;
wire [5:0] tx_ov;
wire [5:0] tx_oe;
wire [5:0] rx_val;
reg  [1:0] sel0;
reg  [1:0] sel1;
reg        gpio0_out_r;
reg        gpio0_oe_r;
reg        uart0_tx_r;
reg        spi_mode_r;
wire       gpio0_in_w;
wire       uart0_loop_w;
wire       uart0_rx_w;
integer    failures;

iomux0_conn u_conn (
    .tx_input_enable_o(tx_ie),
    .tx_output_value_o(tx_ov),
    .tx_output_enable_o(tx_oe),
    .rx_input_value_i(rx_val),
    .hs_p0_s0_input_value_o(gpio0_in_w),
    .hs_p0_s0_output_value_i(gpio0_out_r),
    .hs_p0_s0_output_enable_i(gpio0_oe_r),
    .hs_p0_s1_input_value_o(uart0_loop_w),
    .hs_p0_s1_output_value_i(uart0_tx_r),
    .hs_p1_s0_output_value_i(spi_mode_r),
    .hs_p1_s1_input_value_o(uart0_rx_w)
);

iomux0_core u_core (
    .pad_input_value_i(pad_in),
    .pad_input_enable_o(pad_ie),
    .pad_output_value_o(pad_ov),
    .pad_output_enable_o(pad_oe),
    .tx_input_enable_i(tx_ie),
    .tx_output_value_i(tx_ov),
    .tx_output_enable_i(tx_oe),
    .rx_input_value_o(rx_val),
    .pin_0_select_i(sel0),
    .pin_1_select_i(sel1)
);

task check_value;
    input condition;
    input [255:0] label;
    begin
        if (condition !== 1'b1) begin
            failures = failures + 1;
            $display("CHECK_FAIL %0s", label);
        end
    end
endtask

initial begin
    failures = 0;
    pad_in = 2'b00;
    sel0 = 2'd0;
    sel1 = 2'd0;
    gpio0_out_r = 1'b1;
    gpio0_oe_r = 1'b1;
    uart0_tx_r = 1'b0;
    spi_mode_r = 1'b0;
    #1;
    check_value(pad_ie[0] === 1'b1, "p0 slot0 input_enable constant 1");
    check_value(pad_ov[0] === 1'b1, "p0 slot0 output_value follows link");
    check_value(pad_oe[0] === 1'b1, "p0 slot0 output_enable follows link");
    gpio0_out_r = 1'b0;
    #1;
    check_value(pad_ov[0] === 1'b0, "p0 slot0 output_value tracks source");
    uart0_tx_r = 1'b1;
    #1;
    check_value(pad_ov[0] === 1'b0, "unselected slot1 source is isolated");
    sel0 = 2'd1;
    #1;
    check_value(pad_ie[0] === 1'b0, "p0 slot1 omitted input_enable is 0");
    check_value(pad_ov[0] === 1'b1, "p0 slot1 output_value follows link");
    check_value(pad_oe[0] === 1'b1, "p0 slot1 output_enable constant 1");
    sel0 = 2'd2;
    #1;
    check_value({pad_ie[0], pad_ov[0], pad_oe[0]} === 3'b000, "undeclared slot2 drives zero");
    sel0 = 2'd3;
    #1;
    check_value({pad_ie[0], pad_ov[0], pad_oe[0]} === 3'b000, "invalid code 3 drives zero");
    spi_mode_r = 1'b0;
    #1;
    check_value(pad_ov[1] === 1'b1, "p1 slot0 invert drives 1 from 0");
    check_value(pad_oe[1] === 1'b1, "p1 slot0 output_enable constant 1");
    spi_mode_r = 1'b1;
    #1;
    check_value(pad_ov[1] === 1'b0, "p1 slot0 invert drives 0 from 1");
    pad_in = 2'b11;
    sel0 = 2'd3;
    sel1 = 2'd0;
    #1;
    check_value(gpio0_in_w === 1'b1, "p0 rx sink sees pad input");
    check_value(uart0_loop_w === 1'b1, "p0 second rx sink sees pad input");
    check_value(uart0_rx_w === 1'b1, "p1 rx sink sees pad with selector elsewhere");
    pad_in = 2'b00;
    #1;
    check_value(gpio0_in_w === 1'b0, "p0 rx sink tracks pad input");
    check_value(uart0_loop_w === 1'b0, "p0 second rx sink tracks pad input");
    check_value(uart0_rx_w === 1'b0, "p1 rx sink tracks pad input");
    sel0 = 2'd0;
    #1;
    check_value(pad_ov[1] === 1'b0, "p1 pad ignores p0 selector change");
    check_value(pad_oe[1] === 1'b1, "p1 pad ignores p0 selector change");
    if (failures == 0)
        $display("TEST_PASS");
    else
        $display("TEST_FAIL count=%0d", failures);
    $finish;
end
endmodule
)VERILOG");
}

QString fiveSlotTestbench()
{
    return QString(R"VERILOG(module tb;
reg  [0:0] pad_in;
wire [0:0] pad_ie;
wire [0:0] pad_ov;
wire [0:0] pad_oe;
reg  [4:0] tx_ie;
reg  [4:0] tx_ov;
reg  [4:0] tx_oe;
wire [4:0] rx_val;
reg  [2:0] sel;
integer   failures;

iomux0_core dut (
    .pad_input_value_i(pad_in),
    .pad_input_enable_o(pad_ie),
    .pad_output_value_o(pad_ov),
    .pad_output_enable_o(pad_oe),
    .tx_input_enable_i(tx_ie),
    .tx_output_value_i(tx_ov),
    .tx_output_enable_i(tx_oe),
    .rx_input_value_o(rx_val),
    .pin_0_select_i(sel)
);

task check_value;
    input condition;
    input [255:0] label;
    begin
        if (condition !== 1'b1) begin
            failures = failures + 1;
            $display("CHECK_FAIL %0s", label);
        end
    end
endtask

initial begin
    failures = 0;
    pad_in = 1'b0;
    tx_ie = 5'b11111;
    tx_ov = 5'b11111;
    tx_oe = 5'b11111;
    sel = 3'd0;
    #1;
    check_value({pad_ie, pad_ov, pad_oe} === 3'b111, "slot 0 is valid");
    sel = 3'd4;
    #1;
    check_value({pad_ie, pad_ov, pad_oe} === 3'b111, "slot 4 is valid");
    sel = 3'd5;
    #1;
    check_value({pad_ie, pad_ov, pad_oe} === 3'b000, "code 5 drives zero");
    sel = 3'd6;
    #1;
    check_value({pad_ie, pad_ov, pad_oe} === 3'b000, "code 6 drives zero");
    sel = 3'd7;
    #1;
    check_value({pad_ie, pad_ov, pad_oe} === 3'b000, "code 7 drives zero");
    if (failures == 0)
        $display("TEST_PASS");
    else
        $display("TEST_FAIL count=%0d", failures);
    $finish;
end
endmodule
)VERILOG");
}

QSocModuleDefinition makeTailDefinition(
    quint32 pinCount, quint32 hsSlots, quint32 dataWidth, quint32 addressWidth)
{
    return makeDefinition(QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: %1
    address_width: %2
    pin_count: %3
    hs_slots: %4
%5    route:
      - pin: 0
        slot: 0
        function: head
        signal: oe
        output_enable: 1
      - pin: %6
        slot: 0
        function: tail
        signal: oe
        output_enable: 1
      - pin: %6
        slot: %7
        function: tail
        signal: hi
        input_value: {link: tail_rx}
        input_enable: 1
        output_value: 1
)")
                              .arg(dataWidth)
                              .arg(addressWidth)
                              .arg(pinCount)
                              .arg(hsSlots)
                              .arg(integrationBlock())
                              .arg(pinCount - 1)
                              .arg(hsSlots - 1));
}

QString axiTestbench()
{
    return QString(R"VERILOG(`timescale 1ns/1ps
module tb;
reg               clk_i;
reg               rst_ni;
reg  [@AW@-1:0]   s_axi_awaddr;
reg  [2:0]        s_axi_awprot;
reg               s_axi_awvalid;
wire              s_axi_awready;
reg  [@DW@-1:0]   s_axi_wdata;
reg  [@SW@-1:0]   s_axi_wstrb;
reg               s_axi_wvalid;
wire              s_axi_wready;
wire [1:0]        s_axi_bresp;
wire              s_axi_bvalid;
reg               s_axi_bready;
reg  [@AW@-1:0]   s_axi_araddr;
reg  [2:0]        s_axi_arprot;
reg               s_axi_arvalid;
wire              s_axi_arready;
wire [@DW@-1:0]   s_axi_rdata;
wire [1:0]        s_axi_rresp;
wire              s_axi_rvalid;
reg               s_axi_rready;
reg  [@P@-1:0]    pad_in;
wire [@P@-1:0]    pad_ie;
wire [@P@-1:0]    pad_ov;
wire [@P@-1:0]    pad_oe;
wire              tail_rx_w;
reg  [@DW@-1:0]   rdata;
integer           failures;

iomux0 dut (
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
    .pad_input_value_i(pad_in),
    .pad_input_enable_o(pad_ie),
    .pad_output_value_o(pad_ov),
    .pad_output_enable_o(pad_oe),
    .hs_p@PIN@_s@HISLOT@_input_value_o(tail_rx_w)
);

always #5 clk_i = ~clk_i;

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

task axi_write;
    input [@AW@-1:0] address;
    input [@DW@-1:0] data;
    input [@SW@-1:0] strobe;
    begin
        @(negedge clk_i);
        s_axi_awaddr  = address;
        s_axi_awvalid = 1'b1;
        while (s_axi_awready !== 1'b1)
            @(negedge clk_i);
        @(posedge clk_i);
        #1 s_axi_awvalid = 1'b0;
        @(negedge clk_i);
        s_axi_wdata  = data;
        s_axi_wstrb  = strobe;
        s_axi_wvalid = 1'b1;
        while (s_axi_wready !== 1'b1)
            @(negedge clk_i);
        @(posedge clk_i);
        #1 s_axi_wvalid = 1'b0;
        s_axi_bready = 1'b1;
        while (s_axi_bvalid !== 1'b1)
            @(negedge clk_i);
        @(posedge clk_i);
        #1 s_axi_bready = 1'b0;
    end
endtask

task axi_read;
    input [@AW@-1:0] address;
    begin
        @(negedge clk_i);
        s_axi_araddr  = address;
        s_axi_arvalid = 1'b1;
        while (s_axi_arready !== 1'b1)
            @(negedge clk_i);
        @(posedge clk_i);
        #1 s_axi_arvalid = 1'b0;
        while (s_axi_rvalid !== 1'b1)
            @(negedge clk_i);
        rdata = s_axi_rdata;
        s_axi_rready = 1'b1;
        @(posedge clk_i);
        #1 s_axi_rready = 1'b0;
    end
endtask

initial begin
    failures      = 0;
    clk_i         = 1'b0;
    rst_ni        = 1'b0;
    pad_in        = {@P@{1'b0}};
    s_axi_awaddr  = {@AW@{1'b0}};
    s_axi_awprot  = 3'b000;
    s_axi_awvalid = 1'b0;
    s_axi_wdata   = {@DW@{1'b0}};
    s_axi_wstrb   = {@SW@{1'b0}};
    s_axi_wvalid  = 1'b0;
    s_axi_bready  = 1'b0;
    s_axi_araddr  = {@AW@{1'b0}};
    s_axi_arprot  = 3'b000;
    s_axi_arvalid = 1'b0;
    s_axi_rready  = 1'b0;
    repeat (4) @(negedge clk_i);
    rst_ni = 1'b1;
    repeat (2) @(negedge clk_i);

    check_value(pad_oe[@PIN@] === 1'b1, "reset selects slot 0 oe");
    check_value(pad_ie[@PIN@] === 1'b0, "reset slot 0 ie low");
    check_value(pad_ov[@PIN@] === 1'b0, "reset slot 0 ov low");
    check_value(pad_oe[0] === 1'b1, "reset pin 0 slot 0 oe");

    axi_read({@AW@{1'b0}});
    check_value(rdata[31:0] === 32'h@CAP@, "capability value");

    pad_in[@PIN@] = 1'b1;
    @(negedge clk_i);
    check_value(tail_rx_w === 1'b1, "rx sink sees pad before select");

    axi_write(@AW@'h@SEL_OFFSET@, @DW@'h@CODE@ << @LANE_LSB@, {@SW@{1'b1}});
    repeat (2) @(negedge clk_i);
    check_value(pad_ie[@PIN@] === 1'b1, "selected slot drives ie");
    check_value(pad_ov[@PIN@] === 1'b1, "selected slot drives ov");
    check_value(pad_oe[@PIN@] === 1'b0, "selected slot releases oe");

    axi_read(@AW@'h@SEL_OFFSET@);
    check_value(rdata === (@DW@'h@CODE@ << @LANE_LSB@), "selector readback reserved zero");

    check_value(tail_rx_w === 1'b1, "rx sink sees pad after select");
    pad_in[@PIN@] = 1'b0;
    @(negedge clk_i);
    check_value(tail_rx_w === 1'b0, "rx sink tracks pad");

    axi_write({@AW@{1'b0}}, {@DW@{1'b1}}, {@SW@{1'b1}});
    axi_read({@AW@{1'b0}});
    check_value(rdata[31:0] === 32'h@CAP@, "capability write ignored");

    axi_write(@AW@'h@W0_OFFSET@, @DW@'h@CODE@, {@SW@{1'b1}});
    repeat (2) @(negedge clk_i);
    check_value(pad_oe[0] === 1'b0, "pin 0 undeclared slot drives zero");
    axi_write(@AW@'h@W0_OFFSET@, {@DW@{1'b0}}, @KEEP_STRB@);
    repeat (2) @(negedge clk_i);
    check_value(pad_oe[0] === 1'b0, "byte strobe leaves pin 0 lane");
    axi_write(@AW@'h@W0_OFFSET@, {@DW@{1'b0}}, {@SW@{1'b1}});
    repeat (2) @(negedge clk_i);
    check_value(pad_oe[0] === 1'b1, "full strobe restores slot 0");

    if (failures == 0)
        $display("TEST_PASS");
    else
        $display("TEST_FAIL count=%0d", failures);
    $finish;
end
endmodule
)VERILOG");
}

const QSocMmioFieldPlan *findField(
    const QSocMmioPlan &mmio, const QString &registerName, const QString &fieldName)
{
    for (const QSocMmioRegisterPlan &reg : mmio.registers) {
        if (reg.name != registerName) {
            continue;
        }
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (field.name == fieldName) {
                return &field;
            }
        }
    }
    return nullptr;
}

QMap<QString, QString> wrapperPortSignatures(const QString &top, const QString &moduleName)
{
    const int headerStart = int(top.indexOf(QString("module %1 (").arg(moduleName)));
    const int headerEnd   = int(top.indexOf(");", headerStart));
    if (headerStart < 0 || headerEnd < 0) {
        return {};
    }

    QMap<QString, QString>          signatures;
    static const QRegularExpression declarationPattern(QStringLiteral(
        "^\\s*(input|output)\\s+wire\\s+(?:(\\[[0-9]+:0\\])\\s+)?"
        "([A-Za-z_][A-Za-z0-9_$]*)"));
    const QString                   header = top.mid(headerStart, headerEnd - headerStart);
    for (const QString &line : header.split('\n')) {
        const QRegularExpressionMatch match = declarationPattern.match(line);
        if (!match.hasMatch()) {
            continue;
        }
        signatures.insert(match.captured(3), match.captured(1) + " logic" + match.captured(2));
    }
    return signatures;
}

QMap<QString, QString> projectionPortSignatures(const YAML::Node &projection)
{
    QMap<QString, QString> signatures;
    for (const auto &portPair : projection["port"]) {
        const QString name      = QString::fromStdString(portPair.first.as<std::string>());
        const QString direction = QString::fromStdString(
            portPair.second["direction"].as<std::string>());
        const QString type = QString::fromStdString(portPair.second["type"].as<std::string>());
        signatures.insert(name, direction + " " + type);
    }
    return signatures;
}

void compareText(const QString &actual, const QString &expected)
{
    const QStringList actualLines   = actual.split('\n');
    const QStringList expectedLines = expected.split('\n');
    for (qsizetype index = 0; index < qMin(actualLines.size(), expectedLines.size()); ++index) {
        QCOMPARE(actualLines.at(index), expectedLines.at(index));
    }
    QCOMPARE(actualLines.size(), expectedLines.size());
}

QSocModuleDefinition makeGoldenExtendedDefinition()
{
    return makeDefinition(QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 8
    pin_count: 9
    hs_slots: 3
%1    route:
      - pin: 0
        slot: 0
        function: gpio0
        signal: "a*/b"
        input_value: {link: gpio0_in, bit: 0, invert: true}
        input_enable: 1
        output_value: {link: gpio0_out, bit: 0, invert: true}
        output_enable: 0
      - pin: 0
        slot: 2
        function: uart0
        signal: tx
        output_value: {link: uart0_tx}
        output_enable: 1
      - pin: 8
        slot: 1
        function: tail
        signal: oe
        input_value: {link: tail_in}
        output_enable: 1
)")
                              .arg(integrationBlock()));
}

QString frozenExtendedRegsVerilog()
{
    return QStringLiteral(R"IOMUX(// Generated by QSoC. Do not edit.
module iomux0_regs (
    input  wire        clk_i,
    input  wire        rst_ni,
    input  wire [7:0] s_axi_awaddr,
    input  wire [2:0]  s_axi_awprot,
    input  wire        s_axi_awvalid,
    output wire        s_axi_awready,
    input  wire [31:0] s_axi_wdata,
    input  wire [3:0]  s_axi_wstrb,
    input  wire        s_axi_wvalid,
    output wire        s_axi_wready,
    output reg  [1:0]  s_axi_bresp,
    output reg         s_axi_bvalid,
    input  wire        s_axi_bready,
    input  wire [7:0] s_axi_araddr,
    input  wire [2:0]  s_axi_arprot,
    input  wire        s_axi_arvalid,
    output wire        s_axi_arready,
    output reg  [31:0] s_axi_rdata,
    output reg  [1:0]  s_axi_rresp,
    output reg         s_axi_rvalid,
    input  wire        s_axi_rready,
    output wire [1:0] pin_0_select_o,
    output wire [1:0] pin_1_select_o,
    output wire [1:0] pin_2_select_o,
    output wire [1:0] pin_3_select_o,
    output wire [1:0] pin_4_select_o,
    output wire [1:0] pin_5_select_o,
    output wire [1:0] pin_6_select_o,
    output wire [1:0] pin_7_select_o,
    output wire [1:0] pin_8_select_o
);

localparam [1:0] AXI_RESP_OKAY   = 2'b00;
localparam [1:0] AXI_RESP_SLVERR = 2'b10;

reg [1:0] mmio_field_0_q;
reg [1:0] mmio_field_1_q;
reg [1:0] mmio_field_2_q;
reg [1:0] mmio_field_3_q;
reg [1:0] mmio_field_4_q;
reg [1:0] mmio_field_5_q;
reg [1:0] mmio_field_6_q;
reg [1:0] mmio_field_7_q;
reg [1:0] mmio_field_8_q;
reg        aw_pending_q;
reg [7:0] awaddr_q;
reg        w_pending_q;
reg [31:0] wdata_q;
reg [3:0]  wstrb_q;

function address_is_mapped;
    input [7:0] address;
    begin
        case (address)
            8'h00: address_is_mapped = 1'b1;
            8'h04: address_is_mapped = 1'b1;
            8'h08: address_is_mapped = 1'b1;
            default: address_is_mapped = 1'b0;
        endcase
    end
endfunction

function [31:0] read_register;
    input [7:0] address;
    begin
        read_register = 32'b0;
        case (address)
            8'h00: begin
                read_register[15:0] = 16'h9;
                read_register[23:16] = 8'h3;
            end
            8'h04: begin
                read_register[1:0] = mmio_field_0_q;
                read_register[5:4] = mmio_field_1_q;
                read_register[9:8] = mmio_field_2_q;
                read_register[13:12] = mmio_field_3_q;
                read_register[17:16] = mmio_field_4_q;
                read_register[21:20] = mmio_field_5_q;
                read_register[25:24] = mmio_field_6_q;
                read_register[29:28] = mmio_field_7_q;
            end
            8'h08: begin
                read_register[1:0] = mmio_field_8_q;
            end
            default: begin end
        endcase
    end
endfunction

wire aw_take = s_axi_awvalid && s_axi_awready;
wire w_take  = s_axi_wvalid && s_axi_wready;
wire [7:0] write_address = aw_pending_q ? awaddr_q : s_axi_awaddr;
wire [31:0] write_data    = w_pending_q ? wdata_q : s_axi_wdata;
wire [3:0]  write_strobe  = w_pending_q ? wstrb_q : s_axi_wstrb;
wire write_fire = !s_axi_bvalid && (aw_pending_q || aw_take)
                  && (w_pending_q || w_take);
wire [31:0] write_mask = {{8{write_strobe[3]}}, {8{write_strobe[2]}},
                          {8{write_strobe[1]}}, {8{write_strobe[0]}}};

assign s_axi_awready = rst_ni && !aw_pending_q && !s_axi_bvalid;
assign s_axi_wready  = rst_ni && !w_pending_q && !s_axi_bvalid;
assign s_axi_arready = rst_ni && !s_axi_rvalid;

assign pin_0_select_o = mmio_field_0_q;
assign pin_1_select_o = mmio_field_1_q;
assign pin_2_select_o = mmio_field_2_q;
assign pin_3_select_o = mmio_field_3_q;
assign pin_4_select_o = mmio_field_4_q;
assign pin_5_select_o = mmio_field_5_q;
assign pin_6_select_o = mmio_field_6_q;
assign pin_7_select_o = mmio_field_7_q;
assign pin_8_select_o = mmio_field_8_q;

always @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
        aw_pending_q <= 1'b0;
        awaddr_q     <= 8'b0;
        w_pending_q  <= 1'b0;
        wdata_q      <= 32'b0;
        wstrb_q      <= 4'b0;
        s_axi_bresp  <= AXI_RESP_OKAY;
        s_axi_bvalid <= 1'b0;
        mmio_field_0_q <= 2'h0;
        mmio_field_1_q <= 2'h0;
        mmio_field_2_q <= 2'h0;
        mmio_field_3_q <= 2'h0;
        mmio_field_4_q <= 2'h0;
        mmio_field_5_q <= 2'h0;
        mmio_field_6_q <= 2'h0;
        mmio_field_7_q <= 2'h0;
        mmio_field_8_q <= 2'h0;
    end else begin
        if (s_axi_bvalid && s_axi_bready)
            s_axi_bvalid <= 1'b0;
        if (aw_take) begin
            aw_pending_q <= 1'b1;
            awaddr_q     <= s_axi_awaddr;
        end
        if (w_take) begin
            w_pending_q <= 1'b1;
            wdata_q     <= s_axi_wdata;
            wstrb_q     <= s_axi_wstrb;
        end
        if (write_fire) begin
            aw_pending_q <= 1'b0;
            w_pending_q  <= 1'b0;
            s_axi_bvalid <= 1'b1;
            s_axi_bresp  <= address_is_mapped(write_address)
                            ? AXI_RESP_OKAY : AXI_RESP_SLVERR;
            if (address_is_mapped(write_address)) begin
            case (write_address)
                8'h04: begin
                    mmio_field_0_q <= (mmio_field_0_q & ~write_mask[1:0])
                        | (write_data[1:0] & write_mask[1:0]);
                    mmio_field_1_q <= (mmio_field_1_q & ~write_mask[5:4])
                        | (write_data[5:4] & write_mask[5:4]);
                    mmio_field_2_q <= (mmio_field_2_q & ~write_mask[9:8])
                        | (write_data[9:8] & write_mask[9:8]);
                    mmio_field_3_q <= (mmio_field_3_q & ~write_mask[13:12])
                        | (write_data[13:12] & write_mask[13:12]);
                    mmio_field_4_q <= (mmio_field_4_q & ~write_mask[17:16])
                        | (write_data[17:16] & write_mask[17:16]);
                    mmio_field_5_q <= (mmio_field_5_q & ~write_mask[21:20])
                        | (write_data[21:20] & write_mask[21:20]);
                    mmio_field_6_q <= (mmio_field_6_q & ~write_mask[25:24])
                        | (write_data[25:24] & write_mask[25:24]);
                    mmio_field_7_q <= (mmio_field_7_q & ~write_mask[29:28])
                        | (write_data[29:28] & write_mask[29:28]);
                end
                8'h08: begin
                    mmio_field_8_q <= (mmio_field_8_q & ~write_mask[1:0])
                        | (write_data[1:0] & write_mask[1:0]);
                end
                default: begin end
            endcase
            end
        end
    end
end

always @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
        s_axi_rdata  <= 32'b0;
        s_axi_rresp  <= AXI_RESP_OKAY;
        s_axi_rvalid <= 1'b0;
    end else begin
        if (s_axi_rvalid && s_axi_rready)
            s_axi_rvalid <= 1'b0;
        if (s_axi_arvalid && s_axi_arready) begin
            s_axi_rvalid <= 1'b1;
            s_axi_rdata  <= read_register(s_axi_araddr);
            s_axi_rresp  <= address_is_mapped(s_axi_araddr)
                            ? AXI_RESP_OKAY : AXI_RESP_SLVERR;
        end
    end
end

endmodule
)IOMUX");
}

QString frozenExtendedConnVerilog()
{
    return QStringLiteral(R"IOMUX(// Generated by QSoC. Do not edit.
module iomux0_conn (
    output wire [26:0] tx_input_enable_o,
    output wire [26:0] tx_output_value_o,
    output wire [26:0] tx_output_enable_o,
    input  wire [26:0] rx_input_value_i,
    output wire hs_p0_s0_input_value_o, /* gpio0.a__b */
    input  wire hs_p0_s0_output_value_i, /* gpio0.a__b */
    input  wire hs_p0_s2_output_value_i, /* uart0.tx */
    output wire hs_p8_s1_input_value_o /* tail.oe */
);

assign tx_input_enable_o[0] = 1'b1;
assign tx_output_value_o[0] = hs_p0_s0_output_value_i ^ 1'b1;
assign tx_output_enable_o[0] = 1'b0;
assign tx_input_enable_o[9] = 1'b0;
assign tx_output_value_o[9] = 1'b0;
assign tx_output_enable_o[9] = 1'b0;
assign tx_input_enable_o[18] = 1'b0;
assign tx_output_value_o[18] = hs_p0_s2_output_value_i;
assign tx_output_enable_o[18] = 1'b1;
assign tx_input_enable_o[1] = 1'b0;
assign tx_output_value_o[1] = 1'b0;
assign tx_output_enable_o[1] = 1'b0;
assign tx_input_enable_o[10] = 1'b0;
assign tx_output_value_o[10] = 1'b0;
assign tx_output_enable_o[10] = 1'b0;
assign tx_input_enable_o[19] = 1'b0;
assign tx_output_value_o[19] = 1'b0;
assign tx_output_enable_o[19] = 1'b0;
assign tx_input_enable_o[2] = 1'b0;
assign tx_output_value_o[2] = 1'b0;
assign tx_output_enable_o[2] = 1'b0;
assign tx_input_enable_o[11] = 1'b0;
assign tx_output_value_o[11] = 1'b0;
assign tx_output_enable_o[11] = 1'b0;
assign tx_input_enable_o[20] = 1'b0;
assign tx_output_value_o[20] = 1'b0;
assign tx_output_enable_o[20] = 1'b0;
assign tx_input_enable_o[3] = 1'b0;
assign tx_output_value_o[3] = 1'b0;
assign tx_output_enable_o[3] = 1'b0;
assign tx_input_enable_o[12] = 1'b0;
assign tx_output_value_o[12] = 1'b0;
assign tx_output_enable_o[12] = 1'b0;
assign tx_input_enable_o[21] = 1'b0;
assign tx_output_value_o[21] = 1'b0;
assign tx_output_enable_o[21] = 1'b0;
assign tx_input_enable_o[4] = 1'b0;
assign tx_output_value_o[4] = 1'b0;
assign tx_output_enable_o[4] = 1'b0;
assign tx_input_enable_o[13] = 1'b0;
assign tx_output_value_o[13] = 1'b0;
assign tx_output_enable_o[13] = 1'b0;
assign tx_input_enable_o[22] = 1'b0;
assign tx_output_value_o[22] = 1'b0;
assign tx_output_enable_o[22] = 1'b0;
assign tx_input_enable_o[5] = 1'b0;
assign tx_output_value_o[5] = 1'b0;
assign tx_output_enable_o[5] = 1'b0;
assign tx_input_enable_o[14] = 1'b0;
assign tx_output_value_o[14] = 1'b0;
assign tx_output_enable_o[14] = 1'b0;
assign tx_input_enable_o[23] = 1'b0;
assign tx_output_value_o[23] = 1'b0;
assign tx_output_enable_o[23] = 1'b0;
assign tx_input_enable_o[6] = 1'b0;
assign tx_output_value_o[6] = 1'b0;
assign tx_output_enable_o[6] = 1'b0;
assign tx_input_enable_o[15] = 1'b0;
assign tx_output_value_o[15] = 1'b0;
assign tx_output_enable_o[15] = 1'b0;
assign tx_input_enable_o[24] = 1'b0;
assign tx_output_value_o[24] = 1'b0;
assign tx_output_enable_o[24] = 1'b0;
assign tx_input_enable_o[7] = 1'b0;
assign tx_output_value_o[7] = 1'b0;
assign tx_output_enable_o[7] = 1'b0;
assign tx_input_enable_o[16] = 1'b0;
assign tx_output_value_o[16] = 1'b0;
assign tx_output_enable_o[16] = 1'b0;
assign tx_input_enable_o[25] = 1'b0;
assign tx_output_value_o[25] = 1'b0;
assign tx_output_enable_o[25] = 1'b0;
assign tx_input_enable_o[8] = 1'b0;
assign tx_output_value_o[8] = 1'b0;
assign tx_output_enable_o[8] = 1'b0;
assign tx_input_enable_o[17] = 1'b0;
assign tx_output_value_o[17] = 1'b0;
assign tx_output_enable_o[17] = 1'b1;
assign tx_input_enable_o[26] = 1'b0;
assign tx_output_value_o[26] = 1'b0;
assign tx_output_enable_o[26] = 1'b0;

assign hs_p0_s0_input_value_o = rx_input_value_i[0] ^ 1'b1;
assign hs_p8_s1_input_value_o = rx_input_value_i[17];

endmodule
)IOMUX");
}

QString frozenExtendedTopVerilog()
{
    return QStringLiteral(R"IOMUX(// Generated by QSoC. Do not edit.
module iomux0_core (
    input  wire [8:0] pad_input_value_i,
    output wire [8:0] pad_input_enable_o,
    output wire [8:0] pad_output_value_o,
    output wire [8:0] pad_output_enable_o,
    input  wire [26:0] tx_input_enable_i,
    input  wire [26:0] tx_output_value_i,
    input  wire [26:0] tx_output_enable_i,
    output wire [26:0] rx_input_value_o,
    input  wire [1:0] pin_0_select_i,
    input  wire [1:0] pin_1_select_i,
    input  wire [1:0] pin_2_select_i,
    input  wire [1:0] pin_3_select_i,
    input  wire [1:0] pin_4_select_i,
    input  wire [1:0] pin_5_select_i,
    input  wire [1:0] pin_6_select_i,
    input  wire [1:0] pin_7_select_i,
    input  wire [1:0] pin_8_select_i
);

reg [2:0] tx_bundle_0;
always @(*) begin
    case (pin_0_select_i)
        2'd0: tx_bundle_0 = {tx_input_enable_i[0], tx_output_value_i[0], tx_output_enable_i[0]};
        2'd1: tx_bundle_0 = {tx_input_enable_i[9], tx_output_value_i[9], tx_output_enable_i[9]};
        2'd2: tx_bundle_0 = {tx_input_enable_i[18], tx_output_value_i[18], tx_output_enable_i[18]};
        default: tx_bundle_0 = 3'b000;
    endcase
end
assign pad_input_enable_o[0]  = tx_bundle_0[2];
assign pad_output_value_o[0]  = tx_bundle_0[1];
assign pad_output_enable_o[0] = tx_bundle_0[0];

reg [2:0] tx_bundle_1;
always @(*) begin
    case (pin_1_select_i)
        2'd0: tx_bundle_1 = {tx_input_enable_i[1], tx_output_value_i[1], tx_output_enable_i[1]};
        2'd1: tx_bundle_1 = {tx_input_enable_i[10], tx_output_value_i[10], tx_output_enable_i[10]};
        2'd2: tx_bundle_1 = {tx_input_enable_i[19], tx_output_value_i[19], tx_output_enable_i[19]};
        default: tx_bundle_1 = 3'b000;
    endcase
end
assign pad_input_enable_o[1]  = tx_bundle_1[2];
assign pad_output_value_o[1]  = tx_bundle_1[1];
assign pad_output_enable_o[1] = tx_bundle_1[0];

reg [2:0] tx_bundle_2;
always @(*) begin
    case (pin_2_select_i)
        2'd0: tx_bundle_2 = {tx_input_enable_i[2], tx_output_value_i[2], tx_output_enable_i[2]};
        2'd1: tx_bundle_2 = {tx_input_enable_i[11], tx_output_value_i[11], tx_output_enable_i[11]};
        2'd2: tx_bundle_2 = {tx_input_enable_i[20], tx_output_value_i[20], tx_output_enable_i[20]};
        default: tx_bundle_2 = 3'b000;
    endcase
end
assign pad_input_enable_o[2]  = tx_bundle_2[2];
assign pad_output_value_o[2]  = tx_bundle_2[1];
assign pad_output_enable_o[2] = tx_bundle_2[0];

reg [2:0] tx_bundle_3;
always @(*) begin
    case (pin_3_select_i)
        2'd0: tx_bundle_3 = {tx_input_enable_i[3], tx_output_value_i[3], tx_output_enable_i[3]};
        2'd1: tx_bundle_3 = {tx_input_enable_i[12], tx_output_value_i[12], tx_output_enable_i[12]};
        2'd2: tx_bundle_3 = {tx_input_enable_i[21], tx_output_value_i[21], tx_output_enable_i[21]};
        default: tx_bundle_3 = 3'b000;
    endcase
end
assign pad_input_enable_o[3]  = tx_bundle_3[2];
assign pad_output_value_o[3]  = tx_bundle_3[1];
assign pad_output_enable_o[3] = tx_bundle_3[0];

reg [2:0] tx_bundle_4;
always @(*) begin
    case (pin_4_select_i)
        2'd0: tx_bundle_4 = {tx_input_enable_i[4], tx_output_value_i[4], tx_output_enable_i[4]};
        2'd1: tx_bundle_4 = {tx_input_enable_i[13], tx_output_value_i[13], tx_output_enable_i[13]};
        2'd2: tx_bundle_4 = {tx_input_enable_i[22], tx_output_value_i[22], tx_output_enable_i[22]};
        default: tx_bundle_4 = 3'b000;
    endcase
end
assign pad_input_enable_o[4]  = tx_bundle_4[2];
assign pad_output_value_o[4]  = tx_bundle_4[1];
assign pad_output_enable_o[4] = tx_bundle_4[0];

reg [2:0] tx_bundle_5;
always @(*) begin
    case (pin_5_select_i)
        2'd0: tx_bundle_5 = {tx_input_enable_i[5], tx_output_value_i[5], tx_output_enable_i[5]};
        2'd1: tx_bundle_5 = {tx_input_enable_i[14], tx_output_value_i[14], tx_output_enable_i[14]};
        2'd2: tx_bundle_5 = {tx_input_enable_i[23], tx_output_value_i[23], tx_output_enable_i[23]};
        default: tx_bundle_5 = 3'b000;
    endcase
end
assign pad_input_enable_o[5]  = tx_bundle_5[2];
assign pad_output_value_o[5]  = tx_bundle_5[1];
assign pad_output_enable_o[5] = tx_bundle_5[0];

reg [2:0] tx_bundle_6;
always @(*) begin
    case (pin_6_select_i)
        2'd0: tx_bundle_6 = {tx_input_enable_i[6], tx_output_value_i[6], tx_output_enable_i[6]};
        2'd1: tx_bundle_6 = {tx_input_enable_i[15], tx_output_value_i[15], tx_output_enable_i[15]};
        2'd2: tx_bundle_6 = {tx_input_enable_i[24], tx_output_value_i[24], tx_output_enable_i[24]};
        default: tx_bundle_6 = 3'b000;
    endcase
end
assign pad_input_enable_o[6]  = tx_bundle_6[2];
assign pad_output_value_o[6]  = tx_bundle_6[1];
assign pad_output_enable_o[6] = tx_bundle_6[0];

reg [2:0] tx_bundle_7;
always @(*) begin
    case (pin_7_select_i)
        2'd0: tx_bundle_7 = {tx_input_enable_i[7], tx_output_value_i[7], tx_output_enable_i[7]};
        2'd1: tx_bundle_7 = {tx_input_enable_i[16], tx_output_value_i[16], tx_output_enable_i[16]};
        2'd2: tx_bundle_7 = {tx_input_enable_i[25], tx_output_value_i[25], tx_output_enable_i[25]};
        default: tx_bundle_7 = 3'b000;
    endcase
end
assign pad_input_enable_o[7]  = tx_bundle_7[2];
assign pad_output_value_o[7]  = tx_bundle_7[1];
assign pad_output_enable_o[7] = tx_bundle_7[0];

reg [2:0] tx_bundle_8;
always @(*) begin
    case (pin_8_select_i)
        2'd0: tx_bundle_8 = {tx_input_enable_i[8], tx_output_value_i[8], tx_output_enable_i[8]};
        2'd1: tx_bundle_8 = {tx_input_enable_i[17], tx_output_value_i[17], tx_output_enable_i[17]};
        2'd2: tx_bundle_8 = {tx_input_enable_i[26], tx_output_value_i[26], tx_output_enable_i[26]};
        default: tx_bundle_8 = 3'b000;
    endcase
end
assign pad_input_enable_o[8]  = tx_bundle_8[2];
assign pad_output_value_o[8]  = tx_bundle_8[1];
assign pad_output_enable_o[8] = tx_bundle_8[0];

assign rx_input_value_o[0] = pad_input_value_i[0];
assign rx_input_value_o[9] = pad_input_value_i[0];
assign rx_input_value_o[18] = pad_input_value_i[0];
assign rx_input_value_o[1] = pad_input_value_i[1];
assign rx_input_value_o[10] = pad_input_value_i[1];
assign rx_input_value_o[19] = pad_input_value_i[1];
assign rx_input_value_o[2] = pad_input_value_i[2];
assign rx_input_value_o[11] = pad_input_value_i[2];
assign rx_input_value_o[20] = pad_input_value_i[2];
assign rx_input_value_o[3] = pad_input_value_i[3];
assign rx_input_value_o[12] = pad_input_value_i[3];
assign rx_input_value_o[21] = pad_input_value_i[3];
assign rx_input_value_o[4] = pad_input_value_i[4];
assign rx_input_value_o[13] = pad_input_value_i[4];
assign rx_input_value_o[22] = pad_input_value_i[4];
assign rx_input_value_o[5] = pad_input_value_i[5];
assign rx_input_value_o[14] = pad_input_value_i[5];
assign rx_input_value_o[23] = pad_input_value_i[5];
assign rx_input_value_o[6] = pad_input_value_i[6];
assign rx_input_value_o[15] = pad_input_value_i[6];
assign rx_input_value_o[24] = pad_input_value_i[6];
assign rx_input_value_o[7] = pad_input_value_i[7];
assign rx_input_value_o[16] = pad_input_value_i[7];
assign rx_input_value_o[25] = pad_input_value_i[7];
assign rx_input_value_o[8] = pad_input_value_i[8];
assign rx_input_value_o[17] = pad_input_value_i[8];
assign rx_input_value_o[26] = pad_input_value_i[8];

endmodule

module iomux0 (
    input  wire clk_i,
    input  wire rst_ni,
    input  wire [7:0] s_axi_awaddr,
    input  wire [2:0] s_axi_awprot,
    input  wire s_axi_awvalid,
    output wire s_axi_awready,
    input  wire [31:0] s_axi_wdata,
    input  wire [3:0] s_axi_wstrb,
    input  wire s_axi_wvalid,
    output wire s_axi_wready,
    output wire [1:0] s_axi_bresp,
    output wire s_axi_bvalid,
    input  wire s_axi_bready,
    input  wire [7:0] s_axi_araddr,
    input  wire [2:0] s_axi_arprot,
    input  wire s_axi_arvalid,
    output wire s_axi_arready,
    output wire [31:0] s_axi_rdata,
    output wire [1:0] s_axi_rresp,
    output wire s_axi_rvalid,
    input  wire s_axi_rready,
    input  wire [8:0] pad_input_value_i,
    output wire [8:0] pad_input_enable_o,
    output wire [8:0] pad_output_value_o,
    output wire [8:0] pad_output_enable_o,
    output wire hs_p0_s0_input_value_o, /* gpio0.a__b */
    input  wire hs_p0_s0_output_value_i, /* gpio0.a__b */
    input  wire hs_p0_s2_output_value_i, /* uart0.tx */
    output wire hs_p8_s1_input_value_o /* tail.oe */
);

wire [26:0] tx_input_enable_w;
wire [26:0] tx_output_value_w;
wire [26:0] tx_output_enable_w;
wire [26:0] rx_input_value_w;
wire [1:0] pin_0_select_w;
wire [1:0] pin_1_select_w;
wire [1:0] pin_2_select_w;
wire [1:0] pin_3_select_w;
wire [1:0] pin_4_select_w;
wire [1:0] pin_5_select_w;
wire [1:0] pin_6_select_w;
wire [1:0] pin_7_select_w;
wire [1:0] pin_8_select_w;

iomux0_regs u_regs (
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
    .pin_0_select_o(pin_0_select_w),
    .pin_1_select_o(pin_1_select_w),
    .pin_2_select_o(pin_2_select_w),
    .pin_3_select_o(pin_3_select_w),
    .pin_4_select_o(pin_4_select_w),
    .pin_5_select_o(pin_5_select_w),
    .pin_6_select_o(pin_6_select_w),
    .pin_7_select_o(pin_7_select_w),
    .pin_8_select_o(pin_8_select_w)
);

iomux0_conn u_conn (
    .tx_input_enable_o(tx_input_enable_w),
    .tx_output_value_o(tx_output_value_w),
    .tx_output_enable_o(tx_output_enable_w),
    .rx_input_value_i(rx_input_value_w),
    .hs_p0_s0_input_value_o(hs_p0_s0_input_value_o),
    .hs_p0_s0_output_value_i(hs_p0_s0_output_value_i),
    .hs_p0_s2_output_value_i(hs_p0_s2_output_value_i),
    .hs_p8_s1_input_value_o(hs_p8_s1_input_value_o)
);

iomux0_core u_core (
    .pad_input_value_i(pad_input_value_i),
    .pad_input_enable_o(pad_input_enable_o),
    .pad_output_value_o(pad_output_value_o),
    .pad_output_enable_o(pad_output_enable_o),
    .tx_input_enable_i(tx_input_enable_w),
    .tx_output_value_i(tx_output_value_w),
    .tx_output_enable_i(tx_output_enable_w),
    .rx_input_value_o(rx_input_value_w),
    .pin_0_select_i(pin_0_select_w),
    .pin_1_select_i(pin_1_select_w),
    .pin_2_select_i(pin_2_select_w),
    .pin_3_select_i(pin_3_select_w),
    .pin_4_select_i(pin_4_select_w),
    .pin_5_select_i(pin_5_select_w),
    .pin_6_select_i(pin_6_select_w),
    .pin_7_select_i(pin_7_select_w),
    .pin_8_select_i(pin_8_select_w)
);

endmodule
)IOMUX");
}

QString frozenExtendedReport()
{
    return QStringLiteral(R"IOMUX(IOMUX route report for iomux0
pin_count: 9
hs_slots: 3
data_width: 32
address_width: 8
selector: 2-bit field in a fixed 4-bit lane per pin
selector registers: 2 at offset 0x4 to 0x8
registers total: 3
aperture: 12 bytes
capability: 0x00030009 at offset 0x0
reset: every selector resets to 0 and selects slot 0
rx: pad input broadcasts to every declared sink regardless of the selector

pin 0 selector word 0 lsb 0 offset 0x4
  slot 0 function gpio0 signal a*/b
    input_value: link gpio0_in bit 0 invert
    input_enable: constant 1
    output_value: link gpio0_out bit 0 invert
    output_enable: constant 0
  slot 2 function uart0 signal tx
    input_value: no sink
    input_enable: constant 0
    output_value: link uart0_tx
    output_enable: constant 1
  unused slots: 1
pin 1 selector word 0 lsb 4 offset 0x4
  unused slots: 0, 1, 2
pin 2 selector word 0 lsb 8 offset 0x4
  unused slots: 0, 1, 2
pin 3 selector word 0 lsb 12 offset 0x4
  unused slots: 0, 1, 2
pin 4 selector word 0 lsb 16 offset 0x4
  unused slots: 0, 1, 2
pin 5 selector word 0 lsb 20 offset 0x4
  unused slots: 0, 1, 2
pin 6 selector word 0 lsb 24 offset 0x4
  unused slots: 0, 1, 2
pin 7 selector word 0 lsb 28 offset 0x4
  unused slots: 0, 1, 2
pin 8 selector word 1 lsb 0 offset 0x8
  slot 1 function tail signal oe
    input_value: link tail_in
    input_enable: constant 0
    output_value: constant 0
    output_enable: constant 1
  unused slots: 0, 2

undeclared pin/slot pairs drive a zero tx bundle
)IOMUX");
}

QSocModuleDefinition makeWide64Definition()
{
    return makeDefinition(QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 64
    address_width: 12
    pin_count: 17
    hs_slots: 4
%1    route:
      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
        input_value: {link: gpio0_in, bit: 0}
        input_enable: 1
        output_value: {link: gpio0_out, bit: 0}
        output_enable: {link: gpio0_oe, bit: 0}
      - pin: 16
        slot: 3
        function: tail
        signal: oe
        output_enable: 1
)")
                              .arg(integrationBlock()));
}

QString frozenWide64RegsVerilog()
{
    return QStringLiteral(R"IOMUX(// Generated by QSoC. Do not edit.
module iomux0_regs (
    input  wire        clk_i,
    input  wire        rst_ni,
    input  wire [11:0] s_axi_awaddr,
    input  wire [2:0]  s_axi_awprot,
    input  wire        s_axi_awvalid,
    output wire        s_axi_awready,
    input  wire [63:0] s_axi_wdata,
    input  wire [7:0]  s_axi_wstrb,
    input  wire        s_axi_wvalid,
    output wire        s_axi_wready,
    output reg  [1:0]  s_axi_bresp,
    output reg         s_axi_bvalid,
    input  wire        s_axi_bready,
    input  wire [11:0] s_axi_araddr,
    input  wire [2:0]  s_axi_arprot,
    input  wire        s_axi_arvalid,
    output wire        s_axi_arready,
    output reg  [63:0] s_axi_rdata,
    output reg  [1:0]  s_axi_rresp,
    output reg         s_axi_rvalid,
    input  wire        s_axi_rready,
    output wire [1:0] pin_0_select_o,
    output wire [1:0] pin_1_select_o,
    output wire [1:0] pin_2_select_o,
    output wire [1:0] pin_3_select_o,
    output wire [1:0] pin_4_select_o,
    output wire [1:0] pin_5_select_o,
    output wire [1:0] pin_6_select_o,
    output wire [1:0] pin_7_select_o,
    output wire [1:0] pin_8_select_o,
    output wire [1:0] pin_9_select_o,
    output wire [1:0] pin_10_select_o,
    output wire [1:0] pin_11_select_o,
    output wire [1:0] pin_12_select_o,
    output wire [1:0] pin_13_select_o,
    output wire [1:0] pin_14_select_o,
    output wire [1:0] pin_15_select_o,
    output wire [1:0] pin_16_select_o
);

localparam [1:0] AXI_RESP_OKAY   = 2'b00;
localparam [1:0] AXI_RESP_SLVERR = 2'b10;

reg [1:0] mmio_field_0_q;
reg [1:0] mmio_field_1_q;
reg [1:0] mmio_field_2_q;
reg [1:0] mmio_field_3_q;
reg [1:0] mmio_field_4_q;
reg [1:0] mmio_field_5_q;
reg [1:0] mmio_field_6_q;
reg [1:0] mmio_field_7_q;
reg [1:0] mmio_field_8_q;
reg [1:0] mmio_field_9_q;
reg [1:0] mmio_field_10_q;
reg [1:0] mmio_field_11_q;
reg [1:0] mmio_field_12_q;
reg [1:0] mmio_field_13_q;
reg [1:0] mmio_field_14_q;
reg [1:0] mmio_field_15_q;
reg [1:0] mmio_field_16_q;
reg        aw_pending_q;
reg [11:0] awaddr_q;
reg        w_pending_q;
reg [63:0] wdata_q;
reg [7:0]  wstrb_q;

function address_is_mapped;
    input [11:0] address;
    begin
        case (address)
            12'h000: address_is_mapped = 1'b1;
            12'h008: address_is_mapped = 1'b1;
            12'h010: address_is_mapped = 1'b1;
            default: address_is_mapped = 1'b0;
        endcase
    end
endfunction

function [63:0] read_register;
    input [11:0] address;
    begin
        read_register = 64'b0;
        case (address)
            12'h000: begin
                read_register[15:0] = 16'h11;
                read_register[23:16] = 8'h4;
            end
            12'h008: begin
                read_register[1:0] = mmio_field_0_q;
                read_register[5:4] = mmio_field_1_q;
                read_register[9:8] = mmio_field_2_q;
                read_register[13:12] = mmio_field_3_q;
                read_register[17:16] = mmio_field_4_q;
                read_register[21:20] = mmio_field_5_q;
                read_register[25:24] = mmio_field_6_q;
                read_register[29:28] = mmio_field_7_q;
                read_register[33:32] = mmio_field_8_q;
                read_register[37:36] = mmio_field_9_q;
                read_register[41:40] = mmio_field_10_q;
                read_register[45:44] = mmio_field_11_q;
                read_register[49:48] = mmio_field_12_q;
                read_register[53:52] = mmio_field_13_q;
                read_register[57:56] = mmio_field_14_q;
                read_register[61:60] = mmio_field_15_q;
            end
            12'h010: begin
                read_register[1:0] = mmio_field_16_q;
            end
            default: begin end
        endcase
    end
endfunction

wire aw_take = s_axi_awvalid && s_axi_awready;
wire w_take  = s_axi_wvalid && s_axi_wready;
wire [11:0] write_address = aw_pending_q ? awaddr_q : s_axi_awaddr;
wire [63:0] write_data    = w_pending_q ? wdata_q : s_axi_wdata;
wire [7:0]  write_strobe  = w_pending_q ? wstrb_q : s_axi_wstrb;
wire write_fire = !s_axi_bvalid && (aw_pending_q || aw_take)
                  && (w_pending_q || w_take);
wire [63:0] write_mask = {{8{write_strobe[7]}}, {8{write_strobe[6]}},
                          {8{write_strobe[5]}}, {8{write_strobe[4]}},
                          {8{write_strobe[3]}}, {8{write_strobe[2]}},
                          {8{write_strobe[1]}}, {8{write_strobe[0]}}};

assign s_axi_awready = rst_ni && !aw_pending_q && !s_axi_bvalid;
assign s_axi_wready  = rst_ni && !w_pending_q && !s_axi_bvalid;
assign s_axi_arready = rst_ni && !s_axi_rvalid;

assign pin_0_select_o = mmio_field_0_q;
assign pin_1_select_o = mmio_field_1_q;
assign pin_2_select_o = mmio_field_2_q;
assign pin_3_select_o = mmio_field_3_q;
assign pin_4_select_o = mmio_field_4_q;
assign pin_5_select_o = mmio_field_5_q;
assign pin_6_select_o = mmio_field_6_q;
assign pin_7_select_o = mmio_field_7_q;
assign pin_8_select_o = mmio_field_8_q;
assign pin_9_select_o = mmio_field_9_q;
assign pin_10_select_o = mmio_field_10_q;
assign pin_11_select_o = mmio_field_11_q;
assign pin_12_select_o = mmio_field_12_q;
assign pin_13_select_o = mmio_field_13_q;
assign pin_14_select_o = mmio_field_14_q;
assign pin_15_select_o = mmio_field_15_q;
assign pin_16_select_o = mmio_field_16_q;

always @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
        aw_pending_q <= 1'b0;
        awaddr_q     <= 12'b0;
        w_pending_q  <= 1'b0;
        wdata_q      <= 64'b0;
        wstrb_q      <= 8'b0;
        s_axi_bresp  <= AXI_RESP_OKAY;
        s_axi_bvalid <= 1'b0;
        mmio_field_0_q <= 2'h0;
        mmio_field_1_q <= 2'h0;
        mmio_field_2_q <= 2'h0;
        mmio_field_3_q <= 2'h0;
        mmio_field_4_q <= 2'h0;
        mmio_field_5_q <= 2'h0;
        mmio_field_6_q <= 2'h0;
        mmio_field_7_q <= 2'h0;
        mmio_field_8_q <= 2'h0;
        mmio_field_9_q <= 2'h0;
        mmio_field_10_q <= 2'h0;
        mmio_field_11_q <= 2'h0;
        mmio_field_12_q <= 2'h0;
        mmio_field_13_q <= 2'h0;
        mmio_field_14_q <= 2'h0;
        mmio_field_15_q <= 2'h0;
        mmio_field_16_q <= 2'h0;
    end else begin
        if (s_axi_bvalid && s_axi_bready)
            s_axi_bvalid <= 1'b0;
        if (aw_take) begin
            aw_pending_q <= 1'b1;
            awaddr_q     <= s_axi_awaddr;
        end
        if (w_take) begin
            w_pending_q <= 1'b1;
            wdata_q     <= s_axi_wdata;
            wstrb_q     <= s_axi_wstrb;
        end
        if (write_fire) begin
            aw_pending_q <= 1'b0;
            w_pending_q  <= 1'b0;
            s_axi_bvalid <= 1'b1;
            s_axi_bresp  <= address_is_mapped(write_address)
                            ? AXI_RESP_OKAY : AXI_RESP_SLVERR;
            if (address_is_mapped(write_address)) begin
            case (write_address)
                12'h008: begin
                    mmio_field_0_q <= (mmio_field_0_q & ~write_mask[1:0])
                        | (write_data[1:0] & write_mask[1:0]);
                    mmio_field_1_q <= (mmio_field_1_q & ~write_mask[5:4])
                        | (write_data[5:4] & write_mask[5:4]);
                    mmio_field_2_q <= (mmio_field_2_q & ~write_mask[9:8])
                        | (write_data[9:8] & write_mask[9:8]);
                    mmio_field_3_q <= (mmio_field_3_q & ~write_mask[13:12])
                        | (write_data[13:12] & write_mask[13:12]);
                    mmio_field_4_q <= (mmio_field_4_q & ~write_mask[17:16])
                        | (write_data[17:16] & write_mask[17:16]);
                    mmio_field_5_q <= (mmio_field_5_q & ~write_mask[21:20])
                        | (write_data[21:20] & write_mask[21:20]);
                    mmio_field_6_q <= (mmio_field_6_q & ~write_mask[25:24])
                        | (write_data[25:24] & write_mask[25:24]);
                    mmio_field_7_q <= (mmio_field_7_q & ~write_mask[29:28])
                        | (write_data[29:28] & write_mask[29:28]);
                    mmio_field_8_q <= (mmio_field_8_q & ~write_mask[33:32])
                        | (write_data[33:32] & write_mask[33:32]);
                    mmio_field_9_q <= (mmio_field_9_q & ~write_mask[37:36])
                        | (write_data[37:36] & write_mask[37:36]);
                    mmio_field_10_q <= (mmio_field_10_q & ~write_mask[41:40])
                        | (write_data[41:40] & write_mask[41:40]);
                    mmio_field_11_q <= (mmio_field_11_q & ~write_mask[45:44])
                        | (write_data[45:44] & write_mask[45:44]);
                    mmio_field_12_q <= (mmio_field_12_q & ~write_mask[49:48])
                        | (write_data[49:48] & write_mask[49:48]);
                    mmio_field_13_q <= (mmio_field_13_q & ~write_mask[53:52])
                        | (write_data[53:52] & write_mask[53:52]);
                    mmio_field_14_q <= (mmio_field_14_q & ~write_mask[57:56])
                        | (write_data[57:56] & write_mask[57:56]);
                    mmio_field_15_q <= (mmio_field_15_q & ~write_mask[61:60])
                        | (write_data[61:60] & write_mask[61:60]);
                end
                12'h010: begin
                    mmio_field_16_q <= (mmio_field_16_q & ~write_mask[1:0])
                        | (write_data[1:0] & write_mask[1:0]);
                end
                default: begin end
            endcase
            end
        end
    end
end

always @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
        s_axi_rdata  <= 64'b0;
        s_axi_rresp  <= AXI_RESP_OKAY;
        s_axi_rvalid <= 1'b0;
    end else begin
        if (s_axi_rvalid && s_axi_rready)
            s_axi_rvalid <= 1'b0;
        if (s_axi_arvalid && s_axi_arready) begin
            s_axi_rvalid <= 1'b1;
            s_axi_rdata  <= read_register(s_axi_araddr);
            s_axi_rresp  <= address_is_mapped(s_axi_araddr)
                            ? AXI_RESP_OKAY : AXI_RESP_SLVERR;
        end
    end
end

endmodule
)IOMUX");
}

QString frozenWide64Report()
{
    return QStringLiteral(R"IOMUX(IOMUX route report for iomux0
pin_count: 17
hs_slots: 4
data_width: 64
address_width: 12
selector: 2-bit field in a fixed 4-bit lane per pin
selector registers: 2 at offset 0x8 to 0x10
registers total: 3
aperture: 24 bytes
capability: 0x0000000000040011 at offset 0x0
reset: every selector resets to 0 and selects slot 0
rx: pad input broadcasts to every declared sink regardless of the selector

pin 0 selector word 0 lsb 0 offset 0x8
  slot 0 function gpio0 signal data0
    input_value: link gpio0_in bit 0
    input_enable: constant 1
    output_value: link gpio0_out bit 0
    output_enable: link gpio0_oe bit 0
  unused slots: 1, 2, 3
pin 1 selector word 0 lsb 4 offset 0x8
  unused slots: 0, 1, 2, 3
pin 2 selector word 0 lsb 8 offset 0x8
  unused slots: 0, 1, 2, 3
pin 3 selector word 0 lsb 12 offset 0x8
  unused slots: 0, 1, 2, 3
pin 4 selector word 0 lsb 16 offset 0x8
  unused slots: 0, 1, 2, 3
pin 5 selector word 0 lsb 20 offset 0x8
  unused slots: 0, 1, 2, 3
pin 6 selector word 0 lsb 24 offset 0x8
  unused slots: 0, 1, 2, 3
pin 7 selector word 0 lsb 28 offset 0x8
  unused slots: 0, 1, 2, 3
pin 8 selector word 0 lsb 32 offset 0x8
  unused slots: 0, 1, 2, 3
pin 9 selector word 0 lsb 36 offset 0x8
  unused slots: 0, 1, 2, 3
pin 10 selector word 0 lsb 40 offset 0x8
  unused slots: 0, 1, 2, 3
pin 11 selector word 0 lsb 44 offset 0x8
  unused slots: 0, 1, 2, 3
pin 12 selector word 0 lsb 48 offset 0x8
  unused slots: 0, 1, 2, 3
pin 13 selector word 0 lsb 52 offset 0x8
  unused slots: 0, 1, 2, 3
pin 14 selector word 0 lsb 56 offset 0x8
  unused slots: 0, 1, 2, 3
pin 15 selector word 0 lsb 60 offset 0x8
  unused slots: 0, 1, 2, 3
pin 16 selector word 1 lsb 0 offset 0x10
  slot 3 function tail signal oe
    input_value: no sink
    input_enable: constant 0
    output_value: constant 0
    output_enable: constant 1
  unused slots: 0, 1, 2

undeclared pin/slot pairs drive a zero tx bundle
)IOMUX");
}

QSocModuleDefinition makeWideSlotDefinition()
{
    return makeDefinition(QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 64
    address_width: 16
    pin_count: 32
    hs_slots: 8
%1    route:
      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
        input_value: {link: gpio0_in, bit: 0}
        input_enable: 1
        output_value: {link: gpio0_out, bit: 0}
        output_enable: {link: gpio0_oe, bit: 0}
      - pin: 31
        slot: 7
        function: tail
        signal: oe
        input_value: {link: tail_in}
        output_enable: 1
)")
                              .arg(integrationBlock()));
}

QString frozenWideSlotReport()
{
    return QStringLiteral(R"IOMUX(IOMUX route report for iomux0
pin_count: 32
hs_slots: 8
data_width: 64
address_width: 16
selector: 3-bit field in a fixed 4-bit lane per pin
selector registers: 2 at offset 0x8 to 0x10
registers total: 3
aperture: 24 bytes
capability: 0x0000000000080020 at offset 0x0
reset: every selector resets to 0 and selects slot 0
rx: pad input broadcasts to every declared sink regardless of the selector

pin 0 selector word 0 lsb 0 offset 0x8
  slot 0 function gpio0 signal data0
    input_value: link gpio0_in bit 0
    input_enable: constant 1
    output_value: link gpio0_out bit 0
    output_enable: link gpio0_oe bit 0
  unused slots: 1, 2, 3, 4, 5, 6, 7
pin 1 selector word 0 lsb 4 offset 0x8
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 2 selector word 0 lsb 8 offset 0x8
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 3 selector word 0 lsb 12 offset 0x8
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 4 selector word 0 lsb 16 offset 0x8
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 5 selector word 0 lsb 20 offset 0x8
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 6 selector word 0 lsb 24 offset 0x8
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 7 selector word 0 lsb 28 offset 0x8
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 8 selector word 0 lsb 32 offset 0x8
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 9 selector word 0 lsb 36 offset 0x8
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 10 selector word 0 lsb 40 offset 0x8
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 11 selector word 0 lsb 44 offset 0x8
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 12 selector word 0 lsb 48 offset 0x8
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 13 selector word 0 lsb 52 offset 0x8
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 14 selector word 0 lsb 56 offset 0x8
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 15 selector word 0 lsb 60 offset 0x8
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 16 selector word 1 lsb 0 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 17 selector word 1 lsb 4 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 18 selector word 1 lsb 8 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 19 selector word 1 lsb 12 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 20 selector word 1 lsb 16 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 21 selector word 1 lsb 20 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 22 selector word 1 lsb 24 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 23 selector word 1 lsb 28 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 24 selector word 1 lsb 32 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 25 selector word 1 lsb 36 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 26 selector word 1 lsb 40 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 27 selector word 1 lsb 44 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 28 selector word 1 lsb 48 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 29 selector word 1 lsb 52 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 30 selector word 1 lsb 56 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 31 selector word 1 lsb 60 offset 0x10
  slot 7 function tail signal oe
    input_value: link tail_in
    input_enable: constant 0
    output_value: constant 0
    output_enable: constant 1
  unused slots: 0, 1, 2, 3, 4, 5, 6

undeclared pin/slot pairs drive a zero tx bundle
)IOMUX");
}

QString frozenExtendedIntegrationNetlist()
{
    return QStringLiteral(R"IOMUX(# Generated by QSoC. Do not edit.
instance:
  u_iomux0:
    module: iomux0
    port:
      clk_i:
        link: clk_iomux
      rst_ni:
        link: rst_iomux_n
      pad_input_value_i:
        link: pad_input_value
      pad_input_enable_o:
        link: pad_input_enable
      pad_output_value_o:
        link: pad_output_value
      pad_output_enable_o:
        link: pad_output_enable
      hs_p0_s0_input_value_o:
        link: gpio0_in
        bits: "[0]"
      hs_p0_s0_output_value_i:
        link: gpio0_out
        bits: "[0]"
      hs_p0_s2_output_value_i:
        link: uart0_tx
      hs_p8_s1_input_value_o:
        link: tail_in
bus:
  iomux_control:
    - instance: u_iomux0
      port: control
)IOMUX");
}

QString frozenRegsVerilog()
{
    return QStringLiteral(R"IOMUX(// Generated by QSoC. Do not edit.
module iomux0_regs (
    input  wire        clk_i,
    input  wire        rst_ni,
    input  wire [11:0] s_axi_awaddr,
    input  wire [2:0]  s_axi_awprot,
    input  wire        s_axi_awvalid,
    output wire        s_axi_awready,
    input  wire [31:0] s_axi_wdata,
    input  wire [3:0]  s_axi_wstrb,
    input  wire        s_axi_wvalid,
    output wire        s_axi_wready,
    output reg  [1:0]  s_axi_bresp,
    output reg         s_axi_bvalid,
    input  wire        s_axi_bready,
    input  wire [11:0] s_axi_araddr,
    input  wire [2:0]  s_axi_arprot,
    input  wire        s_axi_arvalid,
    output wire        s_axi_arready,
    output reg  [31:0] s_axi_rdata,
    output reg  [1:0]  s_axi_rresp,
    output reg         s_axi_rvalid,
    input  wire        s_axi_rready,
    output wire pin_0_select_o,
    output wire pin_1_select_o
);

localparam [1:0] AXI_RESP_OKAY   = 2'b00;
localparam [1:0] AXI_RESP_SLVERR = 2'b10;

reg mmio_field_0_q;
reg mmio_field_1_q;
reg        aw_pending_q;
reg [11:0] awaddr_q;
reg        w_pending_q;
reg [31:0] wdata_q;
reg [3:0]  wstrb_q;

function address_is_mapped;
    input [11:0] address;
    begin
        case (address)
            12'h000: address_is_mapped = 1'b1;
            12'h004: address_is_mapped = 1'b1;
            default: address_is_mapped = 1'b0;
        endcase
    end
endfunction

function [31:0] read_register;
    input [11:0] address;
    begin
        read_register = 32'b0;
        case (address)
            12'h000: begin
                read_register[15:0] = 16'h2;
                read_register[23:16] = 8'h2;
            end
            12'h004: begin
                read_register[0] = mmio_field_0_q;
                read_register[4] = mmio_field_1_q;
            end
            default: begin end
        endcase
    end
endfunction

wire aw_take = s_axi_awvalid && s_axi_awready;
wire w_take  = s_axi_wvalid && s_axi_wready;
wire [11:0] write_address = aw_pending_q ? awaddr_q : s_axi_awaddr;
wire [31:0] write_data    = w_pending_q ? wdata_q : s_axi_wdata;
wire [3:0]  write_strobe  = w_pending_q ? wstrb_q : s_axi_wstrb;
wire write_fire = !s_axi_bvalid && (aw_pending_q || aw_take)
                  && (w_pending_q || w_take);
wire [31:0] write_mask = {{8{write_strobe[3]}}, {8{write_strobe[2]}},
                          {8{write_strobe[1]}}, {8{write_strobe[0]}}};

assign s_axi_awready = rst_ni && !aw_pending_q && !s_axi_bvalid;
assign s_axi_wready  = rst_ni && !w_pending_q && !s_axi_bvalid;
assign s_axi_arready = rst_ni && !s_axi_rvalid;

assign pin_0_select_o = mmio_field_0_q;
assign pin_1_select_o = mmio_field_1_q;

always @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
        aw_pending_q <= 1'b0;
        awaddr_q     <= 12'b0;
        w_pending_q  <= 1'b0;
        wdata_q      <= 32'b0;
        wstrb_q      <= 4'b0;
        s_axi_bresp  <= AXI_RESP_OKAY;
        s_axi_bvalid <= 1'b0;
        mmio_field_0_q <= 1'h0;
        mmio_field_1_q <= 1'h0;
    end else begin
        if (s_axi_bvalid && s_axi_bready)
            s_axi_bvalid <= 1'b0;
        if (aw_take) begin
            aw_pending_q <= 1'b1;
            awaddr_q     <= s_axi_awaddr;
        end
        if (w_take) begin
            w_pending_q <= 1'b1;
            wdata_q     <= s_axi_wdata;
            wstrb_q     <= s_axi_wstrb;
        end
        if (write_fire) begin
            aw_pending_q <= 1'b0;
            w_pending_q  <= 1'b0;
            s_axi_bvalid <= 1'b1;
            s_axi_bresp  <= address_is_mapped(write_address)
                            ? AXI_RESP_OKAY : AXI_RESP_SLVERR;
            if (address_is_mapped(write_address)) begin
            case (write_address)
                12'h004: begin
                    mmio_field_0_q <= (mmio_field_0_q & ~write_mask[0])
                        | (write_data[0] & write_mask[0]);
                    mmio_field_1_q <= (mmio_field_1_q & ~write_mask[4])
                        | (write_data[4] & write_mask[4]);
                end
                default: begin end
            endcase
            end
        end
    end
end

always @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
        s_axi_rdata  <= 32'b0;
        s_axi_rresp  <= AXI_RESP_OKAY;
        s_axi_rvalid <= 1'b0;
    end else begin
        if (s_axi_rvalid && s_axi_rready)
            s_axi_rvalid <= 1'b0;
        if (s_axi_arvalid && s_axi_arready) begin
            s_axi_rvalid <= 1'b1;
            s_axi_rdata  <= read_register(s_axi_araddr);
            s_axi_rresp  <= address_is_mapped(s_axi_araddr)
                            ? AXI_RESP_OKAY : AXI_RESP_SLVERR;
        end
    end
end

endmodule
)IOMUX");
}

QString frozenConnVerilog()
{
    return QStringLiteral(R"IOMUX(// Generated by QSoC. Do not edit.
module iomux0_conn (
    output wire [3:0] tx_input_enable_o,
    output wire [3:0] tx_output_value_o,
    output wire [3:0] tx_output_enable_o,
    input  wire [3:0] rx_input_value_i,
    output wire hs_p0_s0_input_value_o, /* gpio0.data0 */
    input  wire hs_p0_s0_output_value_i, /* gpio0.data0 */
    input  wire hs_p0_s0_output_enable_i, /* gpio0.data0 */
    input  wire hs_p0_s1_output_value_i, /* uart0.tx */
    output wire hs_p1_s1_input_value_o /* uart0.rx */
);

assign tx_input_enable_o[0] = 1'b1;
assign tx_output_value_o[0] = hs_p0_s0_output_value_i;
assign tx_output_enable_o[0] = hs_p0_s0_output_enable_i;
assign tx_input_enable_o[2] = 1'b0;
assign tx_output_value_o[2] = hs_p0_s1_output_value_i;
assign tx_output_enable_o[2] = 1'b1;
assign tx_input_enable_o[1] = 1'b0;
assign tx_output_value_o[1] = 1'b0;
assign tx_output_enable_o[1] = 1'b0;
assign tx_input_enable_o[3] = 1'b1;
assign tx_output_value_o[3] = 1'b0;
assign tx_output_enable_o[3] = 1'b0;

assign hs_p0_s0_input_value_o = rx_input_value_i[0];
assign hs_p1_s1_input_value_o = rx_input_value_i[3];

endmodule
)IOMUX");
}

QString frozenTopVerilog()
{
    return QStringLiteral(R"IOMUX(// Generated by QSoC. Do not edit.
module iomux0_core (
    input  wire [1:0] pad_input_value_i,
    output wire [1:0] pad_input_enable_o,
    output wire [1:0] pad_output_value_o,
    output wire [1:0] pad_output_enable_o,
    input  wire [3:0] tx_input_enable_i,
    input  wire [3:0] tx_output_value_i,
    input  wire [3:0] tx_output_enable_i,
    output wire [3:0] rx_input_value_o,
    input  wire [0:0] pin_0_select_i,
    input  wire [0:0] pin_1_select_i
);

reg [2:0] tx_bundle_0;
always @(*) begin
    case (pin_0_select_i)
        1'd0: tx_bundle_0 = {tx_input_enable_i[0], tx_output_value_i[0], tx_output_enable_i[0]};
        1'd1: tx_bundle_0 = {tx_input_enable_i[2], tx_output_value_i[2], tx_output_enable_i[2]};
        default: tx_bundle_0 = 3'b000;
    endcase
end
assign pad_input_enable_o[0]  = tx_bundle_0[2];
assign pad_output_value_o[0]  = tx_bundle_0[1];
assign pad_output_enable_o[0] = tx_bundle_0[0];

reg [2:0] tx_bundle_1;
always @(*) begin
    case (pin_1_select_i)
        1'd0: tx_bundle_1 = {tx_input_enable_i[1], tx_output_value_i[1], tx_output_enable_i[1]};
        1'd1: tx_bundle_1 = {tx_input_enable_i[3], tx_output_value_i[3], tx_output_enable_i[3]};
        default: tx_bundle_1 = 3'b000;
    endcase
end
assign pad_input_enable_o[1]  = tx_bundle_1[2];
assign pad_output_value_o[1]  = tx_bundle_1[1];
assign pad_output_enable_o[1] = tx_bundle_1[0];

assign rx_input_value_o[0] = pad_input_value_i[0];
assign rx_input_value_o[2] = pad_input_value_i[0];
assign rx_input_value_o[1] = pad_input_value_i[1];
assign rx_input_value_o[3] = pad_input_value_i[1];

endmodule

module iomux0 (
    input  wire clk_i,
    input  wire rst_ni,
    input  wire [11:0] s_axi_awaddr,
    input  wire [2:0] s_axi_awprot,
    input  wire s_axi_awvalid,
    output wire s_axi_awready,
    input  wire [31:0] s_axi_wdata,
    input  wire [3:0] s_axi_wstrb,
    input  wire s_axi_wvalid,
    output wire s_axi_wready,
    output wire [1:0] s_axi_bresp,
    output wire s_axi_bvalid,
    input  wire s_axi_bready,
    input  wire [11:0] s_axi_araddr,
    input  wire [2:0] s_axi_arprot,
    input  wire s_axi_arvalid,
    output wire s_axi_arready,
    output wire [31:0] s_axi_rdata,
    output wire [1:0] s_axi_rresp,
    output wire s_axi_rvalid,
    input  wire s_axi_rready,
    input  wire [1:0] pad_input_value_i,
    output wire [1:0] pad_input_enable_o,
    output wire [1:0] pad_output_value_o,
    output wire [1:0] pad_output_enable_o,
    output wire hs_p0_s0_input_value_o, /* gpio0.data0 */
    input  wire hs_p0_s0_output_value_i, /* gpio0.data0 */
    input  wire hs_p0_s0_output_enable_i, /* gpio0.data0 */
    input  wire hs_p0_s1_output_value_i, /* uart0.tx */
    output wire hs_p1_s1_input_value_o /* uart0.rx */
);

wire [3:0] tx_input_enable_w;
wire [3:0] tx_output_value_w;
wire [3:0] tx_output_enable_w;
wire [3:0] rx_input_value_w;
wire [0:0] pin_0_select_w;
wire [0:0] pin_1_select_w;

iomux0_regs u_regs (
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
    .pin_0_select_o(pin_0_select_w),
    .pin_1_select_o(pin_1_select_w)
);

iomux0_conn u_conn (
    .tx_input_enable_o(tx_input_enable_w),
    .tx_output_value_o(tx_output_value_w),
    .tx_output_enable_o(tx_output_enable_w),
    .rx_input_value_i(rx_input_value_w),
    .hs_p0_s0_input_value_o(hs_p0_s0_input_value_o),
    .hs_p0_s0_output_value_i(hs_p0_s0_output_value_i),
    .hs_p0_s0_output_enable_i(hs_p0_s0_output_enable_i),
    .hs_p0_s1_output_value_i(hs_p0_s1_output_value_i),
    .hs_p1_s1_input_value_o(hs_p1_s1_input_value_o)
);

iomux0_core u_core (
    .pad_input_value_i(pad_input_value_i),
    .pad_input_enable_o(pad_input_enable_o),
    .pad_output_value_o(pad_output_value_o),
    .pad_output_enable_o(pad_output_enable_o),
    .tx_input_enable_i(tx_input_enable_w),
    .tx_output_value_i(tx_output_value_w),
    .tx_output_enable_i(tx_output_enable_w),
    .rx_input_value_o(rx_input_value_w),
    .pin_0_select_i(pin_0_select_w),
    .pin_1_select_i(pin_1_select_w)
);

endmodule
)IOMUX");
}

QString frozenReport()
{
    return QStringLiteral(R"IOMUX(IOMUX route report for iomux0
pin_count: 2
hs_slots: 2
data_width: 32
address_width: 12
selector: 1-bit field in a fixed 4-bit lane per pin
selector registers: 1 at offset 0x4 to 0x4
registers total: 2
aperture: 8 bytes
capability: 0x00020002 at offset 0x0
reset: every selector resets to 0 and selects slot 0
rx: pad input broadcasts to every declared sink regardless of the selector

pin 0 selector word 0 lsb 0 offset 0x4
  slot 0 function gpio0 signal data0
    input_value: link gpio0_input bit 0
    input_enable: constant 1
    output_value: link gpio0_output bit 0
    output_enable: link gpio0_enable bit 0
  slot 1 function uart0 signal tx
    input_value: no sink
    input_enable: constant 0
    output_value: link uart0_tx
    output_enable: constant 1
  unused slots: none
pin 1 selector word 0 lsb 4 offset 0x4
  slot 1 function uart0 signal rx
    input_value: link uart0_rx
    input_enable: constant 1
    output_value: constant 0
    output_enable: constant 0
  unused slots: 0

undeclared pin/slot pairs drive a zero tx bundle
)IOMUX");
}

QString frozenIntegrationNetlist()
{
    return QStringLiteral(R"IOMUX(# Generated by QSoC. Do not edit.
instance:
  u_iomux0:
    module: iomux0
    port:
      clk_i:
        link: clk_iomux
      rst_ni:
        link: rst_iomux_n
      pad_input_value_i:
        link: pad_input_value
      pad_input_enable_o:
        link: pad_input_enable
      pad_output_value_o:
        link: pad_output_value
      pad_output_enable_o:
        link: pad_output_enable
      hs_p0_s0_input_value_o:
        link: gpio0_input
        bits: "[0]"
      hs_p0_s0_output_value_i:
        link: gpio0_output
        bits: "[0]"
      hs_p0_s0_output_enable_i:
        link: gpio0_enable
        bits: "[0]"
      hs_p0_s1_output_value_i:
        link: uart0_tx
      hs_p1_s1_input_value_o:
        link: uart0_rx
bus:
  iomux_control:
    - instance: u_iomux0
      port: control
)IOMUX");
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void draftIsRecognizedAndIncomplete();
    void planPreservesSemantics();
    void endpointPortNamesAreStable();
    void omittedSlotCountMatchesExplicitDefault();
    void sourceOrderDoesNotChangeGeneratedVerilog();
    void generatedArtifactsMatchFrozenBaseline();
    void reportRejectsPlanInconsistentWithPinCount();
    void reportCapabilityFollowsComposedRegister();
    void selectorLayoutMatchesFrozenKnownAnswer_data();
    void selectorLayoutMatchesFrozenKnownAnswer();
    void pinCountBoundaryFollowsLaneFormula_data();
    void pinCountBoundaryFollowsLaneFormula();
    void slotCountSetsSelectorFieldWidth_data();
    void slotCountSetsSelectorFieldWidth();
    void apertureExceedingAddressWidthIsRejected();
    void reportListsRoutesAndLayout();
    void invalidSource_data();
    void invalidSource();
    void regsAndTopEmittersComposePlan();
    void projectionMatchesWrapperHeader();
    void integrationNetlistConnectsEverythingOnce();
    void routingSimulationWhenIverilogIsAvailable();
    void fiveSlotInvalidSelectorCodesDriveZeroWhenIverilogIsAvailable();
    void axiSelectorDrivesTailPinWhenIverilogIsAvailable_data();
    void axiSelectorDrivesTailPinWhenIverilogIsAvailable();
    void registerTakeoverDrivesPadWhenIverilogIsAvailable();
    void interruptRecordsEventsWhenIverilogIsAvailable();
};

void Test::draftIsRecognizedAndIncomplete()
{
    QSocModuleDefinition definition;
    definition.libraryName                  = "peripheral";
    definition.moduleName                   = "iomux0";
    definition.extraAttributes["generator"] = QSocIomuxGenerator::createDraftGenerator();

    QVERIFY(QSocIomuxGenerator::isIomux(definition));
    const QStringList errors = QSocIomuxGenerator::validate(definition);
    QCOMPARE(errors.size(), 2);
    QVERIFY(errors.at(0).startsWith("IOMUX_REQUIRED generator.integration"));
    QVERIFY(errors.at(1).startsWith("IOMUX_REQUIRED generator.pin_count"));
}

void Test::planPreservesSemantics()
{
    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeValidDefinition(), &plan, &errors),
        qPrintable(errors.join('\n')));

    QCOMPARE(plan.moduleName, QString("iomux0"));
    QCOMPARE(plan.pinCount, 2U);
    QCOMPARE(plan.hsSlots, 2U);
    QCOMPARE(plan.integration.instance, QString("u_iomux0"));
    QCOMPARE(plan.integration.control, QString("iomux_control"));
    QCOMPARE(plan.integration.padOutputEnable, QString("pad_output_enable"));

    QCOMPARE(plan.routes.size(), 3);
    QCOMPARE(plan.routes.at(0).pin, 0U);
    QCOMPARE(plan.routes.at(0).slot, 0U);
    QCOMPARE(plan.routes.at(0).function, QString("gpio0"));
    QCOMPARE(plan.routes.at(0).inputValue.link, QString("gpio0_input"));
    QCOMPARE(plan.routes.at(0).inputValue.bit.value(), 0U);
    QCOMPARE(plan.routes.at(0).inputEnable.constant.value(), quint8(1));
    QCOMPARE(plan.routes.at(1).slot, 1U);
    QCOMPARE(plan.routes.at(1).outputValue.link, QString("uart0_tx"));
    QVERIFY(!plan.routes.at(1).outputValue.bit.has_value());
    QCOMPARE(plan.routes.at(2).pin, 1U);

    QCOMPARE(plan.mmio.moduleName, QString("iomux0_regs"));
    QCOMPARE(plan.mmio.dataWidth, 32U);
    QCOMPARE(plan.mmio.addressWidth, 12U);
    QCOMPARE(plan.mmio.registers.size(), 2);
    QCOMPARE(plan.mmio.registers.at(0).name, QString("capability"));
    QCOMPARE(plan.mmio.registers.at(0).byteOffset, quint64(0));
    QCOMPARE(plan.mmio.registers.at(1).name, QString("hs_select_0"));
    QCOMPARE(plan.mmio.registers.at(1).byteOffset, quint64(4));
    QCOMPARE(plan.mmio.registers.at(1).fields.size(), 2);
    QCOMPARE(plan.mmio.registers.at(1).fields.at(0).name, QString("pin_0_select"));
    QCOMPARE(plan.mmio.registers.at(1).fields.at(0).width, 1U);
    QCOMPARE(plan.mmio.registers.at(1).fields.at(0).lsb, 0U);
    QCOMPARE(plan.mmio.registers.at(1).fields.at(1).lsb, 4U);
    QCOMPARE(plan.mmio.registers.at(1).fields.at(1).outputPort, QString("pin_1_select_o"));

    const QSocMmioFieldPlan *pinCountField = findField(plan.mmio, "capability", "pin_count");
    QVERIFY(pinCountField != nullptr);
    QCOMPARE(pinCountField->constantValue.value(), quint64(2));
}

void Test::endpointPortNamesAreStable()
{
    QCOMPARE(
        QSocIomuxGenerator::endpointPortName(2, 3, QSocIomuxRole::InputValue),
        QString("hs_p2_s3_input_value_o"));
    QCOMPARE(
        QSocIomuxGenerator::endpointPortName(2, 3, QSocIomuxRole::InputEnable),
        QString("hs_p2_s3_input_enable_i"));
    QCOMPARE(
        QSocIomuxGenerator::endpointPortName(2, 3, QSocIomuxRole::OutputValue),
        QString("hs_p2_s3_output_value_i"));
    QCOMPARE(
        QSocIomuxGenerator::endpointPortName(2, 3, QSocIomuxRole::OutputEnable),
        QString("hs_p2_s3_output_enable_i"));
}

void Test::omittedSlotCountMatchesExplicitDefault()
{
    const QString explicitSource = sourceForConfig(9, 4, 32, 8);
    QString       omittedSource  = explicitSource;
    omittedSource.replace("    hs_slots: 4\n", QString());
    QVERIFY(omittedSource != explicitSource);

    QSocIomuxPlan explicitPlan;
    QSocIomuxPlan omittedPlan;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeDefinition(explicitSource), &explicitPlan));
    QVERIFY(QSocIomuxGenerator::buildPlan(makeDefinition(omittedSource), &omittedPlan));
    QCOMPARE(omittedPlan.hsSlots, 4U);
    QVERIFY(explicitPlan == omittedPlan);

    QCOMPARE(
        QSocIomuxGenerator::generateCoreVerilog(omittedPlan),
        QSocIomuxGenerator::generateCoreVerilog(explicitPlan));
    QCOMPARE(
        QSocIomuxGenerator::generateConnVerilog(omittedPlan),
        QSocIomuxGenerator::generateConnVerilog(explicitPlan));
    QCOMPARE(
        QSocIomuxGenerator::generateReport(omittedPlan),
        QSocIomuxGenerator::generateReport(explicitPlan));
}

void Test::generatedArtifactsMatchFrozenBaseline()
{
    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeValidDefinition(), &plan, &errors),
        qPrintable(errors.join('\n')));
    QVERIFY(errors.isEmpty());

    compareText(QSocIomuxGenerator::generateRegsVerilog(plan), frozenRegsVerilog());
    compareText(QSocIomuxGenerator::generateConnVerilog(plan), frozenConnVerilog());
    compareText(QSocIomuxGenerator::generateTopVerilog(plan), frozenTopVerilog());
    compareText(QSocIomuxGenerator::generateReport(plan), frozenReport());
    compareText(QSocIomuxGenerator::generateIntegrationNetlist(plan), frozenIntegrationNetlist());

    QSocIomuxPlan extended;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeGoldenExtendedDefinition(), &extended, &errors),
        qPrintable(errors.join('\n')));
    QVERIFY(errors.isEmpty());

    compareText(QSocIomuxGenerator::generateRegsVerilog(extended), frozenExtendedRegsVerilog());
    compareText(QSocIomuxGenerator::generateConnVerilog(extended), frozenExtendedConnVerilog());
    compareText(QSocIomuxGenerator::generateTopVerilog(extended), frozenExtendedTopVerilog());
    compareText(QSocIomuxGenerator::generateReport(extended), frozenExtendedReport());

    QSocIomuxPlan wide;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeWide64Definition(), &wide, &errors),
        qPrintable(errors.join('\n')));
    QVERIFY(errors.isEmpty());

    compareText(QSocIomuxGenerator::generateRegsVerilog(wide), frozenWide64RegsVerilog());
    compareText(QSocIomuxGenerator::generateReport(wide), frozenWide64Report());
    compareText(
        QSocIomuxGenerator::generateIntegrationNetlist(extended),
        frozenExtendedIntegrationNetlist());

    QSocIomuxPlan wideSlots;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeWideSlotDefinition(), &wideSlots, &errors),
        qPrintable(errors.join('\n')));
    QVERIFY(errors.isEmpty());
    compareText(QSocIomuxGenerator::generateReport(wideSlots), frozenWideSlotReport());

    /* One line per pin, whatever the pin count or the slot count. */
    QCOMPARE(
        QSocIomuxGenerator::generateReport(wideSlots).count(" selector word "),
        int(wideSlots.pinCount));
}

void Test::reportRejectsPlanInconsistentWithPinCount()
{
    QSocIomuxPlan built;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeValidDefinition(), &built));
    QVERIFY(!QSocIomuxGenerator::generateReport(built).isEmpty());

    QSocIomuxPlan capabilityOnly = built;
    capabilityOnly.mmio.registers.remove(1, capabilityOnly.mmio.registers.size() - 1);
    QCOMPARE(capabilityOnly.mmio.registers.size(), 1);
    QVERIFY(QSocIomuxGenerator::generateReport(capabilityOnly).isEmpty());

    QSocIomuxPlan pinCountOutrunsRegisters = built;
    pinCountOutrunsRegisters.pinCount      = 256;
    QVERIFY(QSocIomuxGenerator::generateReport(pinCountOutrunsRegisters).isEmpty());

    QSocIomuxPlan noRegisters  = built;
    noRegisters.mmio.registers = {};
    QVERIFY(QSocIomuxGenerator::generateReport(noRegisters).isEmpty());

    QSocIomuxPlan unsupportedWidth  = built;
    unsupportedWidth.mmio.dataWidth = 0;
    QVERIFY(QSocIomuxGenerator::generateReport(unsupportedWidth).isEmpty());
}

void Test::reportCapabilityFollowsComposedRegister()
{
    /* A capability field the composer gains must reach the report on its own.
     * A second encoder in generateReport would keep publishing the old value
     * while the read function already returns the new one. */
    QSocMmioFieldPlan spare;
    spare.name          = QStringLiteral("spare");
    spare.width         = 1;
    spare.access        = QSocMmioAccess::ReadOnly;
    spare.constantValue = 1;

    QSocIomuxPlan narrow;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeValidDefinition(), &narrow));
    spare.lsb = 24;
    narrow.mmio.registers[0].fields.append(spare);
    QVERIFY(
        QSocIomuxGenerator::generateReport(narrow).contains("capability: 0x01020002 at offset 0x0"));

    /* A 64-bit capability register reaches past bit 31. A narrower accumulator
     * or a fixed print width drops such a field while the read function still
     * emits it. */
    QSocIomuxPlan wide;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeWide64Definition(), &wide));
    spare.lsb = 32;
    wide.mmio.registers[0].fields.append(spare);
    QVERIFY(
        QSocIomuxGenerator::generateReport(wide).contains(
            "capability: 0x0000000100040011 at offset 0x0"));

    /* generateReport is public and accepts a register no composer builds. A
     * full-width field must not shift the mask past the accumulator, and a
     * field carrying no constant must not be dereferenced. */
    QSocMmioFieldPlan full;
    full.name          = QStringLiteral("full");
    full.width         = 64;
    full.access        = QSocMmioAccess::ReadOnly;
    full.constantValue = ~quint64(0);

    QSocMmioFieldPlan unset;
    unset.name   = QStringLiteral("unset");
    unset.width  = 1;
    unset.access = QSocMmioAccess::ReadOnly;

    QSocIomuxPlan hostile;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeWide64Definition(), &hostile));
    hostile.mmio.registers[0].fields = {full, unset};
    QVERIFY(
        QSocIomuxGenerator::generateReport(hostile).contains(
            "capability: 0xffffffffffffffff at offset 0x0"));
}

void Test::sourceOrderDoesNotChangeGeneratedVerilog()
{
    QSocIomuxPlan plan;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeValidDefinition(), &plan));

    const QString reordered = QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 12
    pin_count: 2
    hs_slots: 2
%1    route:
      - pin: 1
        slot: 1
        function: uart0
        signal: rx
        input_enable: 1
        input_value: {link: uart0_rx}
      - pin: 0
        slot: 1
        function: uart0
        signal: tx
        output_enable: 1
        output_value: {link: uart0_tx}
      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
        output_enable: {link: gpio0_enable, bit: 0}
        output_value: {link: gpio0_output, bit: 0}
        input_enable: 1
        input_value: {link: gpio0_input, bit: 0}
)")
                                  .arg(integrationBlock());
    QSocIomuxPlan reorderedPlan;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeDefinition(reordered), &reorderedPlan));

    QVERIFY(plan == reorderedPlan);
    QCOMPARE(
        QSocIomuxGenerator::generateCoreVerilog(reorderedPlan),
        QSocIomuxGenerator::generateCoreVerilog(plan));
    QCOMPARE(
        QSocIomuxGenerator::generateConnVerilog(reorderedPlan),
        QSocIomuxGenerator::generateConnVerilog(plan));
    QCOMPARE(
        QSocIomuxGenerator::generateReport(reorderedPlan), QSocIomuxGenerator::generateReport(plan));
}

void Test::selectorLayoutMatchesFrozenKnownAnswer_data()
{
    QTest::addColumn<quint32>("pinCount");
    QTest::addColumn<quint32>("hsSlots");
    QTest::addColumn<quint32>("dataWidth");
    QTest::addColumn<int>("registerCount");
    QTest::addColumn<quint64>("lastOffset");
    QTest::addColumn<quint64>("capability");

    QTest::newRow("185-4-32") << 185U << 4U << 32U << 25 << quint64(0x60) << quint64(0x000400B9);
    QTest::newRow("185-4-64") << 185U << 4U << 64U << 13 << quint64(0x60) << quint64(0x000400B9);
    QTest::newRow("256-8-32") << 256U << 8U << 32U << 33 << quint64(0x80) << quint64(0x00080100);
    QTest::newRow("256-8-64") << 256U << 8U << 64U << 17 << quint64(0x80) << quint64(0x00080100);
}

void Test::selectorLayoutMatchesFrozenKnownAnswer()
{
    QFETCH(quint32, pinCount);
    QFETCH(quint32, hsSlots);
    QFETCH(quint32, dataWidth);
    QFETCH(int, registerCount);
    QFETCH(quint64, lastOffset);
    QFETCH(quint64, capability);

    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(
            makeDefinition(sourceForConfig(pinCount, hsSlots, dataWidth, 8)), &plan, &errors),
        qPrintable(errors.join('\n')));

    QCOMPARE(plan.mmio.registers.size(), registerCount);
    QCOMPARE(plan.mmio.registers.constFirst().byteOffset, quint64(0));
    QCOMPARE(plan.mmio.registers.at(1).byteOffset, quint64(dataWidth / 8));
    QCOMPARE(plan.mmio.registers.constLast().byteOffset, lastOffset);

    const QSocMmioFieldPlan *pinCountField = findField(plan.mmio, "capability", "pin_count");
    const QSocMmioFieldPlan *hsSlotsField  = findField(plan.mmio, "capability", "hs_slots");
    QVERIFY(pinCountField != nullptr && hsSlotsField != nullptr);
    QCOMPARE(
        pinCountField->constantValue.value() | (hsSlotsField->constantValue.value() << 16),
        capability);

    const QString            lastSelector = QString("pin_%1_select").arg(pinCount - 1);
    const QSocMmioFieldPlan *lastField
        = findField(plan.mmio, plan.mmio.registers.constLast().name, lastSelector);
    QVERIFY(lastField != nullptr);
    const quint32 lanes = dataWidth / 4;
    QCOMPARE(lastField->lsb, ((pinCount - 1) % lanes) * 4);
    QCOMPARE(lastField->resetValue.value(), quint64(0));
    QCOMPARE(lastField->outputPort, QString("pin_%1_select_o").arg(pinCount - 1));
}

void Test::pinCountBoundaryFollowsLaneFormula_data()
{
    QTest::addColumn<quint32>("pinCount");
    for (const quint32 pinCount : {1U, 7U, 8U, 9U, 15U, 16U, 17U, 184U, 185U, 186U, 255U, 256U}) {
        QTest::newRow(qPrintable(QString::number(pinCount))) << pinCount;
    }
}

void Test::pinCountBoundaryFollowsLaneFormula()
{
    QFETCH(quint32, pinCount);
    for (const quint32 dataWidth : {32U, 64U}) {
        QSocIomuxPlan plan;
        QStringList   errors;
        QVERIFY2(
            QSocIomuxGenerator::buildPlan(
                makeDefinition(sourceForConfig(pinCount, 4, dataWidth, 12)), &plan, &errors),
            qPrintable(errors.join('\n')));
        const quint32 lanes = dataWidth / 4;
        const quint32 words = (pinCount + lanes - 1) / lanes;
        QCOMPARE(plan.mmio.registers.size(), int(words) + 1);
        QCOMPARE(plan.mmio.registers.constLast().byteOffset, quint64(words) * (dataWidth / 8));
        const QSocMmioFieldPlan *lastField = findField(
            plan.mmio,
            plan.mmio.registers.constLast().name,
            QString("pin_%1_select").arg(pinCount - 1));
        QVERIFY(lastField != nullptr);
        QCOMPARE(lastField->lsb, ((pinCount - 1) % lanes) * 4);
    }
}

void Test::slotCountSetsSelectorFieldWidth_data()
{
    QTest::addColumn<quint32>("hsSlots");
    QTest::addColumn<quint32>("fieldWidth");

    QTest::newRow("2") << 2U << 1U;
    QTest::newRow("3") << 3U << 2U;
    QTest::newRow("4") << 4U << 2U;
    QTest::newRow("5") << 5U << 3U;
    QTest::newRow("8") << 8U << 3U;
}

void Test::slotCountSetsSelectorFieldWidth()
{
    QFETCH(quint32, hsSlots);
    QFETCH(quint32, fieldWidth);

    QSocIomuxPlan plan;
    QVERIFY(
        QSocIomuxGenerator::buildPlan(makeDefinition(sourceForConfig(9, hsSlots, 32, 8)), &plan));
    const QSocMmioFieldPlan *field = findField(plan.mmio, "hs_select_1", "pin_8_select");
    QVERIFY(field != nullptr);
    QCOMPARE(field->width, fieldWidth);
    QCOMPARE(field->lsb, 0U);

    const QSocMmioFieldPlan *hsSlotsField = findField(plan.mmio, "capability", "hs_slots");
    QVERIFY(hsSlotsField != nullptr);
    QCOMPARE(hsSlotsField->constantValue.value(), quint64(hsSlots));
}

void Test::apertureExceedingAddressWidthIsRejected()
{
    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY(!QSocIomuxGenerator::buildPlan(
        makeDefinition(sourceForConfig(256, 8, 32, 7)), &plan, &errors));
    QCOMPARE(plan, QSocIomuxPlan());
    QCOMPARE(errors.size(), 1);
    QVERIFY(errors.constFirst().startsWith("IOMUX_RANGE generator.address_width"));
    QVERIFY(errors.constFirst().contains("aperture needs 132 bytes"));
    QVERIFY(errors.constFirst().contains("minimum address_width is 8"));
    QVERIFY(
        QSocIomuxGenerator::generateCoreVerilog(plan).isEmpty()
        && QSocIomuxGenerator::generateConnVerilog(plan).isEmpty()
        && QSocIomuxGenerator::generateReport(plan).isEmpty());

    QVERIFY(QSocIomuxGenerator::buildPlan(makeDefinition(sourceForConfig(256, 8, 32, 8)), &plan));

    QVERIFY(!QSocIomuxGenerator::buildPlan(
        makeDefinition(sourceForConfig(256, 8, 64, 7)), &plan, &errors));
    QCOMPARE(errors.size(), 1);
    QVERIFY(errors.constFirst().contains("aperture needs 136 bytes"));
    QVERIFY(errors.constFirst().contains("minimum address_width is 8"));
    QVERIFY(QSocIomuxGenerator::buildPlan(makeDefinition(sourceForConfig(256, 8, 64, 8)), &plan));

    QVERIFY(
        !QSocIomuxGenerator::buildPlan(makeDefinition(sourceForConfig(1, 2, 32, 2)), &plan, &errors));
    QVERIFY(errors.constFirst().contains("minimum address_width is 3"));
    QVERIFY(QSocIomuxGenerator::buildPlan(makeDefinition(sourceForConfig(1, 2, 32, 3)), &plan));
}

void Test::reportListsRoutesAndLayout()
{
    QSocIomuxPlan plan;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeValidDefinition(), &plan));
    const QString report = QSocIomuxGenerator::generateReport(plan);

    QVERIFY(report.contains("IOMUX route report for iomux0"));
    QVERIFY(report.contains("pin_count: 2"));
    QVERIFY(report.contains("hs_slots: 2"));
    QVERIFY(report.contains("selector: 1-bit field in a fixed 4-bit lane per pin"));
    QVERIFY(report.contains("capability: 0x00020002 at offset 0x0"));
    QVERIFY(report.contains("reset: every selector resets to 0 and selects slot 0"));
    QVERIFY(report.contains("pin 0 selector word 0 lsb 0 offset 0x4"));
    QVERIFY(report.contains("pin 1 selector word 0 lsb 4 offset 0x4"));
    QVERIFY(report.contains("  slot 1 function uart0 signal tx"));
    QVERIFY(report.contains("    input_value: no sink"));
    QVERIFY(report.contains("    output_value: link uart0_tx"));
    QVERIFY(report.contains("    input_enable: constant 1"));
    QVERIFY(report.contains("  unused slots: none"));
    QVERIFY(report.contains("  unused slots: 0"));
    QVERIFY(report.contains("undeclared pin/slot pairs drive a zero tx bundle"));
}

void Test::invalidSource_data()
{
    QTest::addColumn<QString>("source");
    QTest::addColumn<QString>("expectedError");

    const QString basePrefix = QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    pin_count: 2
    hs_slots: 2
%1)")
                                   .arg(integrationBlock());
    const auto    withRoute  = [basePrefix](const QString &route) {
        return basePrefix + "    route:\n" + route;
    };
    const QString minimalRoute = QString(R"(      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
        output_enable: 1
)");

    QTest::newRow("missing-pin-count")
        << QString("generator:\n    kind: iomux\n    bus: axi4_lite\n%1    route: []\n")
               .arg(integrationBlock())
        << "IOMUX_REQUIRED generator.pin_count";
    QTest::newRow("pin-count-zero")
        << sourceForConfig(0, 2, 32, 8) << "IOMUX_RANGE generator.pin_count";
    QTest::newRow("pin-count-over-max")
        << sourceForConfig(257, 2, 32, 8) << "IOMUX_RANGE generator.pin_count";
    QTest::newRow("pin-count-quoted-string")
        << QString(basePrefix).replace("pin_count: 2", "pin_count: \"2\"") + "    route: []\n"
        << "IOMUX_TYPE generator.pin_count";
    QTest::newRow("pin-count-bool")
        << QString(basePrefix).replace("pin_count: 2", "pin_count: true") + "    route: []\n"
        << "IOMUX_TYPE generator.pin_count";
    QTest::newRow("pin-count-float")
        << QString(basePrefix).replace("pin_count: 2", "pin_count: 2.0") + "    route: []\n"
        << "IOMUX_TYPE generator.pin_count";
    QTest::newRow("hs-slots-one") << sourceForConfig(2, 1, 32, 8)
                                  << "IOMUX_RANGE generator.hs_slots";
    QTest::newRow("hs-slots-nine")
        << sourceForConfig(2, 9, 32, 8) << "IOMUX_RANGE generator.hs_slots";
    QTest::newRow("wrong-kind") << QString(basePrefix).replace("kind: iomux", "kind: mmio")
                                       + "    route: []\n"
                                << "IOMUX_KIND generator.kind";
    QTest::newRow("wrong-bus") << QString(basePrefix).replace("bus: axi4_lite", "bus: apb4")
                                      + "    route: []\n"
                               << "IOMUX_BUS generator.bus";
    QTest::newRow("unknown-generator-key") << basePrefix + "    layout: compact\n    route: []\n"
                                           << "IOMUX_UNSUPPORTED generator.layout";
    QTest::newRow("missing-integration") << QString(
        "generator:\n    kind: iomux\n    bus: axi4_lite\n    pin_count: 2\n    route: []\n")
                                         << "IOMUX_REQUIRED generator.integration";
    QTest::newRow("missing-pad") << QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    pin_count: 2
    integration:
      instance: u_iomux0
      clock: clk_iomux
      reset: rst_iomux_n
      control: iomux_control
    route: []
)") << "IOMUX_REQUIRED generator.integration.pad";
    QTest::newRow("missing-route")
        << QString("generator:\n    kind: iomux\n    bus: axi4_lite\n    pin_count: 2\n%1")
               .arg(integrationBlock())
        << "IOMUX_REQUIRED generator.route";
    QTest::newRow("route-not-sequence") << basePrefix + "    route: {}\n"
                                        << "IOMUX_TYPE generator.route";
    QTest::newRow("pin-out-of-range")
        << withRoute(QString(minimalRoute).replace("pin: 0", "pin: 2"))
        << "IOMUX_RANGE generator.route[0].pin";
    QTest::newRow("slot-out-of-range")
        << withRoute(QString(minimalRoute).replace("slot: 0", "slot: 2"))
        << "IOMUX_RANGE generator.route[0].slot";
    QTest::newRow("duplicate-pin-slot")
        << withRoute(minimalRoute + minimalRoute) << "IOMUX_DUPLICATE generator.route[1]";
    QTest::newRow("duplicate-rx-sink") << withRoute(QString(R"(      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
        input_value: {link: shared_in, bit: 3}
      - pin: 1
        slot: 0
        function: gpio0
        signal: data1
        input_value: {link: shared_in, bit: 3}
)")) << "IOMUX_DUPLICATE generator.route[1].input_value";
    QTest::newRow("no-role-key") << withRoute(QString(R"(      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
)")) << "IOMUX_ROLE generator.route[0]";
    QTest::newRow("empty-function")
        << withRoute(QString(minimalRoute).replace("function: gpio0", "function: \"  \""))
        << "IOMUX_EMPTY generator.route[0].function";
    QTest::newRow("input-value-constant") << withRoute(QString(R"(      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
        input_value: 1
)")) << "IOMUX_TYPE generator.route[0].input_value";
    QTest::newRow("hdl-constant") << withRoute(
        QString(minimalRoute).replace("output_enable: 1", "output_enable: 1'b1"))
                                  << "IOMUX_TYPE generator.route[0].output_enable";
    QTest::newRow("invert-not-bool") << withRoute(
        QString(minimalRoute).replace("output_enable: 1", "output_enable: {link: a, invert: 1}"))
                                     << "IOMUX_TYPE generator.route[0].output_enable.invert";
    QTest::newRow("endpoint-unknown-key")
        << withRoute(QString(minimalRoute)
                         .replace("output_enable: 1", "output_enable: {link: a, bits: \"[0]\"}"))
        << "IOMUX_UNSUPPORTED generator.route[0].output_enable.bits";
    QTest::newRow("endpoint-bad-identifier")
        << withRoute(
               QString(minimalRoute).replace("output_enable: 1", "output_enable: {link: 1bad}"))
        << "IOMUX_IDENTIFIER generator.route[0].output_enable.link";
    QTest::newRow("manual-port") << basePrefix + "    route: []\nport:\n  clk:\n    direction: in\n"
                                 << "IOMUX_MANUAL_SECTION module.port";
    QTest::newRow("manual-parameter")
        << basePrefix + "    route: []\nparameter:\n  WIDTH:\n    value: 8\n"
        << "IOMUX_MANUAL_SECTION module.parameter";
}

void Test::invalidSource()
{
    QFETCH(QString, source);
    QFETCH(QString, expectedError);

    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY(!QSocIomuxGenerator::buildPlan(makeDefinition(source), &plan, &errors));
    QCOMPARE(plan, QSocIomuxPlan());
    bool found = false;
    for (const QString &error : errors) {
        found = found || error.startsWith(expectedError);
    }
    QVERIFY2(found, qPrintable(expectedError + "\nactual:\n" + errors.join('\n')));
}

void Test::regsAndTopEmittersComposePlan()
{
    QSocIomuxPlan plan;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeValidDefinition(), &plan));

    const QString regs = QSocIomuxGenerator::generateRegsVerilog(plan);
    QVERIFY(regs.contains("module iomux0_regs ("));
    QVERIFY(regs.contains("pin_0_select_o"));
    QVERIFY(regs.contains("pin_1_select_o"));

    const QString top = QSocIomuxGenerator::generateTopVerilog(plan);
    QVERIFY(top.contains("module iomux0_core ("));
    QVERIFY(top.contains("module iomux0 ("));
    QVERIFY(top.contains("iomux0_regs u_regs ("));
    QVERIFY(top.contains("iomux0_conn u_conn ("));
    QVERIFY(top.contains("iomux0_core u_core ("));
    QVERIFY(top.contains("/* uart0.tx */"));

    const int     headerEnd = int(top.indexOf(");", top.indexOf("module iomux0 (")));
    const QString header
        = top.mid(top.indexOf("module iomux0 ("), headerEnd - int(top.indexOf("module iomux0 (")));
    QVERIFY(!header.contains("pin_0_select"));
    QVERIFY(header.contains("pad_input_value_i"));
    QVERIFY(header.contains("hs_p0_s0_input_value_o"));

    QCOMPARE(
        QSocIomuxGenerator::generateFileList(plan),
        QString("iomux0_regs.v\niomux0_conn.v\niomux0.v\n"));
}

void Test::projectionMatchesWrapperHeader()
{
    QSocIomuxPlan plan;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeValidDefinition(), &plan));
    const YAML::Node projection = QSocIomuxGenerator::describeModuleYaml(plan);

    const QString top = QSocIomuxGenerator::generateTopVerilog(plan);
    QCOMPARE(wrapperPortSignatures(top, plan.moduleName), projectionPortSignatures(projection));

    QCOMPARE(
        QString::fromStdString(projection["bus"]["control"]["bus"].as<std::string>()),
        QString("axi4_lite"));
    QCOMPARE(
        QString::fromStdString(projection["bus"]["control"]["mode"].as<std::string>()),
        QString("slave"));
    QCOMPARE(int(projection["bus"]["control"]["mapping"].size()), 19);
    QCOMPARE(
        QString::fromStdString(projection["bus"]["control"]["mapping"]["awaddr"].as<std::string>()),
        QString("s_axi_awaddr"));
    QVERIFY(!projection["port"]["pin_0_select_o"]);

    QSocIomuxPlan singlePinPlan;
    QVERIFY(
        QSocIomuxGenerator::buildPlan(makeDefinition(sourceForConfig(1, 2, 32, 8)), &singlePinPlan));
    const YAML::Node singlePinProjection = QSocIomuxGenerator::describeModuleYaml(singlePinPlan);
    const QString    singlePinTop        = QSocIomuxGenerator::generateTopVerilog(singlePinPlan);
    QCOMPARE(
        wrapperPortSignatures(singlePinTop, singlePinPlan.moduleName),
        projectionPortSignatures(singlePinProjection));
    QCOMPARE(
        QString::fromStdString(
            singlePinProjection["port"]["pad_input_value_i"]["type"].as<std::string>()),
        QString("logic"));
}

void Test::integrationNetlistConnectsEverythingOnce()
{
    QSocIomuxPlan plan;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeValidDefinition(), &plan));
    const QString fragment = QSocIomuxGenerator::generateIntegrationNetlist(plan);

    QCOMPARE(fragment.count("module: iomux0"), 1);
    QCOMPARE(fragment.count("clk_i:"), 1);
    QCOMPARE(fragment.count("link: clk_iomux"), 1);
    QCOMPARE(fragment.count("rst_ni:"), 1);
    QCOMPARE(fragment.count("pad_input_value_i:"), 1);
    QCOMPARE(fragment.count("pad_input_enable_o:"), 1);
    QCOMPARE(fragment.count("pad_output_value_o:"), 1);
    QCOMPARE(fragment.count("pad_output_enable_o:"), 1);
    QCOMPARE(fragment.count("hs_p0_s0_input_value_o:"), 1);
    QCOMPARE(fragment.count("bits: \"[0]\""), 3);
    QCOMPARE(fragment.count("port: control"), 1);
    QCOMPARE(fragment.count("iomux_control:"), 1);
    QVERIFY(!fragment.contains("hs_p0_s1_output_enable"));

    const YAML::Node parsed = YAML::Load(fragment.toStdString());
    QVERIFY(parsed["instance"]["u_iomux0"]["module"].IsScalar());
    QCOMPARE(int(parsed["bus"]["iomux_control"].size()), 1);
}

void Test::routingSimulationWhenIverilogIsAvailable()
{
    const QString compiler = QStandardPaths::findExecutable("iverilog");
    const QString runtime  = QStandardPaths::findExecutable("vvp");
    if (compiler.isEmpty() || runtime.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("iverilog and vvp"));
    }

    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeSimulationDefinition(), &plan, &errors),
        qPrintable(errors.join('\n')));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString corePath   = QDir(directory.path()).filePath("iomux0_core.v");
    const QString connPath   = QDir(directory.path()).filePath("iomux0_conn.v");
    const QString benchPath  = QDir(directory.path()).filePath("tb.v");
    const QString outputPath = QDir(directory.path()).filePath("iomux0.out");
    writeTextFile(corePath, QSocIomuxGenerator::generateCoreVerilog(plan));
    writeTextFile(connPath, QSocIomuxGenerator::generateConnVerilog(plan));
    writeTextFile(benchPath, routingTestbench());

    QProcess process;
    process.setWorkingDirectory(directory.path());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(compiler, {"-g2001", "-s", "tb", "-o", outputPath, corePath, connPath, benchPath});
    QVERIFY(process.waitForStarted());
    QVERIFY(process.waitForFinished());
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    const QByteArray compilerOutput = process.readAll();
    QVERIFY2(process.exitCode() == 0, compilerOutput.constData());

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
    QVERIFY2(!simulationOutput.contains("CHECK_FAIL"), simulationOutput.constData());
    QVERIFY2(simulationOutput.contains("TEST_PASS"), simulationOutput.constData());
}

void Test::fiveSlotInvalidSelectorCodesDriveZeroWhenIverilogIsAvailable()
{
    const QString compiler = QStandardPaths::findExecutable("iverilog");
    const QString runtime  = QStandardPaths::findExecutable("vvp");
    if (compiler.isEmpty() || runtime.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("iverilog and vvp"));
    }

    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeDefinition(sourceForConfig(1, 5, 32, 8)), &plan, &errors),
        qPrintable(errors.join('\n')));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString corePath   = QDir(directory.path()).filePath("iomux0_core.v");
    const QString benchPath  = QDir(directory.path()).filePath("tb.v");
    const QString outputPath = QDir(directory.path()).filePath("iomux0.out");
    writeTextFile(corePath, QSocIomuxGenerator::generateCoreVerilog(plan));
    writeTextFile(benchPath, fiveSlotTestbench());

    QProcess process;
    process.setWorkingDirectory(directory.path());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(compiler, {"-g2001", "-s", "tb", "-o", outputPath, corePath, benchPath});
    QVERIFY(process.waitForStarted());
    QVERIFY(process.waitForFinished());
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    const QByteArray compilerOutput = process.readAll();
    QVERIFY2(process.exitCode() == 0, compilerOutput.constData());

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
    QVERIFY2(!simulationOutput.contains("CHECK_FAIL"), simulationOutput.constData());
    QVERIFY2(simulationOutput.contains("TEST_PASS"), simulationOutput.constData());
}

void Test::axiSelectorDrivesTailPinWhenIverilogIsAvailable_data()
{
    QTest::addColumn<quint32>("pinCount");
    QTest::addColumn<quint32>("hsSlots");
    QTest::addColumn<quint32>("dataWidth");

    QTest::newRow("2-2-32") << 2U << 2U << 32U;
    QTest::newRow("185-4-32") << 185U << 4U << 32U;
    QTest::newRow("185-4-64") << 185U << 4U << 64U;
    QTest::newRow("256-8-32") << 256U << 8U << 32U;
    QTest::newRow("256-8-64") << 256U << 8U << 64U;
}

QSocModuleDefinition makeGpioDefinition()
{
    return makeDefinition(QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 12
    pin_count: 2
    hs_slots: 2
    option:
      gpio: true
%1    route:
      - pin: 0
        slot: 0
        function: uart0
        signal: tx
        output_value: {link: uart0_tx}
        output_enable: 1
)")
                              .arg(integrationBlock()));
}

QString gpioTestbench()
{
    return QString(R"(`timescale 1ns/1ps
module tb;
    reg clk = 0, rst_n = 0;
    always #5 clk = ~clk;
    reg  [11:0] awaddr, araddr;
    reg         awvalid = 0, wvalid = 0, arvalid = 0, bready = 1, rready = 1;
    reg  [31:0] wdata;
    reg  [3:0]  wstrb = 4'hf;
    wire        awready, wready, bvalid, arready, rvalid;
    wire [1:0]  bresp, rresp;
    wire [31:0] rdata;
    reg  [1:0]  pad_in = 2'b00;
    wire [1:0]  pad_ie, pad_ov, pad_oe;
    reg         uart0_tx = 0;
    integer     fails = 0;
    reg  [31:0] v;

    iomux0 dut (
        .clk_i(clk), .rst_ni(rst_n),
        .s_axi_awaddr(awaddr), .s_axi_awprot(3'b0), .s_axi_awvalid(awvalid),
        .s_axi_awready(awready), .s_axi_wdata(wdata), .s_axi_wstrb(wstrb),
        .s_axi_wvalid(wvalid), .s_axi_wready(wready), .s_axi_bresp(bresp),
        .s_axi_bvalid(bvalid), .s_axi_bready(bready), .s_axi_araddr(araddr),
        .s_axi_arprot(3'b0), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
        .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid),
        .s_axi_rready(rready), .pad_input_value_i(pad_in),
        .pad_input_enable_o(pad_ie), .pad_output_value_o(pad_ov),
        .pad_output_enable_o(pad_oe), .hs_p0_s0_output_value_i(uart0_tx));

    task wr(input [11:0] a, input [31:0] d);
        begin
            @(posedge clk); awaddr <= a; wdata <= d; awvalid <= 1; wvalid <= 1;
            wait (awready && wready); @(posedge clk); awvalid <= 0; wvalid <= 0;
            wait (bvalid); @(posedge clk);
        end
    endtask

    task rd(input [11:0] a);
        begin
            @(posedge clk); araddr <= a; arvalid <= 1;
            wait (arready); @(posedge clk); arvalid <= 0;
            wait (rvalid); v = rdata; @(posedge clk);
        end
    endtask

    task chk(input [255:0] name, input got, input exp);
        begin
            if (got !== exp) begin
                $display("TEST_FAIL %0s got=%b exp=%b", name, got, exp);
                fails = fails + 1;
            end
        end
    endtask

    initial begin
        repeat (4) @(posedge clk); rst_n = 1; repeat (2) @(posedge clk);
        rd(12'h000);
        if (v !== 32'h00020002) begin
            $display("TEST_FAIL capability %h", v); fails = fails + 1;
        end
        uart0_tx = 1; repeat (2) @(posedge clk);
        chk("fast_ov", pad_ov[0], 1'b1);
        chk("fast_oe", pad_oe[0], 1'b1);
        wr(12'h010, 32'h00000000);
        wr(12'h014, 32'h00000001);
        wr(12'h018, 32'h00000014);
        repeat (2) @(posedge clk);
        chk("reg_ov_low", pad_ov[0], 1'b0);
        chk("reg_oe_high", pad_oe[0], 1'b1);
        wr(12'h010, 32'h00000001);
        repeat (2) @(posedge clk);
        chk("reg_ov_high", pad_ov[0], 1'b1);
        uart0_tx = 0; repeat (2) @(posedge clk);
        chk("fast_ignored", pad_ov[0], 1'b1);
        uart0_tx = 1;
        wr(12'h018, 32'h00000020);
        repeat (2) @(posedge clk);
        chk("oe_from_slot_ov_high", pad_oe[0], 1'b1);
        uart0_tx = 0; repeat (2) @(posedge clk);
        chk("oe_from_slot_ov_low", pad_oe[0], 1'b0);
        wr(12'h018, 32'h00000030);
        repeat (2) @(posedge clk);
        chk("oe_reserved", pad_oe[0], 1'b0);
        pad_in = 2'b10; repeat (4) @(posedge clk);
        rd(12'h008);
        chk("readback_pin1", v[1], 1'b1);
        chk("readback_pin0", v[0], 1'b0);
        if (fails == 0) $display("TEST_PASS");
        $finish;
    end
endmodule
)");
}

QSocModuleDefinition makeInterruptDefinition()
{
    return makeDefinition(QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 12
    pin_count: 2
    hs_slots: 2
    option:
      gpio: true
      interrupt: true
%1    route:
      - pin: 0
        slot: 0
        function: uart0
        signal: tx
        output_value: {link: uart0_tx}
        output_enable: 1
)")
                              .arg(integrationBlock()));
}

QString interruptTestbench()
{
    return QString(R"(`timescale 1ns/1ps
module tb;
    reg clk = 0, rst_n = 0;
    always #5 clk = ~clk;
    reg  [11:0] awaddr, araddr;
    reg         awvalid=0, wvalid=0, arvalid=0, bready=1, rready=1;
    reg  [31:0] wdata;  reg [3:0] wstrb = 4'hf;
    wire awready, wready, bvalid, arready, rvalid;
    wire [1:0] bresp, rresp;  wire [31:0] rdata;
    reg  [1:0] pad_in = 2'b00;
    wire [1:0] pad_ie, pad_ov, pad_oe;
    wire       irq;
    reg        uart0_tx = 0;
    integer    fails = 0;  reg [31:0] v;

    iomux0 dut (.clk_i(clk), .rst_ni(rst_n),
        .s_axi_awaddr(awaddr), .s_axi_awprot(3'b0), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
        .s_axi_wdata(wdata), .s_axi_wstrb(wstrb), .s_axi_wvalid(wvalid), .s_axi_wready(wready),
        .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(bready),
        .s_axi_araddr(araddr), .s_axi_arprot(3'b0), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
        .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(rready),
        .pad_input_value_i(pad_in), .pad_input_enable_o(pad_ie),
        .pad_output_value_o(pad_ov), .pad_output_enable_o(pad_oe),
        .irq_o(irq), .hs_p0_s0_output_value_i(uart0_tx));

    task wr(input [11:0] a, input [31:0] d);
        begin @(posedge clk); awaddr<=a; wdata<=d; awvalid<=1; wvalid<=1;
        wait(awready&&wready); @(posedge clk); awvalid<=0; wvalid<=0;
        wait(bvalid); @(posedge clk); end
    endtask
    task rd(input [11:0] a);
        begin @(posedge clk); araddr<=a; arvalid<=1; wait(arready);
        @(posedge clk); arvalid<=0; wait(rvalid); v=rdata; @(posedge clk); end
    endtask
    task chk(input [255:0] n, input g, input e);
        begin if (g!==e) begin $display("TEST_FAIL %0s got=%b exp=%b", n, g, e); fails=fails+1; end end
    endtask

    initial begin
        repeat (4) @(posedge clk); rst_n = 1; repeat (6) @(posedge clk);

        /* fix 1: pending records the event with every enable still at zero */
        rd(12'h034);
        chk("low_pend_set_without_enable", v[0], 1'b1);
        chk("irq_quiet_without_enable", irq, 1'b0);
        rd(12'h030);
        chk("high_pend_clear", v[0], 1'b0);

        /* fix 2: a clear that lands while the source still fires keeps the bit */
        wr(12'h034, 32'h00000001);
        repeat (2) @(posedge clk);
        rd(12'h034);
        chk("set_beats_clear", v[0], 1'b1);

        /* once the source stops, the same write clears it */
        pad_in = 2'b01;
        repeat (4) @(posedge clk);
        wr(12'h034, 32'h00000001);
        repeat (2) @(posedge clk);
        rd(12'h034);
        chk("clear_when_idle", v[0], 1'b0);

        /* the rising edge was recorded on the way up */
        rd(12'h038);
        chk("rise_pend_set", v[0], 1'b1);

        /* enable gates the line, not the bit */
        wr(12'h028, 32'h00000001);
        repeat (2) @(posedge clk);
        chk("irq_after_enable", irq, 1'b1);
        wr(12'h038, 32'h00000001);
        repeat (2) @(posedge clk);
        chk("irq_after_ack", irq, 1'b0);

        if (fails == 0) $display("TEST_PASS"); else $display("TEST_FAIL count %0d", fails);
        $finish;
    end
endmodule
)");
}

void Test::interruptRecordsEventsWhenIverilogIsAvailable()
{
    const QString compiler = QStandardPaths::findExecutable("iverilog");
    const QString runtime  = QStandardPaths::findExecutable("vvp");
    if (compiler.isEmpty() || runtime.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("iverilog and vvp"));
    }

    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeInterruptDefinition(), &plan, &errors),
        qPrintable(errors.join('\n')));
    QVERIFY(plan.option.interrupt);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString regsPath   = QDir(directory.path()).filePath("iomux0_regs.v");
    const QString connPath   = QDir(directory.path()).filePath("iomux0_conn.v");
    const QString topPath    = QDir(directory.path()).filePath("iomux0.v");
    const QString benchPath  = QDir(directory.path()).filePath("tb.v");
    const QString outputPath = QDir(directory.path()).filePath("iomux0.out");
    writeTextFile(regsPath, QSocIomuxGenerator::generateRegsVerilog(plan));
    writeTextFile(connPath, QSocIomuxGenerator::generateConnVerilog(plan));
    writeTextFile(topPath, QSocIomuxGenerator::generateTopVerilog(plan));
    writeTextFile(benchPath, interruptTestbench());

    QProcess process;
    process.setWorkingDirectory(directory.path());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(
        compiler, {"-g2012", "-s", "tb", "-o", outputPath, regsPath, connPath, topPath, benchPath});
    QVERIFY(process.waitForStarted());
    QVERIFY(process.waitForFinished(120000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    const QByteArray compilerOutput = process.readAll();
    QVERIFY2(process.exitCode() == 0, compilerOutput.constData());

    QProcess simulation;
    simulation.setWorkingDirectory(directory.path());
    simulation.setProcessChannelMode(QProcess::MergedChannels);
    simulation.start(runtime, {outputPath});
    QVERIFY(simulation.waitForStarted());
    QVERIFY(simulation.waitForFinished(120000));
    QCOMPARE(simulation.exitStatus(), QProcess::NormalExit);
    const QByteArray simulationOutput = simulation.readAll();
    QCOMPARE(simulation.exitCode(), 0);
    QVERIFY2(!simulationOutput.contains("TEST_FAIL"), simulationOutput.constData());
    QVERIFY2(simulationOutput.contains("TEST_PASS"), simulationOutput.constData());
}

void Test::registerTakeoverDrivesPadWhenIverilogIsAvailable()
{
    const QString compiler = QStandardPaths::findExecutable("iverilog");
    const QString runtime  = QStandardPaths::findExecutable("vvp");
    if (compiler.isEmpty() || runtime.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("iverilog and vvp"));
    }

    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeGpioDefinition(), &plan, &errors),
        qPrintable(errors.join('\n')));
    QVERIFY(plan.option.gpio);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString regsPath   = QDir(directory.path()).filePath("iomux0_regs.v");
    const QString connPath   = QDir(directory.path()).filePath("iomux0_conn.v");
    const QString topPath    = QDir(directory.path()).filePath("iomux0.v");
    const QString benchPath  = QDir(directory.path()).filePath("tb.v");
    const QString outputPath = QDir(directory.path()).filePath("iomux0.out");
    writeTextFile(regsPath, QSocIomuxGenerator::generateRegsVerilog(plan));
    writeTextFile(connPath, QSocIomuxGenerator::generateConnVerilog(plan));
    writeTextFile(topPath, QSocIomuxGenerator::generateTopVerilog(plan));
    writeTextFile(benchPath, gpioTestbench());

    QProcess process;
    process.setWorkingDirectory(directory.path());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(
        compiler, {"-g2001", "-s", "tb", "-o", outputPath, regsPath, connPath, topPath, benchPath});
    QVERIFY(process.waitForStarted());
    QVERIFY(process.waitForFinished(120000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    const QByteArray compilerOutput = process.readAll();
    QVERIFY2(process.exitCode() == 0, compilerOutput.constData());

    QProcess simulation;
    simulation.setWorkingDirectory(directory.path());
    simulation.setProcessChannelMode(QProcess::MergedChannels);
    simulation.start(runtime, {outputPath});
    QVERIFY(simulation.waitForStarted());
    QVERIFY(simulation.waitForFinished(120000));
    QCOMPARE(simulation.exitStatus(), QProcess::NormalExit);
    const QByteArray simulationOutput = simulation.readAll();
    QCOMPARE(simulation.exitCode(), 0);
    QVERIFY2(!simulationOutput.contains("TEST_FAIL"), simulationOutput.constData());
    QVERIFY2(simulationOutput.contains("TEST_PASS"), simulationOutput.constData());
}

void Test::axiSelectorDrivesTailPinWhenIverilogIsAvailable()
{
    const QString compiler = QStandardPaths::findExecutable("iverilog");
    const QString runtime  = QStandardPaths::findExecutable("vvp");
    if (compiler.isEmpty() || runtime.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("iverilog and vvp"));
    }

    QFETCH(quint32, pinCount);
    QFETCH(quint32, hsSlots);
    QFETCH(quint32, dataWidth);
    const quint32 addressWidth = 8;

    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(
            makeTailDefinition(pinCount, hsSlots, dataWidth, addressWidth), &plan, &errors),
        qPrintable(errors.join('\n')));

    const quint32 pin        = pinCount - 1;
    const quint32 byteCount  = dataWidth / 8;
    const quint32 lanes      = dataWidth / 4;
    const quint32 code       = hsSlots - 1;
    const quint64 selOffset  = quint64(1 + pin / lanes) * byteCount;
    const quint32 laneLsb    = (pin % lanes) * 4;
    const quint32 capability = pinCount | (hsSlots << 16);
    const QString keepStrobe = QString("%1'h%2").arg(byteCount).arg(
        QString::number((quint64(1) << byteCount) - 2, 16));

    QString bench = axiTestbench();
    bench.replace("@AW@", QString::number(addressWidth));
    bench.replace("@DW@", QString::number(dataWidth));
    bench.replace("@SW@", QString::number(byteCount));
    bench.replace("@P@", QString::number(pinCount));
    bench.replace("@PIN@", QString::number(pin));
    bench.replace("@HISLOT@", QString::number(hsSlots - 1));
    bench.replace("@CAP@", QString("%1").arg(capability, 8, 16, QLatin1Char('0')));
    bench.replace("@SEL_OFFSET@", QString::number(selOffset, 16));
    bench.replace("@W0_OFFSET@", QString::number(byteCount, 16));
    bench.replace("@LANE_LSB@", QString::number(laneLsb));
    bench.replace("@CODE@", QString::number(code, 16));
    bench.replace("@KEEP_STRB@", keepStrobe);

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QString regsPath   = QDir(directory.path()).filePath("iomux0_regs.v");
    const QString connPath   = QDir(directory.path()).filePath("iomux0_conn.v");
    const QString topPath    = QDir(directory.path()).filePath("iomux0.v");
    const QString benchPath  = QDir(directory.path()).filePath("tb.v");
    const QString outputPath = QDir(directory.path()).filePath("iomux0.out");
    writeTextFile(regsPath, QSocIomuxGenerator::generateRegsVerilog(plan));
    writeTextFile(connPath, QSocIomuxGenerator::generateConnVerilog(plan));
    writeTextFile(topPath, QSocIomuxGenerator::generateTopVerilog(plan));
    writeTextFile(benchPath, bench);

    QProcess process;
    process.setWorkingDirectory(directory.path());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(
        compiler, {"-g2001", "-s", "tb", "-o", outputPath, regsPath, connPath, topPath, benchPath});
    QVERIFY(process.waitForStarted());
    QVERIFY(process.waitForFinished(120000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    const QByteArray compilerOutput = process.readAll();
    QVERIFY2(process.exitCode() == 0, compilerOutput.constData());

    QProcess simulation;
    simulation.setWorkingDirectory(directory.path());
    simulation.setProcessChannelMode(QProcess::MergedChannels);
    simulation.start(runtime, {outputPath});
    QVERIFY(simulation.waitForStarted());
    QVERIFY(simulation.waitForFinished(120000));
    QCOMPARE(simulation.exitStatus(), QProcess::NormalExit);
    const QByteArray simulationOutput = simulation.readAll();
    QCOMPARE(simulation.exitCode(), 0);
    QVERIFY2(!simulationOutput.contains("TEST_FAIL"), simulationOutput.constData());
    QVERIFY2(simulationOutput.contains("TEST_PASS"), simulationOutput.constData());
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsociomuxgenerator.moc"
