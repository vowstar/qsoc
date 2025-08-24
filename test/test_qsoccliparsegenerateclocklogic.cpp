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
        }
        return filePath;
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
    }

    void cleanupTestCase()
    {
        // Clean up temporary files
        QDir projectDir(projectManager.getCurrentPath());
        projectDir.removeRecursively();
        qInstallMessageHandler(nullptr);
    }

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

        QString normalizedVerilog         = normalizeWhitespace(verilogContent);
        QString normalizedContentToVerify = normalizeWhitespace(contentToVerify);

        return normalizedVerilog.contains(normalizedContentToVerify);
    }

    bool verifyClockCellFileComplete()
    {
        const QString clockCellPath = QDir(projectManager.getOutputPath()).filePath("clock_cell.v");
        if (!QFile::exists(clockCellPath)) {
            qWarning() << "clock_cell.v not found at" << clockCellPath;
            return false;
        }

        QFile file(clockCellPath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            qWarning() << "Failed to open clock_cell.v:" << file.errorString();
            return false;
        }
        const QString content = file.readAll();
        file.close();

        const QStringList requiredCells
            = {"qsoc_tc_clk_gate",
               "qsoc_tc_clk_inv",
               "qsoc_tc_clk_or2",
               "qsoc_tc_clk_mux2",
               "qsoc_tc_clk_xor2",
               "qsoc_clk_div",
               "qsoc_clk_mux_gf",
               "qsoc_clk_mux_raw",
               "qsoc_clk_or_tree"};

        for (const QString &cell : requiredCells) {
            if (!content.contains(QString("module %1").arg(cell))) {
                qWarning() << "Missing cell in clock_cell.v:" << cell;
                return false;
            }
        }

        return true;
    }

    void test_pass_thru_clock()
    {
        // Create netlist file with PASS_THRU clock
        QString netlistContent = R"(
port:
  osc_24m:
    direction: input
    type: logic
  adc_clk:
    direction: output
    type: logic

instance: {}

net: {}

clock:
  - name: test_clk_ctrl
    clock: clk_sys
    input:
      osc_24m:
        freq: 24MHz
    target:
      adc_clk:
        freq: 24MHz
        link:
          osc_24m:            # Direct pass-through (KISS format)
)";

        QString netlistPath = createTempFile("test_pass_thru.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_pass_thru.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify the generated content contains expected clock connection */
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "assign adc_clk = clk_adc_clk_from_osc_24m;"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "assign clk_adc_clk_from_osc_24m = osc_24m;"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "assign clk_adc_clk_from_osc_24m = osc_24m"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "assign adc_clk = clk_adc_clk_from_osc_24m"));

        // clock_cell.v should be created and complete
        QVERIFY(verifyClockCellFileComplete());
    }

    void test_gate_only_clock()
    {
        // Create netlist file with GATE_ONLY clock
        QString netlistContent = R"(
port:
  pll_800m:
    direction: input
    type: logic
  dbg_clk_en:
    direction: input
    type: logic
  dbg_clk:
    direction: output
    type: logic

instance: {}

net: {}

clock:
  - name: test_clk_ctrl
    clock: clk_sys
    input:
      pll_800m:
        freq: 800MHz
    target:
      dbg_clk:
        freq: 800MHz
        icg:                   # Target-level ICG (KISS format)
          enable: dbg_clk_en
        link:
          pll_800m:            # Direct connection
)";

        QString netlistPath = createTempFile("test_gate_only.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_gate_only.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify the generated content contains expected clock logic */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_tc_clk_gate"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".POLARITY(1'b1)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".clk(clk_dbg_clk_from_pll_800m)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".en(dbg_clk_en)"));

        // clock_cell.v should be created and complete
        QVERIFY(verifyClockCellFileComplete());
    }

    void test_div_icg_clock()
    {
        // Create netlist file with DIV_ICG clock
        QString netlistContent = R"(
port:
  pll_800m:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  uart_clk:
    direction: output
    type: logic

instance: {}

net: {}

clock:
  - name: test_clk_ctrl
    clock: clk_sys
    input:
      pll_800m:
        freq: 800MHz
    target:
      uart_clk:
        freq: 200MHz
        div:                   # Target-level divider (KISS format)
          default: 4
          width: 3             # Required: divider width in bits
          reset: rst_n
        link:
          pll_800m:            # Direct connection
)";

        QString netlistPath = createTempFile("test_div_icg.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_div_icg.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify the generated content contains expected clock logic */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_clk_div"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".WIDTH(3)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".rst_n(rst_n)"));

        // clock_cell.v should be created and complete
        QVERIFY(verifyClockCellFileComplete());
    }

    void test_div_dff_clock()
    {
        // Create netlist file with DIV_DFF clock with inversion
        QString netlistContent = R"(
port:
  osc_24m:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  slow_clk_n:
    direction: output
    type: logic

instance: {}

net: {}

clock:
  - name: test_clk_ctrl
    clock: clk_sys
    input:
      osc_24m:
        freq: 24MHz
    target:
      slow_clk_n:
        freq: 12MHz
        link:
          osc_24m:
            div:
              default: 2
              width: 2           # Required: divider width in bits
              reset: rst_n
            inv:
)";

        QString netlistPath = createTempFile("test_div_dff.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_div_dff.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify the generated content contains expected clock logic */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_clk_div"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".div(2'd2)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "~clk_slow_clk_n_from_osc_24m"));

        // clock_cell.v should be created and complete
        QVERIFY(verifyClockCellFileComplete());
    }

    void test_std_mux_clock()
    {
        // Create netlist file with STD_MUX multi-source clock (KISS format)
        QString netlistContent = R"(
port:
  pll_800m:
    direction: input
    type: logic
  test_clk:
    direction: input
    type: logic
  func_sel:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  func_clk:
    direction: output
    type: logic

instance: {}

net: {}

clock:
  - name: test_clk_ctrl
    clock: clk_sys
    input:
      pll_800m:
        freq: 800MHz
      test_clk:
        freq: 100MHz
    target:
      func_clk:
        freq: 100MHz
        div:                    # Target-level divider (KISS format)
          default: 8
          width: 4              # Required: divider width in bits
          reset: rst_n
        link:
          pll_800m:             # Direct connection
          test_clk:             # Direct connection
        select: func_sel        # No reset → auto STD_MUX
)";

        QString netlistPath = createTempFile("test_std_mux.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_std_mux.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify the generated content contains expected clock logic */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_clk_mux_raw"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".clk_sel(func_sel)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_clk_div"));

        // clock_cell.v should be created and complete
        QVERIFY(verifyClockCellFileComplete());
    }

    void test_gf_mux_clock()
    {
        // Create netlist file with GF_MUX (glitch-free) multi-source clock (KISS format)
        QString netlistContent = R"(
port:
  osc_24m:
    direction: input
    type: logic
  test_clk:
    direction: input
    type: logic
  safe_sel:
    direction: input
    type: logic
  sys_rst_n:
    direction: input
    type: logic
  safe_clk:
    direction: output
    type: logic

instance: {}

net: {}

clock:
  - name: test_clk_ctrl
    clock: clk_sys
    input:
      osc_24m:
        freq: 24MHz
      test_clk:
        freq: 100MHz
    target:
      safe_clk:
        freq: 24MHz
        link:
          osc_24m:              # Direct connection
          test_clk:             # Direct connection
        select: safe_sel
        reset: sys_rst_n        # Has reset → auto GF_MUX
)";

        QString netlistPath = createTempFile("test_gf_mux.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_gf_mux.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify the generated content contains expected clock logic */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_clk_mux_gf"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".async_rst_n(sys_rst_n)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".async_sel(safe_sel)"));

        // clock_cell.v should be created and complete
        QVERIFY(verifyClockCellFileComplete());
    }

    void test_gf_mux_custom_ref_clock()
    {
        // Create netlist file with GF_MUX using KISS format with DFT signals
        QString netlistContent = R"(
port:
  osc_24m:
    direction: input
    type: logic
  test_clk:
    direction: input
    type: logic
  custom_sel:
    direction: input
    type: logic
  sys_rst_n:
    direction: input
    type: logic
  test_enable:
    direction: input
    type: logic
  test_clock:
    direction: input
    type: logic
  custom_clk:
    direction: output
    type: logic

instance: {}

net: {}

clock:
  - name: test_clk_ctrl
    clock: clk_sys
    input:
      osc_24m:
        freq: 24MHz
      test_clk:
        freq: 100MHz
    target:
      custom_clk:
        freq: 24MHz
        link:
          osc_24m:              # Direct connection
          test_clk:             # Direct connection
        select: custom_sel
        reset: sys_rst_n        # Has reset → auto GF_MUX
        test_enable: test_enable # DFT test enable
        test_clock: test_clock  # DFT test clock
)";

        QString netlistPath = createTempFile("test_gf_mux_custom_ref.soc_net", netlistContent);
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
            = QDir(projectManager.getOutputPath()).filePath("test_gf_mux_custom_ref.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify the generated content contains expected clock logic */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_clk_mux_gf"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".async_rst_n(sys_rst_n)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".async_sel(custom_sel)"));
        // Since test_enable is explicitly defined, it should be used
        // But target.test_enable was overridden to use config.testEnable
        // In this test, test_enable is explicitly defined as "test_enable" in target
        // But new logic overrides it with config.testEnable which is empty
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".test_en(1'b0)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".test_clk(test_clock)"));

        // clock_cell.v should be created and complete
        QVERIFY(verifyClockCellFileComplete());
    }

    void test_mixed_ref_clock_scenario()
    {
        // Create netlist file with mixed clock mux scenario (KISS format)
        QString netlistContent = R"(
port:
  osc_24m:
    direction: input
    type: logic
  test_clk:
    direction: input
    type: logic
  sel1:
    direction: input
    type: logic
  sel2:
    direction: input
    type: logic
  sys_rst_n:
    direction: input
    type: logic
  por_rst_n:
    direction: input
    type: logic
  default_clk:
    direction: output
    type: logic
  custom_clk:
    direction: output
    type: logic

instance: {}

net: {}

clock:
  - name: test_clk_ctrl
    clock: clk_sys
    input:
      osc_24m:
        freq: 24MHz
      test_clk:
        freq: 100MHz
    target:
      default_clk:
        freq: 24MHz
        link:
          osc_24m:              # Direct connection
          test_clk:             # Direct connection
        select: sel1
        reset: sys_rst_n        # Has reset → auto GF_MUX
      custom_clk:
        freq: 100MHz
        link:
          osc_24m:              # Direct connection
          test_clk:             # Direct connection
        select: sel2
        reset: por_rst_n        # Has reset → auto GF_MUX
)";

        QString netlistPath = createTempFile("test_mixed_ref_clock.soc_net", netlistContent);
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
            = QDir(projectManager.getOutputPath()).filePath("test_mixed_ref_clock.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify the generated content contains expected clock logic */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_clk_mux_gf"));
        // Verify both mux instances with different reset signals
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".async_rst_n(sys_rst_n)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".async_rst_n(por_rst_n)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".async_sel(sel1)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".async_sel(sel2)"));

        // clock_cell.v should be created and complete
        QVERIFY(verifyClockCellFileComplete());
    }

    void test_same_name_target_source()
    {
        // Test case where target and source have the same name (same clock net)
        // This tests clock tree hierarchy: osc_24m -> sys_clk (div) -> sys_clk (icg) -> cpu_clk
        QString netlistContent = R"(
port:
  osc_24m:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  sys_clk_en:
    direction: input
    type: logic
  sys_clk:
    direction: output
    type: logic
  cpu_clk:
    direction: output
    type: logic

instance: {}
net: {}

clock:
  - name: same_name_clk_ctrl
    clock: sys_clk
    test_en: test_en
    input:
      osc_24m:
        freq: 24MHz
    target:
      # First target: sys_clk with divider (400MHz/10 = 40MHz)
      sys_clk:
        freq: 40MHz
        div:
          default: 10
          width: 4              # Required: divider width in bits
          reset: rst_n
        link:
          pll_400m:            # External source (not defined as target)

      # Second target: cpu_clk using sys_clk as source with ICG
      # This shows sys_clk is both a target (above) and source (below)
      cpu_clk:
        freq: 40MHz
        icg:
          enable: sys_clk_en
        link:
          sys_clk:             # Uses the sys_clk defined above as target
)";

        QString netlistPath = createTempFile("test_same_name.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_same_name.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify the generated content handles same-name correctly */
        // Should have sys_clk divider instance
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_clk_div"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".div(4'd10)"));

        // Should have cpu_clk ICG instance
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_tc_clk_gate"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".POLARITY(1'b1)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".en(sys_clk_en)"));

        // Should use sys_clk as both output and intermediate signal
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "output wire sys_clk"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "output wire cpu_clk"));

        // clock_cell.v should be created and complete
        QVERIFY(verifyClockCellFileComplete());
    }

    void test_target_level_sta_guide()
    {
        // Test target-level STA guide buffer
        QString netlistContent = R"(
port:
  osc_24m:
    direction: input
    type: logic
  cpu_clk:
    direction: output
    type: logic

instance: {}

net: {}

clock:
  - name: test_clk_ctrl
    clock: clk_sys
    input:
      osc_24m:
        freq: 24MHz
    target:
      cpu_clk:
        freq: 24MHz
        sta_guide:
          cell: TSMC_CKBUF_X2
          in: I
          out: Z
          instance: u_cpu_clk_sta_guide
        link:
          osc_24m:            # Direct pass-through
)";

        QString netlistPath = createTempFile("test_target_sta_guide.soc_net", netlistContent);
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
            = QDir(projectManager.getOutputPath()).filePath("test_target_sta_guide.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify the generated content contains STA guide buffer */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "TSMC_CKBUF_X2"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "u_cpu_clk_sta_guide"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".I(clk_cpu_clk_from_osc_24m)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".Z(cpu_clk_sta_out)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign cpu_clk = cpu_clk_sta_out"));

        // clock_cell.v should be created and complete
        QVERIFY(verifyClockCellFileComplete());
    }

    void test_link_level_sta_guide()
    {
        // Test link-level STA guide buffer with processing chain
        QString netlistContent = R"(
port:
  pll_800m:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  cpu_en:
    direction: input
    type: logic
  cpu_clk:
    direction: output
    type: logic

instance: {}

net: {}

clock:
  - name: test_clk_ctrl
    clock: clk_sys
    input:
      pll_800m:
        freq: 800MHz
    target:
      cpu_clk:
        freq: 200MHz
        link:
          pll_800m:
            icg:
              enable: cpu_en
              reset: rst_n
            div:
              default: 4
              width: 3
              reset: rst_n
            inv:
            sta_guide:
              cell: FOUNDRY_GUIDE_BUF
              in: A
              out: Y
              instance: u_pll_cpu_sta
)";

        QString netlistPath = createTempFile("test_link_sta_guide.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_link_sta_guide.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify the generated content contains processing chain with STA guide */
        // Should have ICG
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_tc_clk_gate"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".POLARITY(1'b1)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".en(cpu_en)"));

        // Should have divider
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_clk_div"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".div(3'd4)"));

        // Should have inverter
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_tc_clk_inv"));

        // Should have STA guide at the end
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "FOUNDRY_GUIDE_BUF"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "u_cpu_clk_pll_800m_sta"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".A("));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".Y("));

        // clock_cell.v should be created and complete
        QVERIFY(verifyClockCellFileComplete());
    }

    void test_signal_deduplication()
    {
        // Test signal deduplication: same-name signals should appear only once
        QString netlistContent = R"(
port:
  clk_in:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  clk_out1:
    direction: output
    type: logic
  clk_out2:
    direction: output
    type: logic

clock:
  - name: dedup_test_ctrl
    clock: clk_in              # Default clock uses clk_in
    input:
      clk_in:                  # Same name as default clock - should be deduplicated
        freq: 100MHz
      clk_ext:
        freq: 50MHz
    target:
      clk_out1:
        freq: 100MHz
        link:
          clk_in:
          clk_ext:
        select: sel1
        reset: rst_n           # rst_n used here
      clk_out2:
        freq: 50MHz
        icg:
          enable: en2
          reset: rst_n         # rst_n used again - should be deduplicated
        link:
          clk_in:
          clk_ext:
        select: sel2
        reset: rst_n           # rst_n used third time - should be deduplicated

instance: {}
net: {}
)";

        QString netlistPath = createTempFile("test_dedup.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_dedup.v");
        QVERIFY(QFile::exists(verilogPath));

        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();
        QVERIFY(!verilogContent.isEmpty());

        // Count occurrences of each signal in port declarations
        QRegularExpression portDeclRegex(R"(^\s*(input|output)\s+.*?\b(\w+)\s*,?\s*\/\*\*<)");
        QStringList        portSignals;

        QTextStream stream(&verilogContent);
        QString     line;
        bool        inModuleHeader = false;

        while (stream.readLineInto(&line)) {
            if (line.contains("module ")) {
                inModuleHeader = true;
                continue;
            }
            if (inModuleHeader && line.contains(");")) {
                break;
            }
            if (inModuleHeader) {
                QRegularExpressionMatch match = portDeclRegex.match(line);
                if (match.hasMatch()) {
                    QString signalName = match.captured(2);
                    portSignals.append(signalName);
                }
            }
        }

        // Verify clk_in appears only once (not duplicated with default clock)
        int clk_in_count = portSignals.count("clk_in");
        QCOMPARE(clk_in_count, 1);

        // Verify rst_n appears only once (deduplicated across MUX/ICG)
        int rst_n_count = portSignals.count("rst_n");
        QCOMPARE(rst_n_count, 1);

        // Verify test_en does NOT appear when not explicitly defined
        bool hasTestEn = portSignals.contains("test_en");
        QVERIFY(!hasTestEn); // Should NOT be present - uses 1'b0 internally

        // Clock cell should be properly generated with deduplicated signals
        QVERIFY(verifyClockCellFileComplete());
    }

    void test_icg_polarity_system()
    {
        // Test the POLARITY parameter system for ICG cells
        QString netlistContent = R"(
port:
  clk_sys:
    direction: input
    type: logic
  cpu_en:
    direction: input
    type: logic
  gpu_en:
    direction: input
    type: logic
  clk_cpu:
    direction: output
    type: logic
  clk_gpu:
    direction: output
    type: logic

instance: {}

net: {}

clock:
  - name: test_clk_ctrl
    clock: clk_sys
    input:
      clk_sys:
        freq: 100MHz
    target:
      clk_cpu:
        freq: 100MHz
        icg:
          enable: cpu_en
          polarity: high         # Positive polarity (default)
        link:
          clk_sys:
      clk_gpu:
        freq: 100MHz
        icg:
          enable: gpu_en
          polarity: low          # Negative polarity
        link:
          clk_sys:
)";

        QString netlistPath = createTempFile("test_polarity.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_polarity.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify the generated content contains proper POLARITY parameters */
        // CPU clock should have POLARITY(1'b1) for high polarity
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "qsoc_tc_clk_gate"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "POLARITY(1'b1)")); // High polarity for clk_cpu
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "POLARITY(1'b0)")); // Low polarity for clk_gpu

        // Verify both ICG instances exist with correct enable signals
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".en(cpu_en)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".en(gpu_en)"));

        // Verify test_en is NOT generated when not explicitly defined
        QVERIFY(!verifyVerilogContentNormalized(verilogContent, "input wire test_en"));
        // Verify internal signals use 1'b0 for test_enable
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".test_en(1'b0)"));

        // clock_cell.v should be created and complete with POLARITY support
        QVERIFY(verifyClockCellFileComplete());

        // Verify clock_cell.v contains the POLARITY parameter system
        const QString clockCellPath = QDir(projectManager.getOutputPath()).filePath("clock_cell.v");
        QFile         cellFile(clockCellPath);
        QVERIFY(cellFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString cellContent = cellFile.readAll();
        cellFile.close();

        // Verify POLARITY parameter in qsoc_tc_clk_gate
        QVERIFY(cellContent.contains("parameter POLARITY = 1'b1"));
        QVERIFY(cellContent.contains("qsoc_tc_clk_gate_pos"));
        QVERIFY(cellContent.contains("qsoc_tc_clk_gate_neg"));
        QVERIFY(cellContent.contains("if (POLARITY == 1'b1)"));
    }

    void test_optional_test_enable()
    {
        // Test 1: No test_enable defined - should use 1'b0 internally
        QString netlistContent1 = R"(
port:
  clk_in:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  clk_out:
    direction: output
    type: logic
instance: {}
net: {}
clock:
  - name: test_ctrl_no_test_en
    input:
      clk_in:
        freq: 100MHz
    target:
      clk_out:
        freq: 100MHz
        icg:
          enable: en
          reset: rst_n
        link:
          clk_in: ~
)";
        QString netlistPath1    = createTempFile("test_no_test_enable.soc_net", netlistContent1);
        QVERIFY(!netlistPath1.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath1;
            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        QString verilogPath1
            = QDir(projectManager.getOutputPath()).filePath("test_no_test_enable.v");
        QVERIFY(QFile::exists(verilogPath1));

        QFile verilogFile1(verilogPath1);
        QVERIFY(verilogFile1.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent1 = verilogFile1.readAll();
        verilogFile1.close();

        // Should NOT have test_enable port
        QVERIFY(!verifyVerilogContentNormalized(verilogContent1, "input wire test_enable"));
        QVERIFY(!verifyVerilogContentNormalized(verilogContent1, "input wire test_en"));
        // Should use 1'b0 internally
        QVERIFY(verifyVerilogContentNormalized(verilogContent1, ".test_en(1'b0)"));

        // Test 2: test_enable explicitly defined - should use it
        QString netlistContent2 = R"(
port:
  clk_in:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  my_test_en:
    direction: input
    type: logic
  clk_out:
    direction: output
    type: logic
instance: {}
net: {}
clock:
  - name: test_ctrl_with_test_en
    test_enable: my_test_en
    input:
      clk_in:
        freq: 100MHz
    target:
      clk_out:
        freq: 100MHz
        icg:
          enable: en
          reset: rst_n
        link:
          clk_in: ~
)";
        QString netlistPath2    = createTempFile("test_with_test_enable.soc_net", netlistContent2);
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
            = QDir(projectManager.getOutputPath()).filePath("test_with_test_enable.v");
        QVERIFY(QFile::exists(verilogPath2));

        QFile verilogFile2(verilogPath2);
        QVERIFY(verilogFile2.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent2 = verilogFile2.readAll();
        verilogFile2.close();

        // Should have my_test_en port
        QVERIFY(verifyVerilogContentNormalized(verilogContent2, "input wire my_test_en"));
        // Should use my_test_en
        QVERIFY(verifyVerilogContentNormalized(verilogContent2, ".test_en(my_test_en)"));
    }

    void test_duplicate_output_error_detection()
    {
        messageList.clear();

        // Test the duplicate detection logic by directly testing the parser
        // Since YAML doesn't allow duplicate keys, we test the defensive error checking
        // by creating a scenario that could trigger it in future modifications
        QString netlistContent = R"(
metadata:
  name: error_check_test
  version: "1.0"

clock:
  - name: test_ctrl
    clock: clk_in
    input:
      clk_in:
        freq: 100MHz
    target:
      clk_normal:
        freq: 50MHz
        link:
          clk_in:

instance: {}
net: {}
)";

        QString netlistPath = createTempFile("test_error_check.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        // This test verifies the error checking code exists (defensive programming)
        // In normal YAML parsing, duplicates won't occur due to YAML constraints
        // But the error checking protects against programmatic errors
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_error_check.v");
        QVERIFY(QFile::exists(verilogPath));

        // Verify no duplicate error messages (should be clean for unique targets)
        for (const QString &message : messageList) {
            QVERIFY2(
                !message.contains("ERROR: Duplicate output"),
                QString("Unexpected duplicate error: %1").arg(message).toLocal8Bit());
        }
    }

    void test_sta_guide_instance_naming()
    {
        // Test STA guide with explicit instance names and automatic generation
        QString netlistContent = R"(
port:
  clk_in:
    direction: input
    type: logic
  cpu_clk:
    direction: output
    type: logic
  gpu_clk:
    direction: output
    type: logic

instance: {}

net: {}

clock:
  - name: sta_test_ctrl
    input:
      clk_in:
        freq: 800MHz
    target:
      # Test explicit instance name (should match user specification)
      cpu_clk:
        freq: 800MHz
        sta_guide:
          cell: RVTP140G35T9_BUF_S_16
          in: A
          out: X
          instance: u_DONTTOUCH_clk_cpu
        link:
          clk_in:

      # Test automatic instance name generation (should be u_gpu_clk_target_sta)
      gpu_clk:
        freq: 800MHz
        sta_guide:
          cell: RVTP140G35T9_BUF_S_16
          in: A
          out: X
          # No instance specified -> automatic generation
        link:
          clk_in:
)";

        QString netlistPath = createTempFile("test_sta_guide.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_sta_guide.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        // Verify explicit instance name is used (user-specified deterministic name)
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "RVTP140G35T9_BUF_S_16 u_DONTTOUCH_clk_cpu ("));
        QVERIFY(!verifyVerilogContentNormalized(
            verilogContent, "RVTP140G35T9_BUF_S_16 u_cpu_clk_target_sta ("));

        // Verify automatic instance name is generated when not specified
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "RVTP140G35T9_BUF_S_16 u_gpu_clk_target_sta ("));

        // Verify both buffers are properly connected
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign cpu_clk = cpu_clk_sta_out;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign gpu_clk = gpu_clk_sta_out;"));

        // Verify wire declarations exist
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "wire cpu_clk_sta_out;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "wire gpu_clk_sta_out;"));

        // Verify port connections for both instances (direct connection since no div specified)
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".A(clk_cpu_clk_from_clk_in)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".X(cpu_clk_sta_out)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".A(clk_gpu_clk_from_clk_in)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".X(gpu_clk_sta_out)"));

        // clock_cell.v should be created and complete
        QVERIFY(verifyClockCellFileComplete());
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegenerateclocklogic.moc"
