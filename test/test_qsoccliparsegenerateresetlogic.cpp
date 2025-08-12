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

    void testBasicResetController()
    {
        QString netlistContent = R"(
# Test netlist with basic reset controller (ASYNC_COMB type)
port:
  clk_sys:
    direction: input
    type: logic
  por_rst_n:
    direction: input
    type: logic
  cpu_rst_n:
    direction: output
    type: logic
  peri_rst_n:
    direction: output
    type: logic
  test_en:
    direction: input
    type: logic

instance: {}

net: {}

reset:
  - name: basic_reset_ctrl
    clock: clk_sys
    test_enable: test_en
    source:
      por_rst_n: low
    target:
      cpu_rst_n:
        polarity: low
        link:
          por_rst_n:
            type: ASYNC_COMB
      peri_rst_n:
        polarity: low
        link:
          por_rst_n:
            type: ASYNC_COMB
)";

        QString netlistPath = createTempFile("test_basic_reset.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_basic_reset.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify reset controller module exists */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "module basic_reset_ctrl"));

        /* Verify basic reset assignments for ASYNC_COMB type (combinational) */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "assign por_rst_n_cpu_rst_n_sync = por_rst_n"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "assign por_rst_n_peri_rst_n_sync = por_rst_n"));

        /* Verify output assignments */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign cpu_rst_n = &"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign peri_rst_n = &"));
    }

    void testSyncResetController()
    {
        QString netlistContent = R"(
# Test netlist with sync reset controller (ASYNC_SYNC type)
port:
  clk_sys:
    direction: input
    type: logic
  i3c_soc_rst:
    direction: input
    type: logic
  cpu_rst_n:
    direction: output
    type: logic
  test_en:
    direction: input
    type: logic

instance: {}

net: {}

reset:
  - name: sync_reset_ctrl
    clock: clk_sys
    test_enable: test_en
    source:
      i3c_soc_rst: high
    target:
      cpu_rst_n:
        polarity: low
        link:
          i3c_soc_rst:
            type: ASYNC_SYNC
            sync_depth: 4
)";

        QString netlistPath = createTempFile("test_sync_reset.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_sync_reset.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify simplified ASYNC_SYNC reset implementation */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "reg [3:0] sync_i3c_soc_rst_cpu_rst_n_ff;"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "always @(posedge clk_sys or posedge i3c_soc_rst)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "if (i3c_soc_rst)"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "sync_i3c_soc_rst_cpu_rst_n_ff <= 4'b0"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent,
            "assign i3c_soc_rst_cpu_rst_n_sync = test_en ? ~i3c_soc_rst : "
            "sync_i3c_soc_rst_cpu_rst_n_ff[3]"));
    }

    void testCounterResetController()
    {
        QString netlistContent = R"(
# Test netlist with counter reset controller (ASYNC_CNT type)
port:
  clk_sys:
    direction: input
    type: logic
  por_rst_n:
    direction: input
    type: logic
  cpu_por_rst_n:
    direction: output
    type: logic
  test_en:
    direction: input
    type: logic

instance: {}

net: {}

reset:
  - name: counter_reset_ctrl
    clock: clk_sys
    test_enable: test_en
    source:
      por_rst_n: low
    target:
      cpu_por_rst_n:
        polarity: low
        link:
          por_rst_n:
            type: ASYNC_CNT
            sync_depth: 2
            counter_width: 8
            timeout_cycles: 255
)";

        QString netlistPath = createTempFile("test_counter_reset.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_counter_reset.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify simplified ASYNC_CNT reset implementation */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "reg [7:0] cnt_por_rst_n_cpu_por_rst_n_counter;"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "reg cnt_por_rst_n_cpu_por_rst_n_counting;"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "always @(posedge clk_sys or negedge por_rst_n)"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "cnt_por_rst_n_cpu_por_rst_n_counter < 255"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent,
            "assign por_rst_n_cpu_por_rst_n_sync = test_en ? por_rst_n : "
            "(cnt_por_rst_n_cpu_por_rst_n_counting ? 1'b0 : 1'b1)"));
    }

    void testMultiSourceMultiTarget()
    {
        QString netlistContent = R"(
# Test netlist with multi-source multi-target reset matrix
port:
  clk_sys:
    direction: input
    type: logic
  por_rst_n:
    direction: input
    type: logic
  i3c_soc_rst:
    direction: input
    type: logic
  trig_cpu_rst:
    direction: input
    type: logic
  cpu_rst_n:
    direction: output
    type: logic
  i3c_rst_n:
    direction: output
    type: logic
  test_en:
    direction: input
    type: logic

instance: {}

net: {}

reset:
  - name: multi_reset_ctrl
    clock: clk_sys
    test_enable: test_en
    source:
      por_rst_n: low
      i3c_soc_rst: high
      trig_cpu_rst: high
    target:
      cpu_rst_n:
        polarity: low
        link:
          por_rst_n:
            type: ASYNC_SYNC
            sync_depth: 4
          i3c_soc_rst:
            type: ASYNC_SYNC
            sync_depth: 4
          trig_cpu_rst:
            type: ASYNC_SYNC
            sync_depth: 4
      i3c_rst_n:
        polarity: low
        link:
          por_rst_n:
            type: ASYNC_COMB
          i3c_soc_rst:
            type: ASYNC_COMB
)";

        QString netlistPath = createTempFile("test_multi_reset.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_multi_reset.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify simplified DFF-based ASYNC_SYNC implementations */
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "reg [3:0] sync_por_rst_n_cpu_rst_n_ff;"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "reg [3:0] sync_i3c_soc_rst_cpu_rst_n_ff;"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "reg [3:0] sync_trig_cpu_rst_cpu_rst_n_ff;"));

        /* Verify wire declarations for intermediate signals */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "wire por_rst_n_cpu_rst_n_sync;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "wire i3c_soc_rst_cpu_rst_n_sync;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "wire trig_cpu_rst_cpu_rst_n_sync;"));

        /* Verify AND logic for combining multiple reset sources */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign cpu_rst_n = &"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign i3c_rst_n = &"));

        /* Verify polarity handling in assign statements */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "test_en ? ~i3c_soc_rst"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "test_en ? ~trig_cpu_rst"));
    }

    void testSyncOnlyReset()
    {
        QString netlistContent = R"(
# Test netlist with SYNC_ONLY reset controller
port:
  clk_sys:
    direction: input
    type: logic
  sync_rst_n:
    direction: input
    type: logic
  peri_rst_n:
    direction: output
    type: logic
  test_en:
    direction: input
    type: logic

instance: {}

net: {}

reset:
  - name: sync_only_reset_ctrl
    clock: clk_sys
    test_enable: test_en
    source:
      sync_rst_n: low
    target:
      peri_rst_n:
        polarity: low
        link:
          sync_rst_n:
            type: SYNC_ONLY
            sync_depth: 2
)";

        QString netlistPath = createTempFile("test_sync_only_reset.soc_net", netlistContent);
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
            = QDir(projectManager.getOutputPath()).filePath("test_sync_only_reset.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify simplified SYNC_ONLY reset implementation */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "reg [1:0] sync_only_sync_rst_n_peri_rst_n_ff;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "always @(posedge clk_sys)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "if (!sync_rst_n)"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "sync_only_sync_rst_n_peri_rst_n_ff <= 2'b0"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent,
            "assign sync_rst_n_peri_rst_n_sync = test_en ? sync_rst_n : "
            "sync_only_sync_rst_n_peri_rst_n_ff[1]"));
    }

    void testAsyncSyncntReset()
    {
        QString netlistContent = R"(
# Test netlist with ASYNC_SYNCNT reset controller
port:
  clk_sys:
    direction: input
    type: logic
  trig_rst:
    direction: input
    type: logic
  dma_rst_n:
    direction: output
    type: logic
  test_en:
    direction: input
    type: logic

instance: {}

net: {}

reset:
  - name: syncnt_reset_ctrl
    clock: clk_sys
    test_enable: test_en
    source:
      trig_rst: low
    target:
      dma_rst_n:
        polarity: low
        link:
          trig_rst:
            type: ASYNC_SYNCNT
            sync_depth: 3
            counter_width: 8
            timeout_cycles: 15
)";

        QString netlistPath = createTempFile("test_syncnt_reset.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_syncnt_reset.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify ASYNC_SYNCNT reset implementation with sync-then-count stages */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "ASYNC_SYNCNT: Async reset with sync-then-count release"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "Stage 1: Sync release"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "Stage 2: Counter timeout"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "reg [2:0] syncnt_trig_rst_dma_rst_n_sync_ff;"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "reg [7:0] syncnt_trig_rst_dma_rst_n_counter;"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "syncnt_trig_rst_dma_rst_n_counter < 15"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "module syncnt_reset_ctrl"));
    }

    void testResetReasonRecording()
    {
        QString netlistContent = R"(
# Test netlist with reset reason recording feature - Per-source sticky flags
port:
  clk_32k:
    direction: input
    type: logic
  clk_sys:
    direction: input
    type: logic
  por_rst_n:
    direction: input
    type: logic
  ext_rst_n:
    direction: input
    type: logic
  wdt_rst_n:
    direction: input
    type: logic
  i3c_soc_rst:
    direction: input
    type: logic
  sys_rst_n:
    direction: output
    type: logic
  reason:
    direction: output
    type: logic [2:0]
  reason_valid:
    direction: output
    type: logic
  test_en:
    direction: input
    type: logic
  reason_clear:
    direction: input
    type: logic

instance: {}

net: {}

reset:
  - name: reason_reset_ctrl_bitvec
    clock: clk_sys
    test_enable: test_en
    
    source:
      por_rst_n: low              # POR (auto-detected, not in bit vector)
      ext_rst_n: low              # bit[0] 
      wdt_rst_n: low              # bit[1]
      i3c_soc_rst: high           # bit[2]
    
    target:
      sys_rst_n:
        polarity: low
        link:
          por_rst_n:
            type: ASYNC_COMB
          ext_rst_n:
            type: ASYNC_COMB
          wdt_rst_n:
            type: ASYNC_COMB
          i3c_soc_rst:
            type: ASYNC_COMB
    
    # Simplified reason configuration
    reason:
      enable: true
      register_clock: clk_32k      # Always-on clock
      output_bus: reason           # Output bit vector name (new unified naming)
      valid_signal: reason_valid   # Valid signal name
      clear_signal: reason_clear   # Software clear signal
)";

        QString netlistPath = createTempFile("test_reset_reason.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_reset_reason.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify new reset reason recording architecture */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "Reset reason recording logic (Sync-clear async-capture sticky flags)"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "async-set + sync-clear only, avoids S+R registers"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "2-cycle clear window after POR release or SW clear pulse"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "Outputs gated by valid signal for proper initialization"));

        /* Verify event normalization */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "Event normalization: convert all sources to LOW-active format"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "wire ext_rst_n_event_n = ext_rst_n"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "wire wdt_rst_n_event_n = wdt_rst_n"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "wire i3c_soc_rst_event_n = ~i3c_soc_rst"));

        /* Verify SW clear synchronizer */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "Synchronize software clear and generate pulse"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "reg swc_d1, swc_d2, swc_d3"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "wire sw_clear_pulse = swc_d2 & ~swc_d3"));

        /* Verify 2-cycle clear controller */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "2-cycle clear controller and valid signal generation"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "reg [1:0]  clr_sr"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "reg        valid_q"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "wire clr_en = |clr_sr"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "clr_sr <= 2'b11"));

        /* Verify simplified sticky flags - NO S+R registers */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "Sticky flags: async-set on event, sync-clear during clear window"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "reg [2:0] flags"));

        /* Verify clean async-set + sync-clear only (no complex sensitivity lists) */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "always @(posedge clk_32k or negedge ext_rst_n_event_n)"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "always @(posedge clk_32k or negedge wdt_rst_n_event_n)"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "always @(posedge clk_32k or negedge i3c_soc_rst_event_n)"));

        /* Verify pure async-set + sync-clear logic */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "flags[0] <= 1'b1;      // Async set on event assert"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "else if (clr_en) begin"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "flags[0] <= 1'b0;      // Sync clear during clear window"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "flags[0] <= flags[0];  // Hold state"));

        /* Verify output gating with new unified naming */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "Output gating: zeros until valid"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign reason_valid = valid_q"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "assign reason = reason_valid ? flags : 3'b0"));

        /* Verify module interface ports with new unified naming */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "input        clk_32k,"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "Always-on clock for reason recording"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "input        reason_clear"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "Software clear signal for reset reason"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "output [2:0] reason,"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "output       reason_valid,"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "Reset reason valid flag (indicates reason output is meaningful)"));

        /* Verify module naming */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "module reason_reset_ctrl_bitvec"));
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegenerateresetlogic.moc"
