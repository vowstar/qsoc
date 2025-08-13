// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/config.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
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
        QString filePath = QDir(projectManager.getOutputPath()).filePath(fileName);
        QFile   file(filePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&file);
            stream << content;
            file.close();
        }
        return filePath;
    }

    void createTestGenerateFiles()
    {
        /* Create c906 module in the module directory */
        const QString c906Content = R"(
c906:
  port:
    axim_clk_en:
      type: logic
      direction: in
    biu_pad_arvalid:
      type: logic
      direction: out
    pad_biu_arready:
      type: logic
      direction: in
    pad_biu_rdata:
      type: logic[127:0]
      direction: in
    pad_cpu_sys_cnt:
      type: logic[63:0]
      direction: in
    pad_tdt_dm_rdata:
      type: logic[127:0]
      direction: in
    pad_biu_bid:
      type: logic[7:0]
      direction: in
    pad_biu_rid:
      type: logic[7:0]
      direction: in
    biu_pad_arid:
      type: logic[7:0]
      direction: out
    biu_pad_awid:
      type: logic[7:0]
      direction: out
    sys_apb_rst_b:
      type: logic
      direction: in
    pad_cpu_rvba:
      type: logic[39:0]
      direction: in
    pll_core_cpuclk:
      type: logic
      direction: in
    pad_cpu_rst_b:
      type: logic
      direction: in
    tdt_dm_pad_wdata:
      type: logic[127:0]
      direction: out
)";

        /* Create the module file */
        const QDir    moduleDir(projectManager.getModulePath());
        const QString modulePath = moduleDir.filePath("c906.soc_mod");
        QFile         moduleFile(modulePath);
        if (moduleFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&moduleFile);
            stream << c906Content;
            moduleFile.close();
        }
    }

    /* Look for Verilog output file in typical locations */
    bool verifyVerilogOutputExistence(const QString &baseFileName)
    {
        /* First check the current project's output directory if available */
        for (const QString &msg : messageList) {
            if (msg.contains("Successfully generated Verilog code:")
                && msg.contains(baseFileName + ".v")) {
                const QRegularExpression regex("Successfully generated Verilog code: (.+\\.v)");
                const QRegularExpressionMatch match = regex.match(msg);
                if (match.hasMatch()) {
                    const QString filePath = match.captured(1);
                    if (QFile::exists(filePath)) {
                        return true;
                    }
                }
            }
        }

        /* Check the project output directory */
        const QString projectOutputPath = projectManager.getOutputPath();
        const QString projectFilePath   = QDir(projectOutputPath).filePath(baseFileName + ".v");
        return QFile::exists(projectFilePath);
    }

    /* Get Verilog content and check if it contains specific text */
    bool verifyVerilogContent(const QString &baseFileName, const QString &contentToVerify)
    {
        if (baseFileName.isNull() || contentToVerify.isNull()) {
            return false;
        }

        QString verilogContent;
        QString filePath;

        /* First try from message logs */
        for (const QString &msg : messageList) {
            if (msg.isNull()) {
                continue;
            }
            if (msg.contains("Successfully generated Verilog code:")
                && msg.contains(baseFileName + ".v")) {
                const QRegularExpression regex("Successfully generated Verilog code: (.+\\.v)");
                const QRegularExpressionMatch match = regex.match(msg);
                if (match.hasMatch()) {
                    filePath = match.captured(1);
                    if (!filePath.isNull() && QFile::exists(filePath)) {
                        QFile file(filePath);
                        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                            verilogContent = file.readAll();
                            file.close();
                            if (!verilogContent.isNull()) {
                                break;
                            }
                        }
                    }
                }
            }
        }

        /* If not found from logs, check the project output directory */
        if (verilogContent.isEmpty()) {
            const QString projectOutputPath = projectManager.getOutputPath();
            if (!projectOutputPath.isNull()) {
                filePath = QDir(projectOutputPath).filePath(baseFileName + ".v");
                if (!filePath.isNull() && QFile::exists(filePath)) {
                    QFile file(filePath);
                    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                        verilogContent = file.readAll();
                        file.close();
                    }
                }
            }
        }

        /* Empty content check */
        if (verilogContent.isEmpty()) {
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
        /* Re-enable message handler for collecting CLI output */
        qInstallMessageHandler(messageOutput);
        /* Set project name */
        projectName = QFileInfo(__FILE__).baseName() + "_data";
        /* Setup project manager */
        projectManager.setProjectName(projectName);
        projectManager.setCurrentPath(QDir::current().filePath(projectName));
        projectManager.mkpath();
        projectManager.save(projectName);
        projectManager.load(projectName);
        /* Create test files */
        createTestGenerateFiles();
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

    void testGenerateCommandHelp()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {"qsoc", "generate", "--help"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Just verify the command doesn't crash */
        QVERIFY(true);
    }

    void testGenerateVerilogHelp()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {"qsoc", "generate", "verilog", "--help"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Just verify the command doesn't crash */
        QVERIFY(true);
    }

    void testGenerateWithInvalidOption()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {"qsoc", "generate", "verilog", "--invalid-option"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Just verify the command doesn't crash */
        QVERIFY(true);
    }

    void testGenerateWithMissingRequiredArgument()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {
            "qsoc", "generate", "verilog"
            /* Missing netlist file argument */
        };
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Just verify the command doesn't crash */
        QVERIFY(true);
    }

    void testGenerateWithVerbosityLevels()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {"qsoc", "--verbose=3", "generate", "verilog", "--help"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Just verify the command doesn't crash */
        QVERIFY(true);
    }

    void testGenerateWithMaxWidthTest()
    {
        /* Clear previous messages */
        messageList.clear();

        const QString content  = R"(
---
version: "1.0"
module: "max_width_test"
port:
  clk:
    direction: in
    type: "logic"
  rst_n:
    direction: in
    type: "logic"
  data_out:
    direction: out
    type: "logic [31:0]"
net:
  mixed_width_net:
    - instance: cpu0
      port: "biu_pad_rdata"
    - instance: cpu1
      port: "pad_biu_rdata"
    - instance: cpu2
      port: "pad_tdt_dm_rdata"
instance:
  cpu0:
    module: "c906"
  cpu1:
    module: "c906"
  cpu2:
    module: "c906"
)";
        const QString filePath = createTempFile("max_width_test.soc_net", content);

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that the Verilog file was generated */
        QVERIFY(verifyVerilogOutputExistence("max_width_test"));

        /* Verify that important content is present */
        QVERIFY(verifyVerilogContent("max_width_test", "module max_width_test"));
        QVERIFY(verifyVerilogContent("max_width_test", "c906 cpu0"));
        QVERIFY(verifyVerilogContent("max_width_test", "c906 cpu1"));
        QVERIFY(verifyVerilogContent("max_width_test", "c906 cpu2"));
    }

    void testGenerateWithTieOverflowTest()
    {
        messageList.clear();

        const QString content  = R"(
instance:
  cpu0:
    module: c906
    port:
      # 128-bit value (exceeds 64-bit limit)
      pad_biu_rdata:
        tie: 128'hDEADBEEFDEADBEEFDEADBEEFDEADBEEF
      # 100-bit value (exceeds 64-bit limit)
      pad_cpu_sys_cnt:
        tie: 100'h12345678901234567890
      # Very large decimal value (exceeds 64-bit limit)
      pad_tdt_dm_rdata:
        tie: 18446744073709551616  # 2^64, exceeds quint64 max
      # Normal 64-bit value (at the limit)
      pad_biu_bid:
        tie: 64'hFFFFFFFFFFFFFFFF
)";
        const QString filePath = createTempFile("tie_overflow_test.soc_net", content);

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that the Verilog file was generated */
        QVERIFY(verifyVerilogOutputExistence("tie_overflow_test"));

        /* Verify the module name */
        QVERIFY(verifyVerilogContent("tie_overflow_test", "module tie_overflow_test"));

        /* Verify CPU instance */
        QVERIFY(verifyVerilogContent("tie_overflow_test", "c906 cpu0"));

        /* Verify the tie values are correctly formatted in the output with port names */
        QVERIFY(verifyVerilogContent(
            "tie_overflow_test", ".pad_biu_rdata(128'hdeadbeefdeadbeefdeadbeefdeadbeef)"));
        QVERIFY(verifyVerilogContent(
            "tie_overflow_test",
            ".pad_cpu_sys_cnt(64'h5678901234567890 /* FIXME: Value 100'h12345678901234567890 wider "
            "than port width 64 bits */"));

        /* Check large decimal value with port name */
        bool hasLargeDecimal
            = verifyVerilogContent("tie_overflow_test", ".pad_tdt_dm_rdata(18446744073709551616)");
        if (!hasLargeDecimal) {
            hasLargeDecimal = verifyVerilogContent(
                "tie_overflow_test", ".pad_tdt_dm_rdata(128'd18446744073709551616)");
        }
        QVERIFY(hasLargeDecimal);

        /* Verify 64-bit limit values with port name */
        QVERIFY(verifyVerilogContent(
            "tie_overflow_test",
            ".pad_biu_bid(8'hff  /* FIXME: Value 64'hffffffffffffffff wider than port width 8 bits "
            "*/)"));
    }

    void testGenerateWithTieFormatTest()
    {
        messageList.clear();

        const QString content  = R"(
instance:
  cpu0:
    module: c906
    port:
      # 1-bit port with binary value
      axim_clk_en:
        tie: 1'b0
      # 1-bit port with decimal value
      sys_apb_rst_b:
        tie: 1'd1
      # 8-bit port with 1-bit value (small value)
      biu_pad_arid:
        tie: 1
      # 8-bit port with 8-bit binary value
      biu_pad_awid:
        tie: 8'b10101010
      # 8-bit port with 8-bit hex value
      pad_biu_bid:
        tie: 8'hAA
      # 8-bit port with value > 8 bits (truncation test)
      pad_biu_rid:
        tie: 16'hFFFF
      # 32-bit port with decimal value (decimal format preservation)
      pad_cpu_rvba:
        tie: 42
      # 32-bit port with binary value (binary format preservation)
      pad_cpu_sys_cnt:
        tie: 32'b101010
      # 32-bit port with octal value (octal format preservation)
      pad_tdt_dm_rdata:
        tie: 8'o77
      # 64-bit port with hex value (hex format preservation)
      tdt_dm_pad_wdata:
        tie: 64'hDEADBEEF
)";
        const QString filePath = createTempFile("tie_format_test.soc_net", content);

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that the Verilog file was generated */
        QVERIFY(verifyVerilogOutputExistence("tie_format_test"));

        /* Verify the module name */
        QVERIFY(verifyVerilogContent("tie_format_test", "module tie_format_test"));

        /* Verify CPU instance */
        QVERIFY(verifyVerilogContent("tie_format_test", "c906 cpu0"));

        /* Verify binary format preserved with port name */
        QVERIFY(verifyVerilogContent("tie_format_test", ".axim_clk_en(1'b0)"));

        /* Verify decimal format preserved with port name */
        QVERIFY(verifyVerilogContent("tie_format_test", ".sys_apb_rst_b(1'd1)"));

        /* Verify biu_pad_awid is correctly marked as missing in the output */
        QVERIFY(verifyVerilogContent(
            "tie_format_test", ".biu_pad_awid(/* FIXME: out [7:0] biu_pad_awid missing */)"));

        /* Verify binary format preserved with port name */
        QVERIFY(verifyVerilogContent("tie_format_test", ".pad_cpu_sys_cnt(64'b101010)"));

        /* Verify 8-bit hex value with port name */
        QVERIFY(verifyVerilogContent("tie_format_test", ".pad_biu_bid(8'haa)"));

        /* Verify truncated hex value with port name */
        QVERIFY(verifyVerilogContent(
            "tie_format_test",
            ".pad_biu_rid(8'hff  /* FIXME: Value 16'hffff wider than port width 8 bits */)"));

        /* Verify decimal value with port name */
        QVERIFY(verifyVerilogContent("tie_format_test", ".pad_cpu_rvba(40'd42)"));

        /* Verify octal format is preserved with port name */
        QVERIFY(verifyVerilogContent("tie_format_test", ".pad_tdt_dm_rdata(128'o77)"));
    }

    void testGenerateWithInvertTest()
    {
        messageList.clear();

        const QString content  = R"(
instance:
  cpu0:
    module: c906
    port:
      axim_clk_en:
        tie: 0
      biu_pad_arvalid:
        invert: true
  cpu1:
    module: c906
    port:
      axim_clk_en:
        tie: 1
        invert: true
      biu_pad_arvalid:
        invert: true
net:
  clk_net:
    - instance: cpu0
      port: pll_core_cpuclk
    - instance: cpu1
      port: pll_core_cpuclk
  reset_net:
    - instance: cpu0
      port: pad_cpu_rst_b
    - instance: cpu1
      port: pad_cpu_rst_b
  arvalid_net:
    - instance: cpu0
      port: biu_pad_arvalid
    - instance: cpu1
      port: biu_pad_arvalid
)";
        const QString filePath = createTempFile("invert_test.soc_net", content);

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that the Verilog file was generated */
        QVERIFY(verifyVerilogOutputExistence("invert_test"));

        /* Verify that module and instances exist */
        QVERIFY(verifyVerilogContent("invert_test", "module invert_test"));
        QVERIFY(verifyVerilogContent("invert_test", "c906 cpu0"));
        QVERIFY(verifyVerilogContent("invert_test", "c906 cpu1"));

        /* Verify invert logic for cpu0 */
        QVERIFY(verifyVerilogContent("invert_test", "cpu0"));
        QVERIFY(verifyVerilogContent("invert_test", ".axim_clk_en(1'd0)"));
        QVERIFY(verifyVerilogContent("invert_test", ".biu_pad_arvalid(~arvalid_net)"));

        /* Verify invert logic for cpu1 */
        QVERIFY(verifyVerilogContent("invert_test", "cpu1"));
        QVERIFY(verifyVerilogContent("invert_test", ".axim_clk_en(~(1'd1))"));
        QVERIFY(verifyVerilogContent("invert_test", ".biu_pad_arvalid(~arvalid_net)"));

        /* Verify net connections */
        QVERIFY(verifyVerilogContent("invert_test", "wire clk_net"));
        QVERIFY(verifyVerilogContent("invert_test", "wire reset_net"));
        QVERIFY(verifyVerilogContent("invert_test", "wire arvalid_net"));
    }

    void testGenerateWithTieWidthTest()
    {
        messageList.clear();

        const QString content  = R"(
instance:
  cpu0:
    module: c906
    port:
      # 1-bit tie to 1-bit port (exact width match)
      axim_clk_en:
        tie: 1'b0
      # 8-bit tie to 1-bit port (width mismatch, should truncate)
      sys_apb_rst_b:
        tie: 8'b10101010
      # 1-bit tie to 8-bit port (width mismatch, should zero extend)
      biu_pad_arid:
        tie: 1'b1
      # 4-bit tie to 8-bit port (width mismatch, should zero extend)
      biu_pad_awid:
        tie: 4'b1010
      # 32-bit decimal to 8-bit port (width mismatch, should truncate)
      pad_biu_bid:
        tie: 300
)";
        const QString filePath = createTempFile("tie_width_test.soc_net", content);

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that the Verilog file was generated */
        QVERIFY(verifyVerilogOutputExistence("tie_width_test"));

        /* Verify that module and instance exist */
        QVERIFY(verifyVerilogContent("tie_width_test", "module tie_width_test"));
        QVERIFY(verifyVerilogContent("tie_width_test", "c906 cpu0"));

        /* Verify exact width match case */
        QVERIFY(verifyVerilogContent("tie_width_test", ".axim_clk_en(1'b0)"));

        /* Verify truncation for 8-bit to 1-bit - should show FIXME comment */
        QVERIFY(verifyVerilogContent(
            "tie_width_test",
            ".sys_apb_rst_b(1'b0  /* FIXME: Value 8'b10101010 wider than port width 1 bits */)"));

        /* Verify output port tie is ignored and marked missing */
        QVERIFY(verifyVerilogContent(
            "tie_width_test", ".biu_pad_arid(  /* FIXME: out [7:0] biu_pad_arid missing */)"));

        /* Verify truncation for large decimal to 8-bit */
        QVERIFY(verifyVerilogContent(
            "tie_width_test",
            ".pad_biu_bid(8'd44  /* FIXME: Value 9'd300 wider than port width 8 bits */)"));
    }

    void testGenerateWithTieFormatInputTest()
    {
        messageList.clear();

        const QString content  = R"(
instance:
  cpu0:
    module: c906
    port:
      # Binary format with different bases
      axim_clk_en:
        tie: 1'b0
      sys_apb_rst_b:
        tie: 1'B1  # capital B
      # Decimal format with different bases
      biu_pad_arid:
        tie: 8'd5
      biu_pad_awid:
        tie: 8'D10  # capital D
      # Hex format with different bases
      pad_biu_bid:
        tie: 8'hff
      pad_biu_rid:
        tie: 8'Haa  # capital H
      # Octal format with different bases
      pad_cpu_rvba:
        tie: 8'o77
      pad_cpu_sys_cnt:
        tie: 8'O123  # capital O
      # Uppercase hex characters
      pad_tdt_dm_rdata:
        tie: 32'hDEADBEEF
      # Lowercase hex characters
      tdt_dm_pad_wdata:
        tie: 32'hdeadbeef
)";
        const QString filePath = createTempFile("tie_format_input_test.soc_net", content);

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that the Verilog file was generated */
        QVERIFY(verifyVerilogOutputExistence("tie_format_input_test"));

        /* Verify that module and instance exist */
        QVERIFY(verifyVerilogContent("tie_format_input_test", "module tie_format_input_test"));
        QVERIFY(verifyVerilogContent("tie_format_input_test", "c906 cpu0"));

        /* Verify binary format handling */
        QVERIFY(verifyVerilogContent("tie_format_input_test", ".axim_clk_en(1'b0)"));
        QVERIFY(verifyVerilogContent("tie_format_input_test", ".sys_apb_rst_b(1'b1)"));

        /* Verify hex format handling (note: usually lowercase in output) */
        QVERIFY(verifyVerilogContent("tie_format_input_test", ".pad_biu_bid(8'hff)"));
        QVERIFY(verifyVerilogContent("tie_format_input_test", ".pad_biu_rid(8'haa)"));

        /* Verify octal format handling */
        QVERIFY(verifyVerilogContent("tie_format_input_test", ".pad_cpu_rvba(40'o77)"));

        /* Verify hex case handling in larger values */
        QVERIFY(verifyVerilogContent("tie_format_input_test", ".pad_tdt_dm_rdata(128'hdeadbeef)"));
    }

    void testGenerateWithComplexTieTest()
    {
        messageList.clear();

        const QString content  = R"(
instance:
  cpu0:
    module: c906
    port:
      # Mixed format types in same file
      axim_clk_en:
        tie: 1'b0
      sys_apb_rst_b:
        tie: 1
      biu_pad_arid:
        tie: 8'hAB
      biu_pad_awid:
        tie: 8'o123
      # Apply both tie and invert
      pad_biu_bid:
        tie: 8'hFF
        invert: true
      # Multi-port interaction
      pad_biu_rid:
        tie: 16'hABCD
  cpu1:
    module: c906
    port:
      # Using same port name as cpu0 with different tie value
      axim_clk_en:
        tie: 1'b1
      # No tie but has invert
      biu_pad_arvalid:
        invert: true
)";
        const QString filePath = createTempFile("complex_tie_test.soc_net", content);

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that the Verilog file was generated */
        QVERIFY(verifyVerilogOutputExistence("complex_tie_test"));

        /* Verify that module and instances exist */
        QVERIFY(verifyVerilogContent("complex_tie_test", "module complex_tie_test"));
        QVERIFY(verifyVerilogContent("complex_tie_test", "c906 cpu0"));
        QVERIFY(verifyVerilogContent("complex_tie_test", "c906 cpu1"));

        /* Verify cpu0 tie values */
        QVERIFY(verifyVerilogContent("complex_tie_test", "cpu0"));
        QVERIFY(verifyVerilogContent("complex_tie_test", ".axim_clk_en(1'b0)"));
        QVERIFY(verifyVerilogContent("complex_tie_test", ".sys_apb_rst_b(1'd1)"));
        QVERIFY(verifyVerilogContent(
            "complex_tie_test", ".biu_pad_arid(/* FIXME: out [7:0] biu_pad_arid missing */)"));
        QVERIFY(verifyVerilogContent(
            "complex_tie_test", ".biu_pad_awid(/* FIXME: out [7:0] biu_pad_awid missing */)"));

        /* Verify the tie+invert combination */
        QVERIFY(verifyVerilogContent("complex_tie_test", ".pad_biu_bid(~(8'hff))"));

        /* Verify truncation with warning comment */
        QVERIFY(verifyVerilogContent(
            "complex_tie_test",
            ".pad_biu_rid(8'hcd  /* FIXME: Value 16'habcd wider than port width 8 bits */)"));

        /* Verify cpu1 ties are different from cpu0 */
        QVERIFY(verifyVerilogContent("complex_tie_test", "cpu1"));
        QVERIFY(verifyVerilogContent("complex_tie_test", ".axim_clk_en(1'b1)"));
        QVERIFY(verifyVerilogContent(
            "complex_tie_test", ".biu_pad_arvalid(/* FIXME: out biu_pad_arvalid missing */)"));
    }

    void testGenerateWithPortWidthTest()
    {
        messageList.clear();

        const QString content  = R"(
---
version: "1.0"
module: "port_width_test"
port:
  clk:
    direction: in
    type: "logic"
  rst_n:
    direction: in
    type: "logic"
  data_in:
    direction: in
    type: "logic [31:0]"
  addr_in:
    direction: in
    type: "logic [15:0]"
  data_out:
    direction: out
    type: "logic [31:0]"
  ready:
    direction: out
    type: "logic"
net:
  mixed_width_net:
    - instance: cpu0
      port: "data_in"
instance:
  cpu0:
    module: "c906"
)";
        const QString filePath = createTempFile("port_width_test.soc_net", content);

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that the Verilog file was generated */
        QVERIFY(verifyVerilogOutputExistence("port_width_test"));

        /* Verify that important content is present */
        QVERIFY(verifyVerilogContent("port_width_test", "module port_width_test"));

        /* Verify port declarations with correct width information */
        QVERIFY(verifyVerilogContent("port_width_test", "input clk"));
        QVERIFY(verifyVerilogContent("port_width_test", "input rst_n"));
        QVERIFY(verifyVerilogContent("port_width_test", "input [31:0] data_in"));
        QVERIFY(verifyVerilogContent("port_width_test", "input [15:0] addr_in"));
        QVERIFY(verifyVerilogContent("port_width_test", "output [31:0] data_out"));
        QVERIFY(verifyVerilogContent("port_width_test", "output ready"));

        /* Verify CPU instance */
        QVERIFY(verifyVerilogContent("port_width_test", "c906 cpu0"));
    }

    void testGenerateWithMultipleFiles()
    {
        messageList.clear();

        const QString content1  = R"(
---
version: "1.0"
module: "example1"
port:
  clk:
    direction: in
    type: "logic"
  rst_n:
    direction: in
    type: "logic"
instance:
  cpu0:
    module: "c906"
)";
        const QString content2  = R"(
---
version: "1.0"
module: "example2"
port:
  clk:
    direction: in
    type: "logic"
  rst_n:
    direction: in
    type: "logic"
instance:
  cpu0:
    module: "c906"
    port:
      axim_clk_en:
        tie: 1'b0
)";
        const QString filePath1 = createTempFile("example1.soc_net", content1);
        const QString filePath2 = createTempFile("example2.soc_net", content2);

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "generate",
               "verilog",
               "-d",
               projectManager.getCurrentPath(),
               filePath1,
               filePath2};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that both Verilog files were generated */
        QVERIFY(verifyVerilogOutputExistence("example1"));
        QVERIFY(verifyVerilogOutputExistence("example2"));

        /* Verify example1 module content */
        QVERIFY(verifyVerilogContent("example1", "module example1"));
        QVERIFY(verifyVerilogContent("example1", "c906 cpu0"));
        QVERIFY(verifyVerilogContent("example1", "input clk"));
        QVERIFY(verifyVerilogContent("example1", "input rst_n"));
        QVERIFY(verifyVerilogContent("example1", "endmodule"));

        /* Verify example2 module content */
        QVERIFY(verifyVerilogContent("example2", "module example2"));
        QVERIFY(verifyVerilogContent("example2", "c906 cpu0"));
        QVERIFY(verifyVerilogContent("example2", "input clk"));
        QVERIFY(verifyVerilogContent("example2", "input rst_n"));
        QVERIFY(verifyVerilogContent("example2", ".axim_clk_en(1'b0)"));
        QVERIFY(verifyVerilogContent("example2", "endmodule"));
    }

    void testGenerateWithBitsSelection()
    {
        messageList.clear();

        /* Create a netlist file with bits selection */
        const QString content = R"(
instance:
  soc_top_cpu:
    module: c906

  soc_top_mux:
    module: simple_mux

  soc_top_flag:
    module: simple_flag

net:
  soc_top_data:
    - instance: soc_top_cpu
      port: pad_biu_rdata
    - instance: soc_top_mux
      port: data_out

  soc_top_data_sliced:
    - instance: soc_top_cpu
      port: biu_pad_arid
      bits: "[3:2]"    # Multi-bit selection
    - instance: soc_top_mux
      port: data_in
      bits: "[7:6]"    # Multi-bit selection

  soc_top_data_bit:
    - instance: soc_top_cpu
      port: axim_clk_en
      bits: "[4]"      # Single-bit selection
    - instance: soc_top_flag
      port: flag_in
      bits: "[6]"      # Single-bit selection
)";

        /* Create a simple_mux module */
        const QString muxContent = R"(
simple_mux:
  port:
    data_in:
      type: logic[7:0]
      direction: in
    data_out:
      type: logic[127:0]
      direction: out
)";

        /* Create a simple_flag module for single bit testing */
        const QString flagContent = R"(
simple_flag:
  port:
    flag_in:
      type: logic
      direction: in
    flag_out:
      type: logic
      direction: out
)";

        /* Create the module files */
        const QDir moduleDir(projectManager.getModulePath());

        /* Create simple_mux module file */
        const QString muxPath = moduleDir.filePath("simple_mux.soc_mod");
        QFile         muxFile(muxPath);
        if (muxFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&muxFile);
            stream << muxContent;
            muxFile.close();
        }

        /* Create simple_flag module file */
        const QString flagPath = moduleDir.filePath("simple_flag.soc_mod");
        QFile         flagFile(flagPath);
        if (flagFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&flagFile);
            stream << flagContent;
            flagFile.close();
        }

        /* Create netlist file */
        const QString filePath = createTempFile("test_bits.soc_net", content);

        /* Run the command to generate Verilog */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the output file exists */
        QVERIFY(verifyVerilogOutputExistence("test_bits"));

        /* Verify multi-bit selection in Verilog content */
        QVERIFY(verifyVerilogContent("test_bits", ".biu_pad_arid(soc_top_data_sliced[3:2])"));
        QVERIFY(verifyVerilogContent("test_bits", ".data_in(soc_top_data_sliced[7:6])"));

        /* Verify single-bit selection in Verilog content */
        QVERIFY(verifyVerilogContent("test_bits", ".axim_clk_en(soc_top_data_bit[4])"));
        QVERIFY(verifyVerilogContent("test_bits", ".flag_in(soc_top_data_bit[6])"));

        /* Normal net connection without bits selection */
        QVERIFY(verifyVerilogContent("test_bits", ".pad_biu_rdata(soc_top_data)"));
        QVERIFY(verifyVerilogContent("test_bits", ".data_out(soc_top_data)"));
    }

    void testGenerateWithBitsSelectionWidthMismatch()
    {
        messageList.clear();

        /* Create a netlist file with bits selection causing width mismatch */
        const QString content = R"(
instance:
  soc_top_cpu:
    module: c906

  soc_top_mux:
    module: simple_mux

  wide_driver:
    module: wide_driver_module

net:
  # This net has width mismatch with bits selection
  soc_mismatch:
    - instance: soc_top_cpu
      port: pad_biu_rid       # 8-bit port
      bits: "[1:0]"           # 2-bit selection
    - instance: soc_top_mux
      port: data_in           # 8-bit port
      bits: "[4]"             # 1-bit selection
    - instance: wide_driver
      port: data_out          # 32-bit output port (intentional width mismatch)
)";

        /* Create a simple_mux module */
        const QString muxContent = R"(
simple_mux:
  port:
    data_in:
      type: logic[7:0]
      direction: in
    data_out:
      type: logic[127:0]
      direction: out
)";

        /* Create a wide driver module with 32-bit output that will create a real width mismatch */
        const QString wideDriverContent = R"(
wide_driver_module:
  port:
    data_out:
      type: logic[31:0]
      direction: output
    enable:
      type: logic
      direction: input
)";

        /* Create the module files */
        const QDir moduleDir(projectManager.getModulePath());

        /* Create mux module file */
        const QString muxPath = moduleDir.filePath("simple_mux.soc_mod");
        QFile         muxFile(muxPath);
        if (muxFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&muxFile);
            stream << muxContent;
            muxFile.close();
        }

        /* Create wide driver module file */
        const QString wideDriverPath = moduleDir.filePath("wide_driver_module.soc_mod");
        QFile         wideDriverFile(wideDriverPath);
        if (wideDriverFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&wideDriverFile);
            stream << wideDriverContent;
            wideDriverFile.close();
        }

        /* Create netlist file */
        const QString filePath = createTempFile("test_bits_mismatch.soc_net", content);

        /* Run the command to generate Verilog */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the output file exists */
        QVERIFY(verifyVerilogOutputExistence("test_bits_mismatch"));

        /* Verify that the bits selections are used in port connections */
        QVERIFY(verifyVerilogContent("test_bits_mismatch", ".pad_biu_rid(soc_mismatch[1:0])"));
        QVERIFY(verifyVerilogContent("test_bits_mismatch", ".data_in(soc_mismatch[4])"));
        QVERIFY(verifyVerilogContent("test_bits_mismatch", ".data_out(soc_mismatch)"));

        /* Verify the warning message contains bits selection information */
        QVERIFY(verifyVerilogContent("test_bits_mismatch", "Bit Selection: [1:0]"));
        QVERIFY(verifyVerilogContent("test_bits_mismatch", "Bit Selection: [4]"));

        /* Verify that width mismatch warning exists */
        QVERIFY(
            verifyVerilogContent("test_bits_mismatch", "FIXME: Net soc_mismatch width mismatch"));

        /* Verify width information is included in warnings */
        QVERIFY(verifyVerilogContent("test_bits_mismatch", "Width: [7:0]"));
        QVERIFY(verifyVerilogContent("test_bits_mismatch", "Width: [31:0]"));
    }

    void testGenerateWithBitsSelectionFullCoverage()
    {
        messageList.clear();

        /* Create a netlist file where bit selections provide complete coverage - no mismatch should occur */
        const QString content = R"(
instance:
  u_ctrl_unit:
    module: ctrl_unit

  u_analog_block_0:
    module: analog_block
  u_analog_block_1:
    module: analog_block
  u_analog_block_2:
    module: analog_block
  u_analog_block_3:
    module: analog_block
  u_analog_block_4:
    module: analog_block
  u_analog_block_5:
    module: analog_block
  u_analog_block_6:
    module: analog_block
  u_analog_block_7:
    module: analog_block

  u_phase_block:
    module: phase_block

port:
  ctrl_data_i:
    type: logic[63:0]
    direction: input

net:
  # This net should NOT have width mismatch - bit selections provide complete coverage
  ctrl_data_i:
    - port: ctrl_data_i              # 64-bit input port [63:0]
    - instance: u_ctrl_unit
      port: data_in                  # 64-bit input port [63:0]
    - instance: u_phase_block
      port: data_out                 # Single bit output
      bits: "[0]"                    # Drives bit 0
    - instance: u_analog_block_0
      port: data_out                 # Single bit output
      bits: "[8]"                    # Drives bit 8
    - instance: u_analog_block_1
      port: data_out                 # Single bit output
      bits: "[9]"                    # Drives bit 9
    - instance: u_analog_block_2
      port: data_out                 # Single bit output
      bits: "[10]"                   # Drives bit 10
    - instance: u_analog_block_3
      port: data_out                 # Single bit output
      bits: "[11]"                   # Drives bit 11
    - instance: u_analog_block_4
      port: data_out                 # Single bit output
      bits: "[12]"                   # Drives bit 12
    - instance: u_analog_block_5
      port: data_out                 # Single bit output
      bits: "[13]"                   # Drives bit 13
    - instance: u_analog_block_6
      port: data_out                 # Single bit output
      bits: "[14]"                   # Drives bit 14
    - instance: u_analog_block_7
      port: data_out                 # Single bit output
      bits: "[15]"                   # Drives bit 15

comb:
  - out: ctrl_data_i[7:1]           # Drives bits 7:1 (7 bits)
    expr: "7'b1010101"
  - out: ctrl_data_i[63:16]         # Drives bits 63:16 (48 bits)
    expr: "48'hDEADBEEFDEADBEEF"

# Total coverage: 1+1+1+1+1+1+1+1+1+7+48 = 64 bits = [63:0] ✓
)";

        /* Create the required module files */
        const QDir moduleDir(projectManager.getModulePath());

        /* Create ctrl_unit module */
        const QString ctrlUnitContent = R"(
ctrl_unit:
  port:
    data_in:
      type: logic[63:0]
      direction: input
    ctrl_out:
      type: logic[7:0]
      direction: output
)";

        /* Create analog_block module */
        const QString analogBlockContent = R"(
analog_block:
  port:
    data_out:
      type: logic
      direction: output
    enable:
      type: logic
      direction: input
)";

        /* Create phase_block module */
        const QString phaseBlockContent = R"(
phase_block:
  port:
    data_out:
      type: logic
      direction: output
    clk:
      type: logic
      direction: input
)";

        /* Write module files */
        const QString ctrlUnitPath = moduleDir.filePath("ctrl_unit.soc_mod");
        QFile         ctrlUnitFile(ctrlUnitPath);
        if (ctrlUnitFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&ctrlUnitFile);
            stream << ctrlUnitContent;
            ctrlUnitFile.close();
        }

        const QString analogBlockPath = moduleDir.filePath("analog_block.soc_mod");
        QFile         analogBlockFile(analogBlockPath);
        if (analogBlockFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&analogBlockFile);
            stream << analogBlockContent;
            analogBlockFile.close();
        }

        const QString phaseBlockPath = moduleDir.filePath("phase_block.soc_mod");
        QFile         phaseBlockFile(phaseBlockPath);
        if (phaseBlockFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&phaseBlockFile);
            stream << phaseBlockContent;
            phaseBlockFile.close();
        }

        /* Create netlist file */
        const QString filePath = createTempFile("test_full_coverage.soc_net", content);

        /* Run the command to generate Verilog */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the output file exists */
        QVERIFY(verifyVerilogOutputExistence("test_full_coverage"));

        /* Verify that bit selections are correctly applied */
        QVERIFY(verifyVerilogContent("test_full_coverage", ".data_out(ctrl_data_i[0])"));
        QVERIFY(verifyVerilogContent("test_full_coverage", ".data_out(ctrl_data_i[8])"));
        QVERIFY(verifyVerilogContent("test_full_coverage", ".data_out(ctrl_data_i[15])"));

        /* Verify that comb outputs are correctly applied */
        QVERIFY(verifyVerilogContent("test_full_coverage", "assign ctrl_data_i[7:1] ="));
        QVERIFY(verifyVerilogContent("test_full_coverage", "assign ctrl_data_i[63:16] ="));

        /* MOST IMPORTANT: Verify that NO width mismatch warning is generated */
        /* This is the key test - the bit selections should provide complete coverage */
        QVERIFY(
            !verifyVerilogContent("test_full_coverage", "FIXME: Net ctrl_data_i width mismatch"));
        QVERIFY(!verifyVerilogContent("test_full_coverage", "FIXME: Port ctrl_data_i"));
    }

    void testGenerateWithLinkUplinkConnections()
    {
        messageList.clear();

        /* Create a netlist file with link and uplink connections */
        const QString content = R"(
instance:
  u_io_cell0_PRCUT_H:
    module: PRCUT_H
    port:
  u_io_cell1_PVDD2POCM_H:
    module: PVDD2POCM_H
    port:
      RTE:
        link: io_ring_rte
  u_io_cell2_PDDWUWSWCDG_H:
    module: PDDWUWSWCDG_H
    port:
      C:
        link: sys_rst_n
      DS0:
        tie: 1'b1
      DS1:
        tie: 1'b1
      I:
        tie: 1'b0
      IE:
        tie: 1'b1
      OEN:
        tie: 1'b1
      PAD:
        uplink: rst_n
      PE:
        tie: 1'b0
      PS:
        tie: 1'b1
      ST:
        tie: 1'b0
      RTE:
        link: io_ring_rte
  u_io_cell3_PDDWUWSWCDG_H:
    module: PDDWUWSWCDG_H
    port:
      C:
        link: spi_sclk
      I:
        tie: 1'b0
      IE:
        tie: 1'b1
      OEN:
        tie: 1'b1
      PAD:
        uplink: sclk
      PE:
        tie: 1'b0
      PS:
        tie: 1'b0
      RTE:
        link: io_ring_rte
  u_cpu:
    module: c906
    port:
      axim_clk_en:
        tie: 1'b1
      pad_cpu_rst_b:
        link: sys_rst_n
)";

        /* Create PRCUT_H module */
        const QString prcutContent = R"(
PRCUT_H:
  port:
    # No ports for this module
)";

        /* Create PVDD2POCM_H module */
        const QString pvddContent = R"(
PVDD2POCM_H:
  port:
    RTE:
      type: logic
      direction: input
)";

        /* Create PDDWUWSWCDG_H module */
        const QString pddwContent = R"(
PDDWUWSWCDG_H:
  port:
    C:
      type: logic
      direction: output
    DS0:
      type: logic
      direction: input
    DS1:
      type: logic
      direction: input
    I:
      type: logic
      direction: input
    IE:
      type: logic
      direction: input
    OEN:
      type: logic
      direction: input
    PAD:
      type: logic
      direction: inout
    PE:
      type: logic
      direction: input
    PS:
      type: logic
      direction: input
    ST:
      type: logic
      direction: input
    RTE:
      type: logic
      direction: input
)";

        /* Create the module files */
        const QDir moduleDir(projectManager.getModulePath());

        /* Create PRCUT_H module file */
        const QString prcutPath = moduleDir.filePath("PRCUT_H.soc_mod");
        QFile         prcutFile(prcutPath);
        if (prcutFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&prcutFile);
            stream << prcutContent;
            prcutFile.close();
        }

        /* Create PVDD2POCM_H module file */
        const QString pvddPath = moduleDir.filePath("PVDD2POCM_H.soc_mod");
        QFile         pvddFile(pvddPath);
        if (pvddFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&pvddFile);
            stream << pvddContent;
            pvddFile.close();
        }

        /* Create PDDWUWSWCDG_H module file */
        const QString pddwPath = moduleDir.filePath("PDDWUWSWCDG_H.soc_mod");
        QFile         pddwFile(pddwPath);
        if (pddwFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&pddwFile);
            stream << pddwContent;
            pddwFile.close();
        }

        /* Create netlist file */
        const QString filePath = createTempFile("test_link_uplink.soc_net", content);

        /* Run the command to generate Verilog */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the output file exists */
        QVERIFY(verifyVerilogOutputExistence("test_link_uplink"));

        /* Verify basic module structure */
        QVERIFY(verifyVerilogContent("test_link_uplink", "module test_link_uplink"));

        /* Verify uplink created top-level ports */
        QVERIFY(verifyVerilogContent("test_link_uplink", "inout rst_n"));
        QVERIFY(verifyVerilogContent("test_link_uplink", "inout sclk"));

        /* Verify link connections - io_ring_rte should connect multiple instances */
        QVERIFY(verifyVerilogContent("test_link_uplink", ".RTE(io_ring_rte)"));

        /* Verify uplink connections - PAD ports should connect to top-level ports */
        QVERIFY(verifyVerilogContent("test_link_uplink", ".PAD(rst_n)"));
        QVERIFY(verifyVerilogContent("test_link_uplink", ".PAD(sclk)"));

        /* Verify link connections - internal signal connections */
        QVERIFY(verifyVerilogContent("test_link_uplink", ".pad_cpu_rst_b(sys_rst_n)"));
        QVERIFY(verifyVerilogContent("test_link_uplink", ".C(sys_rst_n)"));
        QVERIFY(verifyVerilogContent("test_link_uplink", ".C(spi_sclk)"));

        /* Verify wire declarations for link created nets */
        QVERIFY(verifyVerilogContent("test_link_uplink", "wire io_ring_rte"));
        QVERIFY(verifyVerilogContent("test_link_uplink", "wire sys_rst_n"));
        QVERIFY(verifyVerilogContent("test_link_uplink", "wire spi_sclk"));

        /* Verify tie connections still work */
        QVERIFY(verifyVerilogContent("test_link_uplink", ".DS0(1'b1)"));
        QVERIFY(verifyVerilogContent("test_link_uplink", ".axim_clk_en(1'b1)"));
    }

    void testGenerateWithUplinkConflictDetection()
    {
        messageList.clear();

        /* Create a netlist file with uplink conflicts (same port name, different types) */
        const QString content = R"(
instance:
  u_io_cell1:
    module: IO_CELL_8BIT
    port:
      PAD:
        uplink: test_port
  u_io_cell2:
    module: IO_CELL_16BIT
    port:
      PAD:
        uplink: test_port  # Same port name but different width - should cause conflict
)";

        /* Create IO_CELL_8BIT module */
        const QString io8bitContent = R"(
IO_CELL_8BIT:
  port:
    PAD:
      type: logic[7:0]
      direction: inout
)";

        /* Create IO_CELL_16BIT module */
        const QString io16bitContent = R"(
IO_CELL_16BIT:
  port:
    PAD:
      type: logic[15:0]
      direction: inout
)";

        /* Create the module files */
        const QDir moduleDir(projectManager.getModulePath());

        /* Create IO_CELL_8BIT module file */
        const QString io8bitPath = moduleDir.filePath("IO_CELL_8BIT.soc_mod");
        QFile         io8bitFile(io8bitPath);
        if (io8bitFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&io8bitFile);
            stream << io8bitContent;
            io8bitFile.close();
        }

        /* Create IO_CELL_16BIT module file */
        const QString io16bitPath = moduleDir.filePath("IO_CELL_16BIT.soc_mod");
        QFile         io16bitFile(io16bitPath);
        if (io16bitFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&io16bitFile);
            stream << io16bitContent;
            io16bitFile.close();
        }

        /* Create netlist file */
        const QString filePath = createTempFile("test_uplink_conflict.soc_net", content);

        /* Run the command to generate Verilog - this should detect width mismatch */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Check if the process failed due to width mismatch error */
        bool foundWidthMismatchError = false;
        for (const QString &msg : messageList) {
            if (msg.contains("Type/width mismatch for uplink port test_port")) {
                foundWidthMismatchError = true;
                break;
            }
        }

        /* Verify that width mismatch was detected */
        QVERIFY(foundWidthMismatchError);
    }

    void testGenerateWithUplinkCompatiblePorts()
    {
        messageList.clear();

        /* Create a netlist file with compatible uplink ports (same width) */
        const QString content = R"(
instance:
  u_io_cell1:
    module: IO_CELL_COMPATIBLE1
    port:
      PAD:
        uplink: shared_port
  u_io_cell2:
    module: IO_CELL_COMPATIBLE2
    port:
      PAD:
        uplink: shared_port  # Same port name and compatible type
)";

        /* Create compatible IO cell modules */
        const QString ioCompatible1Content = R"(
IO_CELL_COMPATIBLE1:
  port:
    PAD:
      type: logic[7:0]
      direction: inout
)";

        const QString ioCompatible2Content = R"(
IO_CELL_COMPATIBLE2:
  port:
    PAD:
      type: logic[7:0]  # Same width as first one
      direction: inout
)";

        /* Create the module files */
        const QDir moduleDir(projectManager.getModulePath());

        /* Create compatible module files */
        const QString ioComp1Path = moduleDir.filePath("IO_CELL_COMPATIBLE1.soc_mod");
        QFile         ioComp1File(ioComp1Path);
        if (ioComp1File.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&ioComp1File);
            stream << ioCompatible1Content;
            ioComp1File.close();
        }

        const QString ioComp2Path = moduleDir.filePath("IO_CELL_COMPATIBLE2.soc_mod");
        QFile         ioComp2File(ioComp2Path);
        if (ioComp2File.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&ioComp2File);
            stream << ioCompatible2Content;
            ioComp2File.close();
        }

        /* Create netlist file */
        const QString filePath = createTempFile("test_uplink_compatible.soc_net", content);

        /* Run the command to generate Verilog */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the output file exists */
        QVERIFY(verifyVerilogOutputExistence("test_uplink_compatible"));

        /* Verify the shared port was created */
        QVERIFY(verifyVerilogContent("test_uplink_compatible", "inout [7:0] shared_port"));

        /* Verify both instances connect to the shared port */
        QVERIFY(verifyVerilogContent("test_uplink_compatible", ".PAD(shared_port)"));

        /* Verify module structure */
        QVERIFY(verifyVerilogContent("test_uplink_compatible", "module test_uplink_compatible"));
        QVERIFY(verifyVerilogContent("test_uplink_compatible", "IO_CELL_COMPATIBLE1 u_io_cell1"));
        QVERIFY(verifyVerilogContent("test_uplink_compatible", "IO_CELL_COMPATIBLE2 u_io_cell2"));
    }

    void testGenerateWithLinkBitSelection()
    {
        messageList.clear();

        /* Create a netlist file with link bit selection */
        const QString content = R"(
instance:
  u_ampfifo_east0:
    module: ampfifo_2phase
    port:
      A1P0_VOUTP:
        link: vout_amp_e0[3:0]
  u_ampfifo_east1:
    module: ampfifo_2phase
    port:
      A1P0_VOUTP:
        link: vout_amp_e1[7]
  u_comp_south0:
    module: comp_stage
    port:
      A1P0_IREF:
        link: iref_signal[15:8]
      A1P0_VIN:
        link: vin_comp_s0[2]
  u_mixcore_center:
    module: mixer_core
    port:
      A1P0_DATA:
        link: data_path_c0[31:16]
)";

        /* Create ampfifo_2phase module */
        const QString ampfifoContent = R"(
ampfifo_2phase:
  port:
    A1P0_VOUTP:
      type: logic[7:0]
      direction: output
)";

        /* Create comp_stage module */
        const QString compContent = R"(
comp_stage:
  port:
    A1P0_IREF:
      type: logic[15:0]
      direction: input
    A1P0_VIN:
      type: logic
      direction: input
)";

        /* Create mixer_core module */
        const QString mixerContent = R"(
mixer_core:
  port:
    A1P0_DATA:
      type: logic[31:0]
      direction: output
)";

        /* Create the module files */
        const QDir moduleDir(projectManager.getModulePath());

        /* Create ampfifo_2phase module file */
        const QString ampfifoPath = moduleDir.filePath("ampfifo_2phase.soc_mod");
        QFile         ampfifoFile(ampfifoPath);
        if (ampfifoFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&ampfifoFile);
            stream << ampfifoContent;
            ampfifoFile.close();
        }

        /* Create comp_stage module file */
        const QString compPath = moduleDir.filePath("comp_stage.soc_mod");
        QFile         compFile(compPath);
        if (compFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&compFile);
            stream << compContent;
            compFile.close();
        }

        /* Create mixer_core module file */
        const QString mixerPath = moduleDir.filePath("mixer_core.soc_mod");
        QFile         mixerFile(mixerPath);
        if (mixerFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&mixerFile);
            stream << mixerContent;
            mixerFile.close();
        }

        /* Create netlist file */
        const QString filePath = createTempFile("test_link_bits.soc_net", content);

        /* Run the command to generate Verilog */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the output file exists */
        QVERIFY(verifyVerilogOutputExistence("test_link_bits"));

        /* Verify basic module structure */
        QVERIFY(verifyVerilogContent("test_link_bits", "module test_link_bits"));

        /* Verify wire declarations for nets created from link names */
        QVERIFY(verifyVerilogContent("test_link_bits", "wire [7:0] vout_amp_e0"));
        QVERIFY(verifyVerilogContent("test_link_bits", "wire [7:0] vout_amp_e1"));
        QVERIFY(verifyVerilogContent("test_link_bits", "wire [15:0] iref_signal"));
        QVERIFY(verifyVerilogContent("test_link_bits", "wire [31:0] data_path_c0"));
        QVERIFY(verifyVerilogContent("test_link_bits", "wire vin_comp_s0"));

        /* Verify bit selection in port connections */
        QVERIFY(verifyVerilogContent("test_link_bits", ".A1P0_VOUTP(vout_amp_e0[3:0])"));
        QVERIFY(verifyVerilogContent("test_link_bits", ".A1P0_VOUTP(vout_amp_e1[7])"));
        QVERIFY(verifyVerilogContent("test_link_bits", ".A1P0_IREF(iref_signal[15:8])"));
        QVERIFY(verifyVerilogContent("test_link_bits", ".A1P0_VIN(vin_comp_s0[2])"));
        QVERIFY(verifyVerilogContent("test_link_bits", ".A1P0_DATA(data_path_c0[31:16])"));

        /* Verify module instances */
        QVERIFY(verifyVerilogContent("test_link_bits", "ampfifo_2phase u_ampfifo_east0"));
        QVERIFY(verifyVerilogContent("test_link_bits", "ampfifo_2phase u_ampfifo_east1"));
        QVERIFY(verifyVerilogContent("test_link_bits", "comp_stage u_comp_south0"));
        QVERIFY(verifyVerilogContent("test_link_bits", "mixer_core u_mixcore_center"));
    }

    void testGenerateWithMultipleLinkDeduplication()
    {
        messageList.clear();

        /* Create a netlist file with multiple links to the same net (testing deduplication) */
        const QString content = R"(
instance:
  u_cpu0:
    module: c906
    port:
      axim_clk_en:
        link: shared_enable
  u_cpu1:
    module: c906
    port:
      axim_clk_en:
        link: shared_enable    # Same module, same port -> should deduplicate
  u_io_cell0:
    module: PDDWUWSWCDG_H
    port:
      RTE:
        link: io_ring_rte
  u_io_cell1:
    module: PDDWUWSWCDG_H
    port:
      RTE:
        link: io_ring_rte      # Different module instance, same port -> should allow
  u_io_cell2:
    module: PDDWUWSWCDG_H
    port:
      RTE:
        link: io_ring_rte      # Another instance to same net -> should allow
      C:
        link: clk_out
  u_pll:
    module: simple_pll
    port:
      clk_out:
        link: clk_out          # Different module, same net name -> should allow
)";

        /* Create simple_pll module */
        const QString pllContent = R"(
simple_pll:
  port:
    clk_out:
      type: logic
      direction: output
    enable:
      type: logic
      direction: input
)";

        /* Create PDDWUWSWCDG_H module (reuse from previous test) */
        const QString pddwContent = R"(
PDDWUWSWCDG_H:
  port:
    C:
      type: logic
      direction: input
    RTE:
      type: logic
      direction: input
)";

        /* Create the module files */
        const QDir moduleDir(projectManager.getModulePath());

        /* Create simple_pll module file */
        const QString pllPath = moduleDir.filePath("simple_pll.soc_mod");
        QFile         pllFile(pllPath);
        if (pllFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&pllFile);
            stream << pllContent;
            pllFile.close();
        }

        /* Create PDDWUWSWCDG_H module file */
        const QString pddwPath = moduleDir.filePath("PDDWUWSWCDG_H.soc_mod");
        QFile         pddwFile(pddwPath);
        if (pddwFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&pddwFile);
            stream << pddwContent;
            pddwFile.close();
        }

        /* Create netlist file */
        const QString filePath = createTempFile("test_multiple_link_dedup.soc_net", content);

        /* Run the command to generate Verilog */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the output file exists */
        QVERIFY(verifyVerilogOutputExistence("test_multiple_link_dedup"));

        /* Verify basic module structure */
        QVERIFY(verifyVerilogContent("test_multiple_link_dedup", "module test_multiple_link_dedup"));

        /* Verify all instances exist */
        QVERIFY(verifyVerilogContent("test_multiple_link_dedup", "c906 u_cpu0"));
        QVERIFY(verifyVerilogContent("test_multiple_link_dedup", "c906 u_cpu1"));
        QVERIFY(verifyVerilogContent("test_multiple_link_dedup", "PDDWUWSWCDG_H u_io_cell0"));
        QVERIFY(verifyVerilogContent("test_multiple_link_dedup", "PDDWUWSWCDG_H u_io_cell1"));
        QVERIFY(verifyVerilogContent("test_multiple_link_dedup", "PDDWUWSWCDG_H u_io_cell2"));
        QVERIFY(verifyVerilogContent("test_multiple_link_dedup", "simple_pll u_pll"));

        /* Verify wire declarations for the shared nets */
        QVERIFY(verifyVerilogContent("test_multiple_link_dedup", "wire shared_enable"));
        QVERIFY(verifyVerilogContent("test_multiple_link_dedup", "wire io_ring_rte"));
        QVERIFY(verifyVerilogContent("test_multiple_link_dedup", "wire clk_out"));

        /* Verify connections - shared_enable should connect to both CPU instances */
        QVERIFY(verifyVerilogContent("test_multiple_link_dedup", ".axim_clk_en(shared_enable)"));

        /* Verify connections - io_ring_rte should connect to all three IO cells */
        QVERIFY(verifyVerilogContent("test_multiple_link_dedup", ".RTE(io_ring_rte)"));

        /* Verify connections - clk_out should connect to both PLL output and IO cell input */
        QVERIFY(verifyVerilogContent("test_multiple_link_dedup", ".clk_out(clk_out)"));
        QVERIFY(verifyVerilogContent("test_multiple_link_dedup", ".C(clk_out)"));

        /* Check debug messages for deduplication (if any duplicates were found and ignored) */
        bool foundDuplicateMessage = false;
        for (const QString &msg : messageList) {
            if (msg.contains("Ignoring duplicate connection")) {
                foundDuplicateMessage = true;
                break;
            }
        }
        /* Note: Currently we don't expect true duplicates in this test case
         * since each connection is to a different instance */
    }

    void testGenerateWithExactDuplicateLinks()
    {
        messageList.clear();

        /* Create a netlist file with exact duplicate links (same instance, same port, same net) */
        const QString content = R"(
instance:
  u_cpu0:
    module: c906
    port:
      axim_clk_en:
        link: enable_signal
      sys_apb_rst_b:
        link: reset_signal

  # This would create a duplicate if we had two identical link statements
  # We'll test this by manually adding to the net section after link processing
net:
  enable_signal:
    - instance: u_cpu0
      port: axim_clk_en
    # This duplicate should be detected and ignored during processing
  reset_signal:
    - instance: u_cpu0
      port: sys_apb_rst_b
)";

        /* Create netlist file */
        const QString filePath = createTempFile("test_exact_duplicate_links.soc_net", content);

        /* Run the command to generate Verilog */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the output file exists */
        QVERIFY(verifyVerilogOutputExistence("test_exact_duplicate_links"));

        /* Verify basic module structure */
        QVERIFY(
            verifyVerilogContent("test_exact_duplicate_links", "module test_exact_duplicate_links"));
        QVERIFY(verifyVerilogContent("test_exact_duplicate_links", "c906 u_cpu0"));

        /* Verify wire declarations */
        QVERIFY(verifyVerilogContent("test_exact_duplicate_links", "wire enable_signal"));
        QVERIFY(verifyVerilogContent("test_exact_duplicate_links", "wire reset_signal"));

        /* Verify connections exist and are properly formed */
        QVERIFY(verifyVerilogContent("test_exact_duplicate_links", ".axim_clk_en(enable_signal)"));
        QVERIFY(verifyVerilogContent("test_exact_duplicate_links", ".sys_apb_rst_b(reset_signal)"));
    }

    void testGenerateWithLinkBitSelectionDeduplication()
    {
        messageList.clear();

        /* Create a netlist file with link bit selection deduplication */
        const QString content = R"(
instance:
  u_ampfifo_east0:
    module: ampfifo_2phase
    port:
      A1P0_VOUTP:
        link: vout_bus[7:4]
  u_ampfifo_east1:
    module: ampfifo_2phase
    port:
      A1P0_VOUTP:
        link: vout_bus[3:0]     # Different bit selection, should be allowed
  u_ampfifo_east2:
    module: ampfifo_2phase
    port:
      A1P0_VOUTP:
        link: vout_bus[7:4]     # Same instance type, same bit selection -> should deduplicate if exactly same
)";

        /* Create ampfifo_2phase module */
        const QString ampfifoContent = R"(
ampfifo_2phase:
  port:
    A1P0_VOUTP:
      type: logic[7:0]
      direction: output
)";

        /* Create the module files */
        const QDir moduleDir(projectManager.getModulePath());

        /* Create ampfifo_2phase module file */
        const QString ampfifoPath = moduleDir.filePath("ampfifo_2phase.soc_mod");
        QFile         ampfifoFile(ampfifoPath);
        if (ampfifoFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&ampfifoFile);
            stream << ampfifoContent;
            ampfifoFile.close();
        }

        /* Create netlist file */
        const QString filePath = createTempFile("test_link_bits_dedup.soc_net", content);

        /* Run the command to generate Verilog */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the output file exists */
        QVERIFY(verifyVerilogOutputExistence("test_link_bits_dedup"));

        /* Verify basic module structure */
        QVERIFY(verifyVerilogContent("test_link_bits_dedup", "module test_link_bits_dedup"));

        /* Verify all instances exist */
        QVERIFY(verifyVerilogContent("test_link_bits_dedup", "ampfifo_2phase u_ampfifo_east0"));
        QVERIFY(verifyVerilogContent("test_link_bits_dedup", "ampfifo_2phase u_ampfifo_east1"));
        QVERIFY(verifyVerilogContent("test_link_bits_dedup", "ampfifo_2phase u_ampfifo_east2"));

        /* Verify wire declaration for the shared net */
        QVERIFY(verifyVerilogContent("test_link_bits_dedup", "wire [7:0] vout_bus"));

        /* Verify bit selection in port connections */
        QVERIFY(verifyVerilogContent("test_link_bits_dedup", ".A1P0_VOUTP(vout_bus[7:4])"));
        QVERIFY(verifyVerilogContent("test_link_bits_dedup", ".A1P0_VOUTP(vout_bus[3:0])"));

        /* The third instance should also connect with [7:4] bit selection */
        /* Count how many times [7:4] appears - should be twice for the two instances using that range */
        QString verilogContent;
        for (const QString &msg : messageList) {
            if (msg.contains("Successfully generated Verilog code:")
                && msg.contains("test_link_bits_dedup.v")) {
                const QRegularExpression regex("Successfully generated Verilog code: (.+\\.v)");
                const QRegularExpressionMatch match = regex.match(msg);
                if (match.hasMatch()) {
                    const QString filePath = match.captured(1);
                    if (QFile::exists(filePath)) {
                        QFile file(filePath);
                        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                            verilogContent = file.readAll();
                            file.close();
                            break;
                        }
                    }
                }
            }
        }

        if (!verilogContent.isEmpty()) {
            /* Count occurrences of [7:4] - should appear at least twice */
            int count = verilogContent.count("vout_bus[7:4]");
            QVERIFY(count >= 2);
        }
    }

    void testGenerateWithSameInstanceMultiplePortsToSameNet()
    {
        messageList.clear();

        /* Create a netlist file with same instance multiple ports linking to same net */
        const QString content = R"(
instance:
  u_test_core:
    module: test_core
    port:
      out_a:
        link: shared_signal
      out_b:
        link: shared_signal
      out_c:
        link: shared_signal
      out_d:
        link: shared_signal
)";

        /* Create test_core module */
        const QString coreContent = R"(
test_core:
  port:
    out_a:
      type: logic
      direction: output
    out_b:
      type: logic
      direction: output
    out_c:
      type: logic
      direction: output
    out_d:
      type: logic
      direction: output
)";

        /* Create the module files */
        const QDir moduleDir(projectManager.getModulePath());

        /* Create test_core module file */
        const QString corePath = moduleDir.filePath("test_core.soc_mod");
        QFile         coreFile(corePath);
        if (coreFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&coreFile);
            stream << coreContent;
            coreFile.close();
        }

        /* Create netlist file */
        const QString filePath
            = createTempFile("test_same_instance_multiple_ports.soc_net", content);

        /* Run the command to generate Verilog */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the output file exists */
        QVERIFY(verifyVerilogOutputExistence("test_same_instance_multiple_ports"));

        /* Verify basic module structure */
        QVERIFY(verifyVerilogContent(
            "test_same_instance_multiple_ports", "module test_same_instance_multiple_ports"));
        QVERIFY(verifyVerilogContent("test_same_instance_multiple_ports", "test_core u_test_core"));

        /* Verify wire declaration for the shared net */
        QVERIFY(verifyVerilogContent("test_same_instance_multiple_ports", "wire shared_signal"));

        /* Verify all four ports connect to the same net */
        QVERIFY(verifyVerilogContent("test_same_instance_multiple_ports", ".out_a(shared_signal)"));
        QVERIFY(verifyVerilogContent("test_same_instance_multiple_ports", ".out_b(shared_signal)"));
        QVERIFY(verifyVerilogContent("test_same_instance_multiple_ports", ".out_c(shared_signal)"));
        QVERIFY(verifyVerilogContent("test_same_instance_multiple_ports", ".out_d(shared_signal)"));
    }

    void testCombSeqFsmOutputDriveAnalysis()
    {
        messageList.clear();

        /* Create a netlist with comb/seq/fsm outputs that should drive nets */
        const QString content = R"(
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
  status_out:
    type: logic
    direction: out
  counter_out:
    type: logic[3:0]
    direction: out

# Test comb output driving
comb:
  - out: data_out[7:4]
    expr: "4'b1010"
  - out: status_out
    expr: "1'b1"

# Test seq output driving
seq:
  - reg: counter_out
    clk: clk
    next: "counter_out + 1"

# Empty instance section (required)
instance: {}
)";

        /* Create netlist file */
        const QString filePath = createTempFile("test_comb_seq_fsm_drive.soc_net", content);

        /* Run the command to generate Verilog */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the output file exists */
        QVERIFY(verifyVerilogOutputExistence("test_comb_seq_fsm_drive"));

        /* Verify basic module structure */
        QVERIFY(verifyVerilogContent("test_comb_seq_fsm_drive", "module test_comb_seq_fsm_drive"));

        /* Verify comb/seq logic is generated */
        QVERIFY(verifyVerilogContent("test_comb_seq_fsm_drive", "assign data_out"));
        QVERIFY(verifyVerilogContent("test_comb_seq_fsm_drive", "assign status_out"));
        QVERIFY(verifyVerilogContent("test_comb_seq_fsm_drive", "assign counter_out"));

        /* Verify that no undriven FIXME warnings are generated for driven outputs */
        QVERIFY(!verifyVerilogContent("test_comb_seq_fsm_drive", "FIXME: Net data_out"));
        QVERIFY(!verifyVerilogContent("test_comb_seq_fsm_drive", "FIXME: Net status_out"));
        QVERIFY(!verifyVerilogContent("test_comb_seq_fsm_drive", "FIXME: Net counter_out"));
    }

    void testCombSeqFsmOutputWithBitSelectDriveAnalysis()
    {
        messageList.clear();

        /* Create a netlist with comb outputs that have bit selection */
        const QString content = R"(
port:
  data_bus:
    type: logic[15:0]
    direction: out
  enable:
    type: logic
    direction: out

# Test comb output with bit selection
comb:
  - out: data_bus[7:0]
    expr: "8'hAA"
  - out: data_bus[15:8]
    expr: "8'h55"
  - out: enable
    expr: "1'b1"

# Empty instance section (required)
instance: {}
)";

        /* Create netlist file */
        const QString filePath = createTempFile("test_comb_bit_select.soc_net", content);

        /* Run the command to generate Verilog */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the output file exists */
        QVERIFY(verifyVerilogOutputExistence("test_comb_bit_select"));

        /* Verify basic module structure */
        QVERIFY(verifyVerilogContent("test_comb_bit_select", "module test_comb_bit_select"));

        /* Verify that no width mismatch warnings are generated when bit selection is used */
        QVERIFY(!verifyVerilogContent("test_comb_bit_select", "FIXME: Net data_bus width mismatch"));
        QVERIFY(!verifyVerilogContent("test_comb_bit_select", "FIXME: Port data_bus"));

        /* Verify that no undriven warnings are generated for properly driven outputs */
        QVERIFY(!verifyVerilogContent("test_comb_bit_select", "FIXME: Net data_bus"));
        QVERIFY(!verifyVerilogContent("test_comb_bit_select", "FIXME: Net enable"));
    }

    void testMultiDriverNonOverlappingBitSelections()
    {
        messageList.clear();

        /* Create a netlist with multiple drivers on non-overlapping bits */
        const QString content = R"(
port:
  ctrl_data_i:
    type: logic[63:0]
    direction: in

net:
  ctrl_data_i:
    - instance: timing_ctrl
      port: tdo
      bits: "[0]"
    - instance: power_ctrl_0
      port: tdo
      bits: "[8]"
    - instance: power_ctrl_1
      port: tdo
      bits: "[9]"
    - instance: power_ctrl_2
      port: tdo
      bits: "[10]"
    - instance: power_ctrl_3
      port: tdo
      bits: "[11]"
    - instance: power_ctrl_4
      port: tdo
      bits: "[12]"
    - instance: power_ctrl_5
      port: tdo
      bits: "[13]"
    - instance: power_ctrl_6
      port: tdo
      bits: "[14]"
    - instance: power_ctrl_7
      port: tdo
      bits: "[15]"

instance:
  timing_ctrl:
    module: timing_control
  power_ctrl_0:
    module: power_control
  power_ctrl_1:
    module: power_control
  power_ctrl_2:
    module: power_control
  power_ctrl_3:
    module: power_control
  power_ctrl_4:
    module: power_control
  power_ctrl_5:
    module: power_control
  power_ctrl_6:
    module: power_control
  power_ctrl_7:
    module: power_control

# Add comb/seq/fsm outputs for remaining bits
comb:
  - out: ctrl_data_i
    bits: "[7:1]"
    expr: "7'b0000000"
  - out: ctrl_data_i
    bits: "[63:16]"
    expr: "48'h000000000000"
)";

        /* Create timing_control module */
        const QString timingModuleContent = R"(
timing_control:
  port:
    tdo:
      type: logic
      direction: out
)";

        /* Create power_control module */
        const QString powerModuleContent = R"(
power_control:
  port:
    tdo:
      type: logic
      direction: out
)";

        /* Create the module files */
        const QDir moduleDir(projectManager.getModulePath());

        /* Create timing_control file */
        const QString timingModulePath = moduleDir.filePath("timing_control.soc_mod");
        QFile         timingModuleFile(timingModulePath);
        if (timingModuleFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&timingModuleFile);
            stream << timingModuleContent;
            timingModuleFile.close();
        }

        /* Create power_control file */
        const QString powerModulePath = moduleDir.filePath("power_control.soc_mod");
        QFile         powerModuleFile(powerModulePath);
        if (powerModuleFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&powerModuleFile);
            stream << powerModuleContent;
            powerModuleFile.close();
        }

        /* Create netlist file */
        const QString filePath = createTempFile("test_non_overlapping_bits.soc_net", content);

        /* Run the command to generate Verilog */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the output file exists */
        QVERIFY(verifyVerilogOutputExistence("test_non_overlapping_bits"));

        /* This test should NOT generate multi-driver warnings since bits don't overlap */
        QVERIFY(!verifyVerilogContent(
            "test_non_overlapping_bits", "FIXME: Net ctrl_data_i has multiple drivers"));

        /* Verify the generated Verilog has proper connections */
        QVERIFY(verifyVerilogContent("test_non_overlapping_bits", "timing_ctrl"));
        QVERIFY(verifyVerilogContent("test_non_overlapping_bits", "power_ctrl_0"));
        QVERIFY(verifyVerilogContent("test_non_overlapping_bits", "ctrl_data_i"));
    }

    void testMultiDriverWithCombSeqFsmOutput()
    {
        messageList.clear();

        /* Create a netlist with multiple drivers including comb output */
        const QString content = R"(
port:
  data_out:
    type: logic[7:0]
    direction: out

net:
  data_out:
    - instance: test_driver
      port: data_out

instance:
  test_driver:
    module: test_module

# Add a comb output that will create a multi-driver situation
comb:
  - out: data_out
    expr: "8'hFF"
)";

        /* Create test_module */
        const QString moduleContent = R"(
test_module:
  port:
    data_out:
      type: logic[7:0]
      direction: out
)";

        /* Create the module files */
        const QDir moduleDir(projectManager.getModulePath());

        /* Create test_module file */
        const QString modulePath = moduleDir.filePath("test_module.soc_mod");
        QFile         moduleFile(modulePath);
        if (moduleFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&moduleFile);
            stream << moduleContent;
            moduleFile.close();
        }

        /* Create netlist file */
        const QString filePath = createTempFile("test_multi_driver.soc_net", content);

        /* Run the command to generate Verilog */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the output file exists */
        QVERIFY(verifyVerilogOutputExistence("test_multi_driver"));

        /* Verify that multi-driver warning is generated */
        QVERIFY(
            verifyVerilogContent("test_multi_driver", "FIXME: Net data_out has multiple drivers"));

        /* Verify that both sources are identified in the warning */
        QVERIFY(verifyVerilogContent("test_multi_driver", "Comb/Seq/FSM Output: data_out"));
        QVERIFY(verifyVerilogContent("test_multi_driver", "Module: test_module"));
    }

    void testBusExpansionWidthPreservation()
    {
        messageList.clear();

        /* Create a simple test that verifies bus expansion preserves width information */
        /* Use the existing c906 module and create a simple bus-like structure */
        const QString content = R"(
---
version: "1.0"
module: "test_bus_width_preservation"
port:
  clk:
    direction: in
    type: "logic"
  rst_n:
    direction: in
    type: "logic"
instance:
  u_cpu0:
    module: "c906"
    port:
      # Test range preservation: [21:2] should not become [21:0]
      biu_pad_arid:
        type: "logic[21:2]"
      # Test another range: [15:4] should not become [15:0]  
      biu_pad_awid:
        type: "logic[15:4]"
      # Test single bit
      axim_clk_en:
        type: "logic"
net:
  test_addr_signal:
    - instance: u_cpu0
      port: biu_pad_arid
      type: "logic[21:2]"
  test_data_signal:
    - instance: u_cpu0
      port: biu_pad_awid
      type: "logic[15:4]"
  test_enable_signal:
    - instance: u_cpu0
      port: axim_clk_en
      type: "logic"
)";

        const QString filePath = createTempFile("test_bus_width_preservation.soc_net", content);

        /* Run the command to generate Verilog */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the output file exists */
        QVERIFY(verifyVerilogOutputExistence("test_bus_width_preservation"));

        /* Verify that wire declarations preserve the original range format */
        /* The preserved type should maintain [21:2] range, not convert to [21:0] */
        QVERIFY(
            verifyVerilogContent("test_bus_width_preservation", "wire [ 21:2] test_addr_signal"));

        /* Verify that [15:4] range is preserved for data */
        QVERIFY(
            verifyVerilogContent("test_bus_width_preservation", "wire [ 15:4] test_data_signal"));

        /* Verify that single-bit enable signal works correctly */
        QVERIFY(verifyVerilogContent("test_bus_width_preservation", "wire test_enable_signal"));

        /* Verify that the wire declarations do NOT use incorrect [msb:0] format */
        QVERIFY(
            !verifyVerilogContent("test_bus_width_preservation", "wire [ 21:0] test_addr_signal"));
        QVERIFY(
            !verifyVerilogContent("test_bus_width_preservation", "wire [ 15:0] test_data_signal"));
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegenerate.moc"
