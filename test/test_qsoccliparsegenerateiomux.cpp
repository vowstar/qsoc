// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/qslangdriver.h"
#include "common/qsocconsole.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

namespace {

const QString busLibrary = R"(axi4_lite:
  port:
    awaddr:
      master: {direction: out, width: 8}
      slave: {direction: in, width: 8}
    awprot:
      master: {direction: out, width: 3}
      slave: {direction: in, width: 3}
    awvalid:
      master: {direction: out, width: 1}
      slave: {direction: in, width: 1}
    awready:
      master: {direction: in, width: 1}
      slave: {direction: out, width: 1}
    wdata:
      master: {direction: out, width: 32}
      slave: {direction: in, width: 32}
    wstrb:
      master: {direction: out, width: 4}
      slave: {direction: in, width: 4}
    wvalid:
      master: {direction: out, width: 1}
      slave: {direction: in, width: 1}
    wready:
      master: {direction: in, width: 1}
      slave: {direction: out, width: 1}
    bresp:
      master: {direction: in, width: 2}
      slave: {direction: out, width: 2}
    bvalid:
      master: {direction: in, width: 1}
      slave: {direction: out, width: 1}
    bready:
      master: {direction: out, width: 1}
      slave: {direction: in, width: 1}
    araddr:
      master: {direction: out, width: 8}
      slave: {direction: in, width: 8}
    arprot:
      master: {direction: out, width: 3}
      slave: {direction: in, width: 3}
    arvalid:
      master: {direction: out, width: 1}
      slave: {direction: in, width: 1}
    arready:
      master: {direction: in, width: 1}
      slave: {direction: out, width: 1}
    rdata:
      master: {direction: in, width: 32}
      slave: {direction: out, width: 32}
    rresp:
      master: {direction: in, width: 2}
      slave: {direction: out, width: 2}
    rvalid:
      master: {direction: in, width: 1}
      slave: {direction: out, width: 1}
    rready:
      master: {direction: out, width: 1}
      slave: {direction: in, width: 1}
)";

const QString moduleLibrary = R"(periph_stub:
  port:
    clk: {type: logic, direction: input}
    rst_n: {type: logic, direction: input}
    gpio_out: {type: "logic[3:0]", direction: output}
    gpio_oe: {type: "logic[3:0]", direction: output}
    gpio_in: {type: "logic[3:0]", direction: input}
    uart_tx: {type: logic, direction: output}
    m_awaddr: {type: "logic[7:0]", direction: output}
    m_awprot: {type: "logic[2:0]", direction: output}
    m_awvalid: {type: logic, direction: output}
    m_awready: {type: logic, direction: input}
    m_wdata: {type: "logic[31:0]", direction: output}
    m_wstrb: {type: "logic[3:0]", direction: output}
    m_wvalid: {type: logic, direction: output}
    m_wready: {type: logic, direction: input}
    m_bresp: {type: "logic[1:0]", direction: input}
    m_bvalid: {type: logic, direction: input}
    m_bready: {type: logic, direction: output}
    m_araddr: {type: "logic[7:0]", direction: output}
    m_arprot: {type: "logic[2:0]", direction: output}
    m_arvalid: {type: logic, direction: output}
    m_arready: {type: logic, direction: input}
    m_rdata: {type: "logic[31:0]", direction: input}
    m_rresp: {type: "logic[1:0]", direction: input}
    m_rvalid: {type: logic, direction: input}
    m_rready: {type: logic, direction: output}
  bus:
    host:
      bus: axi4_lite
      mode: master
      mapping:
        awaddr: m_awaddr
        awprot: m_awprot
        awvalid: m_awvalid
        awready: m_awready
        wdata: m_wdata
        wstrb: m_wstrb
        wvalid: m_wvalid
        wready: m_wready
        bresp: m_bresp
        bvalid: m_bvalid
        bready: m_bready
        araddr: m_araddr
        arprot: m_arprot
        arvalid: m_arvalid
        arready: m_arready
        rdata: m_rdata
        rresp: m_rresp
        rvalid: m_rvalid
        rready: m_rready
iomux0:
  generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 8
    pin_count: 4
    integration:
      instance: u_iomux0
      clock: clk_iomux
      reset: rst_iomux_n
      control: iomux_control
      pad:
        input_value: pad_input_value
        input_enable: pad_input_enable
        output_value: pad_output_value
        output_enable: pad_output_enable
    route:
      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
        input_value: {link: gpio0_in, bit: 0}
        input_enable: 1
        output_value: {link: gpio0_out, bit: 0}
        output_enable: {link: gpio0_oe, bit: 0}
      - pin: 1
        slot: 0
        function: gpio0
        signal: data1
        input_value: {link: gpio0_in, bit: 1}
        input_enable: 1
        output_value: {link: gpio0_out, bit: 1}
        output_enable: {link: gpio0_oe, bit: 1}
      - pin: 2
        slot: 0
        function: gpio0
        signal: data2
        input_value: {link: gpio0_in, bit: 2}
        input_enable: 1
        output_value: {link: gpio0_out, bit: 2}
        output_enable: {link: gpio0_oe, bit: 2}
      - pin: 3
        slot: 0
        function: gpio0
        signal: data3
        input_value: {link: gpio0_in, bit: 3}
        input_enable: 1
        output_value: {link: gpio0_out, bit: 3}
        output_enable: {link: gpio0_oe, bit: 3}
      - pin: 3
        slot: 1
        function: uart0
        signal: tx
        output_value: {link: uart0_tx}
        output_enable: 1
)";

const QString baseNetlist = R"(---
version: "1.0"
port:
  pad_input_value:
    direction: input
    type: "logic[3:0]"
    connect: pad_input_value
  pad_input_enable:
    direction: output
    type: "logic[3:0]"
    connect: pad_input_enable
  pad_output_value:
    direction: output
    type: "logic[3:0]"
    connect: pad_output_value
  pad_output_enable:
    direction: output
    type: "logic[3:0]"
    connect: pad_output_enable
  clk_iomux:
    direction: input
    type: logic
    connect: clk_iomux
  rst_iomux_n:
    direction: input
    type: logic
    connect: rst_iomux_n
instance:
  u_periph:
    module: periph_stub
    port:
      clk:
        link: clk_iomux
      rst_n:
        link: rst_iomux_n
      gpio_out:
        link: gpio0_out
      gpio_oe:
        link: gpio0_oe
      gpio_in:
        link: gpio0_in
      uart_tx:
        link: uart0_tx
bus:
  iomux_control:
    - instance: u_periph
      port: host
)";

QString peripheralVerilog()
{
    return QString(R"VERILOG(module periph_stub (
    input  wire        clk,
    input  wire        rst_n,
    output reg  [3:0]  gpio_out,
    output reg  [3:0]  gpio_oe,
    input  wire [3:0]  gpio_in,
    output reg         uart_tx,
    output reg  [7:0]  m_awaddr,
    output reg  [2:0]  m_awprot,
    output reg         m_awvalid,
    input  wire        m_awready,
    output reg  [31:0] m_wdata,
    output reg  [3:0]  m_wstrb,
    output reg         m_wvalid,
    input  wire        m_wready,
    input  wire [1:0]  m_bresp,
    input  wire        m_bvalid,
    output reg         m_bready,
    output reg  [7:0]  m_araddr,
    output reg  [2:0]  m_arprot,
    output reg         m_arvalid,
    input  wire        m_arready,
    input  wire [31:0] m_rdata,
    input  wire [1:0]  m_rresp,
    input  wire        m_rvalid,
    output reg         m_rready
);

initial begin
    gpio_out  = 4'b0101;
    gpio_oe   = 4'b1111;
    uart_tx   = 1'b1;
    m_awaddr  = 8'h04;
    m_awprot  = 3'b000;
    m_awvalid = 1'b0;
    m_wdata   = 32'h00001000;
    m_wstrb   = 4'b0010;
    m_wvalid  = 1'b0;
    m_bready  = 1'b0;
    m_araddr  = 8'h00;
    m_arprot  = 3'b000;
    m_arvalid = 1'b0;
    m_rready  = 1'b0;

    wait (rst_n === 1'b1);
    repeat (4) @(negedge clk);
    m_awvalid = 1'b1;
    m_wvalid  = 1'b1;
    while (m_awvalid || m_wvalid) begin
        @(negedge clk);
        if (m_awready)
            m_awvalid = 1'b0;
        if (m_wready)
            m_wvalid = 1'b0;
    end

    m_bready = 1'b1;
    while (!m_bvalid)
        @(negedge clk);
    @(negedge clk);
    m_bready = 1'b0;
end

endmodule
)VERILOG");
}

QString mergedTopTestbench()
{
    return QString(R"VERILOG(`timescale 1ns/1ps
module tb;
reg        clk_iomux;
reg        rst_iomux_n;
reg  [3:0] pad_input_value;
wire [3:0] pad_input_enable;
wire [3:0] pad_output_value;
wire [3:0] pad_output_enable;
integer    failures;
integer    timeout_cycles;

iomux_soc_top dut (
    .pad_input_value(pad_input_value),
    .pad_input_enable(pad_input_enable),
    .pad_output_value(pad_output_value),
    .pad_output_enable(pad_output_enable),
    .clk_iomux(clk_iomux),
    .rst_iomux_n(rst_iomux_n)
);

always #5 clk_iomux = ~clk_iomux;

task check_value;
    input condition;
    input [8*64-1:0] label;
    begin
        if (condition !== 1'b1) begin
            failures = failures + 1;
            $display("CHECK_FAIL %0s", label);
        end
    end
endtask

initial begin
    failures       = 0;
    timeout_cycles = 0;
    clk_iomux      = 1'b0;
    rst_iomux_n    = 1'b0;
    pad_input_value = 4'b1001;
    repeat (4) @(negedge clk_iomux);
    rst_iomux_n = 1'b1;
    repeat (2) @(negedge clk_iomux);

    check_value(
        {pad_input_enable[3], pad_output_value[3], pad_output_enable[3]} === 3'b101,
        "reset selects pin 3 slot 0 bundle");
    check_value(pad_output_value[0] === 1'b1, "vector carrier drives pin 0");
    check_value(dut.u_periph.gpio_in[3] === 1'b1, "rx reaches vector carrier bit");

    while ({pad_input_enable[3], pad_output_value[3], pad_output_enable[3]} !== 3'b011
           && timeout_cycles < 40) begin
        @(negedge clk_iomux);
        timeout_cycles = timeout_cycles + 1;
    end

    check_value(timeout_cycles < 40, "AXI selector write completes");
    check_value(
        {pad_input_enable[3], pad_output_value[3], pad_output_enable[3]} === 3'b011,
        "AXI selector routes scalar slot 1 bundle");
    check_value(pad_output_value[0] === 1'b1, "other pin remains on slot 0");
    check_value(dut.u_periph.gpio_in[3] === 1'b1, "rx broadcast ignores selector");

    if (failures == 0)
        $display("TEST_PASS");
    else
        $display("TEST_FAIL count=%0d", failures);
    $finish;
end
endmodule
)VERILOG");
}

enum InvalidAssemblyFixture {
    AxiWidthMismatchFixture,
    CarrierWithoutBitFixture,
    CarrierBitOutOfRangeFixture,
    TxWithoutDriverFixture,
    InstanceNameCollisionFixture,
    MissingWstrbBusLeafFixture,
};

struct CommandResult
{
    int     exitCode = -1;
    QString output;
};

class Test : public QObject
{
    Q_OBJECT

private:
    static QStringList      messages;
    static QtMessageHandler previousMessageHandler;

    static void messageOutput(QtMsgType type, const QMessageLogContext &context, const QString &text)
    {
        Q_UNUSED(type);
        Q_UNUSED(context);
        messages.append(text);
    }

    static void writeTextFile(const QString &path, const QString &text)
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream(&file) << text;
    }

    static QString readTextFile(const QString &path)
    {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return QString();
        }
        return QString::fromUtf8(file.readAll());
    }

    static void createProject(
        const QTemporaryDir &directory,
        const QString       &moduleText = moduleLibrary,
        const QString       &busText    = busLibrary)
    {
        QVERIFY(directory.isValid());
        QSocProjectManager projectManager;
        projectManager.setCurrentPath(directory.path());
        QVERIFY(projectManager.create("iomux_soc"));
        writeTextFile(QDir(directory.path()).filePath("bus/axi4_lite.soc_bus"), busText);
        writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), moduleText);
    }

    static QStringList projectOptions(const QTemporaryDir &directory)
    {
        return {"-d", directory.path(), "-p", "iomux_soc"};
    }

    static CommandResult runCommand(const QStringList &arguments)
    {
        messages.clear();
        QSocCliWorker worker;
        QSignalSpy    exitSpy(&worker, &QSocCliWorker::exit);
        worker.setup(arguments, false);
        worker.run();

        CommandResult result;
        if (exitSpy.count() == 1) {
            result.exitCode = exitSpy.takeFirst().first().toInt();
        }
        result.output = messages.join('\n');
        return result;
    }

    static CommandResult generateModule(const QTemporaryDir &directory)
    {
        QStringList arguments = {"qsoc", "generate", "module", "-l", "peripheral"};
        arguments.append(projectOptions(directory));
        arguments.append("iomux0");
        return runCommand(arguments);
    }

    static CommandResult mergeTop(const QTemporaryDir &directory)
    {
        const QString basePath = QDir(directory.path()).filePath("output/iomux_soc_top.soc_net");
        const QString fragmentPath
            = QDir(directory.path()).filePath("output/peripheral/iomux0/iomux0_integration.soc_net");
        QStringList arguments = {"qsoc", "generate", "verilog", "--merge"};
        arguments.append(projectOptions(directory));
        arguments.append(basePath);
        arguments.append(fragmentPath);
        return runCommand(arguments);
    }

private slots:
    void initTestCase();
    void cleanupTestCase();
    void mergedTopInstantiatesWrapperAndElaborates();
    void mergedTopAxiWriteChangesPadWhenIverilogIsAvailable();
    void sparseVectorCarrierMerges();
    void combinationalVectorCarrierMerges();
    void invalidGeneratorBlocksNetlistGeneration();
    void controlBusInstanceLinkMerges();
    void controlBusRequiresExactlyOneMasterAndSlave();
    void generatedIntegrationRejectsInvalidAssemblyAndKeepsSentinel_data();
    void generatedIntegrationRejectsInvalidAssemblyAndKeepsSentinel();
};

QStringList      Test::messages;
QtMessageHandler Test::previousMessageHandler = nullptr;

void Test::initTestCase()
{
    previousMessageHandler = qInstallMessageHandler(messageOutput);
    QSocConsole::setTeeToMessageHandler(true);
}

void Test::cleanupTestCase()
{
    QSocConsole::setTeeToMessageHandler(false);
    qInstallMessageHandler(previousMessageHandler);
}

void Test::mergedTopInstantiatesWrapperAndElaborates()
{
    QTemporaryDir directory;
    createProject(directory);

    const CommandResult generated = generateModule(directory);
    QCOMPARE(generated.exitCode, 0);

    const QString fragmentPath
        = QDir(directory.path()).filePath("output/peripheral/iomux0/iomux0_integration.soc_net");
    const QString fragment = readTextFile(fragmentPath);
    QVERIFY(fragment.contains("module: iomux0"));
    QVERIFY(fragment.contains("link: clk_iomux"));
    QVERIFY(fragment.contains("bits: \"[0]\""));
    QVERIFY(fragment.contains("port: control"));
    QVERIFY(!fragment.contains("FIXME"));
    QVERIFY(!fragment.contains("..."));

    writeTextFile(QDir(directory.path()).filePath("output/iomux_soc_top.soc_net"), baseNetlist);
    const CommandResult merged = mergeTop(directory);
    QCOMPARE(merged.exitCode, 0);

    const QString topPath = QDir(directory.path()).filePath("output/iomux_soc_top.v");
    const QString top     = readTextFile(topPath);
    QVERIFY2(!top.isEmpty(), qPrintable(merged.output));
    QVERIFY2(top.contains("iomux0 u_iomux0"), qPrintable(top));
    QVERIFY2(top.contains("periph_stub u_periph"), qPrintable(top));
    QVERIFY2(top.contains("iomux_control_awaddr"), qPrintable(top));
    QVERIFY2(!top.contains("FIXME"), qPrintable(top));

    const QString moduleOutput   = QDir(directory.path()).filePath("output/peripheral/iomux0");
    const QString peripheralPath = QDir(directory.path()).filePath("periph_stub.v");
    writeTextFile(peripheralPath, peripheralVerilog());

    QSlangDriver driver;
    const QString files = QStringList{
        topPath,
        QDir(moduleOutput).filePath("iomux0.v"),
        QDir(moduleOutput).filePath("iomux0_regs.v"),
        QDir(moduleOutput).filePath("iomux0_conn.v"),
        peripheralPath}
                              .join(' ');
    QVERIFY(driver.parseArgs(QString("slang --single-unit %1").arg(files)));
}

void Test::mergedTopAxiWriteChangesPadWhenIverilogIsAvailable()
{
    const QString compiler = QStandardPaths::findExecutable("iverilog");
    const QString runtime  = QStandardPaths::findExecutable("vvp");
    if (compiler.isEmpty() || runtime.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY("iverilog and vvp");
    }

    QTemporaryDir directory;
    createProject(directory);

    const CommandResult generated = generateModule(directory);
    QCOMPARE(generated.exitCode, 0);

    writeTextFile(QDir(directory.path()).filePath("output/iomux_soc_top.soc_net"), baseNetlist);
    const CommandResult merged = mergeTop(directory);
    QCOMPARE(merged.exitCode, 0);

    const QString topPath        = QDir(directory.path()).filePath("output/iomux_soc_top.v");
    const QString moduleOutput   = QDir(directory.path()).filePath("output/peripheral/iomux0");
    const QString peripheralPath = QDir(directory.path()).filePath("periph_stub.v");
    const QString benchPath      = QDir(directory.path()).filePath("tb.v");
    const QString executablePath = QDir(directory.path()).filePath("iomux_soc_top.out");
    writeTextFile(peripheralPath, peripheralVerilog());
    writeTextFile(benchPath, mergedTopTestbench());

    QProcess process;
    process.setWorkingDirectory(directory.path());
    process.start(
        compiler,
        {"-g2001",
         "-s",
         "tb",
         "-o",
         executablePath,
         topPath,
         QDir(moduleOutput).filePath("iomux0.v"),
         QDir(moduleOutput).filePath("iomux0_regs.v"),
         QDir(moduleOutput).filePath("iomux0_conn.v"),
         peripheralPath,
         benchPath});
    QVERIFY(process.waitForStarted());
    QVERIFY(process.waitForFinished(120000));
    const QByteArray compileOutput = process.readAllStandardOutput()
                                     + process.readAllStandardError();
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QVERIFY2(process.exitCode() == 0, compileOutput.constData());

    process.start(runtime, {executablePath});
    QVERIFY(process.waitForStarted());
    QVERIFY(process.waitForFinished(120000));
    const QByteArray simulationOutput = process.readAllStandardOutput()
                                        + process.readAllStandardError();
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 0);
    QVERIFY2(!simulationOutput.contains("CHECK_FAIL"), simulationOutput.constData());
    QVERIFY2(!simulationOutput.contains("TEST_FAIL"), simulationOutput.constData());
    QVERIFY2(simulationOutput.contains("TEST_PASS"), simulationOutput.constData());
}

void Test::sparseVectorCarrierMerges()
{
    QString         sparseModule = moduleLibrary;
    const qsizetype secondRoute  = sparseModule.indexOf("      - pin: 1\n");
    QVERIFY(secondRoute >= 0);
    sparseModule.truncate(secondRoute);

    QTemporaryDir directory;
    createProject(directory, sparseModule);

    const CommandResult generated = generateModule(directory);
    QCOMPARE(generated.exitCode, 0);

    writeTextFile(QDir(directory.path()).filePath("output/iomux_soc_top.soc_net"), baseNetlist);
    const CommandResult merged = mergeTop(directory);
    QCOMPARE(merged.exitCode, 0);
    const QString top = readTextFile(QDir(directory.path()).filePath("output/iomux_soc_top.v"));
    QVERIFY(!top.isEmpty());
    QVERIFY2(!top.contains("FIXME"), qPrintable(top));
}

void Test::combinationalVectorCarrierMerges()
{
    QTemporaryDir directory;
    createProject(directory);

    const CommandResult generated = generateModule(directory);
    QCOMPARE(generated.exitCode, 0);

    QString netlist = baseNetlist;
    netlist.replace(
        "version: \"1.0\"\nport:\n",
        "version: \"1.0\"\n"
        "port:\n"
        "  gpio0_out:\n"
        "    direction: output\n"
        "    type: logic[3:0]\n"
        "    connect: gpio0_out\n");
    netlist.replace("      gpio_out:\n        link: gpio0_out\n", QString());
    netlist.append("comb:\n  - out: gpio0_out\n    expr: \"4'b0101\"\n");
    writeTextFile(QDir(directory.path()).filePath("output/iomux_soc_top.soc_net"), netlist);

    const CommandResult merged = mergeTop(directory);
    QCOMPARE(merged.exitCode, 0);
    QVERIFY(!readTextFile(QDir(directory.path()).filePath("output/iomux_soc_top.v")).isEmpty());
}

void Test::invalidGeneratorBlocksNetlistGeneration()
{
    QTemporaryDir directory;
    createProject(directory);

    const CommandResult generated = generateModule(directory);
    QCOMPARE(generated.exitCode, 0);

    QString broken = moduleLibrary;
    broken.replace("    pin_count: 4\n", QString());
    writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), broken);

    writeTextFile(QDir(directory.path()).filePath("output/iomux_soc_top.soc_net"), baseNetlist);
    const CommandResult merged = mergeTop(directory);
    QVERIFY2(merged.exitCode != 0, qPrintable(merged.output));
    QVERIFY2(
        merged.output.contains("is invalid and blocks netlist generation"),
        qPrintable(merged.output));
    QVERIFY2(merged.output.contains("IOMUX_REQUIRED generator.pin_count"), qPrintable(merged.output));
    QVERIFY(!QFile::exists(QDir(directory.path()).filePath("output/iomux_soc_top.v")));
}

void Test::controlBusInstanceLinkMerges()
{
    QTemporaryDir directory;
    createProject(directory);

    const CommandResult generated = generateModule(directory);
    QCOMPARE(generated.exitCode, 0);

    QString netlist = baseNetlist;
    netlist.replace(
        "      uart_tx:\n"
        "        link: uart0_tx\n"
        "bus:\n"
        "  iomux_control:\n"
        "    - instance: u_periph\n"
        "      port: host\n",
        "      uart_tx:\n"
        "        link: uart0_tx\n"
        "    bus:\n"
        "      host:\n"
        "        link: iomux_control\n");
    QVERIFY(netlist != baseNetlist);
    writeTextFile(QDir(directory.path()).filePath("output/iomux_soc_top.soc_net"), netlist);

    const CommandResult merged = mergeTop(directory);
    QCOMPARE(merged.exitCode, 0);
    QVERIFY(!readTextFile(QDir(directory.path()).filePath("output/iomux_soc_top.v")).isEmpty());
}

void Test::controlBusRequiresExactlyOneMasterAndSlave()
{
    QTemporaryDir directory;
    createProject(directory);

    const CommandResult generated = generateModule(directory);
    QCOMPARE(generated.exitCode, 0);

    QString crowded = baseNetlist;
    crowded.replace(
        "bus:\n  iomux_control:\n    - instance: u_periph\n      port: host\n",
        "bus:\n  iomux_control:\n    - instance: u_periph\n      port: host\n"
        "    - instance: u_periph2\n      port: host\n");
    crowded.replace(
        "instance:\n  u_periph:", "instance:\n  u_periph2:\n    module: periph_stub\n  u_periph:");
    writeTextFile(QDir(directory.path()).filePath("output/iomux_soc_top.soc_net"), crowded);

    const CommandResult merged = mergeTop(directory);
    QVERIFY2(merged.exitCode != 0, qPrintable(merged.output));
    QVERIFY2(merged.output.contains("exactly one master and one slave"), qPrintable(merged.output));
    QVERIFY(!QFile::exists(QDir(directory.path()).filePath("output/iomux_soc_top.v")));
}

void Test::generatedIntegrationRejectsInvalidAssemblyAndKeepsSentinel_data()
{
    QTest::addColumn<int>("fixture");
    QTest::addColumn<QString>("expectedDiagnostic");

    QTest::newRow("axi-64-module-on-32-bit-bus")
        << int(AxiWidthMismatchFixture)
        << QString("iomux_control_wdata is invalid: width mismatch");
    QTest::newRow("carrier-without-bit")
        << int(CarrierWithoutBitFixture) << QString("gpio0_out is invalid: width mismatch");
    QTest::newRow("carrier-bit-out-of-range")
        << int(CarrierBitOutOfRangeFixture) << QString("gpio0_out is invalid: width mismatch");
    QTest::newRow("tx-link-without-driver")
        << int(TxWithoutDriverFixture) << QString("uart0_tx is invalid: missing driver");
    QTest::newRow("generated-instance-name-collision")
        << int(InstanceNameCollisionFixture)
        << QString("generated IOMUX instance is declared in more than one merged netlist: u_iomux0");
    QTest::newRow("generated-physical-wstrb-port-missing")
        << int(MissingWstrbBusLeafFixture) << QString("s_axi_wstrb");
}

void Test::generatedIntegrationRejectsInvalidAssemblyAndKeepsSentinel()
{
    QFETCH(int, fixture);
    QFETCH(QString, expectedDiagnostic);

    QString moduleText  = moduleLibrary;
    QString netlistText = baseNetlist;
    QString busText     = busLibrary;

    switch (fixture) {
    case AxiWidthMismatchFixture:
        moduleText.replace("    data_width: 32\n", "    data_width: 64\n");
        break;
    case CarrierWithoutBitFixture:
        moduleText
            .replace("output_value: {link: gpio0_out, bit: 0}", "output_value: {link: gpio0_out}");
        break;
    case CarrierBitOutOfRangeFixture:
        moduleText.append(
            "      - pin: 0\n"
            "        slot: 1\n"
            "        function: invalid\n"
            "        signal: out_of_range\n"
            "        output_value: {link: gpio0_out, bit: 4}\n");
        break;
    case TxWithoutDriverFixture:
        netlistText.replace("      uart_tx:\n        link: uart0_tx\n", QString());
        break;
    case InstanceNameCollisionFixture:
        netlistText.replace("instance:\n", "instance:\n  u_iomux0:\n    module: periph_stub\n");
        break;
    case MissingWstrbBusLeafFixture:
        busText.replace(
            "    wstrb:\n"
            "      master: {direction: out, width: 4}\n"
            "      slave: {direction: in, width: 4}\n",
            QString());
        break;
    default:
        QFAIL("unknown invalid assembly fixture");
    }

    QVERIFY(moduleText != moduleLibrary || netlistText != baseNetlist || busText != busLibrary);

    QTemporaryDir directory;
    createProject(directory, moduleText, busText);

    const CommandResult generated = generateModule(directory);
    QCOMPARE(generated.exitCode, 0);

    const QString netlistPath = QDir(directory.path()).filePath("output/iomux_soc_top.soc_net");
    const QString topPath     = QDir(directory.path()).filePath("output/iomux_soc_top.v");
    const QString sentinel    = "// sentinel top must survive failed integration\n";
    writeTextFile(netlistPath, netlistText);
    writeTextFile(topPath, sentinel);

    const CommandResult merged = mergeTop(directory);
    QVERIFY2(merged.exitCode != 0, qPrintable(merged.output));
    QVERIFY2(merged.output.contains(expectedDiagnostic), qPrintable(merged.output));
    QCOMPARE(readTextFile(topPath), sentinel);
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoccliparsegenerateiomux.moc"
