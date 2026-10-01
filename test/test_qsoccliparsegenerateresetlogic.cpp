// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/config.h"
#include "common/qsocconsole.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QRegularExpression>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QTextStream>
#include <QtCore>
#include <QtTest>

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
        qInstallMessageHandler(messageOutput);
        QSocConsole::setTeeToMessageHandler(true);
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

    /* A netlist whose only section is `reset:` is complete; the controller
       must generate without port/instance/net scaffolding. */
    void testPureResetNetlistGenerates()
    {
        const QString netlistContent = R"(
reset:
  - name: pure_rst_ctrl
    source:
      por_rst_n:
        active: low
    target:
      sys_rst_n:
        active: low
        link:
          por_rst_n:
            async:
              clock: clk_sys
              stage: 4
)";
        const QString netlistPath    = createTempFile("test_pure_reset.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());
        /* A stale artifact from an earlier run would satisfy the existence
           check even if this generation failed. */
        QFile::remove(
            QDir(projectManager.getOutputPath()).filePath("test_pure_reset/rtl/test_pure_reset.v"));
        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;
            socCliWorker.setup(args, false);
            socCliWorker.run();
        }
        const QString verilogPath
            = QDir(projectManager.getOutputPath()).filePath("test_pure_reset/rtl/test_pure_reset.v");
        QVERIFY(QFile::exists(verilogPath));
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString verilogContent = verilogFile.readAll();
        verilogFile.close();
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "module pure_rst_ctrl"));
    }

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
      peri_rst_n:
        active: low
        link:
          por_rst_n:
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
        QString verilogPath = QDir(projectManager.getOutputPath())
                                  .filePath("test_basic_reset/rtl/test_basic_reset.v");
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
        QString verilogPath
            = QDir(projectManager.getOutputPath()).filePath("test_sync_reset/rtl/test_sync_reset.v");
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
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".rst_in_n(~i3c_soc_rst)"));
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
        QString verilogPath = QDir(projectManager.getOutputPath())
                                  .filePath("test_counter_reset/rtl/test_counter_reset.v");
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
            async:
              clock: clk_sys
              stage: 4
          i3c_soc_rst:
            async:
              clock: clk_sys
              stage: 4
          trig_cpu_rst:
            async:
              clock: clk_sys
              stage: 4
      i3c_rst_n:
        active: low
        link:
          por_rst_n:
          i3c_soc_rst:
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
        QString verilogPath = QDir(projectManager.getOutputPath())
                                  .filePath("test_multi_reset/rtl/test_multi_reset.v");
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
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".rst_in_n(~i3c_soc_rst)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".rst_in_n(~trig_cpu_rst)"));
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
        QString verilogPath = QDir(projectManager.getOutputPath())
                                  .filePath("test_sync_only_reset/rtl/test_sync_only_reset.v");
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
        QString verilogPath = QDir(projectManager.getOutputPath())
                                  .filePath("test_syncnt_reset/rtl/test_syncnt_reset.v");
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
          ext_rst_n:
          wdt_rst_n:
          i3c_soc_rst:

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
        QString verilogPath = QDir(projectManager.getOutputPath())
                                  .filePath("test_reset_reason/rtl/test_reset_reason.v");
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
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/**< Clock inputs */"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "input  wire       reason_clear"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/**< Reset reason clear */"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "output wire [2:0] reason"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "output wire reason_valid"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* reason_valid register */"));

        /* Verify module naming */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "module reason_reset_ctrl_bitvec"));
    }

    void testResetReasonOnlySourcesBecomePorts()
    {
        const QString netlistContent = R"(
port:
  clk_32k:
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
  sys_rst_n:
    direction: output
    type: logic
  reason:
    direction: output
    type: logic [1:0]
  reason_valid:
    direction: output
    type: logic
  reason_clear:
    direction: input
    type: logic

instance: {}

net: {}

reset:
  - name: reason_only_ctrl
    clock: clk_32k
    source:
      por_rst_n:
        active: low
      ext_rst_n:
        active: low
      wdt_rst_n:
        active: high
    target:
      sys_rst_n:
        active: low
        link:
          ext_rst_n:
    reason:
      clock: clk_32k
      output: reason
      valid: reason_valid
      clear: reason_clear
      root_reset: por_rst_n
)";

        const QString netlistPath = createTempFile("test_reset_reason_only.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        const QDir    outputDir(projectManager.getOutputPath());
        const QString verilogPath = outputDir.filePath(
            "test_reset_reason_only/rtl/test_reset_reason_only.v");
        const QString cellPath = outputDir.filePath("qsoc_cell/rtl/qsoc_cell_reset.v");
        QVERIFY(QFile::exists(verilogPath));
        QVERIFY(QFile::exists(cellPath));

        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Root reset and an unlinked recorded source are read only by the recorder */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "input wire por_rst_n, /**< Reset sources */"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "input wire wdt_rst_n, /**< Reset sources */"));

        const QString compiler = QStandardPaths::findExecutable("iverilog");
        if (compiler.isEmpty()) {
            QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("iverilog"));
        }

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString nettypePath = QDir(directory.path()).filePath("nettype.v");
        QFile         nettypeFile(nettypePath);
        QVERIFY(nettypeFile.open(QIODevice::WriteOnly | QIODevice::Text));
        nettypeFile.write("`default_nettype none\n");
        nettypeFile.close();

        QProcess process;
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(
            compiler,
            {"-g2005",
             "-s",
             "reason_only_ctrl",
             "-o",
             QDir(directory.path()).filePath("a.out"),
             nettypePath,
             verilogPath,
             cellPath});
        QVERIFY(process.waitForStarted());
        QVERIFY(process.waitForFinished());
        const QByteArray compilerOutput = process.readAll();
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QVERIFY2(process.exitCode() == 0, compilerOutput.constData());
    }

    void testResetCellFileGeneration()
    {
        QString netlistContent = R"(
# Test netlist for qsoc_cell_reset.v file generation
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
            async:
              clock: clk_sys
              stage: 4
      peri_rst_n:
        active: low
        link:
          wdt_rst_n:
            count:
              clock: clk_sys
              cycle: 255
      sync_rst_n:
        active: low
        link:
          ext_rst:
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

        /* Check if both Verilog file and qsoc_cell_reset.v were generated */
        QString verilogPath
            = QDir(projectManager.getOutputPath()).filePath("test_reset_cell/rtl/test_reset_cell.v");
        QString resetCellPath
            = QDir(projectManager.getOutputPath()).filePath("qsoc_cell/rtl/qsoc_cell_reset.v");

        qDebug() << "Checking files:" << verilogPath << resetCellPath;
        qDebug() << "Verilog exists:" << QFile::exists(verilogPath);
        qDebug() << "Reset cell exists:" << QFile::exists(resetCellPath);
        qDebug() << "Output path:" << projectManager.getOutputPath();

        // List all files in output directory for debugging
        QDir outputDir(projectManager.getOutputPath());
        qDebug() << "Files in output dir:" << outputDir.entryList(QDir::Files);

        QVERIFY(QFile::exists(verilogPath));
        QVERIFY(QFile::exists(resetCellPath));

        /* Read qsoc_cell_reset.v content */
        QFile resetCellFile(resetCellPath);
        QVERIFY(resetCellFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString resetCellContent = resetCellFile.readAll();

        /* Verify qsoc_cell_reset.v header */
        QVERIFY(verifyVerilogContentNormalized(resetCellContent, "@file qsoc_cell_reset.v"));
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

    void test_optional_test_enable()
    {
        // Test 1: No test_enable defined - should use 1'b0 internally
        QString netlistContent1 = R"(
port:
  por_rst_n:
    direction: input
    type: logic
  clk_sys:
    direction: input
    type: logic
  cpu_rst_n:
    direction: output
    type: logic
instance: {}
net: {}
reset:
  - name: test_reset_no_test_en
    source:
      por_rst_n:
        active: low
    target:
      cpu_rst_n:
        active: low
        link:
          por_rst_n:
            async:
              clock: clk_sys
              stage: 4
)";
        QString netlistPath1 = createTempFile("test_reset_no_test_enable.soc_net", netlistContent1);
        QVERIFY(!netlistPath1.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath1;
            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        QString verilogPath1 = QDir(projectManager.getOutputPath())
                                   .filePath(
                                       "test_reset_no_test_enable/rtl/test_reset_no_test_enable.v");
        QVERIFY(QFile::exists(verilogPath1));

        QFile verilogFile1(verilogPath1);
        QVERIFY(verilogFile1.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent1 = verilogFile1.readAll();
        verilogFile1.close();

        // Should NOT have test_enable port
        QVERIFY(!verifyVerilogContentNormalized(verilogContent1, "input wire test_enable"));
        QVERIFY(!verifyVerilogContentNormalized(verilogContent1, "input wire test_en"));
        // Should use 1'b0 internally
        QVERIFY(verifyVerilogContentNormalized(verilogContent1, ".test_enable(1'b0)"));

        // Test 2: test_enable explicitly defined - should use it
        QString netlistContent2 = R"(
port:
  por_rst_n:
    direction: input
    type: logic
  clk_sys:
    direction: input
    type: logic
  my_test_en:
    direction: input
    type: logic
  cpu_rst_n:
    direction: output
    type: logic
instance: {}
net: {}
reset:
  - name: test_reset_with_test_en
    test_enable: my_test_en
    source:
      por_rst_n:
        active: low
    target:
      cpu_rst_n:
        active: low
        link:
          por_rst_n:
            async:
              clock: clk_sys
              stage: 4
)";
        QString netlistPath2
            = createTempFile("test_reset_with_test_enable.soc_net", netlistContent2);
        QVERIFY(!netlistPath2.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath2;
            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        QString verilogPath2
            = QDir(projectManager.getOutputPath())
                  .filePath("test_reset_with_test_enable/rtl/test_reset_with_test_enable.v");
        QVERIFY(QFile::exists(verilogPath2));

        QFile verilogFile2(verilogPath2);
        QVERIFY(verilogFile2.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent2 = verilogFile2.readAll();
        verilogFile2.close();

        // Should have my_test_en port
        QVERIFY(verifyVerilogContentNormalized(verilogContent2, "input wire my_test_en"));
        // Should use my_test_en
        QVERIFY(verifyVerilogContentNormalized(verilogContent2, ".test_enable(my_test_en)"));
    }

    void test_reset_output_win()
    {
        QString netlistContent = R"(
# Test netlist with reset output win mechanism
# rst_apb_n is both a source (referenced by uart) and a target (output)
port:
  por_rst_n:
    direction: input
    type: logic
  sw_rst_n:
    direction: input
    type: logic
  test_en:
    direction: input
    type: logic

instance: {}

net: {}

reset:
  - name: test_reset_output_win
    test_enable: test_en
    source:
      por_rst_n:
        active: low
      sw_rst_n:
        active: low
    target:
      rst_apb_n:
        active: low
        link:
          por_rst_n:
          sw_rst_n:
      rst_uart0_n:
        active: low
        link:
          por_rst_n:
          rst_apb_n:
)";

        QString netlistPath = createTempFile("test_reset_output_win.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        QString verilogPath = QDir(projectManager.getOutputPath())
                                  .filePath("test_reset_output_win/rtl/test_reset_output_win.v");
        QVERIFY(QFile::exists(verilogPath));

        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        // Verify "output win" mechanism: rst_apb_n should only appear as output port
        // Count occurrences of rst_apb_n in port declarations within reset module
        QRegularExpression      moduleRegex("module\\s+test_reset_output_win\\s*\\([^)]*\\)");
        QRegularExpressionMatch moduleMatch = moduleRegex.match(verilogContent);
        QVERIFY(moduleMatch.hasMatch());

        QString            moduleHeader = moduleMatch.captured(0);
        QRegularExpression portRegex("(input|output)\\s+wire\\s+([\\w\\[\\]:]+\\s+)?rst_apb_n\\b");
        QRegularExpressionMatchIterator iterator = portRegex.globalMatch(moduleHeader);

        int  portCount     = 0;
        bool hasInputPort  = false;
        bool hasOutputPort = false;

        while (iterator.hasNext()) {
            QRegularExpressionMatch match = iterator.next();
            portCount++;
            QString portType = match.captured(1);
            if (portType == "input")
                hasInputPort = true;
            if (portType == "output")
                hasOutputPort = true;
        }

        // Should have exactly 1 port declaration for rst_apb_n in reset module
        QCOMPARE(portCount, 1);

        // Should be output port only (not input)
        QVERIFY(hasOutputPort);
        QVERIFY(!hasInputPort);

        // Verify rst_apb_n is used correctly in the logic
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "output wire rst_apb_n"));

        // Verify internal wires are created for rst_apb_n links
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "wire rst_apb_link0_n"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "wire rst_apb_link1_n"));

        // Verify rst_apb_n is assigned from combined links
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "rst_apb_n_combined = rst_apb_link0_n & rst_apb_link1_n"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "assign rst_apb_n = rst_apb_n_combined"));

        // Verify uart0 uses rst_apb_n as a source signal
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "assign rst_uart0_link1_n = rst_apb_n"));

        // Verify module header contains correct ports
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "module test_reset_output_win"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "input  wire por_rst_n"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "input  wire sw_rst_n"));

        // Should NOT have rst_apb_n as input since it's an output (check only in reset module)
        QVERIFY(!verifyVerilogContentNormalized(moduleHeader, "input  wire rst_apb_n"));
    }
    /* A malformed scalar in the reset shape used to abort the process. */
    void test_malformed_reset_shape_is_reported_not_fatal()
    {
        const QString netlistContent = R"(
reset:
  - name: guard_ctl
    source:
      por_n:
        active: low
    target:
      sys_rst_n:
        active: low
        async:
          clock: clk
          stage: abc
        link:
          por_n: ~
)";
        const QString netlistPath    = createTempFile("test_reset_guard.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());
        const QString verilogPath = QDir(projectManager.getOutputPath())
                                        .filePath("test_reset_guard/rtl/test_reset_guard.v");
        QFile::remove(verilogPath);
        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;
            socCliWorker.setup(args, false);
            socCliWorker.run();
        }
        QVERIFY(!QFile::exists(verilogPath));
        QCOMPARE(messageList.filter("Invalid reset configuration:").size(), 1);
    }

    /* Mid-parse errors reject the whole controller, never truncate it. */
    void test_reset_structural_errors_are_rejected_data()
    {
        QTest::addColumn<QString>("stem");
        QTest::addColumn<QString>("netlist");
        QTest::addColumn<QString>("fragment");

        QTest::newRow("second-target-broken") << "rej_second_target" << QString(R"(
reset:
  - name: trunc_ctl
    source:
      por_n:
        active: low
    target:
      first_rst_n:
        active: low
        link:
          por_n: ~
      second_rst_n:
        link:
          por_n: ~
)") << "active";

        QTest::newRow("zero-link-target") << "rej_zero_link" << QString(R"(
reset:
  - name: nolink_ctl
    source:
      por_n:
        active: low
    target:
      dead_rst_n:
        active: low
)") << "requires at least one link";
        QTest::newRow("target-async-stage-zero") << "rej_target_async_stage_zero" << QString(R"(
reset:
  - name: count_ctl
    source:
      por_n:
        active: low
    target:
      rst_o_n:
        active: low
        async:
          clock: clk
          stage: 0
        link:
          por_n: ~
)") << "Reset target 'rst_o_n' async stage must be at least 1, got 0";
        QTest::newRow("target-sync-stage-negative")
            << "rej_target_sync_stage_negative" << QString(R"(
reset:
  - name: count_ctl
    source:
      por_n:
        active: low
    target:
      rst_o_n:
        active: low
        sync:
          clock: clk
          stage: -1
        link:
          por_n: ~
)") << "Reset target 'rst_o_n' sync stage must be at least 1, got -1";
        QTest::newRow("target-count-cycle-zero") << "rej_target_count_cycle_zero" << QString(R"(
reset:
  - name: count_ctl
    source:
      por_n:
        active: low
    target:
      rst_o_n:
        active: low
        count:
          clock: clk
          cycle: 0
        link:
          por_n: ~
)") << "Reset target 'rst_o_n' count cycle must be at least 1, got 0";
        QTest::newRow("link-async-stage-zero") << "rej_link_async_stage_zero" << QString(R"(
reset:
  - name: count_ctl
    source:
      por_n:
        active: low
    target:
      rst_o_n:
        active: low
        link:
          por_n:
            async:
              clock: clk
              stage: 0
)") << "Reset link 'por_n' of target 'rst_o_n' async stage must be at least 1, got 0";
        QTest::newRow("link-sync-stage-zero") << "rej_link_sync_stage_zero" << QString(R"(
reset:
  - name: count_ctl
    source:
      por_n:
        active: low
    target:
      rst_o_n:
        active: low
        link:
          por_n:
            sync:
              clock: clk
              stage: 0
)") << "Reset link 'por_n' of target 'rst_o_n' sync stage must be at least 1, got 0";
        QTest::newRow("link-count-cycle-negative") << "rej_link_count_cycle_negative" << QString(R"(
reset:
  - name: count_ctl
    source:
      por_n:
        active: low
    target:
      rst_o_n:
        active: low
        link:
          por_n:
            count:
              clock: clk
              cycle: -3
)") << "Reset link 'por_n' of target 'rst_o_n' count cycle must be at least 1, got -3";
    }

    void test_reset_structural_errors_are_rejected()
    {
        QFETCH(QString, stem);
        QFETCH(QString, netlist);
        QFETCH(QString, fragment);
        messageList.clear();
        const QString netlistPath = createTempFile(stem + ".soc_net", netlist);
        QVERIFY(!netlistPath.isEmpty());
        const QString verilogPath
            = QDir(projectManager.getOutputPath()).filePath(stem + "/rtl/" + stem + ".v");
        QFile::remove(verilogPath);
        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;
            socCliWorker.setup(args, false);
            socCliWorker.run();
        }
        QVERIFY(!QFile::exists(verilogPath));
        QVERIFY2(
            messageList.join('\n').contains(fragment),
            qPrintable(fragment + " | " + messageList.join('\n').right(600)));
    }

    void test_source_polarity_simulates_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<QString>("active");
        const QStringList kinds = {"async", "sync", "count", "direct", "post"};
        for (const QString &kind : kinds) {
            for (const QString &active : {QStringLiteral("high"), QStringLiteral("low")}) {
                QTest::addRow("%s-%s", qPrintable(kind), qPrintable(active)) << kind << active;
            }
        }
    }

    /* The target must be asserted exactly while the source is at its active
       level, allowing a few clocks of synchronizer or counter latency. */
    void test_source_polarity_simulates()
    {
        QFETCH(QString, kind);
        QFETCH(QString, active);
        const QString compiler = QStandardPaths::findExecutable("iverilog");
        const QString runtime  = QStandardPaths::findExecutable("vvp");
        if (compiler.isEmpty() || runtime.isEmpty()) {
            QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("iverilog and vvp"));
        }

        const QString stem = QStringLiteral("rst_pol_%1_%2").arg(kind, active);
        QString       linkBody;
        QString       targetBody;
        if (kind == "async" || kind == "sync") {
            linkBody = QStringLiteral("            %1: {clock: clk, stage: 2}\n").arg(kind);
        } else if (kind == "count") {
            linkBody = QStringLiteral("            count: {clock: clk, cycle: 3}\n");
        } else if (kind == "post") {
            targetBody = QStringLiteral("        async: {clock: clk, stage: 2}\n");
        }
        const QString netlist     = QStringLiteral(
                                        "reset:\n"
                                        "  - name: %1_ctrl\n"
                                        "    source:\n"
                                        "      rst_src: {active: %2}\n"
                                        "    target:\n"
                                        "      dst_n:\n"
                                        "        active: low\n"
                                        "%3"
                                        "        link:\n"
                                        "          rst_src:\n"
                                        "%4")
                                        .arg(stem, active, targetBody, linkBody);
        const QString netlistPath = createTempFile(stem + ".soc_net", netlist);
        QVERIFY(!netlistPath.isEmpty());
        const QDir    outputDir(projectManager.getOutputPath());
        const QString verilogPath = outputDir.filePath(stem + "/rtl/" + stem + ".v");
        QFile::remove(verilogPath);
        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;
            socCliWorker.setup(args, false);
            socCliWorker.run();
        }
        QVERIFY(QFile::exists(verilogPath));

        /* The diagram inverts a high-active source where the RTL does: at the
           link component input, not between the component and the AND gate. */
        if (kind == "async" || kind == "sync" || kind == "count") {
            QFile typstFile(outputDir.filePath(stem + "/doc/" + stem + "_ctrl.typ"));
            QVERIFY(typstFile.open(QIODevice::ReadOnly | QIODevice::Text));
            const QStringList lines = QString::fromUtf8(typstFile.readAll()).split('\n');
            const QString     stub
                = active == "high"
                      ? QStringLiteral("), \"west\", name: \"rst_src\")")
                      : QStringLiteral("wire.stub(\"dst_n_L0_%1-port-in\"").arg(kind.toUpper());
            QString beforeStub;
            bool    found = false;
            for (int i = 1; i < lines.size(); ++i) {
                if (lines[i].contains("wire.stub(") && lines[i].contains(stub)) {
                    beforeStub = lines[i - 1];
                    found      = true;
                }
            }
            QVERIFY2(found, qPrintable(stub));
            const int bubbles = static_cast<int>(
                lines.filter(QStringLiteral("draw.circle(")).size());
            QCOMPARE(bubbles, active == "high" ? 1 : 0);
            QCOMPARE(beforeStub.contains("draw.circle("), active == "high");
            QVERIFY(lines.join('\n').contains("wire.wire(\"w_dst_n_l0_to_and\""));
        }

        const QString on    = active == "high" ? "1'b1" : "1'b0";
        const QString bench = QStringLiteral(
                                  "`timescale 1ns/1ps\n"
                                  "module tb;\n"
                                  "  localparam LAT = 6;\n"
                                  "  reg clk = 1'b0;\n"
                                  "  reg rst_src = %2;\n"
                                  "  reg asserted = 1'b1;\n"
                                  "  integer since = 0;\n"
                                  "  integer errors = 0;\n"
                                  "  wire dst_n;\n"
                                  "  %1_ctrl dut(%3.rst_src(rst_src), .dst_n(dst_n));\n"
                                  "  always #5 clk = ~clk;\n"
                                  "  always @(negedge clk) begin\n"
                                  "    since = since + 1;\n"
                                  "    if (since > LAT && dst_n !== !asserted) begin\n"
                                  "      errors = errors + 1;\n"
                                  "      $display(\"MISMATCH t=%t src=%b dst_n=%b\", $time, "
                                  "rst_src, dst_n);\n"
                                  "    end\n"
                                  "  end\n"
                                  "  task drive(input a);\n"
                                  "    begin\n"
                                  "      asserted = a;\n"
                                  "      rst_src = a ? %2 : ~%2;\n"
                                  "      since = 0;\n"
                                  "      repeat (12) @(posedge clk);\n"
                                  "    end\n"
                                  "  endtask\n"
                                  "  initial begin\n"
                                  "    drive(1'b1);\n"
                                  "    drive(1'b0);\n"
                                  "    drive(1'b1);\n"
                                  "    drive(1'b0);\n"
                                  "    if (errors == 0) $display(\"TEST_PASS\");\n"
                                  "    else $display(\"TEST_FAIL\");\n"
                                  "    $finish;\n"
                                  "  end\n"
                                  "endmodule\n")
                                  .arg(stem, on, kind == "direct" ? "" : ".clk(clk), ");

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString benchPath = QDir(directory.path()).filePath("tb.v");
        const QString imagePath = QDir(directory.path()).filePath("tb.vvp");
        QFile         benchFile(benchPath);
        QVERIFY(benchFile.open(QIODevice::WriteOnly | QIODevice::Text));
        benchFile.write(bench.toUtf8());
        benchFile.close();

        QProcess build;
        build.setProcessChannelMode(QProcess::MergedChannels);
        build.start(
            compiler,
            {"-g2005",
             "-s",
             "tb",
             "-o",
             imagePath,
             verilogPath,
             outputDir.filePath("qsoc_cell/rtl/qsoc_cell_reset.v"),
             benchPath});
        QVERIFY(build.waitForStarted());
        QVERIFY(build.waitForFinished());
        const QByteArray buildLog = build.readAll();
        QVERIFY2(build.exitCode() == 0, buildLog.constData());

        QProcess simulation;
        simulation.setProcessChannelMode(QProcess::MergedChannels);
        simulation.start(runtime, {imagePath});
        QVERIFY(simulation.waitForStarted());
        QVERIFY(simulation.waitForFinished());
        const QByteArray log = simulation.readAll();
        QVERIFY2(log.contains("TEST_PASS"), log.constData());
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegenerateresetlogic.moc"
