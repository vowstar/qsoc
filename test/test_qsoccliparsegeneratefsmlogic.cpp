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
        static auto                  argc      = 1;
        static char                  appName[] = "qsoc";
        static std::array<char *, 1> argv      = {{appName}};
        /* Use QCoreApplication for cli test */
        static const QCoreApplication app = QCoreApplication(argc, argv.data());
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

    void createTestModuleFiles()
    {
        /* Create module directory if it doesn't exist */
        QDir moduleDir(projectManager.getModulePath());
        if (!moduleDir.exists()) {
            moduleDir.mkpath(".");
        }
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

private slots:
    void initTestCase()
    {
        TestApp::instance();
        qInstallMessageHandler(messageOutput);
        projectName = QFileInfo(__FILE__).baseName() + "_data";
        projectManager.setProjectName(projectName);
        projectManager.setCurrentPath(QDir::current().filePath(projectName));
        projectManager.mkpath();
        projectManager.save(projectName);
        projectManager.load(projectName);
        createTestModuleFiles();
    }

    void cleanupTestCase()
    {
#ifdef ENABLE_TEST_CLEANUP
        /* Clean up the test project directory */
        QDir projectDir(projectManager.getCurrentPath());
        if (projectDir.exists()) {
            projectDir.removeRecursively();
        }
#endif // ENABLE_TEST_CLEANUP
    }

    void init() { messageList.clear(); }

    void testTableMooreFSM()
    {
        QString netlistContent = R"(
# Test netlist with Table-mode Moore FSM
port:
  clk:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  start:
    direction: input
    type: logic
  done_load:
    direction: input
    type: logic
  done:
    direction: input
    type: logic
  busy:
    direction: output
    type: logic

instance: {}

net: {}

fsm:
  - name: cpu_ctrl
    clk: clk
    rst: rst_n
    rst_state: IDLE
    trans:
      IDLE: [{cond: start, next: LOAD}]
      LOAD: [{cond: done_load, next: RUN}]
      RUN: [{cond: done, next: IDLE}]
    moore:
      IDLE: {busy: 0}
      LOAD: {busy: 1}
      RUN: {busy: 1}
)";

        QString netlistPath = createTempFile("test_table_moore_fsm.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        /* Check if Verilog file was generated */
        QString verilogPath
            = QDir(projectManager.getOutputPath()).filePath("test_table_moore_fsm.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify FSM comment header */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "/* cpu_ctrl : Table FSM generated by YAML-DSL */"));

        /* Verify state register declarations for Verilog 2005 */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "reg [1:0] cpu_ctrl_cur_state"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "localparam CPU_CTRL_IDLE = 2'd0"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "localparam CPU_CTRL_LOAD = 2'd1"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "localparam CPU_CTRL_RUN = 2'd2"));

        /* Verify state registers */
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "cpu_ctrl_cur_state, cpu_ctrl_nxt_state"));

        /* Verify next-state logic */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* cpu_ctrl next-state logic */"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "cpu_ctrl_nxt_state = cpu_ctrl_cur_state"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "case (cpu_ctrl_cur_state)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "CPU_CTRL_IDLE:"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "if (start) cpu_ctrl_nxt_state = CPU_CTRL_LOAD"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "CPU_CTRL_LOAD:"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "if (done_load) cpu_ctrl_nxt_state = CPU_CTRL_RUN"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "CPU_CTRL_RUN:"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "if (done) cpu_ctrl_nxt_state = CPU_CTRL_IDLE"));

        /* Verify state register with async reset */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "/* cpu_ctrl state register w/ async reset */"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "always @(posedge clk or negedge rst_n)"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "if (!rst_n) cpu_ctrl_cur_state <= CPU_CTRL_IDLE"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "else cpu_ctrl_cur_state <= cpu_ctrl_nxt_state"));

        /* Verify Moore outputs */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* cpu_ctrl Moore outputs */"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "reg cpu_ctrl_busy_reg"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign busy = cpu_ctrl_busy_reg"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "cpu_ctrl_busy_reg = 1'b0"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "CPU_CTRL_LOAD:"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "CPU_CTRL_RUN:"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "cpu_ctrl_busy_reg = 1'b1"));
    }

    void testTableMealyFSM()
    {
        QString netlistContent = R"(
# Test netlist with Table-mode Moore + Mealy FSM
port:
  clk:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  cs_n:
    direction: input
    type: logic
  bit_cnt:
    direction: input
    type: logic[2:0]
  shift_en:
    direction: output
    type: logic
  byte_ready:
    direction: output
    type: logic

instance: {}

net: {}

fsm:
  - name: spi_rx
    clk: clk
    rst: rst_n
    rst_state: IDLE
    trans:
      IDLE:
        - {cond: "cs_n==0", next: SHIFT}
      SHIFT:
        - {cond: "bit_cnt==7", next: DONE}
        - {cond: "1", next: SHIFT}
      DONE:
        - {cond: "cs_n==1", next: IDLE}
    moore:
      SHIFT: {shift_en: 1}
    mealy:
      - {cond: "spi_rx_cur_state==SPI_RX_DONE && cs_n==1",
         sig: byte_ready,
         val: 1}
)";

        QString netlistPath = createTempFile("test_table_mealy_fsm.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        /* Check if Verilog file was generated */
        QString verilogPath
            = QDir(projectManager.getOutputPath()).filePath("test_table_mealy_fsm.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify FSM structure */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "/* spi_rx : Table FSM generated by YAML-DSL */"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "SPI_RX_IDLE = 2'd0"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "SPI_RX_SHIFT = 2'd1"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "SPI_RX_DONE = 2'd2"));

        /* Verify transitions */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "if (cs_n==1'b0) spi_rx_nxt_state = SPI_RX_SHIFT"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "if (bit_cnt==3'd7) spi_rx_nxt_state = SPI_RX_DONE"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "if (cs_n==1'b1) spi_rx_nxt_state = SPI_RX_IDLE"));

        /* Verify Moore outputs */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* spi_rx Moore outputs */"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "reg spi_rx_shift_en_reg"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "assign shift_en = spi_rx_shift_en_reg"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "spi_rx_shift_en_reg = 1'b1"));

        /* Verify Mealy outputs */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* spi_rx Mealy outputs */"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent,
            "assign byte_ready = (spi_rx_cur_state==SPI_RX_DONE && cs_n==1'b1) ? 1'b1 : 1'b0"));
    }

    void testMicrocodeFixedROMFSM()
    {
        QString netlistContent = R"(
# Test netlist with Microcode-mode Fixed ROM FSM
port:
  clk:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  cond:
    direction: input
    type: logic
  ctrl_bus:
    direction: output
    type: logic[7:0]

instance: {}

net: {}

fsm:
  - name: mseq_fixed
    clk: clk
    rst: rst_n
    rst_state: 0
    fields:
      ctrl: [0, 7]
      branch: [8, 9]
      next: [10, 14]
    rom_mode: parameter
    rom:
      0: {ctrl: 0x55, branch: 0, next: 1}
      1: {ctrl: 0x3C, branch: 1, next: 4}
      2: {ctrl: 0x18, branch: 0, next: 3}
      3: {ctrl: 0x80, branch: 3, next: 0}
      4: {ctrl: 0xA0, branch: 2, next: 3}
)";

        QString netlistPath = createTempFile("test_microcode_fixed_fsm.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        /* Check if Verilog file was generated */
        QString verilogPath
            = QDir(projectManager.getOutputPath()).filePath("test_microcode_fixed_fsm.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify microcode FSM structure */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "/* mseq_fixed : microcode FSM with constant ROM */"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "localparam MSEQ_FIXED_AW = "));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "localparam MSEQ_FIXED_DW = "));

        /* Verify program counter */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* mseq_fixed program counter */"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "mseq_fixed_pc, mseq_fixed_nxt_pc"));

        /* Verify ROM array */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* mseq_fixed ROM array */"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "mseq_fixed_rom"));

        /* Verify ROM initialization */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "/* mseq_fixed reset-time ROM initialization */"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "mseq_fixed_rom[0] <= {"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "mseq_fixed_rom[1] <= {"));

        /* Verify branch decode */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* mseq_fixed branch decode */"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "mseq_fixed_nxt_pc = mseq_fixed_pc + 1'b1"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "case (mseq_fixed_rom[mseq_fixed_pc][9:8])"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "2'd0: mseq_fixed_nxt_pc = mseq_fixed_pc + 1'b1"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent,
            "2'd1: if (cond) mseq_fixed_nxt_pc = mseq_fixed_rom[mseq_fixed_pc][14:10]"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent,
            "2'd2: if (!cond) mseq_fixed_nxt_pc = mseq_fixed_rom[mseq_fixed_pc][14:10]"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "2'd3: mseq_fixed_nxt_pc = mseq_fixed_rom[mseq_fixed_pc][14:10]"));

        /* Verify PC register */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* mseq_fixed pc register */"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "if (!rst_n) mseq_fixed_pc <= "));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "else        mseq_fixed_pc <= mseq_fixed_nxt_pc"));

        /* Verify control outputs */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* mseq_fixed control outputs */"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "assign ctrl_bus = mseq_fixed_rom[mseq_fixed_pc][7:0]"));
    }

    void testMicrocodeProgrammableROMFSM()
    {
        QString netlistContent = R"(
# Test netlist with Microcode-mode Programmable ROM FSM
port:
  clk:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  mseq_prog_rom_we:
    direction: input
    type: logic
  mseq_prog_rom_addr:
    direction: input
    type: logic[4:0]
  mseq_prog_rom_wdata:
    direction: input
    type: logic[15:0]
  cond:
    direction: input
    type: logic
  ctrl_bus:
    direction: output
    type: logic[7:0]

instance: {}

net: {}

fsm:
  - name: mseq_prog
    clk: clk
    rst: rst_n
    rst_state: 0
    rom_mode: port
    rom_depth: 32
    fields:
      ctrl: [0, 7]
      branch: [8, 9]
      next: [10, 14]
)";

        QString netlistPath = createTempFile("test_microcode_prog_fsm.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        /* Check if Verilog file was generated */
        QString verilogPath
            = QDir(projectManager.getOutputPath()).filePath("test_microcode_prog_fsm.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify programmable microcode FSM structure */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "/* mseq_prog : microcode FSM with programmable ROM */"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "localparam MSEQ_PROG_AW = 5"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "localparam MSEQ_PROG_DW = 14"));

        /* Verify write port */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* mseq_prog write port */"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent,
            "if (mseq_prog_rom_we) mseq_prog_rom[mseq_prog_rom_addr] <= mseq_prog_rom_wdata"));

        /* Verify branch decode exists */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* mseq_prog branch decode */"));

        /* Verify control outputs */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "assign ctrl_bus = mseq_prog_rom[mseq_prog_pc][7:0]"));
    }

    void testFSMWithEncodingTypes()
    {
        QString netlistContent = R"(
# Test netlist with different encoding types
port:
  clk:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  trigger:
    direction: input
    type: logic
  onehot_output:
    direction: output
    type: logic
  gray_output:
    direction: output
    type: logic

instance: {}

net: {}

fsm:
  - name: test_onehot
    clk: clk
    rst: rst_n
    rst_state: S0
    encoding: onehot
    trans:
      S0: [{cond: trigger, next: S1}]
      S1: [{cond: trigger, next: S2}]
      S2: [{cond: trigger, next: S0}]
    moore:
      S1: {onehot_output: 1}
  - name: test_gray
    clk: clk
    rst: rst_n
    rst_state: A
    encoding: gray
    trans:
      A: [{cond: trigger, next: B}]
      B: [{cond: trigger, next: C}]
      C: [{cond: trigger, next: A}]
    moore:
      B: {gray_output: 1}
)";

        QString netlistPath = createTempFile("test_fsm_encodings.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        /* Check if Verilog file was generated */
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_fsm_encodings.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify onehot encoding */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "TEST_ONEHOT_S0 = 3'd1"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "TEST_ONEHOT_S1 = 3'd2"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "TEST_ONEHOT_S2 = 3'd4"));

        /* Verify gray encoding */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "TEST_GRAY_A = 2'd0"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "TEST_GRAY_B = 2'd1"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "TEST_GRAY_C = 2'd3"));
    }

    void testMultipleFSMsCoexistence()
    {
        QString netlistContent = R"(
# Test netlist with multiple FSMs of different types
port:
  clk:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  start:
    direction: input
    type: logic
  done:
    direction: input
    type: logic
  cond:
    direction: input
    type: logic
  ctrl_bus:
    direction: output
    type: logic[7:0]
  busy:
    direction: output
    type: logic

instance: {}
net: {}

fsm:
  - name: main_ctrl
    clk: clk
    rst: rst_n
    rst_state: IDLE
    trans:
      IDLE: [{cond: start, next: WORK}]
      WORK: [{cond: done, next: IDLE}]
    moore:
      WORK: {busy: 1}
  - name: micro_seq
    clk: clk
    rst: rst_n
    rst_state: 0
    fields:
      ctrl: [0, 7]
      branch: [8, 9]
      next: [10, 14]
    rom_mode: parameter
    data_width: 16
    addr_width: 4
    use_parameters: true
    rom:
      0: {ctrl: 0x55, branch: 0, next: 1}
      1: {ctrl: 0x3C, branch: 1, next: 0}
)";

        QString netlistPath = createTempFile("test_multiple_fsms.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        /* Check if Verilog file was generated */
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_multiple_fsms.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify both FSMs are present with correct prefixes */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* main_ctrl : Table FSM"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* micro_seq : microcode FSM"));

        /* Verify Table FSM variables have correct prefixes */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "main_ctrl_cur_state"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "main_ctrl_nxt_state"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "main_ctrl_busy_reg"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "MAIN_CTRL_IDLE"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "MAIN_CTRL_WORK"));

        /* Verify Microcode FSM variables have correct prefixes */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "micro_seq_pc"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "micro_seq_nxt_pc"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "micro_seq_rom"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "parameter MICRO_SEQ_AW = 4"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "parameter MICRO_SEQ_DW = 15"));

        /* Verify no naming conflicts - each FSM should have unique variable names */
        int mainCtrlCount = verilogContent.count("main_ctrl_");
        int microSeqCount = verilogContent.count("micro_seq_");
        QVERIFY(mainCtrlCount > 0);
        QVERIFY(microSeqCount > 0);

        /* Verify user-specified parameters are respected */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "parameter MICRO_SEQ_AW = 4")); /* User specified addr_width: 4 */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent,
            "parameter MICRO_SEQ_DW = 15")); /* User specified data_width: 16, but DW = width-1 */
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegeneratefsmlogic.moc"
