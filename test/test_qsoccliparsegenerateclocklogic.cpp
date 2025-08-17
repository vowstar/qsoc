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
          ratio: 4
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
              ratio: 2
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
          ratio: 8
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
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".test_en(test_enable)"));
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
          ratio: 10
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
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".en(sys_clk_en)"));

        // Should use sys_clk as both output and intermediate signal
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "output sys_clk"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "output cpu_clk"));

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
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "u_cpu_clk_target_sta"));
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
              ratio: 4
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
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegenerateclocklogic.moc"
