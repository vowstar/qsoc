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

        /* Verify reset controller comment */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* Reset primitive controllers */"));

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
            verilogContent, "reg [3:0] u_sync_reset_ctrl_sync_i3c_soc_rst_cpu_rst_n_ff;"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "always @(posedge clk_sys or posedge i3c_soc_rst)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "if (i3c_soc_rst)"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "u_sync_reset_ctrl_sync_i3c_soc_rst_cpu_rst_n_ff <= 4'b0"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent,
            "assign i3c_soc_rst_cpu_rst_n_sync = test_en ? ~i3c_soc_rst : "
            "u_sync_reset_ctrl_sync_i3c_soc_rst_cpu_rst_n_ff[3]"));
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
            verilogContent, "reg [7:0] u_counter_reset_ctrl_cnt_por_rst_n_cpu_por_rst_n_counter;"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "reg u_counter_reset_ctrl_cnt_por_rst_n_cpu_por_rst_n_counting;"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "always @(posedge clk_sys or negedge por_rst_n)"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "u_counter_reset_ctrl_cnt_por_rst_n_cpu_por_rst_n_counter < 255"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent,
            "assign por_rst_n_cpu_por_rst_n_sync = test_en ? por_rst_n : "
            "(u_counter_reset_ctrl_cnt_por_rst_n_cpu_por_rst_n_counting ? 1'b0 : 1'b1)"));
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
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "reg [3:0] u_multi_reset_ctrl_sync_por_rst_n_cpu_rst_n_ff;"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "reg [3:0] u_multi_reset_ctrl_sync_i3c_soc_rst_cpu_rst_n_ff;"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "reg [3:0] u_multi_reset_ctrl_sync_trig_cpu_rst_cpu_rst_n_ff;"));

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
            verilogContent, "reg [1:0] u_sync_only_reset_ctrl_sync_only_sync_rst_n_peri_rst_n_ff;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "always @(posedge clk_sys)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "if (!sync_rst_n)"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "u_sync_only_reset_ctrl_sync_only_sync_rst_n_peri_rst_n_ff <= 2'b0"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent,
            "assign sync_rst_n_peri_rst_n_sync = test_en ? sync_rst_n : "
            "u_sync_only_reset_ctrl_sync_only_sync_rst_n_peri_rst_n_ff[1]"));
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
            verilogContent, "reg [2:0] u_syncnt_reset_ctrl_syncnt_trig_rst_dma_rst_n_sync_ff;"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "reg [7:0] u_syncnt_reset_ctrl_syncnt_trig_rst_dma_rst_n_counter;"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "u_syncnt_reset_ctrl_syncnt_trig_rst_dma_rst_n_counter < 15"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "module syncnt_reset_ctrl"));
    }

    void testResetReasonRecording()
    {
        QString netlistContent = R"(
# Test netlist with reset reason recording feature
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
  sys_rst_n:
    direction: output
    type: logic
  last_reset_reason:
    direction: output
    type: logic [2:0]
  test_en:
    direction: input
    type: logic
  reason_clear:
    direction: input
    type: logic

instance: {}

net: {}

reset:
  - name: reason_reset_ctrl
    clock: clk_sys
    test_enable: test_en
    record_reset_reason: true
    aon_clock: clk_32k
    por_signal: por_rst_n
    reason_bus: last_reset_reason
    reason_clear: reason_clear
    source:
      por_rst_n: low
      ext_rst_n: low
      wdt_rst_n: low
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

        /* Verify reset reason recording logic (Per-source async-set flops) */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "Reset reason recording logic (Per-source async-set flops)"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "Each reset source drives async-set of a capture flop"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "POR encoding: 0, Sources encoding: 1-3"));

        /* Verify per-source async-set flag registers */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "reg rst_reason_flag_0"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "reg rst_reason_flag_1"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "reg rst_reason_flag_2"));

        /* Verify async-set logic for each source */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "always @(posedge clk_32k or negedge por_rst_n"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "always @(posedge clk_32k or negedge ext_rst_n"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "always @(posedge clk_32k or negedge wdt_rst_n"));

        /* Verify priority encoder logic */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "Priority encoder: Higher index = higher priority"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "reg [1:0] last_reset_reason_reg"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "if (rst_reason_flag_2)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "last_reset_reason_reg <= 2'd3"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "else if (rst_reason_flag_1)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "last_reset_reason_reg <= 2'd2"));

        /* Verify output assignment */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "assign last_reset_reason = last_reset_reason_reg"));

        /* Verify external clear signal support */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "posedge reason_clear"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "External clear"));

        /* Verify module naming */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "module reason_reset_ctrl"));
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegenerateresetlogic.moc"
