// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/qslangdriver.h"
#include "common/qsocconsole.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
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

    static void createProject(const QTemporaryDir &directory)
    {
        QVERIFY(directory.isValid());
        QSocProjectManager projectManager;
        projectManager.setCurrentPath(directory.path());
        QVERIFY(projectManager.create("iomux_soc"));
        writeTextFile(QDir(directory.path()).filePath("bus/axi4_lite.soc_bus"), busLibrary);
        writeTextFile(QDir(directory.path()).filePath("module/peripheral.soc_mod"), moduleLibrary);
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
    void invalidGeneratorBlocksNetlistGeneration();
    void controlBusRequiresExactlyOneMasterAndSlave();
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

    const QString moduleOutput = QDir(directory.path()).filePath("output/peripheral/iomux0");
    QSlangDriver  driver;
    const QString files = QStringList{
        topPath,
        QDir(moduleOutput).filePath("iomux0.v"),
        QDir(moduleOutput).filePath("iomux0_regs.v"),
        QDir(moduleOutput).filePath("iomux0_conn.v")}
                              .join(' ');
    QVERIFY(driver.parseArgs(QString("slang --single-unit --ignore-unknown-modules %1").arg(files)));
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

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoccliparsegenerateiomux.moc"
