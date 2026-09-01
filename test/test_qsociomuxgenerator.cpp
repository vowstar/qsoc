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

    axi_read(@AW@'h8);
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

    axi_write(@AW@'h8, {@DW@{1'b1}}, {@SW@{1'b1}});
    axi_read(@AW@'h8);
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
            8'h0c: address_is_mapped = 1'b1;
            8'h10: address_is_mapped = 1'b1;
            8'h14: address_is_mapped = 1'b1;
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
                read_register[15:8] = 8'h0;
                read_register[23:16] = 8'h0;
                read_register[31:24] = 8'h2;
            end
            8'h04: begin
                read_register[31:0] = 32'h494f4d58;
            end
            8'h08: begin
                read_register[15:0] = 16'h9;
                read_register[23:16] = 8'h3;
            end
            8'h0c: begin
                read_register[0] = 1'h0;
                read_register[1] = 1'h0;
                read_register[2] = 1'h0;
                read_register[3] = 1'h0;
                read_register[4] = 1'h0;
            end
            8'h10: begin
                read_register[1:0] = mmio_field_0_q;
                read_register[5:4] = mmio_field_1_q;
                read_register[9:8] = mmio_field_2_q;
                read_register[13:12] = mmio_field_3_q;
                read_register[17:16] = mmio_field_4_q;
                read_register[21:20] = mmio_field_5_q;
                read_register[25:24] = mmio_field_6_q;
                read_register[29:28] = mmio_field_7_q;
            end
            8'h14: begin
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
                8'h10: begin
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
                8'h14: begin
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
identity: version 2.0.0, type 0x494f4d58 at offset 0x0 to 0xc
selector registers: 2 at offset 0x10 to 0x14
registers total: 6
aperture: 24 bytes
capability: 0x00030009 at offset 0x8
feature: 0x00000000 at offset 0xc
reset: every selector resets to 0 and selects slot 0
rx: pad input broadcasts to every declared sink regardless of the selector

pin 0 selector word 0 lsb 0 offset 0x10
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
pin 1 selector word 0 lsb 4 offset 0x10
  unused slots: 0, 1, 2
pin 2 selector word 0 lsb 8 offset 0x10
  unused slots: 0, 1, 2
pin 3 selector word 0 lsb 12 offset 0x10
  unused slots: 0, 1, 2
pin 4 selector word 0 lsb 16 offset 0x10
  unused slots: 0, 1, 2
pin 5 selector word 0 lsb 20 offset 0x10
  unused slots: 0, 1, 2
pin 6 selector word 0 lsb 24 offset 0x10
  unused slots: 0, 1, 2
pin 7 selector word 0 lsb 28 offset 0x10
  unused slots: 0, 1, 2
pin 8 selector word 1 lsb 0 offset 0x14
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
            12'h018: address_is_mapped = 1'b1;
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
                read_register[15:8] = 8'h0;
                read_register[23:16] = 8'h0;
                read_register[31:24] = 8'h2;
                read_register[63:32] = 32'h494f4d58;
            end
            12'h008: begin
                read_register[15:0] = 16'h11;
                read_register[23:16] = 8'h4;
                read_register[32] = 1'h0;
                read_register[33] = 1'h0;
                read_register[34] = 1'h0;
                read_register[35] = 1'h0;
                read_register[36] = 1'h0;
            end
            12'h010: begin
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
            12'h018: begin
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
                12'h010: begin
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
                12'h018: begin
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
identity: version 2.0.0, type 0x494f4d58 at offset 0x0 to 0xc
selector registers: 2 at offset 0x10 to 0x18
registers total: 4
aperture: 32 bytes
capability: 0x00040011 at offset 0x8
feature: 0x00000000 at offset 0xc
reset: every selector resets to 0 and selects slot 0
rx: pad input broadcasts to every declared sink regardless of the selector

pin 0 selector word 0 lsb 0 offset 0x10
  slot 0 function gpio0 signal data0
    input_value: link gpio0_in bit 0
    input_enable: constant 1
    output_value: link gpio0_out bit 0
    output_enable: link gpio0_oe bit 0
  unused slots: 1, 2, 3
pin 1 selector word 0 lsb 4 offset 0x10
  unused slots: 0, 1, 2, 3
pin 2 selector word 0 lsb 8 offset 0x10
  unused slots: 0, 1, 2, 3
pin 3 selector word 0 lsb 12 offset 0x10
  unused slots: 0, 1, 2, 3
pin 4 selector word 0 lsb 16 offset 0x10
  unused slots: 0, 1, 2, 3
pin 5 selector word 0 lsb 20 offset 0x10
  unused slots: 0, 1, 2, 3
pin 6 selector word 0 lsb 24 offset 0x10
  unused slots: 0, 1, 2, 3
pin 7 selector word 0 lsb 28 offset 0x10
  unused slots: 0, 1, 2, 3
pin 8 selector word 0 lsb 32 offset 0x10
  unused slots: 0, 1, 2, 3
pin 9 selector word 0 lsb 36 offset 0x10
  unused slots: 0, 1, 2, 3
pin 10 selector word 0 lsb 40 offset 0x10
  unused slots: 0, 1, 2, 3
pin 11 selector word 0 lsb 44 offset 0x10
  unused slots: 0, 1, 2, 3
pin 12 selector word 0 lsb 48 offset 0x10
  unused slots: 0, 1, 2, 3
pin 13 selector word 0 lsb 52 offset 0x10
  unused slots: 0, 1, 2, 3
pin 14 selector word 0 lsb 56 offset 0x10
  unused slots: 0, 1, 2, 3
pin 15 selector word 0 lsb 60 offset 0x10
  unused slots: 0, 1, 2, 3
pin 16 selector word 1 lsb 0 offset 0x18
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
identity: version 2.0.0, type 0x494f4d58 at offset 0x0 to 0xc
selector registers: 2 at offset 0x10 to 0x18
registers total: 4
aperture: 32 bytes
capability: 0x00080020 at offset 0x8
feature: 0x00000000 at offset 0xc
reset: every selector resets to 0 and selects slot 0
rx: pad input broadcasts to every declared sink regardless of the selector

pin 0 selector word 0 lsb 0 offset 0x10
  slot 0 function gpio0 signal data0
    input_value: link gpio0_in bit 0
    input_enable: constant 1
    output_value: link gpio0_out bit 0
    output_enable: link gpio0_oe bit 0
  unused slots: 1, 2, 3, 4, 5, 6, 7
pin 1 selector word 0 lsb 4 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 2 selector word 0 lsb 8 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 3 selector word 0 lsb 12 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 4 selector word 0 lsb 16 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 5 selector word 0 lsb 20 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 6 selector word 0 lsb 24 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 7 selector word 0 lsb 28 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 8 selector word 0 lsb 32 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 9 selector word 0 lsb 36 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 10 selector word 0 lsb 40 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 11 selector word 0 lsb 44 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 12 selector word 0 lsb 48 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 13 selector word 0 lsb 52 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 14 selector word 0 lsb 56 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 15 selector word 0 lsb 60 offset 0x10
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 16 selector word 1 lsb 0 offset 0x18
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 17 selector word 1 lsb 4 offset 0x18
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 18 selector word 1 lsb 8 offset 0x18
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 19 selector word 1 lsb 12 offset 0x18
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 20 selector word 1 lsb 16 offset 0x18
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 21 selector word 1 lsb 20 offset 0x18
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 22 selector word 1 lsb 24 offset 0x18
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 23 selector word 1 lsb 28 offset 0x18
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 24 selector word 1 lsb 32 offset 0x18
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 25 selector word 1 lsb 36 offset 0x18
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 26 selector word 1 lsb 40 offset 0x18
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 27 selector word 1 lsb 44 offset 0x18
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 28 selector word 1 lsb 48 offset 0x18
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 29 selector word 1 lsb 52 offset 0x18
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 30 selector word 1 lsb 56 offset 0x18
  unused slots: 0, 1, 2, 3, 4, 5, 6, 7
pin 31 selector word 1 lsb 60 offset 0x18
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
            12'h008: address_is_mapped = 1'b1;
            12'h00c: address_is_mapped = 1'b1;
            12'h010: address_is_mapped = 1'b1;
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
                read_register[15:8] = 8'h0;
                read_register[23:16] = 8'h0;
                read_register[31:24] = 8'h2;
            end
            12'h004: begin
                read_register[31:0] = 32'h494f4d58;
            end
            12'h008: begin
                read_register[15:0] = 16'h2;
                read_register[23:16] = 8'h2;
            end
            12'h00c: begin
                read_register[0] = 1'h0;
                read_register[1] = 1'h0;
                read_register[2] = 1'h0;
                read_register[3] = 1'h0;
                read_register[4] = 1'h0;
            end
            12'h010: begin
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
                12'h010: begin
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
identity: version 2.0.0, type 0x494f4d58 at offset 0x0 to 0xc
selector registers: 1 at offset 0x10 to 0x10
registers total: 5
aperture: 20 bytes
capability: 0x00020002 at offset 0x8
feature: 0x00000000 at offset 0xc
reset: every selector resets to 0 and selects slot 0
rx: pad input broadcasts to every declared sink regardless of the selector

pin 0 selector word 0 lsb 0 offset 0x10
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
pin 1 selector word 0 lsb 4 offset 0x10
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
    void padCellPortsAreCheckedAgainstTheLibrary();
    void padCellRejectsWhatItLacks_data();
    void padCellRejectsWhatItLacks();
    void padModuleDrivesPinsFromTheTable();
    void padLevelsFollowRequestsWhenIverilogIsAvailable();
    void frozenPadArtifactsMatch();
    void frozenGpioInterruptReportMatches();
    void interruptAloneNeedsNoGpioRegisters();
    void optionRegistersKeepFixedBitPositions();
    void optionLogicLayersOnTheSlotBundle();
    void openDrainExpandsToAnInvertedEnable();
    void padControlNeedsAPadCellWithSomethingToControl();
    void composedRegistersAreAlreadyCanonical();
    void registerPadControlReachesThePadWhenIverilogIsAvailable();
    void inversionAndOverrideReachThePinsWhenIverilogIsAvailable();
    void wovenKeeperCarriesItsStrength();
    void padTablesAreBoundedToEightBitCodes();
    void unreachableModeLandsOnNone();
    void nativeKeeperRowIsSelectedNotWoven();
    void layoutVersionTracksTheRegisterMap();
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
    QCOMPARE(plan.mmio.registers.size(), 5);
    QCOMPARE(plan.mmio.registers.at(0).name, QString("version"));
    QCOMPARE(plan.mmio.registers.at(0).byteOffset, quint64(0));
    QCOMPARE(plan.mmio.registers.at(1).name, QString("type"));
    QCOMPARE(plan.mmio.registers.at(2).name, QString("capability"));
    QCOMPARE(plan.mmio.registers.at(3).name, QString("feature"));
    QCOMPARE(plan.mmio.registers.at(4).name, QString("hs_select_0"));
    QCOMPARE(plan.mmio.registers.at(4).byteOffset, quint64(16));
    QCOMPARE(plan.mmio.registers.at(4).fields.size(), 2);
    QCOMPARE(plan.mmio.registers.at(4).fields.at(0).name, QString("pin_0_select"));
    QCOMPARE(plan.mmio.registers.at(4).fields.at(0).width, 1U);
    QCOMPARE(plan.mmio.registers.at(4).fields.at(0).lsb, 0U);
    QCOMPARE(plan.mmio.registers.at(4).fields.at(1).lsb, 4U);
    QCOMPARE(plan.mmio.registers.at(4).fields.at(1).outputPort, QString("pin_1_select_o"));

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
    QCOMPARE(narrow.mmio.registers[2].name, QString("capability"));
    narrow.mmio.registers[2].fields.append(spare);
    QVERIFY(
        QSocIomuxGenerator::generateReport(narrow).contains("capability: 0x01020002 at offset 0x8"));

    /* A 64-bit beat holds capability and feature together. A field past bit
     * 31 is the feature word, and the report must say so. */
    QSocIomuxPlan wide;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeWide64Definition(), &wide));
    spare.lsb = 37;
    QCOMPARE(wide.mmio.registers[1].name, QString("capability"));
    wide.mmio.registers[1].fields.append(spare);
    const QString wideReport = QSocIomuxGenerator::generateReport(wide);
    QVERIFY(wideReport.contains("capability: 0x00040011 at offset 0x8"));
    QVERIFY(wideReport.contains("feature: 0x00000020 at offset 0xc"));

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
    hostile.mmio.registers[1].fields = {full, unset};
    const QString hostileReport      = QSocIomuxGenerator::generateReport(hostile);
    QVERIFY(hostileReport.contains("capability: 0xffffffff at offset 0x8"));
    QVERIFY(hostileReport.contains("feature: 0xffffffff at offset 0xc"));
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

    QTest::newRow("185-4-32") << 185U << 4U << 32U << 28 << quint64(0x6c) << quint64(0x000400B9);
    QTest::newRow("185-4-64") << 185U << 4U << 64U << 14 << quint64(0x68) << quint64(0x000400B9);
    QTest::newRow("256-8-32") << 256U << 8U << 32U << 36 << quint64(0x8c) << quint64(0x00080100);
    QTest::newRow("256-8-64") << 256U << 8U << 64U << 18 << quint64(0x88) << quint64(0x00080100);
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
    QCOMPARE(plan.mmio.registers.at(dataWidth == 64 ? 2 : 4).byteOffset, quint64(16));
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
        QCOMPARE(plan.mmio.registers.size(), int(words) + (dataWidth == 64 ? 2 : 4));
        QCOMPARE(
            plan.mmio.registers.constLast().byteOffset,
            quint64(16) + quint64(words - 1) * (dataWidth / 8));
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
    QVERIFY(errors.constFirst().contains("aperture needs 144 bytes"));
    QVERIFY(errors.constFirst().contains("minimum address_width is 8"));
    QVERIFY(
        QSocIomuxGenerator::generateCoreVerilog(plan).isEmpty()
        && QSocIomuxGenerator::generateConnVerilog(plan).isEmpty()
        && QSocIomuxGenerator::generateReport(plan).isEmpty());

    QVERIFY(QSocIomuxGenerator::buildPlan(makeDefinition(sourceForConfig(256, 8, 32, 8)), &plan));

    QVERIFY(!QSocIomuxGenerator::buildPlan(
        makeDefinition(sourceForConfig(256, 8, 64, 7)), &plan, &errors));
    QCOMPARE(errors.size(), 1);
    QVERIFY(errors.constFirst().contains("aperture needs 144 bytes"));
    QVERIFY(errors.constFirst().contains("minimum address_width is 8"));
    QVERIFY(QSocIomuxGenerator::buildPlan(makeDefinition(sourceForConfig(256, 8, 64, 8)), &plan));

    QVERIFY(
        !QSocIomuxGenerator::buildPlan(makeDefinition(sourceForConfig(1, 2, 32, 2)), &plan, &errors));
    QVERIFY(errors.constFirst().contains("minimum address_width is 5"));
    QVERIFY(QSocIomuxGenerator::buildPlan(makeDefinition(sourceForConfig(1, 2, 32, 5)), &plan));
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
    QVERIFY(report.contains("capability: 0x00020002 at offset 0x8"));
    QVERIFY(report.contains("reset: every selector resets to 0 and selects slot 0"));
    QVERIFY(report.contains("pin 0 selector word 0 lsb 0 offset 0x10"));
    QVERIFY(report.contains("pin 1 selector word 0 lsb 4 offset 0x10"));
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
        rd(12'h008);
        if (v !== 32'h00020002) begin
            $display("TEST_FAIL capability %h", v); fails = fails + 1;
        end
        uart0_tx = 1; repeat (2) @(posedge clk);
        chk("fast_ov", pad_ov[0], 1'b1);
        chk("fast_oe", pad_oe[0], 1'b1);
        wr(12'h01c, 32'h00000000);
        wr(12'h020, 32'h00000001);
        wr(12'h024, 32'h00000014);
        repeat (2) @(posedge clk);
        chk("reg_ov_low", pad_ov[0], 1'b0);
        chk("reg_oe_high", pad_oe[0], 1'b1);
        wr(12'h01c, 32'h00000001);
        repeat (2) @(posedge clk);
        chk("reg_ov_high", pad_ov[0], 1'b1);
        uart0_tx = 0; repeat (2) @(posedge clk);
        chk("fast_ignored", pad_ov[0], 1'b1);
        uart0_tx = 1;
        wr(12'h024, 32'h00000020);
        repeat (2) @(posedge clk);
        chk("oe_from_slot_ov_high", pad_oe[0], 1'b1);
        uart0_tx = 0; repeat (2) @(posedge clk);
        chk("oe_from_slot_ov_low", pad_oe[0], 1'b0);
        wr(12'h024, 32'h00000030);
        repeat (2) @(posedge clk);
        chk("oe_reserved", pad_oe[0], 1'b0);
        pad_in = 2'b10; repeat (4) @(posedge clk);
        rd(12'h014);
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
        rd(12'h040);
        chk("low_pend_set_without_enable", v[0], 1'b1);
        chk("irq_quiet_without_enable", irq, 1'b0);
        rd(12'h03c);
        chk("high_pend_clear", v[0], 1'b0);

        /* fix 2: a clear that lands while the source still fires keeps the bit */
        wr(12'h040, 32'h00000001);
        repeat (2) @(posedge clk);
        rd(12'h040);
        chk("set_beats_clear", v[0], 1'b1);

        /* once the source stops, the same write clears it */
        pad_in = 2'b01;
        repeat (4) @(posedge clk);
        wr(12'h040, 32'h00000001);
        repeat (2) @(posedge clk);
        rd(12'h040);
        chk("clear_when_idle", v[0], 1'b0);

        /* the rising edge was recorded on the way up */
        rd(12'h044);
        chk("rise_pend_set", v[0], 1'b1);

        /* enable gates the line, not the bit */
        wr(12'h034, 32'h00000001);
        repeat (2) @(posedge clk);
        chk("irq_after_enable", irq, 1'b1);
        wr(12'h044, 32'h00000001);
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

/* A pad cell that takes a pull enable and a pull direction. The port table
 * below is what the module library would answer for it. */
QString padCellBlock()
{
    return QStringLiteral(R"yaml(    pad_cell:
      cell: gpio_pad_ps
      port:
        pad: PAD
        input_value: C
        input_enable: IE
        output_value: I
        output_enable: OE
      pull:
        port: [PE, PS]
        table:
          none: ["0", "x"]
          up: ["1", "1"]
          down: ["1", "0"]
          bus_hold: ["1", "1"]
      drive:
        port: [DS]
        table:
          low: ["0"]
          high: ["1"]
      constraint:
        - name: pull_select_needs_enable
          expr: "!PS || PE"
        - name: ie_oe_exclusive
          expr: "!(IE && OE)"
        - name: pull_holds_across_oe_rise
          property: "!$rose(OE) || $stable(PE)"
)yaml");
}

QMap<QString, QString> padCellPorts()
{
    return {
        {"PAD", "inout"},
        {"C", "out"},
        {"IE", "in"},
        {"I", "in"},
        {"OE", "in"},
        {"PE", "in"},
        {"PS", "in"},
        {"DS", "in"}};
}

QString padIntegrationBlock()
{
    return QStringLiteral(R"(    integration:
      instance: u_iomux0
      clock: clk_iomux
      reset: rst_iomux_n
      control: iomux_control
      pad:
        io: chip_gpio
)");
}

QSocModuleDefinition makePadCellDefinition(const QString &padCell = padCellBlock())
{
    return makeDefinition(QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 12
    pin_count: 2
    hs_slots: 3
%1%2    route:
      - pin: 0
        slot: 0
        function: uart0
        signal: tx
        output_value: {link: uart0_tx}
        output_enable: 1
        pull: up
        drive: high
      - pin: 1
        slot: 0
        function: i2c0
        signal: sda
        input_value: {link: i2c0_sda_in}
        input_enable: 1
        pull: bus_hold
      - pin: 1
        slot: 1
        function: gpio0
        signal: in1
        input_value: {link: gpio0_in1}
        input_enable: 1
        pull: keeper
      - pin: 1
        slot: 2
        function: osc0
        signal: ring
        input_enable: 1
        pull: oscillator
)")
                              .arg(padCell, padIntegrationBlock()));
}

/* The same cell, modelled here so the test owns every line of it. PE gates
 * both resistors and PS picks the one that conducts. */
QString padCellModel()
{
    return QStringLiteral(R"(module gpio_pad_ps(PAD, I, OE, DS, IE, C, PE, PS);
    inout PAD; input I, OE, DS, IE, PE, PS; output C;
    assign C = IE & PAD;
    bufif1 (PAD, I, OE);
    rnmos (PAD, 1'b1, PE & PS);
    rnmos (PAD, 1'b0, PE & ~PS);
endmodule
)");
}

void Test::padCellPortsAreCheckedAgainstTheLibrary()
{
    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makePadCellDefinition(), &plan, &errors),
        qPrintable(errors.join('\n')));
    QVERIFY(plan.integration.padCell.declared());
    QVERIFY(QSocIomuxGenerator::checkPadCellPorts(plan, padCellPorts(), &errors));
    QVERIFY(errors.isEmpty());

    QMap<QString, QString> missing = padCellPorts();
    missing.remove("OE");
    QVERIFY(!QSocIomuxGenerator::checkPadCellPorts(plan, missing, &errors));
    QCOMPARE(errors.size(), 1);
    QVERIFY(errors.first().contains("gpio_pad_ps has no port OE"));

    QMap<QString, QString> wrongWay = padCellPorts();
    wrongWay["C"]                   = "in";
    QVERIFY(!QSocIomuxGenerator::checkPadCellPorts(plan, wrongWay, &errors));
    QCOMPARE(errors.size(), 1);
    QVERIFY(errors.first().contains("port C is in, expected out"));
}

void Test::padCellRejectsWhatItLacks_data()
{
    QTest::addColumn<QString>("edit");
    QTest::addColumn<QString>("replacement");
    QTest::addColumn<QString>("message");

    QTest::newRow("route drives a cell without an output enable")
        << "        output_enable: OE\n"
        << ""
        << "pin 0 slot 0.output_enable: pad cell gpio_pad_ps declares no port for this role";
    QTest::newRow("unknown pull mode")
        << "        pull: up\n"
        << "        pull: strong_up\n"
        << "pin 0 slot 0.pull.mode: pad cell gpio_pad_ps has no pull mode strong_up";
    QTest::newRow("strength on a single row") << "        pull: up\n"
                                              << "        pull: {mode: up, strength: x}\n"
                                              << "pin 0 slot 0.pull.strength: pull mode up of "
                                                 "gpio_pad_ps has a single row, drop strength";
    QTest::newRow("unknown drive level")
        << "        drive: high\n"
        << "        drive: max\n"
        << "pin 0 slot 0.drive: pad cell gpio_pad_ps has no drive level max";
    QTest::newRow("keeper woven from a driver")
        << "        port: [PE, PS]\n"
        << "        port: [PE, PS]\n        kind: driver\n"
        << "pin 1 slot 1.pull.mode: pad cell gpio_pad_ps pulls with its driver, so keeper cannot "
           "be woven";
    QTest::newRow("keeper woven without a down row")
        << "          down: [\"1\", \"0\"]\n"
        << ""
        << "pin 1 slot 1.pull.mode: pad cell gpio_pad_ps needs both up and down rows to weave "
           "keeper";
    QTest::newRow("strength on a woven keeper") << "        pull: keeper\n"
                                                << "        pull: {mode: keeper, strength: x}\n"
                                                << "pin 1 slot 1.pull.strength: pull mode keeper "
                                                   "of gpio_pad_ps has a single row, drop strength";
    QTest::newRow("sinks with no slot enabling the receiver")
        << "        input_enable: 1\n"
        << ""
        << "generator.route.pin 1: has input_value sinks but no slot enables the input buffer";
    QTest::newRow("constraint names a word that is no port")
        << "          expr: \"!PS || PE\"\n"
        << "          expr: \"!PS || PEE\"\n"
        << "constraint[0]: PEE is not a port of gpio_pad_ps nor a SystemVerilog word";
    QTest::newRow("constraint names no port") << "          expr: \"!PS || PE\"\n"
                                              << "          expr: \"1\"\n"
                                              << "constraint[0]: names no port of the pad cell";
    QTest::newRow("constraint with both expr and property")
        << "          expr: \"!PS || PE\"\n"
        << "          expr: \"!PS || PE\"\n          property: \"1\"\n"
        << "constraint[0]: needs exactly one of expr or property";
    QTest::newRow("table row wider than the port list")
        << "          up: [\"1\", \"1\"]\n"
        << "          up: [\"1\", \"1\", \"1\"]\n"
        << "pull.table.up: holds 3 entries but the port list holds 2";
    QTest::newRow("table without a none row") << "          none: [\"0\", \"x\"]\n"
                                              << ""
                                              << "pull.table.none: the table needs a none row";
    QTest::newRow("integration still names the four vectors")
        << "        io: chip_gpio\n"
        << "        io: chip_gpio\n        input_value: pad_in\n"
        << "integration.pad.input_value: is owned by pad_cell, remove it";
}

void Test::padCellRejectsWhatItLacks()
{
    QFETCH(QString, edit);
    QFETCH(QString, replacement);
    QFETCH(QString, message);

    QString source = padCellBlock() + padIntegrationBlock();
    QString body   = QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 12
    pin_count: 2
    hs_slots: 3
%1    route:
      - pin: 0
        slot: 0
        function: uart0
        signal: tx
        output_value: {link: uart0_tx}
        output_enable: 1
        pull: up
        drive: high
      - pin: 1
        slot: 0
        function: i2c0
        signal: sda
        input_value: {link: i2c0_sda_in}
        input_enable: 1
        pull: bus_hold
      - pin: 1
        slot: 1
        function: gpio0
        signal: in1
        input_value: {link: gpio0_in1}
        input_enable: 1
        pull: keeper
)")
                         .arg(source);
    QVERIFY2(body.contains(edit), qPrintable("fixture lost the anchor: " + edit));
    body.replace(edit, replacement);

    QStringList   errors;
    QSocIomuxPlan plan;
    QVERIFY(!QSocIomuxGenerator::buildPlan(makeDefinition(body), &plan, &errors));
    QVERIFY2(errors.join('\n').contains(message), qPrintable(errors.join('\n')));
}

void Test::padModuleDrivesPinsFromTheTable()
{
    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makePadCellDefinition(), &plan, &errors),
        qPrintable(errors.join('\n')));

    const QString pad = QSocIomuxGenerator::generatePadVerilog(plan);
    /* Modes are fixed: 1 up, 2 down, 3 keeper, 4 oscillator, then bus_hold 5. */
    QVERIFY(pad.contains(
        "wire PE_0_w = (pad_mode_eff_0 == 3'd1) ? 1'b1 : "
        "(pad_mode_eff_0 == 3'd2) ? 1'b1 : (pad_mode_eff_0 == 3'd5) ? 1'b1 : 1'b0;"));
    QVERIFY(pad.contains(
        "wire PS_0_w = (pad_mode_eff_0 == 3'd1) ? 1'b1 : "
        "(pad_mode_eff_0 == 3'd2) ? 1'b0 : (pad_mode_eff_0 == 3'd5) ? 1'b1 : 1'b0;"));
    QVERIFY(pad.contains(
        "wire [2:0] pad_mode_eff_1 = (pad_pull_mode_i[5:3] == 3'd3) ? (pad_io[1] ? 3'd1 : 3'd2) : "
        "(pad_pull_mode_i[5:3] == 3'd4) ? (pad_io[1] ? 3'd2 : 3'd1) : pad_pull_mode_i[5:3];"));
    QVERIFY(pad.contains("gpio_pad_ps u_pad_1 ("));
    QVERIFY(pad.contains("    .PE(PE_1_w),"));
    QVERIFY(pad.contains("`ifdef FORMAL"));
    QVERIFY(
        pad.contains("always @(*) assert_pull_select_needs_enable_0: assert(!PS_0_w || PE_0_w);"));
    QVERIFY(pad.contains(
        "always @(*) assume_ie_oe_exclusive_1: "
        "assume(!(pad_input_enable_i[1] && pad_output_enable_i[1]));"));
    QVERIFY(pad.contains(
        "always @(posedge formal_clk) assume_pull_holds_across_oe_rise_0: "
        "assume(!$rose(pad_output_enable_i[0]) || $stable(PE_0_w));"));
    QVERIFY(!pad.contains("always @(*) begin"));

    const QString top = QSocIomuxGenerator::generateTopVerilog(plan);
    QVERIFY(top.contains("    inout  wire [1:0] pad_io,"));
    QVERIFY(top.contains("wire [1:0] pad_output_enable_o;"));
    /* The core resolves the slot codes; the wrapper only carries them. */
    QVERIFY(top.contains("assign pad_pull_mode_o[2:0] = (pin_0_select_i == 2'd0) ? 3'd1 : 3'd0;"));
    QVERIFY(top.contains(
        "assign pad_pull_mode_o[5:3] = (pin_1_select_i == 2'd0) ? 3'd5 : "
        "(pin_1_select_i == 2'd1) ? 3'd3 : (pin_1_select_i == 2'd2) ? 3'd4 : 3'd0;"));
    QVERIFY(top.contains("wire [5:0] pad_pull_mode_w;"));
    QVERIFY(top.contains("    .pad_pull_mode_i(pad_pull_mode_w)"));
    QVERIFY(top.contains("    .pad_pull_mode_o(pad_pull_mode_w)"));
    QVERIFY(top.contains("iomux0_pad u_pad ("));

    QVERIFY(QSocIomuxGenerator::generateFileList(plan).endsWith("iomux0_pad.v\n"));
    const QString netlist = QSocIomuxGenerator::generateIntegrationNetlist(plan);
    QVERIFY(netlist.contains("      pad_io:\n        uplink: chip_gpio"));
    QVERIFY(!netlist.contains("pad_input_value_i"));
    QVERIFY(
        QSocIomuxGenerator::generateReport(plan).contains(
            "pad cell: gpio_pad_ps, pull modes 4, drive levels 2, constraints 3"));
}

QString padLevelTestbench()
{
    return QStringLiteral(R"(`timescale 1ns/1ps
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
    wire [1:0]  pad;
    reg         uart0_tx = 0;
    wire        sda_in, gp_in1;
    reg         drv_en = 0, drv_val = 0;
    assign pad[1] = drv_en ? drv_val : 1'bz;
    integer     fails = 0;

    iomux0 dut (
        .clk_i(clk), .rst_ni(rst_n),
        .s_axi_awaddr(awaddr), .s_axi_awprot(3'b0), .s_axi_awvalid(awvalid),
        .s_axi_awready(awready), .s_axi_wdata(wdata), .s_axi_wstrb(wstrb),
        .s_axi_wvalid(wvalid), .s_axi_wready(wready), .s_axi_bresp(bresp),
        .s_axi_bvalid(bvalid), .s_axi_bready(bready), .s_axi_araddr(araddr),
        .s_axi_arprot(3'b0), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
        .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid),
        .s_axi_rready(rready), .pad_io(pad), .hs_p0_s0_output_value_i(uart0_tx),
        .hs_p1_s0_input_value_o(sda_in), .hs_p1_s1_input_value_o(gp_in1));

    task wr(input [11:0] a, input [31:0] d);
        begin
            @(posedge clk); awaddr <= a; wdata <= d; awvalid <= 1; wvalid <= 1;
            wait (awready && wready); @(posedge clk); awvalid <= 0; wvalid <= 0;
            wait (bvalid); @(posedge clk);
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
        chk("pin0_driven_low", pad[0], 1'b0);
        uart0_tx = 1; #1 chk("pin0_driven_high", pad[0], 1'b1);
        chk("pin0_drive_high_selected", dut.u_pad.DS_0_w, 1'b1);
        chk("pin1_bus_hold_row_pulls_up", pad[1], 1'b1);
        chk("pin1_receiver_sees_it", sda_in, 1'b1);
        /* keeper: hold the pad while the mode changes, a floating node in a
         * zero-delay simulation has no level for the loop to keep */
        drv_en = 1; drv_val = 1;
        wr(12'h010, 32'h0000_0010);
        repeat (2) @(posedge clk);
        drv_en = 0; #2 chk("keeper_holds_high", pad[1], 1'b1);
        drv_en = 1; drv_val = 0; #2 drv_en = 0; #2 chk("keeper_holds_low", pad[1], 1'b0);
        chk("keeper_receiver_sees_low", gp_in1, 1'b0);
        drv_en = 1; drv_val = 1;
        wr(12'h010, 32'h0000_0020);
        repeat (2) @(posedge clk);
        chk("osc_pad_high_asks_pull_down_PE", dut.u_pad.PE_1_w, 1'b1);
        chk("osc_pad_high_asks_pull_down_PS", dut.u_pad.PS_1_w, 1'b0);
        drv_val = 0; #2
        chk("osc_pad_low_asks_pull_up_PE", dut.u_pad.PE_1_w, 1'b1);
        chk("osc_pad_low_asks_pull_up_PS", dut.u_pad.PS_1_w, 1'b1);
        if (fails == 0) $display("TEST_PASS");
        $finish;
    end
endmodule
)");
}

void Test::padLevelsFollowRequestsWhenIverilogIsAvailable()
{
    const QString compiler = QStandardPaths::findExecutable("iverilog");
    const QString runtime  = QStandardPaths::findExecutable("vvp");
    if (compiler.isEmpty() || runtime.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("iverilog and vvp"));
    }

    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makePadCellDefinition(), &plan, &errors),
        qPrintable(errors.join('\n')));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir    dir(directory.path());
    const QString outputPath = dir.filePath("iomux0.out");
    writeTextFile(dir.filePath("iomux0_regs.v"), QSocIomuxGenerator::generateRegsVerilog(plan));
    writeTextFile(dir.filePath("iomux0_conn.v"), QSocIomuxGenerator::generateConnVerilog(plan));
    writeTextFile(dir.filePath("iomux0.v"), QSocIomuxGenerator::generateTopVerilog(plan));
    writeTextFile(dir.filePath("iomux0_pad.v"), QSocIomuxGenerator::generatePadVerilog(plan));
    writeTextFile(dir.filePath("gpio_pad_ps.v"), padCellModel());
    writeTextFile(dir.filePath("tb.v"), padLevelTestbench());

    QProcess process;
    process.setWorkingDirectory(directory.path());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(
        compiler,
        {"-g2012",
         "-s",
         "tb",
         "-o",
         outputPath,
         "iomux0_regs.v",
         "iomux0_conn.v",
         "iomux0.v",
         "iomux0_pad.v",
         "gpio_pad_ps.v",
         "tb.v"});
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

QString frozenPadVerilog()
{
    return QStringLiteral(R"gold(// Generated by QSoC. Do not edit.
module iomux0_pad (
    inout  wire [1:0] pad_io,
    output wire [1:0] pad_input_value_o,
    input  wire [1:0] pad_input_enable_i,
    input  wire [1:0] pad_output_value_i,
    input  wire [1:0] pad_output_enable_i,
    input  wire [5:0] pad_pull_mode_i,
    input  wire [1:0] pad_drive_select_i
);

wire [2:0] pad_mode_eff_0 = (pad_pull_mode_i[2:0] == 3'd3) ? (pad_io[0] ? 3'd1 : 3'd2) : (pad_pull_mode_i[2:0] == 3'd4) ? (pad_io[0] ? 3'd2 : 3'd1) : pad_pull_mode_i[2:0];
wire PE_0_w = (pad_mode_eff_0 == 3'd1) ? 1'b1 : (pad_mode_eff_0 == 3'd2) ? 1'b1 : (pad_mode_eff_0 == 3'd5) ? 1'b1 : 1'b0;
wire PS_0_w = (pad_mode_eff_0 == 3'd1) ? 1'b1 : (pad_mode_eff_0 == 3'd2) ? 1'b0 : (pad_mode_eff_0 == 3'd5) ? 1'b1 : 1'b0;
wire DS_0_w = (pad_drive_select_i[0:0] == 1'd1) ? 1'b1 : 1'b0;
gpio_pad_ps u_pad_0 (
    .PAD(pad_io[0]),
    .C(pad_input_value_o[0]),
    .IE(pad_input_enable_i[0]),
    .I(pad_output_value_i[0]),
    .OE(pad_output_enable_i[0]),
    .PE(PE_0_w),
    .PS(PS_0_w),
    .DS(DS_0_w)
);

wire [2:0] pad_mode_eff_1 = (pad_pull_mode_i[5:3] == 3'd3) ? (pad_io[1] ? 3'd1 : 3'd2) : (pad_pull_mode_i[5:3] == 3'd4) ? (pad_io[1] ? 3'd2 : 3'd1) : pad_pull_mode_i[5:3];
wire PE_1_w = (pad_mode_eff_1 == 3'd1) ? 1'b1 : (pad_mode_eff_1 == 3'd2) ? 1'b1 : (pad_mode_eff_1 == 3'd5) ? 1'b1 : 1'b0;
wire PS_1_w = (pad_mode_eff_1 == 3'd1) ? 1'b1 : (pad_mode_eff_1 == 3'd2) ? 1'b0 : (pad_mode_eff_1 == 3'd5) ? 1'b1 : 1'b0;
wire DS_1_w = (pad_drive_select_i[1:1] == 1'd1) ? 1'b1 : 1'b0;
gpio_pad_ps u_pad_1 (
    .PAD(pad_io[1]),
    .C(pad_input_value_o[1]),
    .IE(pad_input_enable_i[1]),
    .I(pad_output_value_i[1]),
    .OE(pad_output_enable_i[1]),
    .PE(PE_1_w),
    .PS(PS_1_w),
    .DS(DS_1_w)
);

`ifdef FORMAL
(* gclk *) wire formal_clk;
always @(*) assert_pull_select_needs_enable_0: assert(!PS_0_w || PE_0_w);
always @(*) assert_pull_select_needs_enable_1: assert(!PS_1_w || PE_1_w);
always @(*) assume_ie_oe_exclusive_0: assume(!(pad_input_enable_i[0] && pad_output_enable_i[0]));
always @(*) assume_ie_oe_exclusive_1: assume(!(pad_input_enable_i[1] && pad_output_enable_i[1]));
always @(posedge formal_clk) assume_pull_holds_across_oe_rise_0: assume(!$rose(pad_output_enable_i[0]) || $stable(PE_0_w));
always @(posedge formal_clk) assume_pull_holds_across_oe_rise_1: assume(!$rose(pad_output_enable_i[1]) || $stable(PE_1_w));
`endif

endmodule
)gold");
}

QString frozenPadReport()
{
    return QStringLiteral(R"gold(IOMUX route report for iomux0
pin_count: 2
hs_slots: 3
data_width: 32
address_width: 12
selector: 2-bit field in a fixed 4-bit lane per pin
identity: version 2.0.0, type 0x494f4d58 at offset 0x0 to 0xc
selector registers: 1 at offset 0x10 to 0x10
registers total: 5
aperture: 20 bytes
capability: 0x00030002 at offset 0x8
feature: 0x00000000 at offset 0xc
reset: every selector resets to 0 and selects slot 0
rx: pad input broadcasts to every declared sink regardless of the selector
pad cell: gpio_pad_ps, pull modes 4, drive levels 2, constraints 3
pull modes: 0 none, 1 up, 2 down, 3 keeper, 4 oscillator, 5 bus_hold
drive codes: 0 low, 1 high

pin 0 selector word 0 lsb 0 offset 0x10
  slot 0 function uart0 signal tx
    input_value: no sink
    input_enable: constant 0
    output_value: link uart0_tx
    output_enable: constant 1
    pull: up
    drive: high
  unused slots: 1, 2
pin 1 selector word 0 lsb 4 offset 0x10
  slot 0 function i2c0 signal sda
    input_value: link i2c0_sda_in
    input_enable: constant 1
    output_value: constant 0
    output_enable: constant 0
    pull: bus_hold
  slot 1 function gpio0 signal in1
    input_value: link gpio0_in1
    input_enable: constant 1
    output_value: constant 0
    output_enable: constant 0
    pull: keeper
  slot 2 function osc0 signal ring
    input_value: no sink
    input_enable: constant 1
    output_value: constant 0
    output_enable: constant 0
    pull: oscillator
  unused slots: none

undeclared pin/slot pairs drive a zero tx bundle
)gold");
}

QString frozenGpioInterruptReport()
{
    return QStringLiteral(R"gold(IOMUX route report for iomux0
pin_count: 2
hs_slots: 2
data_width: 32
address_width: 12
selector: 1-bit field in a fixed 4-bit lane per pin
identity: version 2.0.0, type 0x494f4d58 at offset 0x0 to 0xc
selector registers: 1 at offset 0x10 to 0x10
gpio registers: 4 at offset 0x14 to 0x20
source control registers: 2 at offset 0x24 to 0x28
interrupt registers: 8 at offset 0x2c to 0x48
interrupt lines: 1, one per 32 pins
registers total: 19
aperture: 76 bytes
capability: 0x00020002 at offset 0x8
feature: 0x00000003 at offset 0xc
reset: every selector resets to 0 and selects slot 0
rx: pad input broadcasts to every declared sink regardless of the selector

pin 0 selector word 0 lsb 0 offset 0x10
  slot 0 function uart0 signal tx
    input_value: no sink
    input_enable: constant 0
    output_value: link uart0_tx
    output_enable: constant 1
  unused slots: 1
pin 1 selector word 0 lsb 4 offset 0x10
  unused slots: 0, 1

undeclared pin/slot pairs drive a zero tx bundle
)gold");
}

void Test::frozenPadArtifactsMatch()
{
    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makePadCellDefinition(), &plan, &errors),
        qPrintable(errors.join('\n')));
    compareText(QSocIomuxGenerator::generatePadVerilog(plan), frozenPadVerilog());
    compareText(QSocIomuxGenerator::generateReport(plan), frozenPadReport());
}

void Test::frozenGpioInterruptReportMatches()
{
    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeInterruptDefinition(), &plan, &errors),
        qPrintable(errors.join('\n')));
    compareText(QSocIomuxGenerator::generateReport(plan), frozenGpioInterruptReport());
}

void Test::interruptAloneNeedsNoGpioRegisters()
{
    /* The one option combination nothing else exercises. It is the only path
     * that reaches the synchroniser through interrupt alone while every gpio
     * branch stays closed. */
    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(
            makeDefinition(QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 12
    pin_count: 2
    hs_slots: 2
    option:
      interrupt: true
%1    route:
      - pin: 0
        slot: 0
        function: uart0
        signal: tx
        output_value: {link: uart0_tx}
        output_enable: 1
)")
                               .arg(integrationBlock())),
            &plan,
            &errors),
        qPrintable(errors.join('\n')));
    QVERIFY(plan.option.interrupt);
    QVERIFY(!plan.option.gpio);

    const QString report = QSocIomuxGenerator::generateReport(plan);
    QVERIFY(report.contains("interrupt registers: 8 at offset 0x14 to 0x30"));
    QVERIFY(!report.contains("gpio registers"));
    QVERIFY(report.contains("registers total: 13"));

    const QString top = QSocIomuxGenerator::generateTopVerilog(plan);
    QVERIFY(top.contains("pad_input_sync_q <= pad_input_meta_q;"));
    QVERIFY(top.contains("pad_input_prev_q <= pad_input_sync_q;"));
    QVERIFY(!top.contains("pin_0_input_value_i"));
    QVERIFY(!top.contains("pin_0_output_value_src_w"));
    QVERIFY(top.contains("assign irq_o = "));

    const QString regs = QSocIomuxGenerator::generateRegsVerilog(plan);
    QVERIFY(regs.contains("pin_0_rise_detect_i"));
    QVERIFY(!regs.contains("pin_0_output_enable_src_o"));
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
    const quint64 selOffset  = 16 + quint64(pin / lanes) * byteCount;
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
    bench.replace("@W0_OFFSET@", QString::number(16, 16));
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

QString allOptionBlock()
{
    return QStringLiteral(R"(    option:
      gpio: true
      interrupt: true
      pad_control: true
      invert: true
      rx_override: true
)");
}

/* The pad cell fixture with every option on. Pin count and slot count match
 * makePadCellDefinition so the per-pin layout can be checked by hand. */
QSocModuleDefinition makeAllOptionDefinition()
{
    return makeDefinition(QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 12
    pin_count: 2
    hs_slots: 3
%1%2%3    route:
      - pin: 0
        slot: 0
        function: uart0
        signal: tx
        output_value: {link: uart0_tx}
        output_enable: 1
        pull: up
        drive: high
      - pin: 1
        slot: 0
        function: i2c0
        signal: sda
        input_value: {link: i2c0_sda_in}
        input_enable: 1
        output_value: {link: i2c0_sda_out, open_drain: true}
        pull: bus_hold
      - pin: 1
        slot: 1
        function: gpio0
        signal: in1
        input_value: {link: gpio0_in1}
        input_enable: 1
        pull: keeper
)")
                              .arg(allOptionBlock(), padCellBlock(), padIntegrationBlock()));
}

const QSocMmioRegisterPlan *findRegister(const QSocMmioPlan &mmio, const QString &name)
{
    for (const QSocMmioRegisterPlan &reg : mmio.registers) {
        if (reg.name == name) {
            return &reg;
        }
    }
    return nullptr;
}

QString frozenAllOptionReport()
{
    return QStringLiteral(R"gold(IOMUX route report for iomux0
pin_count: 2
hs_slots: 3
data_width: 32
address_width: 12
selector: 2-bit field in a fixed 4-bit lane per pin
identity: version 2.0.0, type 0x494f4d58 at offset 0x0 to 0xc
selector registers: 1 at offset 0x10 to 0x10
gpio registers: 4 at offset 0x14 to 0x20
source control registers: 2 at offset 0x24 to 0x28
pad control registers: 2 at offset 0x2c to 0x30
invert registers: 6 at offset 0x34 to 0x48
rx override registers: 3 at offset 0x4c to 0x54
interrupt registers: 8 at offset 0x58 to 0x74
interrupt lines: 1, one per 32 pins
registers total: 30
aperture: 120 bytes
capability: 0x00030002 at offset 0x8
feature: 0x0000001f at offset 0xc
reset: every selector resets to 0 and selects slot 0
rx: pad input broadcasts to every declared sink regardless of the selector
pad cell: gpio_pad_ps, pull modes 4, drive levels 2, constraints 3
pull modes: 0 none, 1 up, 2 down, 3 keeper, 4 oscillator, 5 bus_hold
drive codes: 0 low, 1 high

pin 0 selector word 0 lsb 0 offset 0x10
  slot 0 function uart0 signal tx
    input_value: no sink
    input_enable: constant 0
    output_value: link uart0_tx
    output_enable: constant 1
    pull: up
    drive: high
  unused slots: 1, 2
pin 1 selector word 0 lsb 4 offset 0x10
  slot 0 function i2c0 signal sda
    input_value: link i2c0_sda_in
    input_enable: constant 1
    output_value: constant 0
    output_enable: link i2c0_sda_out invert
    pull: bus_hold
  slot 1 function gpio0 signal in1
    input_value: link gpio0_in1
    input_enable: constant 1
    output_value: constant 0
    output_enable: constant 0
    pull: keeper
  unused slots: 2

undeclared pin/slot pairs drive a zero tx bundle
)gold");
}

void Test::optionRegistersKeepFixedBitPositions()
{
    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeAllOptionDefinition(), &plan, &errors),
        qPrintable(errors.join('\n')));
    QVERIFY(plan.option.padControl && plan.option.invert && plan.option.rxOverride);

    /* Composition order: gpio banks, source words, pad words, inversion banks,
     * receive override banks, interrupt banks. */
    const QStringList expectedNames
        = {"version",
           "type",
           "capability",
           "feature",
           "hs_select_0",
           "input_value_0",
           "input_enable_0",
           "output_value_0",
           "output_enable_0",
           "pin_src_ctrl_0",
           "pin_src_ctrl_1",
           "pin_pad_ctrl_0",
           "pin_pad_ctrl_1",
           "input_enable_inv_0",
           "output_value_inv_0",
           "output_enable_inv_0",
           "rx_inv_s0_0",
           "rx_inv_s1_0",
           "rx_inv_s2_0",
           "rx_value_s0_0",
           "rx_value_s1_0",
           "rx_value_s2_0",
           "high_int_en_0",
           "low_int_en_0",
           "rise_int_en_0",
           "fall_int_en_0",
           "high_int_pend_0",
           "low_int_pend_0",
           "rise_int_pend_0",
           "fall_int_pend_0"};
    QStringList names;
    for (const QSocMmioRegisterPlan &reg : plan.mmio.registers) {
        names.append(reg.name);
        QCOMPARE(reg.byteOffset, quint64(names.size() - 1) * 4);
    }
    QCOMPARE(names, expectedNames);

    /* A missing field reads as -1 so a lookup failure is a comparison, not a crash. */
    const auto lsbOf = [&](const char *reg, const char *field) -> qint64 {
        const QSocMmioFieldPlan *found = findField(plan.mmio, reg, field);
        return found ? qint64(found->lsb) : -1;
    };
    const auto widthOf = [&](const char *reg, const char *field) -> qint64 {
        const QSocMmioFieldPlan *found = findField(plan.mmio, reg, field);
        return found ? qint64(found->width) : -1;
    };
    const auto portOf = [&](const char *reg, const char *field) -> QString {
        const QSocMmioFieldPlan *found = findField(plan.mmio, reg, field);
        return found ? found->outputPort : QString();
    };

    /* The source word keeps every field at its fixed position. */
    const QSocMmioRegisterPlan *source = findRegister(plan.mmio, "pin_src_ctrl_1");
    QVERIFY(source != nullptr);
    QCOMPARE(source->fields.size(), 8);
    QCOMPARE(lsbOf("pin_src_ctrl_1", "input_enable_src"), 0);
    QCOMPARE(lsbOf("pin_src_ctrl_1", "output_value_src"), 2);
    QCOMPARE(lsbOf("pin_src_ctrl_1", "output_enable_src"), 4);
    QCOMPARE(lsbOf("pin_src_ctrl_1", "pull_src"), 6);
    QCOMPARE(lsbOf("pin_src_ctrl_1", "drive_src"), 7);
    QCOMPARE(lsbOf("pin_src_ctrl_1", "rx_src_s0"), 8);
    QCOMPARE(lsbOf("pin_src_ctrl_1", "rx_src_s2"), 10);
    QCOMPARE(portOf("pin_src_ctrl_1", "pull_src"), QString("pin_1_pull_src_o"));

    /* The pad word: the mode covers five fixed values plus bus_hold, the
     * single-row directions need no strength select, drive sits at 24. */
    QCOMPARE(lsbOf("pin_pad_ctrl_0", "pull_mode"), 0);
    QCOMPARE(widthOf("pin_pad_ctrl_0", "pull_mode"), 3);
    QCOMPARE(lsbOf("pin_pad_ctrl_0", "up_sel"), -1);
    QCOMPARE(lsbOf("pin_pad_ctrl_0", "down_sel"), -1);
    QCOMPARE(lsbOf("pin_pad_ctrl_0", "drive"), 24);
    QCOMPARE(widthOf("pin_pad_ctrl_0", "drive"), 1);
    const QSocMmioFieldPlan *pull = findField(plan.mmio, "pin_pad_ctrl_0", "pull_mode");
    QVERIFY(pull != nullptr);
    QCOMPARE(pull->resetValue.value(), quint64(0));

    QCOMPARE(lsbOf("rx_inv_s2_0", "pin_1_rx_inv_s2"), 1);
    QCOMPARE(portOf("rx_value_s1_0", "pin_0_rx_value_s1"), QString("pin_0_rx_value_s1_o"));

    compareText(QSocIomuxGenerator::generateReport(plan), frozenAllOptionReport());
}

void Test::optionLogicLayersOnTheSlotBundle()
{
    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeAllOptionDefinition(), &plan, &errors),
        qPrintable(errors.join('\n')));
    const QString core = QSocIomuxGenerator::generateCoreVerilog(plan);

    /* Inversion wraps the source mux output, so it inverts whichever source
     * the pin uses; the cross taps inside the mux stay uninverted. */
    QVERIFY(core.contains(
        "assign pad_input_enable_o[0] = (pin_0_input_enable_src_i ? "
        "pin_0_input_enable_i : tx_bundle_0[2]) ^ pin_0_input_enable_inv_i;"));
    QVERIFY(core.contains(
        "assign pad_output_value_o[0] = (pad_output_value_0) ^ pin_0_output_value_inv_i;"));
    QVERIFY(core.contains("        2'd2: pad_output_enable_0 = tx_bundle_0[1];"));
    /* The register takes over a code only through its source bit. */
    QVERIFY(core.contains(
        "assign pad_pull_mode_o[2:0] = pin_0_pull_src_i ? pin_0_pull_mode_i : "
        "(pin_0_select_i == 2'd0) ? 3'd1 : 3'd0;"));
    QVERIFY(core.contains(
        "assign pad_pull_mode_o[5:3] = pin_1_pull_src_i ? pin_1_pull_mode_i : "
        "(pin_1_select_i == 2'd0) ? 3'd5 : (pin_1_select_i == 2'd1) ? 3'd3 : 3'd0;"));
    QVERIFY(core.contains(
        "assign pad_drive_select_o[0:0] = pin_0_drive_src_i ? pin_0_drive_i : "
        "(pin_0_select_i == 2'd0) ? 1'd1 : 1'd0;"));
    /* Receive: substitute first, invert second. */
    QVERIFY(core.contains(
        "assign rx_input_value_o[3] = (pin_1_rx_src_s1_i ? pin_1_rx_value_s1_i "
        ": pad_input_value_i[1]) ^ pin_1_rx_inv_s1_i;"));

    const QString top = QSocIomuxGenerator::generateTopVerilog(plan);
    QVERIFY(top.contains("    .pin_1_rx_value_s2_o(pin_1_rx_value_s2_w)"));
    QVERIFY(top.contains("    .pin_1_rx_value_s2_i(pin_1_rx_value_s2_w)"));
    QVERIFY(top.contains("wire [2:0] pin_0_pull_mode_w;"));
    QVERIFY(!top.contains("pad_keep"));
}

void Test::openDrainExpandsToAnInvertedEnable()
{
    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeAllOptionDefinition(), &plan, &errors),
        qPrintable(errors.join('\n')));
    const QSocIomuxRoutePlan &sda = plan.routes.at(1);
    QCOMPARE(sda.function, QString("i2c0"));
    QCOMPARE(sda.outputValue.constant.value(), quint8(0));
    QVERIFY(sda.outputValue.link.isEmpty());
    QCOMPARE(sda.outputEnable.link, QString("i2c0_sda_out"));
    QVERIFY(sda.outputEnable.invert);
    QVERIFY(
        QSocIomuxGenerator::generateConnVerilog(plan).contains(
            "assign tx_output_enable_o[1] = hs_p1_s0_output_enable_i ^ 1'b1;"));

    /* An inverted open-drain link drives low on a one. */
    QString inverted = QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 12
    pin_count: 1
    hs_slots: 2
%1    route:
      - pin: 0
        slot: 0
        function: i2c0
        signal: sda
        output_value: {link: sda_n, open_drain: true, invert: true}
)")
                           .arg(integrationBlock());
    QVERIFY(QSocIomuxGenerator::buildPlan(makeDefinition(inverted), &plan, &errors));
    QVERIFY(!plan.routes.at(0).outputEnable.invert);

    QString withEnable = inverted;
    withEnable.replace(
        "output_value: {link: sda_n, open_drain: true, invert: true}\n",
        "output_value: {link: sda_n, open_drain: true}\n        output_enable: 1\n");
    QVERIFY(!QSocIomuxGenerator::buildPlan(makeDefinition(withEnable), &plan, &errors));
    QCOMPARE(
        errors,
        QStringList{"IOMUX_ROLE generator.route[0].output_enable: is derived from an open_drain "
                    "output_value, drop it"});

    QString onEnable = inverted;
    onEnable.replace(
        "output_value: {link: sda_n, open_drain: true, invert: true}\n",
        "output_enable: {link: sda_n, open_drain: true}\n");
    QVERIFY(!QSocIomuxGenerator::buildPlan(makeDefinition(onEnable), &plan, &errors));
    QCOMPARE(
        errors,
        QStringList{"IOMUX_ROLE generator.route[0].output_enable.open_drain: applies to "
                    "output_value only"});
}

void Test::padControlNeedsAPadCellWithSomethingToControl()
{
    QSocIomuxPlan plan;
    QStringList   errors;
    const QString noCell = QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 12
    pin_count: 1
    hs_slots: 2
    option:
      pad_control: true
%1    route:
      - pin: 0
        slot: 0
        function: uart0
        signal: tx
        output_value: {link: uart0_tx}
)")
                               .arg(integrationBlock());
    QVERIFY(!QSocIomuxGenerator::buildPlan(makeDefinition(noCell), &plan, &errors));
    QCOMPARE(
        errors,
        QStringList{"IOMUX_OPTION generator.option.pad_control: needs a pad_cell declaration"});

    QString         bareCell = padCellBlock();
    const qsizetype pullAt   = bareCell.indexOf("      pull:");
    QVERIFY(pullAt > 0);
    bareCell.truncate(pullAt);
    const QString source = QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 12
    pin_count: 1
    hs_slots: 2
    option:
      pad_control: true
%1%2    route:
      - pin: 0
        slot: 0
        function: uart0
        signal: tx
        output_value: {link: uart0_tx}
        output_enable: 1
)")
                               .arg(bareCell, padIntegrationBlock());
    QVERIFY(!QSocIomuxGenerator::buildPlan(makeDefinition(source), &plan, &errors));
    QCOMPARE(
        errors,
        QStringList{"IOMUX_OPTION generator.option.pad_control: pad cell gpio_pad_ps has no "
                    "pull or drive table to control"});
}

void Test::composedRegistersAreAlreadyCanonical()
{
    /* buildPlan runs the MMIO invariant gate over what composeMmio produced.
     * No source can make that gate fail, so the gate is proven here from the
     * other side: the largest layouts of every option come out canonical. */
    for (const QString &options :
         {QString(),
          QStringLiteral("    option: {gpio: true, interrupt: true}\n"),
          QStringLiteral("    option: {invert: true, rx_override: true}\n"),
          QStringLiteral(
              "    option: {gpio: true, interrupt: true, invert: true, rx_override: true}\n")}) {
        for (const quint32 dataWidth : {32U, 64U}) {
            const QString source = QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: %1
    address_width: 32
    pin_count: 256
    hs_slots: 8
%2%3    route: []
)")
                                       .arg(dataWidth)
                                       .arg(options, integrationBlock());
            QSocIomuxPlan plan;
            QStringList   errors;
            QVERIFY2(
                QSocIomuxGenerator::buildPlan(makeDefinition(source), &plan, &errors),
                qPrintable(errors.join('\n')));
            QSocMmioPlan copy = plan.mmio;
            QVERIFY(QSocMmioGenerator::canonicalizePlan(&copy, &errors));
            QVERIFY(errors.isEmpty());
            QVERIFY(copy == plan.mmio);
            QVERIFY(!QSocIomuxGenerator::generateReport(plan).isEmpty());
        }
    }
    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY(QSocIomuxGenerator::buildPlan(makeAllOptionDefinition(), &plan, &errors));
    QSocMmioPlan copy = plan.mmio;
    QVERIFY(QSocMmioGenerator::canonicalizePlan(&copy, &errors));
    QVERIFY(copy == plan.mmio);
}

QString padControlTestbench()
{
    return QStringLiteral(R"(`timescale 1ns/1ps
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
    wire [1:0]  pad;
    reg         uart0_tx = 0, sda_out = 1;
    wire        sda_in, gp_in1;
    reg         drv_en = 0, drv_val = 0;
    assign pad[1] = drv_en ? drv_val : 1'bz;
    integer     fails = 0;

    iomux0 dut (
        .clk_i(clk), .rst_ni(rst_n),
        .s_axi_awaddr(awaddr), .s_axi_awprot(3'b0), .s_axi_awvalid(awvalid),
        .s_axi_awready(awready), .s_axi_wdata(wdata), .s_axi_wstrb(wstrb),
        .s_axi_wvalid(wvalid), .s_axi_wready(wready), .s_axi_bresp(bresp),
        .s_axi_bvalid(bvalid), .s_axi_bready(bready), .s_axi_araddr(araddr),
        .s_axi_arprot(3'b0), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
        .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid),
        .s_axi_rready(rready), .pad_io(pad), .hs_p0_s0_output_value_i(uart0_tx),
        .hs_p1_s0_input_value_o(sda_in), .hs_p1_s0_output_enable_i(sda_out),
        .hs_p1_s1_input_value_o(gp_in1));

    task wr(input [11:0] a, input [31:0] d);
        begin
            @(posedge clk); awaddr <= a; wdata <= d; awvalid <= 1; wvalid <= 1;
            wait (awready && wready); @(posedge clk); awvalid <= 0; wvalid <= 0;
            wait (bvalid); @(posedge clk);
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
        /* slot 0 of pin 0 asks for up and high drive */
        chk("slot_pull_up_PE", dut.u_pad.PE_0_w, 1'b1);
        chk("slot_pull_up_PS", dut.u_pad.PS_0_w, 1'b1);
        chk("slot_drive_high", dut.u_pad.DS_0_w, 1'b1);
        /* the register alone changes nothing: mode 2 is down */
        wr(12'h01c, 32'h0000_0002);
        repeat (2) @(posedge clk);
        chk("reg_idle_PS", dut.u_pad.PS_0_w, 1'b1);
        chk("reg_idle_DS", dut.u_pad.DS_0_w, 1'b1);
        /* each source bit hands over its own code and nothing else */
        wr(12'h014, 32'h0000_0040);
        repeat (2) @(posedge clk);
        chk("reg_pull_down_PE", dut.u_pad.PE_0_w, 1'b1);
        chk("reg_pull_down_PS", dut.u_pad.PS_0_w, 1'b0);
        chk("drive_still_slot", dut.u_pad.DS_0_w, 1'b1);
        wr(12'h014, 32'h0000_00c0);
        repeat (2) @(posedge clk);
        chk("reg_drive_low", dut.u_pad.DS_0_w, 1'b0);
        /* mode 0 is none, drive 1 is high */
        wr(12'h01c, 32'h0100_0000);
        repeat (2) @(posedge clk);
        chk("reg_pull_none_PE", dut.u_pad.PE_0_w, 1'b0);
        chk("reg_drive_high", dut.u_pad.DS_0_w, 1'b1);
        /* releasing the source bits returns the slot request */
        wr(12'h014, 32'h0000_0000);
        repeat (2) @(posedge clk);
        chk("slot_again_PS", dut.u_pad.PS_0_w, 1'b1);
        /* mode 3 reaches the weave through the register too */
        drv_en = 1; drv_val = 1;
        wr(12'h020, 32'h0000_0003);
        wr(12'h018, 32'h0000_0040);
        repeat (2) @(posedge clk);
        drv_en = 0; #2 chk("reg_keeper_holds_high", pad[1], 1'b1);
        drv_en = 1; drv_val = 0; #2 drv_en = 0; #2 chk("reg_keeper_holds_low", pad[1], 1'b0);
        /* open drain from the route: sda_out low drives the pad low */
        wr(12'h018, 32'h0000_0000);
        drv_en = 1; drv_val = 1; #2 drv_en = 0;
        sda_out = 0; #2 chk("open_drain_drives_low", pad[1], 1'b0);
        sda_out = 1; #2 chk("open_drain_releases", dut.pad_output_enable_o[1], 1'b0);
        if (fails == 0) $display("TEST_PASS");
        $finish;
    end
endmodule
)");
}

void Test::registerPadControlReachesThePadWhenIverilogIsAvailable()
{
    const QString compiler = QStandardPaths::findExecutable("iverilog");
    const QString runtime  = QStandardPaths::findExecutable("vvp");
    if (compiler.isEmpty() || runtime.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("iverilog and vvp"));
    }

    QSocIomuxPlan plan;
    QStringList   errors;
    QString       source = QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 12
    pin_count: 2
    hs_slots: 3
    option:
      pad_control: true
%1%2    route:
      - pin: 0
        slot: 0
        function: uart0
        signal: tx
        output_value: {link: uart0_tx}
        output_enable: 1
        pull: up
        drive: high
      - pin: 1
        slot: 0
        function: i2c0
        signal: sda
        input_value: {link: i2c0_sda_in}
        input_enable: 1
        output_value: {link: i2c0_sda_out, open_drain: true}
        pull: bus_hold
      - pin: 1
        slot: 1
        function: gpio0
        signal: in1
        input_value: {link: gpio0_in1}
        input_enable: 1
        pull: keeper
)")
                               .arg(padCellBlock(), padIntegrationBlock());
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeDefinition(source), &plan, &errors),
        qPrintable(errors.join('\n')));

    const auto offsetOf = [&](const char *name) -> qint64 {
        const QSocMmioRegisterPlan *found = findRegister(plan.mmio, name);
        return found ? qint64(found->byteOffset) : -1;
    };
    QCOMPARE(offsetOf("pin_src_ctrl_0"), qint64(0x14));
    QCOMPARE(offsetOf("pin_pad_ctrl_0"), qint64(0x1c));
    QCOMPARE(offsetOf("pin_pad_ctrl_1"), qint64(0x20));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir    dir(directory.path());
    const QString outputPath = dir.filePath("iomux0.out");
    writeTextFile(dir.filePath("iomux0_regs.v"), QSocIomuxGenerator::generateRegsVerilog(plan));
    writeTextFile(dir.filePath("iomux0_conn.v"), QSocIomuxGenerator::generateConnVerilog(plan));
    writeTextFile(dir.filePath("iomux0.v"), QSocIomuxGenerator::generateTopVerilog(plan));
    writeTextFile(dir.filePath("iomux0_pad.v"), QSocIomuxGenerator::generatePadVerilog(plan));
    writeTextFile(dir.filePath("gpio_pad_ps.v"), padCellModel());
    writeTextFile(dir.filePath("tb.v"), padControlTestbench());

    QProcess process;
    process.setWorkingDirectory(directory.path());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(
        compiler,
        {"-g2012",
         "-s",
         "tb",
         "-o",
         outputPath,
         "iomux0_regs.v",
         "iomux0_conn.v",
         "iomux0.v",
         "iomux0_pad.v",
         "gpio_pad_ps.v",
         "tb.v"});
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
    QVERIFY2(!simulationOutput.contains("TEST_FAIL"), simulationOutput.constData());
    QVERIFY2(simulationOutput.contains("TEST_PASS"), simulationOutput.constData());
}

QSocModuleDefinition makeInvertOverrideDefinition()
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
      invert: true
      rx_override: true
%1    route:
      - pin: 0
        slot: 0
        function: uart0
        signal: tx
        output_value: {link: uart0_tx}
        output_enable: 1
      - pin: 1
        slot: 0
        function: uart0
        signal: rx
        input_value: {link: uart0_rx}
        input_enable: 1
      - pin: 1
        slot: 1
        function: spi0
        signal: miso
        input_value: {link: spi0_miso}
        input_enable: 1
)")
                              .arg(integrationBlock()));
}

QString invertOverrideTestbench()
{
    return QStringLiteral(R"(`timescale 1ns/1ps
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
    wire        uart0_rx, spi0_miso;
    integer     fails = 0;

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
        .pad_output_enable_o(pad_oe), .hs_p0_s0_output_value_i(uart0_tx),
        .hs_p1_s0_input_value_o(uart0_rx), .hs_p1_s1_input_value_o(spi0_miso));

    task wr(input [11:0] a, input [31:0] d);
        begin
            @(posedge clk); awaddr <= a; wdata <= d; awvalid <= 1; wvalid <= 1;
            wait (awready && wready); @(posedge clk); awvalid <= 0; wvalid <= 0;
            wait (bvalid); @(posedge clk);
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
        uart0_tx = 1; #1;
        chk("plain_ov", pad_ov[0], 1'b1);
        chk("plain_oe", pad_oe[0], 1'b1);
        /* output value inversion flips the slot value */
        wr(12'h030, 32'h0000_0001);
        repeat (2) @(posedge clk);
        chk("inv_ov", pad_ov[0], 1'b0);
        uart0_tx = 0; #1 chk("inv_ov_low", pad_ov[0], 1'b1);
        wr(12'h030, 32'h0000_0000);
        /* open drain at run time: enable follows the inverted slot value */
        wr(12'h024, 32'h0000_0020);
        wr(12'h034, 32'h0000_0001);
        repeat (2) @(posedge clk);
        uart0_tx = 0; #1;
        chk("od_low_drives", pad_oe[0], 1'b1);
        chk("od_low_value", pad_ov[0], 1'b0);
        uart0_tx = 1; #1 chk("od_high_releases", pad_oe[0], 1'b0);
        /* cross taps: value from the slot enable (1) and from the slot input enable (0) */
        wr(12'h034, 32'h0000_0000);
        wr(12'h024, 32'h0000_000c);
        repeat (2) @(posedge clk);
        uart0_tx = 0; #1 chk("ov_from_slot_oe", pad_ov[0], 1'b1);
        wr(12'h024, 32'h0000_0008);
        repeat (2) @(posedge clk);
        uart0_tx = 1; #1 chk("ov_from_slot_ie", pad_ov[0], 1'b0);
        wr(12'h024, 32'h0000_0000);
        /* input enable inversion turns the constant zero of an idle pin on */
        chk("ie_pin0_off", pad_ie[0], 1'b0);
        wr(12'h02c, 32'h0000_0001);
        repeat (2) @(posedge clk);
        chk("ie_pin0_inverted_on", pad_ie[0], 1'b1);
        /* receive: broadcast, then a per-slot override, then a per-slot invert */
        pad_in = 2'b10; #1;
        chk("rx_s0_broadcast", uart0_rx, 1'b1);
        chk("rx_s1_broadcast", spi0_miso, 1'b1);
        wr(12'h044, 32'h0000_0000);
        repeat (2) @(posedge clk);
        chk("rx_value_idle_without_src", spi0_miso, 1'b1);
        wr(12'h028, 32'h0000_0200);
        repeat (2) @(posedge clk);
        chk("rx_s1_overridden_low", spi0_miso, 1'b0);
        chk("rx_s0_untouched", uart0_rx, 1'b1);
        wr(12'h044, 32'h0000_0002);
        repeat (2) @(posedge clk);
        chk("rx_s1_overridden_high", spi0_miso, 1'b1);
        pad_in = 2'b00; #1 chk("rx_s1_ignores_pad", spi0_miso, 1'b1);
        wr(12'h038, 32'h0000_0002);
        repeat (2) @(posedge clk);
        chk("rx_s0_inverted", uart0_rx, 1'b1);
        wr(12'h03c, 32'h0000_0002);
        repeat (2) @(posedge clk);
        chk("rx_s1_override_then_invert", spi0_miso, 1'b0);
        if (fails == 0) $display("TEST_PASS");
        $finish;
    end
endmodule
)");
}

void Test::inversionAndOverrideReachThePinsWhenIverilogIsAvailable()
{
    const QString compiler = QStandardPaths::findExecutable("iverilog");
    const QString runtime  = QStandardPaths::findExecutable("vvp");
    if (compiler.isEmpty() || runtime.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("iverilog and vvp"));
    }

    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeInvertOverrideDefinition(), &plan, &errors),
        qPrintable(errors.join('\n')));

    const auto offsetOf = [&](const char *name) -> qint64 {
        const QSocMmioRegisterPlan *found = findRegister(plan.mmio, name);
        return found ? qint64(found->byteOffset) : -1;
    };
    QCOMPARE(offsetOf("pin_src_ctrl_0"), qint64(0x24));
    QCOMPARE(offsetOf("input_enable_inv_0"), qint64(0x2c));
    QCOMPARE(offsetOf("output_value_inv_0"), qint64(0x30));
    QCOMPARE(offsetOf("output_enable_inv_0"), qint64(0x34));
    QCOMPARE(offsetOf("rx_inv_s0_0"), qint64(0x38));
    QCOMPARE(offsetOf("rx_inv_s1_0"), qint64(0x3c));
    QCOMPARE(offsetOf("rx_value_s1_0"), qint64(0x44));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir    dir(directory.path());
    const QString outputPath = dir.filePath("iomux0.out");
    writeTextFile(dir.filePath("iomux0_regs.v"), QSocIomuxGenerator::generateRegsVerilog(plan));
    writeTextFile(dir.filePath("iomux0_conn.v"), QSocIomuxGenerator::generateConnVerilog(plan));
    writeTextFile(dir.filePath("iomux0.v"), QSocIomuxGenerator::generateTopVerilog(plan));
    writeTextFile(dir.filePath("tb.v"), invertOverrideTestbench());

    QProcess process;
    process.setWorkingDirectory(directory.path());
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(
        compiler,
        {"-g2001", "-s", "tb", "-o", outputPath, "iomux0_regs.v", "iomux0_conn.v", "iomux0.v", "tb.v"});
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
    QVERIFY2(!simulationOutput.contains("TEST_FAIL"), simulationOutput.constData());
    QVERIFY2(simulationOutput.contains("TEST_PASS"), simulationOutput.constData());
}

void Test::wovenKeeperCarriesItsStrength()
{
    QString       padCell = padCellBlock();
    const QString single  = "          up: [\"1\", \"1\"]\n";
    QVERIFY(padCell.contains(single));
    padCell.replace(single, "          up: {weak: [\"1\", \"1\"], strong: [\"1\", \"1\"]}\n");
    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(
            makeDefinition(QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 12
    pin_count: 2
    hs_slots: 2
    option:
      pad_control: true
%1%2    route:
      - pin: 0
        slot: 0
        function: uart0
        signal: tx
        output_value: {link: uart0_tx}
        output_enable: 1
        pull: {mode: up, strength: strong}
      - pin: 1
        slot: 1
        function: gpio0
        signal: in1
        input_value: {link: gpio0_in1}
        input_enable: 1
        pull: {mode: keeper, strength: strong}
)")
                               .arg(padCell, padIntegrationBlock())),
            &plan,
            &errors),
        qPrintable(errors.join('\n')));
    const QSocPadEncoding encoding = QSocIomuxGenerator::padEncoding(plan.integration.padCell);
    QCOMPARE(encoding.modeWidth, 3U);
    QCOMPARE(encoding.upSelWidth, 1U);
    QCOMPARE(encoding.downSelWidth, 0U);
    /* Only up is graded, so only up_sel exists, at bit 8. */
    const QSocMmioFieldPlan *upSel = findField(plan.mmio, "pin_pad_ctrl_0", "up_sel");
    QVERIFY(upSel != nullptr);
    QCOMPARE(upSel->lsb, 8U);
    QVERIFY(findField(plan.mmio, "pin_pad_ctrl_0", "down_sel") == nullptr);
    const QString pad = QSocIomuxGenerator::generatePadVerilog(plan);
    QVERIFY(pad.contains(
        "wire PE_1_w = (pad_mode_eff_1 == 3'd1 && pad_up_sel_i[1:1] == 1'd0) ? 1'b1 : "
        "(pad_mode_eff_1 == 3'd1 && pad_up_sel_i[1:1] == 1'd1) ? 1'b1 : "
        "(pad_mode_eff_1 == 3'd2) ? 1'b1 : (pad_mode_eff_1 == 3'd5) ? 1'b1 : 1'b0;"));
    /* The keeper route carries strong into the up select; the down side has
     * a single row and no select. */
    const QString top = QSocIomuxGenerator::generateTopVerilog(plan);
    QVERIFY(top.contains(
        "assign pad_pull_mode_o[5:3] = pin_1_pull_src_i ? pin_1_pull_mode_i : "
        "(pin_1_select_i == 1'd1) ? 3'd3 : 3'd0;"));
    QVERIFY(top.contains(
        "assign pad_up_sel_o[1:1] = pin_1_pull_src_i ? pin_1_up_sel_i : "
        "(pin_1_select_i == 1'd1) ? 1'd1 : 1'd0;"));
    QVERIFY(top.contains(
        "assign pad_up_sel_o[0:0] = pin_0_pull_src_i ? pin_0_up_sel_i : "
        "(pin_0_select_i == 1'd0) ? 1'd1 : 1'd0;"));
    const QString report = QSocIomuxGenerator::generateReport(plan);
    QVERIFY(report.contains("pull modes: 0 none, 1 up, 2 down, 3 keeper, 4 oscillator, 5 bus_hold"));
    QVERIFY(report.contains("up strengths: 0 weak, 1 strong"));
    QVERIFY(!report.contains("down strengths"));
}

void Test::padTablesAreBoundedToEightBitCodes()
{
    QString rows;
    for (int index = 0; index < 257; ++index) {
        rows += QString("            r%1: [\"1\", \"1\"]\n").arg(index);
    }
    QString       padCell = padCellBlock();
    const QString single  = "          up: [\"1\", \"1\"]\n";
    QVERIFY(padCell.contains(single));
    padCell.replace(single, "          up:\n" + rows);
    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY(!QSocIomuxGenerator::buildPlan(makePadCellDefinition(padCell), &plan, &errors));
    QVERIFY2(
        errors.contains(
            "IOMUX_RANGE generator.pad_cell.pull.table: at most 256 strength rows per "
            "direction and 251 named modes"),
        qPrintable(errors.join('\n')));
}

void Test::unreachableModeLandsOnNone()
{
    /* Only a register can present a mode with no row. The none row of this
     * cell drives the enable high, which is what such a mode must produce. */
    QString       padCell = padCellBlock();
    const QString none    = "          none: [\"0\", \"x\"]\n";
    QVERIFY(padCell.contains(none));
    padCell.replace(none, "          none: [\"1\", \"x\"]\n");
    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makePadCellDefinition(padCell), &plan, &errors),
        qPrintable(errors.join('\n')));
    const QString pad = QSocIomuxGenerator::generatePadVerilog(plan);
    QVERIFY(pad.contains(
        "wire PE_0_w = (pad_mode_eff_0 == 3'd1) ? 1'b1 : (pad_mode_eff_0 == 3'd2) "
        "? 1'b1 : (pad_mode_eff_0 == 3'd5) ? 1'b1 : 1'b1;"));
    QVERIFY(pad.contains(
        "wire PS_0_w = (pad_mode_eff_0 == 3'd1) ? 1'b1 : (pad_mode_eff_0 == 3'd2) "
        "? 1'b0 : (pad_mode_eff_0 == 3'd5) ? 1'b1 : 1'b0;"));
}

void Test::nativeKeeperRowIsSelectedNotWoven()
{
    /* A cell with its own keeper pin state: mode 3 selects that row and no
     * loop is woven, so oscillator is not reachable either. */
    QString       padCell = padCellBlock();
    const QString down    = "          down: [\"1\", \"0\"]\n";
    QVERIFY(padCell.contains(down));
    padCell.replace(down, down + "          keeper: [\"0\", \"1\"]\n");
    QSocIomuxPlan plan;
    QStringList   errors;
    const QString source = QString(R"(generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 12
    pin_count: 1
    hs_slots: 2
%1%2    route:
      - pin: 0
        slot: 0
        function: gpio0
        signal: in0
        input_value: {link: gpio0_in0}
        input_enable: 1
        pull: keeper
)")
                               .arg(padCell, padIntegrationBlock());
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeDefinition(source), &plan, &errors),
        qPrintable(errors.join('\n')));
    const QSocPadEncoding encoding = QSocIomuxGenerator::padEncoding(plan.integration.padCell);
    QVERIFY(!encoding.weaves);
    QVERIFY(encoding.supports(QSocPadEncoding::Keeper));
    QVERIFY(!encoding.supports(QSocPadEncoding::Oscillator));
    const QString pad = QSocIomuxGenerator::generatePadVerilog(plan);
    QVERIFY(!pad.contains("pad_mode_eff_"));
    QVERIFY(pad.contains(
        "wire PS_0_w = (pad_pull_mode_i[2:0] == 3'd1) ? 1'b1 : "
        "(pad_pull_mode_i[2:0] == 3'd2) ? 1'b0 : (pad_pull_mode_i[2:0] == 3'd3) "
        "? 1'b1 : (pad_pull_mode_i[2:0] == 3'd5) ? 1'b1 : 1'b0;"));
    QVERIFY(
        QSocIomuxGenerator::generateTopVerilog(plan).contains(
            "assign pad_pull_mode_o[2:0] = (pin_0_select_i == 1'd0) ? 3'd3 : 3'd0;"));
    QVERIFY(
        QSocIomuxGenerator::generateReport(plan).contains(
            "pull modes: 0 none, 1 up, 2 down, 3 keeper, 5 bus_hold"));

    QString oscillator = source;
    oscillator.replace("        pull: keeper\n", "        pull: oscillator\n");
    QVERIFY(!QSocIomuxGenerator::buildPlan(makeDefinition(oscillator), &plan, &errors));
    QCOMPARE(
        errors,
        QStringList{"IOMUX_CAPABILITY generator.route.pin 0 slot 0.pull.mode: pad cell gpio_pad_ps "
                    "names its own keeper or oscillator row, so oscillator is not woven"});

    QString graded = source;
    graded.replace("        pull: keeper\n", "        pull: {mode: keeper, strength: x}\n");
    QVERIFY(!QSocIomuxGenerator::buildPlan(makeDefinition(graded), &plan, &errors));
    QCOMPARE(
        errors,
        QStringList{"IOMUX_CAPABILITY generator.route.pin 0 slot 0.pull.strength: pull mode keeper "
                    "of gpio_pad_ps has a single row, drop strength"});
}

void Test::layoutVersionTracksTheRegisterMap()
{
    /* The version word is a promise about offsets. This list is that promise
     * for the layout with every option on. Any change here that moves an
     * existing line is a major step, a new line at the end is a minor step,
     * and either one changes QSocIomuxGenerator::layoutVersion() first. */
    const QSocIomuxLayoutVersion layout = QSocIomuxGenerator::layoutVersion();
    QCOMPARE(layout.major, 2U);
    QCOMPARE(layout.minor, 0U);
    QCOMPARE(layout.patch, 0U);

    QSocIomuxPlan plan;
    QStringList   errors;
    QVERIFY2(
        QSocIomuxGenerator::buildPlan(makeAllOptionDefinition(), &plan, &errors),
        qPrintable(errors.join('\n')));
    QStringList map;
    for (const QSocMmioRegisterPlan &reg : plan.mmio.registers) {
        QStringList fields;
        for (const QSocMmioFieldPlan &field : reg.fields) {
            fields.append(QString("%1@%2").arg(field.name).arg(field.lsb));
        }
        map.append(QString("0x%1 %2: %3")
                       .arg(reg.byteOffset, 2, 16, QLatin1Char('0'))
                       .arg(reg.name, fields.join(' ')));
    }
    const QStringList frozen = {
        "0x00 version: patch@8 minor@16 major@24",
        "0x04 type: type_id@0",
        "0x08 capability: pin_count@0 hs_slots@16",
        "0x0c feature: gpio@0 interrupt@1 pad_control@2 invert@3 rx_override@4",
        "0x10 hs_select_0: pin_0_select@0 pin_1_select@4",
        "0x14 input_value_0: pin_0_input_value@0 pin_1_input_value@1",
        "0x18 input_enable_0: pin_0_input_enable@0 pin_1_input_enable@1",
        "0x1c output_value_0: pin_0_output_value@0 pin_1_output_value@1",
        "0x20 output_enable_0: pin_0_output_enable@0 pin_1_output_enable@1",
        "0x24 pin_src_ctrl_0: input_enable_src@0 output_value_src@2 output_enable_src@4 "
        "pull_src@6 drive_src@7 rx_src_s0@8 rx_src_s1@9 rx_src_s2@10",
        "0x28 pin_src_ctrl_1: input_enable_src@0 output_value_src@2 output_enable_src@4 "
        "pull_src@6 drive_src@7 rx_src_s0@8 rx_src_s1@9 rx_src_s2@10",
        "0x2c pin_pad_ctrl_0: pull_mode@0 drive@24",
        "0x30 pin_pad_ctrl_1: pull_mode@0 drive@24",
        "0x34 input_enable_inv_0: pin_0_input_enable_inv@0 pin_1_input_enable_inv@1",
        "0x38 output_value_inv_0: pin_0_output_value_inv@0 pin_1_output_value_inv@1",
        "0x3c output_enable_inv_0: pin_0_output_enable_inv@0 pin_1_output_enable_inv@1",
        "0x40 rx_inv_s0_0: pin_0_rx_inv_s0@0 pin_1_rx_inv_s0@1",
        "0x44 rx_inv_s1_0: pin_0_rx_inv_s1@0 pin_1_rx_inv_s1@1",
        "0x48 rx_inv_s2_0: pin_0_rx_inv_s2@0 pin_1_rx_inv_s2@1",
        "0x4c rx_value_s0_0: pin_0_rx_value_s0@0 pin_1_rx_value_s0@1",
        "0x50 rx_value_s1_0: pin_0_rx_value_s1@0 pin_1_rx_value_s1@1",
        "0x54 rx_value_s2_0: pin_0_rx_value_s2@0 pin_1_rx_value_s2@1",
        "0x58 high_int_en_0: pin_0_high_int_en@0 pin_1_high_int_en@1",
        "0x5c low_int_en_0: pin_0_low_int_en@0 pin_1_low_int_en@1",
        "0x60 rise_int_en_0: pin_0_rise_int_en@0 pin_1_rise_int_en@1",
        "0x64 fall_int_en_0: pin_0_fall_int_en@0 pin_1_fall_int_en@1",
        "0x68 high_int_pend_0: pin_0_high_int_pend@0 pin_1_high_int_pend@1",
        "0x6c low_int_pend_0: pin_0_low_int_pend@0 pin_1_low_int_pend@1",
        "0x70 rise_int_pend_0: pin_0_rise_int_pend@0 pin_1_rise_int_pend@1",
        "0x74 fall_int_pend_0: pin_0_fall_int_pend@0 pin_1_fall_int_pend@1",
    };
    QCOMPARE(map, frozen);
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsociomuxgenerator.moc"
