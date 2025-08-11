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
            = {"QSOC_CKMUX_CELL",
               "QSOC_CKMUX_GF_CELL",
               "QSOC_CKGATE_CELL",
               "QSOC_CKDIV_ICG",
               "QSOC_CKDIV_DFF"};

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
          osc_24m:
            type: PASS_THRU
            invert: false
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

        /* Verify the generated content contains expected clock logic */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "PASS_THRU"));
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
        link:
          pll_800m:
            type: GATE_ONLY
            gate:
              enable: dbg_clk_en
              polarity: high
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
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "CKGATE_CELL"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".CLK_IN(pll_800m)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".CLK_EN(dbg_clk_en)"));

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
        link:
          pll_800m:
            type: DIV_ICG
            div:
              ratio: 4
              reset: rst_n
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
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "CKDIV_ICG"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".RATIO(4)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".RST_N(rst_n)"));

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
            type: DIV_DFF
            invert: true
            div:
              ratio: 2
              reset: rst_n
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
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "CKDIV_DFF"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".RATIO(2)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "~clk_slow_clk_n_from_osc_24m"));

        // clock_cell.v should be created and complete
        QVERIFY(verifyClockCellFileComplete());
    }

    void test_std_mux_clock()
    {
        // Create netlist file with STD_MUX multi-source clock
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
        link:
          pll_800m:
            type: DIV_ICG
            div:
              ratio: 8
              reset: rst_n
          test_clk:
            type: PASS_THRU
        mux:
          type: STD_MUX
          select: func_sel
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
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "CKMUX_CELL"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".SEL(func_sel)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "CKDIV_ICG"));

        // clock_cell.v should be created and complete
        QVERIFY(verifyClockCellFileComplete());
    }

    void test_gf_mux_clock()
    {
        // Create netlist file with GF_MUX (glitch-free) multi-source clock
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
  clk_sys:
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
    default_ref_clock: clk_sys
    input:
      osc_24m:
        freq: 24MHz
      test_clk:
        freq: 100MHz
    target:
      safe_clk:
        freq: 24MHz
        link:
          osc_24m:
            type: PASS_THRU
          test_clk:
            type: PASS_THRU
        mux:
          type: GF_MUX
          select: safe_sel
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
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "CKMUX_GF_CELL"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".REF_CLK(clk_sys)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".SEL(safe_sel)"));

        // clock_cell.v should be created and complete
        QVERIFY(verifyClockCellFileComplete());
    }

    void test_gf_mux_custom_ref_clock()
    {
        // Create netlist file with GF_MUX using custom ref_clock (different from default)
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
  clk_sys:
    direction: input
    type: logic
  custom_ref:
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
    default_ref_clock: clk_sys
    input:
      osc_24m:
        freq: 24MHz
      test_clk:
        freq: 100MHz
    target:
      custom_clk:
        freq: 24MHz
        link:
          osc_24m:
            type: PASS_THRU
          test_clk:
            type: PASS_THRU
        mux:
          type: GF_MUX
          select: custom_sel
          ref_clock: custom_ref
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
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "CKMUX_GF_CELL"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".REF_CLK(custom_ref)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".SEL(custom_sel)"));

        // clock_cell.v should be created and complete
        QVERIFY(verifyClockCellFileComplete());
    }

    void test_mixed_ref_clock_scenario()
    {
        // Create netlist file with mixed ref_clock scenario - some using default, some custom
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
  clk_sys:
    direction: input
    type: logic
  special_ref:
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
    default_ref_clock: clk_sys
    input:
      osc_24m:
        freq: 24MHz
      test_clk:
        freq: 100MHz
    target:
      default_clk:
        freq: 24MHz
        link:
          osc_24m:
            type: PASS_THRU
          test_clk:
            type: PASS_THRU
        mux:
          type: GF_MUX
          select: sel1
          # Uses default_ref_clock (clk_sys)
      custom_clk:
        freq: 100MHz
        link:
          osc_24m:
            type: PASS_THRU
          test_clk:
            type: PASS_THRU
        mux:
          type: GF_MUX
          select: sel2
          ref_clock: special_ref
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
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "CKMUX_GF_CELL"));
        // First target uses default ref_clock
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".REF_CLK(clk_sys)"));
        // Second target uses custom ref_clock
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".REF_CLK(special_ref)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".SEL(sel1)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, ".SEL(sel2)"));

        // clock_cell.v should be created and complete
        QVERIFY(verifyClockCellFileComplete());
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegenerateclocklogic.moc"
