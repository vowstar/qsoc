// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/config.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QTemporaryFile>
#include <QTextStream>
#include <QtCore>
#include <QtTest>

struct TestApp
{
    static auto &instance()
    {
        static auto                   argc      = 1;
        static char                   appName[] = "qsoc";
        static std::array<char *, 1>  argv      = {{appName}};
        static const QCoreApplication app       = QCoreApplication(argc, argv.data());
        return app;
    }
};

class Test : public QObject
{
    Q_OBJECT

private:
    static QStringList messageList;
    QString            projectName;
    QSocProjectManager projectManager;

    static void messageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
    {
        Q_UNUSED(type);
        Q_UNUSED(context);
        messageList << msg;
    }

    QString createTempFile(const QString &fileName, const QString &content)
    {
        QString filePath = QDir(projectManager.getCurrentPath()).filePath(fileName);
        QFile   file(filePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&file);
            stream << content;
            file.close();
            return filePath;
        }
        return QString();
    }

    /* Helper function to verify Verilog content with normalized whitespace */
    bool verifyVerilogContentNormalized(const QString &verilogContent, const QString &contentToVerify)
    {
        if (verilogContent.isEmpty() || contentToVerify.isEmpty()) {
            return false;
        }

        /* Helper function to normalize whitespace */
        auto normalizeWhitespace = [](const QString &input) -> QString {
            QString result = input;
            /* Replace all whitespace (including tabs and newlines) with a single space */
            result.replace(QRegularExpression("\\s+"), " ");
            /* Remove whitespace before any symbol/operator/punctuation */
            result.replace(
                QRegularExpression("\\s+([\\[\\]\\(\\)\\{\\}<>\"'`+\\-*/%&|^~!#$,.:;=@_])"), "\\1");
            /* Remove whitespace after any symbol/operator/punctuation */
            result.replace(
                QRegularExpression("([\\[\\]\\(\\)\\{\\}<>\"'`+\\-*/%&|^~!#$,.:;=@_])\\s+"), "\\1");

            return result;
        };

        /* Normalize whitespace in both strings before comparing */
        const QString normalizedContent = normalizeWhitespace(verilogContent);
        const QString normalizedVerify  = normalizeWhitespace(contentToVerify);

        /* Check if the normalized content contains the normalized text we're looking for */
        return normalizedContent.contains(normalizedVerify);
    }

    void createTestFiles()
    {
        /* Create module directory if it doesn't exist */
        QDir moduleDir(projectManager.getModulePath());
        if (!moduleDir.exists()) {
            moduleDir.mkpath(".");
        }

        // Create a simple module with input/output ports
        const QString moduleContent = R"(
test_module:
  port:
    clk:
      type: logic
      direction: in
    rst_n:
      type: logic
      direction: in
    data_out:
      type: logic[7:0]
      direction: out
    enable:
      type: logic
      direction: in
)";

        createTempFile("module/test_module.soc_mod", moduleContent);

        // Create IO cell module (similar to the original PDDWUWSWCDG_H module)
        const QString ioCellContent = R"(
test_io_cell:
  port:
    I:
      type: logic
      direction: in
    O:
      type: logic
      direction: out
    C:
      type: logic
      direction: out
    OEN:
      type: logic
      direction: in
)";

        createTempFile("module/test_io_cell.soc_mod", ioCellContent);

        // Create a project that uses this module and has top-level ports
        // This project simulates the original issue: top-level ports that should NOT trigger warnings
        const QString projectContent = R"(
project:
  name: test_toplevel_ports
  description: Test project for top-level port direction checking
  version: 1.0.0
  author: Test

  toplevel:
    name: top_test_chip

    port:
      # Top-level input ports (externally driven, internally consumed)
      test_tck:
        type: logic
        direction: input
      test_tdi:
        type: logic
        direction: input
      # Top-level output ports (internally driven, externally consumed)
      test_tdo:
        type: logic
        direction: output
      test_tdo_oe:
        type: logic
        direction: output

  instances:
    u_test_module:
      module: test_module
      location:
        x: 100
        y: 100
    u_io_cell_tck:
      module: test_io_cell
      location:
        x: 200
        y: 100
    u_io_cell_tdo:
      module: test_io_cell
      location:
        x: 300
        y: 100

  nets:
    # Top-level input driving internal logic - should be valid
    test_tck:
      - { instance: top, port: test_tck }
      - { instance: u_io_cell_tck, port: I }
    test_tdi:
      - { instance: top, port: test_tdi }
      - { instance: u_test_module, port: rst_n }
    # Top-level output driven by internal logic - should be valid
    test_tdo:
      - { instance: top, port: test_tdo }
      - { instance: u_io_cell_tdo, port: O }
    test_tdo_oe:
      - { instance: top, port: test_tdo_oe }
      - { instance: u_test_module, port: data_out }
)";

        createTempFile("project.yaml", projectContent);
    }

private slots:
    void initTestCase()
    {
        TestApp::instance();
        projectName = "test_netlist_toplevel_ports_"
                      + QString::number(QDateTime::currentMSecsSinceEpoch());
    }

    void init()
    {
        messageList.clear();
        qInstallMessageHandler(messageOutput);
    }

    void cleanup() { qInstallMessageHandler(nullptr); }

    void cleanupTestCase()
    {
        if (!projectName.isEmpty()) {
            QDir projectDir(projectManager.getProjectPath());
            if (projectDir.exists()) {
                projectDir.removeRecursively();
            }
        }
    }

    void testToplevelPortDirectionCheck()
    {
        // Create a new project
        projectManager.setProjectName(projectName);
        projectManager.setCurrentPath(QDir::current().filePath(projectName));
        projectManager.mkpath();
        projectManager.save(projectName);
        projectManager.load(projectName);

        // Set up the test files
        createTestFiles();

        // Generate netlist
        QSocCliWorker worker;
        QStringList   arguments;
        arguments << "qsoc" << "generate" << "--project"
                  << projectManager.getProjectPath() + "/project.yaml";

        worker.setup(arguments, true);
        worker.run();

        // Check that no incorrect warnings were generated for proper top-level port connections
        bool hasIncorrectMultidriveWarning = false;
        bool hasIncorrectUndrivenWarning   = false;

        for (const QString &message : messageList) {
            // Top-level input ports should NOT be reported as "multiple drivers"
            // because they are inputs to the chip - only external source should drive them
            if ((message.contains("test_tck") || message.contains("test_tdi"))
                && (message.contains("multiple drivers") || message.contains("Multidrive"))) {
                hasIncorrectMultidriveWarning = true;
                qWarning() << "Incorrect multidrive warning for top-level input:" << message;
            }

            // Top-level output ports should NOT be reported as "undriven"
            // because they should be driven by internal logic
            if ((message.contains("test_tdo") || message.contains("test_tdo_oe"))
                && (message.contains("undriven") || message.contains("Undriven"))) {
                hasIncorrectUndrivenWarning = true;
                qWarning() << "Incorrect undriven warning for top-level output:" << message;
            }
        }

        // These should not have incorrect warnings
        QVERIFY2(
            !hasIncorrectMultidriveWarning,
            "Top-level input ports should not be reported as having multiple drivers");
        QVERIFY2(
            !hasIncorrectUndrivenWarning,
            "Top-level output ports should not be reported as undriven");
    }

    void testToplevelPortCorrectBehavior()
    {
        // Clear previous project
        if (!projectName.isEmpty()) {
            QDir projectDir(projectManager.getProjectPath());
            if (projectDir.exists()) {
                projectDir.removeRecursively();
            }
        }

        // Create new project for this specific test
        projectName = "test_correct_behavior_"
                      + QString::number(QDateTime::currentMSecsSinceEpoch());
        projectManager.setProjectName(projectName);
        projectManager.setCurrentPath(QDir::current().filePath(projectName));
        projectManager.mkpath();
        projectManager.save(projectName);
        projectManager.load(projectName);

        /* Create module directory if it doesn't exist */
        QDir moduleDir(projectManager.getModulePath());
        if (!moduleDir.exists()) {
            moduleDir.mkpath(".");
        }

        // Create module file
        const QString moduleContent = R"(
test_driver:
  port:
    output_port:
      type: logic
      direction: out
)";

        createTempFile("module/test_driver.soc_mod", moduleContent);

        // Create a project that correctly connects top-level ports
        const QString correctProject = R"(
project:
  name: test_correct_behavior
  description: Test project with correct top-level port connections
  version: 1.0.0
  author: Test

  toplevel:
    name: top_correct_chip

    port:
      external_output:
        type: logic
        direction: output  # Should be driven by internal logic

  instances:
    u_driver:
      module: test_driver
      location:
        x: 100
        y: 100

  nets:
    # Correct: internal driver -> top-level output
    output_net:
      - { instance: u_driver, port: output_port }
      - { instance: top, port: external_output }
)";

        createTempFile("project.yaml", correctProject);

        // Clear message list for this test
        messageList.clear();

        // Generate netlist
        QSocCliWorker worker;
        QStringList   arguments;
        arguments << "qsoc" << "generate" << "--project"
                  << projectManager.getProjectPath() + "/project.yaml";

        worker.setup(arguments, true);
        worker.run();

        // Should not have incorrect warnings for this correct configuration
        bool hasIncorrectWarning = false;
        for (const QString &message : messageList) {
            if (message.contains("external_output")
                && (message.contains("undriven") || message.contains("multiple drivers"))) {
                hasIncorrectWarning = true;
                qWarning() << "Unexpected warning for correct top-level output:" << message;
            }
        }

        QVERIFY2(
            !hasIncorrectWarning,
            "Should not generate warnings for correctly connected top-level output");
    }

    void testLinkUplinkConnections()
    {
        // Clear previous project
        if (!projectName.isEmpty()) {
            QDir projectDir(projectManager.getProjectPath());
            if (projectDir.exists()) {
                projectDir.removeRecursively();
            }
        }

        // Create new project for link/uplink testing
        projectName = "test_link_uplink_" + QString::number(QDateTime::currentMSecsSinceEpoch());
        projectManager.setProjectName(projectName);
        projectManager.setCurrentPath(QDir::current().filePath(projectName));
        projectManager.mkpath();
        projectManager.save(projectName);
        projectManager.load(projectName);

        /* Create module directory if it doesn't exist */
        QDir moduleDir(projectManager.getModulePath());
        if (!moduleDir.exists()) {
            moduleDir.mkpath(".");
        }

        /* Create bus directory if it doesn't exist */
        QDir busDir(projectManager.getBusPath());
        if (!busDir.exists()) {
            busDir.mkpath(".");
        }

        // Create a simple bus definition for testing
        const QString busContent = R"(
test_bus:
  port:
    signal:
      master:
        direction: out
      slave:
        direction: in
)";

        createTempFile("bus/test_bus.soc_bus", busContent);

        // Create an IO module similar to the real io_top module
        const QString ioModuleContent = R"(
io_test_cell:
  port:
    signal_in:
      type: logic
      direction: in
    signal_out:
      type: logic
      direction: out
    pad_signal:
      type: logic
      direction: inout
)";

        createTempFile("module/io_test_cell.soc_mod", ioModuleContent);

        // Create a netlist file that uses link and uplink connections
        const QString linkUplinkNetlist = R"(
instance:
  u_io_cell:
    module: io_test_cell
    port:
      signal_in:
        link: internal_sig  # link creates net connection
      signal_out:
        link: output_sig    # link creates net connection
      pad_signal:
        uplink: PAD_SIGNAL  # uplink creates top-level port

# Additional nets will be auto-generated by link processing
# Top-level ports will be auto-generated by uplink processing
)";

        QString netlistPath = createTempFile("test_link_uplink.soc_net", linkUplinkNetlist);
        QVERIFY(!netlistPath.isEmpty());

        // Clear message list for this test
        messageList.clear();

        // Generate Verilog using the correct command
        QSocCliWorker worker;
        QStringList   arguments;
        arguments << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                  << netlistPath;

        worker.setup(arguments, true);
        worker.run();

        // Basic validation only

        // Check generated Verilog file
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_link_uplink.v");
        QFile   verilogFile(verilogPath);
        QString verilogContent;
        if (verilogFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream stream(&verilogFile);
            verilogContent = stream.readAll();
            verilogFile.close();
        }

        // Basic validation
        QVERIFY(!verilogPath.isEmpty());
        QVERIFY(QFile::exists(verilogPath));
        QVERIFY(!verilogContent.isEmpty());

        // Verify that top-level port was created from uplink
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, "inout PAD_SIGNAL"),
            "uplink should create top-level port PAD_SIGNAL");

        // Verify no incorrect FIXME warnings for uplink connection
        bool hasIncorrectUplinkWarning = false;
        for (const QString &message : messageList) {
            if (message.contains("PAD_SIGNAL")
                && (message.contains("FIXME") || message.contains("multiple drivers")
                    || message.contains("undriven"))) {
                hasIncorrectUplinkWarning = true;
                // Found incorrect warning
            }
        }

        // Check for incorrect FIXME in Verilog content - specifically lines that mention both PAD_SIGNAL and FIXME
        QStringList lines = verilogContent.split('\n');
        for (const QString &line : lines) {
            if (line.contains("PAD_SIGNAL") && line.contains("FIXME")) {
                hasIncorrectUplinkWarning = true;
                // Found FIXME warning for PAD_SIGNAL
            }
        }

        QVERIFY2(
            !hasIncorrectUplinkWarning,
            "uplink connections should not generate incorrect FIXME warnings");

        // Verify instance connection is generated
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, ".pad_signal(PAD_SIGNAL)"),
            "instance should connect pad_signal to PAD_SIGNAL");
    }

    void testLinkConnectionTypes()
    {
        // Clear previous project
        if (!projectName.isEmpty()) {
            QDir projectDir(projectManager.getProjectPath());
            if (projectDir.exists()) {
                projectDir.removeRecursively();
            }
        }

        // Create new project for link connection testing
        projectName = "test_link_types_" + QString::number(QDateTime::currentMSecsSinceEpoch());
        projectManager.setProjectName(projectName);
        projectManager.setCurrentPath(QDir::current().filePath(projectName));
        projectManager.mkpath();
        projectManager.save(projectName);
        projectManager.load(projectName);

        /* Create module directory if it doesn't exist */
        QDir moduleDir(projectManager.getModulePath());
        if (!moduleDir.exists()) {
            moduleDir.mkpath(".");
        }

        /* Create bus directory if it doesn't exist */
        QDir busDir(projectManager.getBusPath());
        if (!busDir.exists()) {
            busDir.mkpath(".");
        }

        // Create a simple bus definition for testing
        const QString busContent = R"(
test_bus:
  port:
    signal:
      master:
        direction: out
      slave:
        direction: in
)";

        createTempFile("bus/test_bus.soc_bus", busContent);

        // Create modules for testing different link scenarios
        const QString clockGenModuleContent = R"(
clock_gen:
  port:
    clk_out:
      type: logic
      direction: out
)";

        const QString sourceModuleContent = R"(
source_module:
  port:
    clk:
      type: logic
      direction: in
    data_out:
      type: logic[7:0]
      direction: out
)";

        const QString sinkModuleContent = R"(
sink_module:
  port:
    clk:
      type: logic
      direction: in
    data_in:
      type: logic[7:0]
      direction: in
)";

        createTempFile("module/clock_gen.soc_mod", clockGenModuleContent);
        createTempFile("module/source_module.soc_mod", sourceModuleContent);
        createTempFile("module/sink_module.soc_mod", sinkModuleContent);

        // Create a netlist that tests link connections between modules
        const QString linkNetlist = R"(
instance:
  u_clock_gen:
    module: clock_gen
    port:
      clk_out:
        link: sys_clk
  u_source:
    module: source_module
    port:
      clk:
        link: sys_clk
      data_out:
        link: data_bus
  u_sink:
    module: sink_module
    port:
      clk:
        link: sys_clk
      data_in:
        link: data_bus

# Additional nets will be auto-generated by link processing
)";

        QString netlistPath = createTempFile("test_link_types.soc_net", linkNetlist);
        QVERIFY(!netlistPath.isEmpty());

        // Clear message list for this test
        messageList.clear();

        // Generate Verilog
        QSocCliWorker worker;
        QStringList   arguments;
        arguments << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                  << netlistPath;

        worker.setup(arguments, true);
        worker.run();

        // Check generated Verilog file
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_link_types.v");
        QFile   verilogFile(verilogPath);
        QString verilogContent;
        if (verilogFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream stream(&verilogFile);
            verilogContent = stream.readAll();
            verilogFile.close();
        }

        // Basic validation
        QVERIFY(!verilogContent.isEmpty());

        // Verify wire declarations are generated
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, "wire sys_clk;"),
            "sys_clk wire should be declared");
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, "wire [7:0] data_bus;"),
            "data_bus wire should be declared");

        // Verify instance connections
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, ".clk(sys_clk)"),
            "instances should connect to sys_clk");
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, ".data_out(data_bus)"),
            "source should connect data_out to data_bus");
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, ".data_in(data_bus)"),
            "sink should connect data_in to data_bus");

        // Check that there are no incorrect warnings for proper link connections
        bool hasIncorrectLinkWarning = false;
        for (const QString &message : messageList) {
            // sys_clk should not be reported as undriven - it's driven by clock_gen
            if (message.contains("sys_clk") && message.contains("undriven")) {
                hasIncorrectLinkWarning = true;
                // Found incorrect undriven warning for sys_clk
            }
            // data_bus should not be reported as undriven - it's driven by source
            if (message.contains("data_bus") && message.contains("undriven")) {
                hasIncorrectLinkWarning = true;
                // Found incorrect undriven warning for data_bus
            }
            // Check for multiple drivers (should not happen with proper link setup)
            if ((message.contains("sys_clk") || message.contains("data_bus"))
                && message.contains("multiple drivers")) {
                hasIncorrectLinkWarning = true;
                // Found incorrect multiple driver warning
            }
        }

        QVERIFY2(!hasIncorrectLinkWarning, "link connections should not generate incorrect warnings");
    }

    void testMixedLinkUplinkConnections()
    {
        // Clear previous project
        if (!projectName.isEmpty()) {
            QDir projectDir(projectManager.getProjectPath());
            if (projectDir.exists()) {
                projectDir.removeRecursively();
            }
        }

        // Create new project for mixed link/uplink testing
        projectName = "test_mixed_" + QString::number(QDateTime::currentMSecsSinceEpoch());
        projectManager.setProjectName(projectName);
        projectManager.setCurrentPath(QDir::current().filePath(projectName));
        projectManager.mkpath();
        projectManager.save(projectName);
        projectManager.load(projectName);

        /* Create module directory if it doesn't exist */
        QDir moduleDir(projectManager.getModulePath());
        if (!moduleDir.exists()) {
            moduleDir.mkpath(".");
        }

        /* Create bus directory if it doesn't exist */
        QDir busDir(projectManager.getBusPath());
        if (!busDir.exists()) {
            busDir.mkpath(".");
        }

        // Create a simple bus definition for testing
        const QString busContent = R"(
test_bus:
  port:
    signal:
      master:
        direction: out
      slave:
        direction: in
)";

        createTempFile("bus/test_bus.soc_bus", busContent);

        // Create modules for mixed testing
        const QString clockDriverModuleContent = R"(
clock_driver:
  port:
    clk_out:
      type: logic
      direction: out
    data_out:
      type: logic[7:0]
      direction: out
    enable_out:
      type: logic
      direction: out
)";

        const QString complexIoModuleContent = R"(
complex_io:
  port:
    clk_in:
      type: logic
      direction: in
    data_in:
      type: logic[7:0]
      direction: in
    enable_in:
      type: logic
      direction: in
    external_clk:
      type: logic
      direction: inout
    external_data:
      type: logic[15:0]
      direction: inout
)";

        createTempFile("module/clock_driver.soc_mod", clockDriverModuleContent);
        createTempFile("module/complex_io.soc_mod", complexIoModuleContent);

        // Create a netlist that mixes link and uplink connections
        const QString mixedNetlist = R"(
instance:
  u_clock_driver:
    module: clock_driver
    port:
      clk_out:
        link: internal_clk      # link - internal clock distribution
      data_out:
        link: internal_data_in  # link - internal data input
      enable_out:
        link: internal_enable   # link - internal enable signal
  u_complex:
    module: complex_io
    port:
      clk_in:
        link: internal_clk      # link - internal clock distribution
      data_in:
        link: internal_data_in  # link - internal data input
      enable_in:
        link: internal_enable   # link - internal enable signal
      external_clk:
        uplink: EXTERNAL_CLK    # uplink - external clock pad
      external_data:
        uplink: EXTERNAL_DATA   # uplink - external data bus pad

# Mix of link and uplink creates both internal nets and top-level ports
)";

        QString netlistPath = createTempFile("test_mixed.soc_net", mixedNetlist);
        QVERIFY(!netlistPath.isEmpty());

        // Clear message list for this test
        messageList.clear();

        // Generate Verilog
        QSocCliWorker worker;
        QStringList   arguments;
        arguments << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                  << netlistPath;

        worker.setup(arguments, true);
        worker.run();

        // Check generated Verilog file
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_mixed.v");
        QFile   verilogFile(verilogPath);
        QString verilogContent;
        if (verilogFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream stream(&verilogFile);
            verilogContent = stream.readAll();
            verilogFile.close();
        }

        // Basic validation
        QVERIFY(!verilogContent.isEmpty());

        // Verify top-level ports from uplink
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, "inout EXTERNAL_CLK"),
            "uplink should create EXTERNAL_CLK top-level port");
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, "inout [15:0] EXTERNAL_DATA"),
            "uplink should create EXTERNAL_DATA top-level port");

        // Verify internal wires from link
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, "wire internal_clk;"),
            "link should create internal_clk wire");
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, "wire [7:0] internal_data_in;"),
            "link should create internal_data_in wire");
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, "wire internal_enable;"),
            "link should create internal_enable wire");

        // Verify instance connections (mix of wires and top-level ports)
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, ".clk_in(internal_clk)"),
            "complex_io instance should connect clk_in to internal_clk wire");
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, ".clk_out(internal_clk)"),
            "clock_driver instance should connect clk_out to internal_clk wire");
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, ".external_clk(EXTERNAL_CLK)"),
            "instance should connect external_clk to EXTERNAL_CLK port");
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, ".external_data(EXTERNAL_DATA)"),
            "instance should connect external_data to EXTERNAL_DATA port");

        // Check for no incorrect warnings
        bool hasIncorrectWarning = false;
        for (const QString &message : messageList) {
            // Uplink ports should not generate warnings
            if ((message.contains("EXTERNAL_CLK") || message.contains("EXTERNAL_DATA"))
                && (message.contains("FIXME") || message.contains("undriven")
                    || message.contains("multiple drivers"))) {
                hasIncorrectWarning = true;
                // Found incorrect warning for uplink port
            }
        }

        QVERIFY2(
            !hasIncorrectWarning,
            "mixed link/uplink connections should not generate incorrect warnings");
    }

    void testRealIoTopNetlist()
    {
        // Test a realistic io_top.soc_net based on real format but without sensitive info

        // Set up temporary project
        QString tempProjectName = "test_real_io_top_"
                                  + QString::number(QDateTime::currentMSecsSinceEpoch());
        projectManager.setProjectName(tempProjectName);
        projectManager.setCurrentPath(QDir::current().filePath(tempProjectName));
        projectManager.mkpath();
        projectManager.save(tempProjectName);
        projectManager.load(tempProjectName);

        /* Create module directory if it doesn't exist */
        QDir moduleDir(projectManager.getModulePath());
        if (!moduleDir.exists()) {
            moduleDir.mkpath(".");
        }

        /* Create bus directory if it doesn't exist */
        QDir busDir(projectManager.getBusPath());
        if (!busDir.exists()) {
            busDir.mkpath(".");
        }

        // Create a simple bus definition for testing
        const QString busContent = R"(
test_bus:
  port:
    signal:
      master:
        direction: out
      slave:
        direction: in
)";

        createTempFile("bus/test_bus.soc_bus", busContent);

        // Create IO cell modules exactly like real ones to reproduce the bug
        const QString ioCellContent = R"(
IO_CELL:
  port:
    C:
      type: logic
      direction: out    # This is the key - output port like real PDDWUWSWCDG_H
    I:
      type: logic
      direction: in
    IE:
      type: logic
      direction: in
    OEN:
      type: logic
      direction: in
    PAD:
      type: logic
      direction: inout
    PE:
      type: logic
      direction: in
    PS:
      type: logic
      direction: in
    ST:
      type: logic
      direction: in
    RTE:
      type: logic
      direction: in
)";

        const QString powerCellContent = R"(
POWER_CELL:
  port:
    RTE:
      type: logic
      direction: in
)";

        createTempFile("module/IO_CELL.soc_mod", ioCellContent);
        createTempFile("module/POWER_CELL.soc_mod", powerCellContent);

        // Create a realistic io_top netlist with link/uplink patterns like the real one
        const QString ioTopNetlist = R"(
instance:
  u_power_1:
    module: POWER_CELL
    port:
      RTE:
        link: rte_west
  u_io_cell_tck:
    module: IO_CELL
    port:
      C:
        uplink: jtag_tck
      I:
        tie: 1'b0
      IE:
        tie: 1'b1
      OEN:
        tie: 1'b1
      PAD:
        uplink: PAD_tck
      PE:
        tie: 1'b0
      PS:
        tie: 1'b0
      ST:
        tie: 1'b0
      RTE:
        link: rte_west
  u_power_2:
    module: POWER_CELL
    port:
      RTE:
        link: rte_west
  u_io_cell_tdo:
    module: IO_CELL
    port:
      I:
        uplink: jtag_tdo
      IE:
        tie: 1'b1
      OEN:
        uplink: jtag_tdo_oe
      PAD:
        uplink: PAD_tdo
      PE:
        tie: 1'b0
      PS:
        tie: 1'b0
      ST:
        tie: 1'b0
      RTE:
        link: rte_west
  u_power_3:
    module: POWER_CELL
    port:
      RTE:
        link: rte_west
  u_io_cell_tdi:
    module: IO_CELL
    port:
      C:
        uplink: jtag_tdi
      I:
        tie: 1'b0
      IE:
        tie: 1'b1
      OEN:
        tie: 1'b1
      PAD:
        uplink: PAD_tdi
      PE:
        tie: 1'b0
      PS:
        tie: 1'b1
      ST:
        tie: 1'b0
      RTE:
        link: rte_west
  u_power_4:
    module: POWER_CELL
    port:
      RTE:
        link: rte_west
  u_io_cell_tms:
    module: IO_CELL
    port:
      C:
        uplink: jtag_tms
      I:
        tie: 1'b0
      IE:
        tie: 1'b1
      OEN:
        tie: 1'b1
      PAD:
        uplink: PAD_tms
      PE:
        tie: 1'b0
      PS:
        tie: 1'b0
      ST:
        tie: 1'b0
      RTE:
        link: rte_west
)";

        QString ioTopNetlistPath = createTempFile("io_top_test.soc_net", ioTopNetlist);
        QVERIFY(!ioTopNetlistPath.isEmpty());

        // Clear message list for this test
        messageList.clear();

        // Generate Verilog using the realistic io_top netlist
        QSocCliWorker worker;
        QStringList   arguments;
        arguments << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                  << ioTopNetlistPath;

        worker.setup(arguments, true);
        worker.run();

        // Check generated Verilog file
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("io_top_test.v");
        QFile   verilogFile(verilogPath);
        QString verilogContent;
        if (verilogFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            QTextStream stream(&verilogFile);
            verilogContent = stream.readAll();
            verilogFile.close();
        }

        // Basic validation
        QVERIFY(!verilogPath.isEmpty());
        QVERIFY(QFile::exists(verilogPath));
        QVERIFY(!verilogContent.isEmpty());

        // Verify some expected uplink ports from io_top.soc_net are created
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, "inout PAD_tck"),
            "uplink should create PAD_tck top-level port");
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, "inout PAD_tdi"),
            "uplink should create PAD_tdi top-level port");
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, "inout PAD_tms"),
            "uplink should create PAD_tms top-level port");

        // Check for FIXME warnings in the generated Verilog content
        bool        hasFIXMEWarning = false;
        QStringList verilogLines    = verilogContent.split('\n');
        QStringList fixmeLines;
        for (const QString &line : verilogLines) {
            if (line.contains("FIXME")
                && (line.contains("jtag_tck") || line.contains("jtag_tdi")
                    || line.contains("jtag_tms") || line.contains("jtag_tdo"))) {
                hasFIXMEWarning = true;
                fixmeLines << line.trimmed();
            }
        }

        // Check for no incorrect undriven/multidriven warnings for uplink ports
        bool hasIncorrectUplinkWarning = false;
        for (const QString &message : messageList) {
            // Check for uplink ports that should NOT be reported as undriven or multidriven
            if ((message.contains("PAD_tck") || message.contains("PAD_tdi")
                 || message.contains("PAD_tms") || message.contains("PAD_tdo")
                 || message.contains("PAD_SIGNAL") || message.contains("jtag_tck")
                 || message.contains("jtag_tdi") || message.contains("jtag_tms"))
                && (message.contains("undriven") || message.contains("multiple drivers")
                    || message.contains("Undriven") || message.contains("Multidrive"))) {
                hasIncorrectUplinkWarning = true;
                break; /* Exit early to prevent potential issues */
            }
        }

        QVERIFY2(
            !hasFIXMEWarning,
            "Real io_top.soc_net uplink connections should not generate FIXME warnings in Verilog");

        QVERIFY2(
            !hasIncorrectUplinkWarning,
            "Real io_top.soc_net uplink connections should not generate incorrect "
            "undriven/multidriven warnings");

        // Verify some link connections work properly (internal nets should exist)
        QVERIFY2(
            verifyVerilogContentNormalized(verilogContent, "wire rte_west;")
                || verifyVerilogContentNormalized(verilogContent, "wire rte_east;"),
            "link should create RTE wire connections");

        // Clean up temporary project
        QDir tempProjectDir(projectManager.getProjectPath());
        if (tempProjectDir.exists()) {
            tempProjectDir.removeRecursively();
        }
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegeneratetoplevelport.moc"
