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
# Test netlist with basic reset controller (component-based architecture)
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
      por_rst_n:
        active: low
    target:
      cpu_rst_n:
        active: low
        link:
          por_rst_n:
            source: por_rst_n
      peri_rst_n:
        active: low
        link:
          por_rst_n:
            source: por_rst_n
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

        /* Verify direct wire connections (no components used) */
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "assign cpu_rst_link0_n = por_rst_n"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "assign peri_rst_link0_n = por_rst_n"));

        /* Verify target output assignments */
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "assign cpu_rst_n = cpu_rst_link0_n"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "assign peri_rst_n = peri_rst_link0_n"));
    }

    void testSyncResetController()
    {
        QString netlistContent = R"(
# Test netlist with sync reset controller (component-based architecture)
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
      i3c_soc_rst:
        active: high
    target:
      cpu_rst_n:
        active: low
        link:
          i3c_soc_rst:
            source: i3c_soc_rst
            async:
              clock: clk_sys
              stage: 4
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

        /* Verify component-based async reset synchronizer implementation */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "module sync_reset_ctrl"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_rst_sync #("));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".STAGE(4)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "i_cpu_rst_link0_async"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".rst_in_n   (i3c_soc_rst)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".rst_out_n  (cpu_rst_link0_n)"));
    }

    void testCounterResetController()
    {
        QString netlistContent = R"(
# Test netlist with counter reset controller (component-based architecture)
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
      por_rst_n:
        active: low
    target:
      cpu_por_rst_n:
        active: low
        link:
          por_rst_n:
            source: por_rst_n
            count:
              clock: clk_sys
              cycle: 255
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

        /* Verify component-based ASYNC_COUNT reset implementation */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_rst_count #("));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".CYCLE(255)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "i_cpu_por_rst_link0_count"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".clk(clk_sys)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".rst_in_n(por_rst_n)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".test_enable(test_en)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".rst_out_n(cpu_por_rst_link0_n)"));
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
      por_rst_n:
        active: low
      i3c_soc_rst:
        active: high
      trig_cpu_rst:
        active: high
    target:
      cpu_rst_n:
        active: low
        link:
          por_rst_n:
            source: por_rst_n
            async:
              clock: clk_sys
              stage: 4
          i3c_soc_rst:
            source: i3c_soc_rst
            async:
              clock: clk_sys
              stage: 4
          trig_cpu_rst:
            source: trig_cpu_rst
            async:
              clock: clk_sys
              stage: 4
      i3c_rst_n:
        active: low
        link:
          por_rst_n:
            source: por_rst_n
          i3c_soc_rst:
            source: i3c_soc_rst
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

        /* Verify component-based ASYNC_SYNC implementations using qsoc_rst_sync */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_rst_sync #("));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".STAGE(4)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "i_cpu_rst_link0_async"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "i_cpu_rst_link1_async"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "i_cpu_rst_link2_async"));

        /* Verify wire declarations for link signals */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "wire cpu_rst_link0_n;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "wire cpu_rst_link1_n;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "wire cpu_rst_link2_n;"));

        /* Verify AND logic for combining multiple reset sources */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent,
            "cpu_rst_n_combined = cpu_rst_link0_n & cpu_rst_link1_n & cpu_rst_link2_n"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "i3c_rst_n_combined = i3c_rst_link0_n & i3c_rst_link1_n"));

        /* Verify polarity handling in direct assign statements */
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "assign i3c_rst_link1_n = ~i3c_soc_rst"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".rst_in_n(i3c_soc_rst)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".rst_in_n(trig_cpu_rst)"));
    }

    void testSyncOnlyReset()
    {
        QString netlistContent = R"(
# Test netlist with sync pipeline reset controller
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
      sync_rst_n:
        active: low
    target:
      peri_rst_n:
        active: low
        link:
          sync_rst_n:
            source: sync_rst_n
            sync:
              clock: clk_sys
              stage: 2
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

        /* Verify component-based SYNC_ONLY reset implementation using qsoc_rst_pipe */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_rst_pipe #("));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".STAGE(2)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "i_peri_rst_link0_sync"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".clk(clk_sys)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".rst_in_n(sync_rst_n)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".test_enable(test_en)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".rst_out_n(peri_rst_link0_n)"));
    }

    void testAsyncSyncntReset()
    {
        QString netlistContent = R"(
# Test netlist with async+sync+count reset controller
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
      trig_rst:
        active: low
    target:
      dma_rst_n:
        active: low
        link:
          trig_rst:
            source: trig_rst
            async:
              clock: clk_sys
              stage: 3
            count:
              clock: clk_sys
              cycle: 15
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

        /* Verify component-based reset implementation (async takes priority over count) */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_rst_sync #("));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".STAGE(3)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "i_dma_rst_link0_async"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".clk(clk_sys)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".rst_in_n(trig_rst)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".test_enable(test_en)"));
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
      por_rst_n:
        active: low               # POR (auto-detected, not in bit vector)
      ext_rst_n:
        active: low               # bit[0]
      wdt_rst_n:
        active: low               # bit[1]
      i3c_soc_rst:
        active: high              # bit[2]

    target:
      sys_rst_n:
        active: low
        link:
          por_rst_n:
            source: por_rst_n
          ext_rst_n:
            source: ext_rst_n
          wdt_rst_n:
            source: wdt_rst_n
          i3c_soc_rst:
            source: i3c_soc_rst

    # Simplified reason configuration
    reason:
      clock: clk_32k               # Always-on clock for recording logic
      output: reason               # Output bit vector name
      valid: reason_valid          # Valid signal name
      clear: reason_clear          # Software clear signal
      root_reset: por_rst_n        # Root reset signal for async clear (explicitly specified)
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

        /* Verify simplified reset reason flags - NO S+R registers */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "Reset reason flags generation using generate for loop"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "reg [2:0] flags"));

        /* Verify event vector for generate block */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "Event vector for generate block"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "wire [2:0] src_event_n"));

        /* Verify generate block for reset reason flags */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "genvar reason_idx;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "generate"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent,
            "for (reason_idx = 0; reason_idx < 3; reason_idx = reason_idx + 1) begin : "
            "gen_reason"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "always @(posedge clk_32k or negedge src_event_n[reason_idx])"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "endgenerate"));

        /* Verify pure async-set + sync-clear logic within generate block (no else clause) */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "flags[reason_idx] <= 1'b1;      /* Async set on event assert"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "else if (clr_en) begin"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "flags[reason_idx] <= 1'b0;      /* Sync clear during clear window"));

        /* Verify output gating with new unified naming */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "Output gating: zeros until valid"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign reason_valid = valid_q"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "assign reason = reason_valid ? flags : 3'b0"));

        /* Verify module interface ports with new unified naming */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "input  wire       clk_32k,"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* Clock inputs */"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "input  wire       reason_clear"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* Reset reason clear */"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "output wire [2:0] reason"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "output wire reason_valid"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* reason_valid register */"));

        /* Verify module naming */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "module reason_reset_ctrl_bitvec"));
    }

    void testResetCellFileGeneration()
    {
        QString netlistContent = R"(
# Test netlist for reset_cell.v file generation
port:
  clk_sys:
    direction: input
    type: logic
  por_rst_n:
    direction: input
    type: logic
  ext_rst:
    direction: input
    type: logic
  wdt_rst_n:
    direction: input
    type: logic
  cpu_rst_n:
    direction: output
    type: logic
  peri_rst_n:
    direction: output
    type: logic
  sync_rst_n:
    direction: output
    type: logic
  test_en:
    direction: input
    type: logic

reset:
  - name: cell_test_reset_ctrl
    clock: clk_sys
    test_enable: test_en
    source:
      por_rst_n:
        active: low
      ext_rst:
        active: high
      wdt_rst_n:
        active: low
    target:
      cpu_rst_n:
        active: low
        link:
          por_rst_n:
            source: por_rst_n
            async:
              clock: clk_sys
              stage: 4
      peri_rst_n:
        active: low
        link:
          wdt_rst_n:
            source: wdt_rst_n
            count:
              clock: clk_sys
              cycle: 255
      sync_rst_n:
        active: low
        link:
          ext_rst:
            source: ext_rst
            sync:
              clock: clk_sys
              stage: 2
)";

        QString netlistPath = createTempFile("test_reset_cell.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        /* Check if both Verilog file and reset_cell.v were generated */
        QString verilogPath   = QDir(projectManager.getOutputPath()).filePath("test_reset_cell.v");
        QString resetCellPath = QDir(projectManager.getOutputPath()).filePath("reset_cell.v");

        qDebug() << "Checking files:" << verilogPath << resetCellPath;
        qDebug() << "Verilog exists:" << QFile::exists(verilogPath);
        qDebug() << "Reset cell exists:" << QFile::exists(resetCellPath);
        qDebug() << "Output path:" << projectManager.getOutputPath();

        // List all files in output directory for debugging
        QDir outputDir(projectManager.getOutputPath());
        qDebug() << "Files in output dir:" << outputDir.entryList(QDir::Files);

        QVERIFY(QFile::exists(verilogPath));
        QVERIFY(QFile::exists(resetCellPath));

        /* Read reset_cell.v content */
        QFile resetCellFile(resetCellPath);
        QVERIFY(resetCellFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString resetCellContent = resetCellFile.readAll();

        /* Verify reset_cell.v header */
        QVERIFY(verifyVerilogContentNormalized(resetCellContent, "@file reset_cell.v"));
        QVERIFY(verifyVerilogContentNormalized(
            resetCellContent, "Template reset cells for QSoC reset primitives"));
        QVERIFY(verifyVerilogContentNormalized(
            resetCellContent, "Auto-generated template file. Generated by qsoc"));
        QVERIFY(
            verifyVerilogContentNormalized(resetCellContent, "CAUTION: Please replace the templates"));

        /* Verify qsoc_rst_sync module */
        QVERIFY(verifyVerilogContentNormalized(resetCellContent, "module qsoc_rst_sync"));
        QVERIFY(verifyVerilogContentNormalized(resetCellContent, "parameter integer STAGE = 3"));
        QVERIFY(verifyVerilogContentNormalized(resetCellContent, "input  wire clk"));
        QVERIFY(verifyVerilogContentNormalized(resetCellContent, "input  wire rst_in_n"));
        QVERIFY(verifyVerilogContentNormalized(resetCellContent, "input  wire test_enable"));
        QVERIFY(verifyVerilogContentNormalized(resetCellContent, "output wire rst_out_n"));

        /* Verify qsoc_rst_pipe module */
        QVERIFY(verifyVerilogContentNormalized(resetCellContent, "module qsoc_rst_pipe"));
        QVERIFY(verifyVerilogContentNormalized(resetCellContent, "parameter integer STAGE = 4"));

        /* Verify qsoc_rst_count module */
        QVERIFY(verifyVerilogContentNormalized(resetCellContent, "module qsoc_rst_count"));
        QVERIFY(verifyVerilogContentNormalized(resetCellContent, "parameter integer CYCLE"));

        /* Verify clean timescale */
        QVERIFY(verifyVerilogContentNormalized(resetCellContent, "`timescale 1ns / 1ps"));

        /* Verify that the main reset controller uses the generated modules */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();

        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_rst_sync #("));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_rst_pipe #("));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_rst_count #("));
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegenerateresetlogic.moc"
