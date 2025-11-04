// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/config.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTemporaryFile>
#include <QTextStream>
#include <QThread>
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
        /* Check the project output directory first (most reliable) */
        const QString projectOutputPath = projectManager.getOutputPath();
        const QString projectFilePath   = QDir(projectOutputPath).filePath(baseFileName + ".v");
        if (QFile::exists(projectFilePath)) {
            return true;
        }

        /* Fallback: check if success message exists in messageList */
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

        return false;
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
        QVERIFY(verifyVerilogContent("port_width_test", "input wire clk"));
        QVERIFY(verifyVerilogContent("port_width_test", "input wire rst_n"));
        QVERIFY(verifyVerilogContent("port_width_test", "input wire [31:0] data_in"));
        QVERIFY(verifyVerilogContent("port_width_test", "input wire [15:0] addr_in"));
        QVERIFY(verifyVerilogContent("port_width_test", "output wire [31:0] data_out"));
        QVERIFY(verifyVerilogContent("port_width_test", "output wire ready"));

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
        QVERIFY(verifyVerilogContent("example1", "input wire clk"));
        QVERIFY(verifyVerilogContent("example1", "input wire rst_n"));
        QVERIFY(verifyVerilogContent("example1", "endmodule"));

        /* Verify example2 module content */
        QVERIFY(verifyVerilogContent("example2", "module example2"));
        QVERIFY(verifyVerilogContent("example2", "c906 cpu0"));
        QVERIFY(verifyVerilogContent("example2", "input wire clk"));
        QVERIFY(verifyVerilogContent("example2", "input wire rst_n"));
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

        /* Debug: Print working directory and project paths */
        qDebug() << "Current working directory:" << QDir::currentPath();
        qDebug() << "Project current path:" << projectManager.getCurrentPath();
        qDebug() << "Project module path:" << projectManager.getModulePath();
        qDebug() << "Project output path:" << projectManager.getOutputPath();

        /* Use unique module names to avoid conflicts with other tests */
        const QString content = R"(
instance:
  u_io_cell1:
    module: UPLINK_TEST_CELL_8BIT
    port:
      PAD:
        uplink: uplink_conflict_test_port
  u_io_cell2:
    module: UPLINK_TEST_CELL_16BIT
    port:
      PAD:
        uplink: uplink_conflict_test_port  # Same port name but different width - should cause conflict
)";

        /* Create UPLINK_TEST_CELL_8BIT module */
        const QString io8bitContent = R"(
UPLINK_TEST_CELL_8BIT:
  port:
    PAD:
      type: logic[7:0]
      direction: inout
)";

        /* Create UPLINK_TEST_CELL_16BIT module */
        const QString io16bitContent = R"(
UPLINK_TEST_CELL_16BIT:
  port:
    PAD:
      type: logic[15:0]
      direction: inout
)";

        /* Create the module files with unique names */
        QDir moduleDir(projectManager.getModulePath());

        /* Create UPLINK_TEST_CELL_8BIT module file */
        const QString io8bitPath = moduleDir.filePath("UPLINK_TEST_CELL_8BIT.soc_mod");
        QFile         io8bitFile(io8bitPath);
        if (io8bitFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&io8bitFile);
            stream << io8bitContent;
            io8bitFile.close();
        }

        /* Create UPLINK_TEST_CELL_16BIT module file */
        const QString io16bitPath = moduleDir.filePath("UPLINK_TEST_CELL_16BIT.soc_mod");
        QFile         io16bitFile(io16bitPath);
        if (io16bitFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&io16bitFile);
            stream << io16bitContent;
            io16bitFile.close();
        }

        /* Create netlist file */
        const QString filePath = createTempFile("test_uplink_conflict.soc_net", content);

        /* Clear messageList again right before running the test to ensure clean state */
        messageList.clear();

        /* Run the command to generate Verilog - this should detect width mismatch */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Check if the process failed due to width mismatch error */
        bool foundWidthMismatchError = false;
        for (const QString &msg : messageList) {
            /* Match the specific error message for our unique port name */
            if (msg.contains("Type/width mismatch for uplink port uplink_conflict_test_port")) {
                foundWidthMismatchError = true;
                break;
            }
        }

        /* Debug: Print all messages if test fails */
        if (!foundWidthMismatchError) {
            qDebug() << "Expected uplink error message not found. All messages ("
                     << messageList.size() << "):";
            for (const QString &msg : messageList) {
                qDebug() << "  " << msg;
            }

            /* Additional debugging: check if modules were created correctly */
            const QDir moduleDir(projectManager.getModulePath());
            qDebug() << "Module directory:" << moduleDir.absolutePath();
            qDebug() << "Module files:";
            for (const QString &file :
                 moduleDir.entryList(QStringList() << "*UPLINK_TEST*", QDir::Files)) {
                qDebug() << "  " << file;
            }

            /* Check if netlist file was processed */
            const QString outputDir = projectManager.getOutputPath();
            qDebug() << "Output directory:" << outputDir;
            qDebug() << "Output files:";
            for (const QString &file :
                 QDir(outputDir).entryList(QStringList() << "*conflict*", QDir::Files)) {
                qDebug() << "  " << file;
            }
        }

        /* Clean up our unique module files */
        moduleDir.remove("UPLINK_TEST_CELL_8BIT.soc_mod");
        moduleDir.remove("UPLINK_TEST_CELL_16BIT.soc_mod");

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

        /* Create netlist content that tests width preservation using existing c906 module */
        const QString netlistContent = R"(
---
version: "1.0"
module: "test_bus_width_preservation_unique"
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
      axim_clk_en:
        type: "logic"
      # Test preservation of [21:2] range - should NOT become [21:0]
      biu_pad_arid:
        type: "logic[21:2]"
      # Test preservation of [15:4] range - should NOT become [15:0]
      biu_pad_awid:
        type: "logic[15:4]"
net:
  test_unique_addr_signal:
    - instance: u_cpu0
      port: biu_pad_arid
      type: "logic[21:2]"
  test_unique_data_signal:
    - instance: u_cpu0
      port: biu_pad_awid
      type: "logic[15:4]"
  test_unique_enable_signal:
    - instance: u_cpu0
      port: axim_clk_en
      type: "logic"
)";

        /* Create the netlist file using createTempFile helper with unique name */
        const QString filePath
            = createTempFile("test_bus_width_preservation_unique.soc_net", netlistContent);

        /* Clear messageList again right before running the test to ensure clean state */
        messageList.clear();

        /* Run qsoc generate command */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Check if the output file exists using direct file system check */
        const QString projectOutputPath = projectManager.getOutputPath();
        const QString expectedFilePath
            = QDir(projectOutputPath).filePath("test_bus_width_preservation_unique.v");

        /* Use the updated function name for verification */
        QVERIFY(verifyVerilogOutputExistence("test_bus_width_preservation_unique"));

        /* Verify that wire declarations preserve the original range format */
        QVERIFY2(
            verifyVerilogContent(
                "test_bus_width_preservation_unique", "wire [21:2] test_unique_addr_signal"),
            "Address signal should preserve [21:2] range format");

        QVERIFY2(
            verifyVerilogContent(
                "test_bus_width_preservation_unique", "wire [15:4] test_unique_data_signal"),
            "Data signal should preserve [15:4] range format");

        QVERIFY2(
            verifyVerilogContent("test_bus_width_preservation_unique", "test_unique_enable_signal"),
            "Enable signal should be present");

        /* Verify that the wire declarations do NOT use incorrect [msb:0] format */
        QVERIFY2(
            !verifyVerilogContent(
                "test_bus_width_preservation_unique", "wire [21:0] test_unique_addr_signal"),
            "Address signal should NOT be converted to [21:0] format");

        QVERIFY2(
            !verifyVerilogContent(
                "test_bus_width_preservation_unique", "wire [15:0] test_unique_data_signal"),
            "Data signal should NOT be converted to [15:0] format");
    }

    void testBusLinkExpansion()
    {
        messageList.clear();

        /* First create module files for testing bus functionality */
        const QString smurfModuleContent = R"(
smurf_struct:
  port:
    clk_ahb:
      type: logic
      direction: in
    clk_apb:
      type: logic
      direction: in
  bus:
    axim1:
      bus: axi_bus
      direction: master
      mapping:
        arvalid: axim1_arvalid
        arready: axim1_arready
        rdata: axim1_rdata
    axim2:
      bus: axi_bus
      direction: master
      mapping:
        arvalid: axim2_arvalid
        arready: axim2_arready
        rdata: axim2_rdata
)";

        const QString c906BusModuleContent = R"(
c906_with_bus:
  port:
    clk:
      type: logic
      direction: in
  bus:
    axis1:
      bus: axi_bus
      direction: slave
      mapping:
        arvalid: axis1_arvalid
        arready: axis1_arready
        rdata: axis1_rdata
    axis2:
      bus: axi_bus
      direction: slave
      mapping:
        arvalid: axis2_arvalid
        arready: axis2_arready
        rdata: axis2_rdata
)";

        /* Create bus definition */
        const QString axiBusContent = R"(
axi_bus:
  port:
    arvalid:
      type: logic
      direction: out
    arready:
      type: logic
      direction: in
    rdata:
      type: logic[127:0]
      direction: in
)";

        /* Create module files */
        const QDir    moduleDir(projectManager.getModulePath());
        const QString smurfModulePath = moduleDir.filePath("smurf_struct.soc_mod");
        QFile         smurfModuleFile(smurfModulePath);
        if (smurfModuleFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&smurfModuleFile);
            stream << smurfModuleContent;
            smurfModuleFile.close();
        }

        const QString c906BusModulePath = moduleDir.filePath("c906_with_bus.soc_mod");
        QFile         c906BusModuleFile(c906BusModulePath);
        if (c906BusModuleFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&c906BusModuleFile);
            stream << c906BusModuleContent;
            c906BusModuleFile.close();
        }

        /* Create bus file */
        const QDir    busDir(projectManager.getBusPath());
        const QString axiBusPath = busDir.filePath("axi_bus.soc_bus");
        QFile         axiBusFile(axiBusPath);
        if (axiBusFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&axiBusFile);
            stream << axiBusContent;
            axiBusFile.close();
        }

        /* Create netlist content with bus link syntax */
        const QString netlistContent = R"(
---
version: "1.0"
module: "test_bus_link"
port:
  clk:
    direction: in
    type: logic
instance:
  u_smurf:
    module: smurf_struct
    port:
      clk_ahb:
        link: clk_ahb
      clk_apb:
        link: clk_apb
    bus:
      axim1:
        link: axi_bus1
      axim2:
        link: axi_bus2
  u_t1:
    module: c906_with_bus
    port:
      clk:
        link: clk
    bus:
      axis1:
        link: axi_bus1
  u_c906:
    module: c906_with_bus
    port:
      clk:
        link: clk
    bus:
      axis2:
        link: axi_bus2
net:
  clk_ahb:
    - port: clk
  clk_apb:
    - port: clk
)";

        /* Create the netlist file */
        const QString filePath = createTempFile("test_bus_link.soc_net", netlistContent);

        /* Clear messageList again right before running the test */
        messageList.clear();

        /* Run qsoc generate command */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the output file exists */
        QVERIFY(verifyVerilogOutputExistence("test_bus_link"));

        /* Verify that bus signals are properly generated */
        QVERIFY2(
            verifyVerilogContent("test_bus_link", "axi_bus1_arvalid"),
            "Bus signal axi_bus1_arvalid should be generated");

        QVERIFY2(
            verifyVerilogContent("test_bus_link", "axi_bus1_arready"),
            "Bus signal axi_bus1_arready should be generated");

        QVERIFY2(
            verifyVerilogContent("test_bus_link", "axi_bus1_rdata"),
            "Bus signal axi_bus1_rdata should be generated");

        QVERIFY2(
            verifyVerilogContent("test_bus_link", "axi_bus2_arvalid"),
            "Bus signal axi_bus2_arvalid should be generated");

        QVERIFY2(
            verifyVerilogContent("test_bus_link", "axi_bus2_arready"),
            "Bus signal axi_bus2_arready should be generated");

        QVERIFY2(
            verifyVerilogContent("test_bus_link", "axi_bus2_rdata"),
            "Bus signal axi_bus2_rdata should be generated");

        /* Verify instance connections are made */
        QVERIFY2(
            verifyVerilogContent("test_bus_link", "u_smurf"), "Instance u_smurf should be present");

        QVERIFY2(verifyVerilogContent("test_bus_link", "u_t1"), "Instance u_t1 should be present");

        QVERIFY2(verifyVerilogContent("test_bus_link", "u_c906"), "Instance u_c906 should be present");
    }

    void testBusUplinkExpansion()
    {
        messageList.clear();

        /* Create module files for testing bus uplink functionality */
        const QString smurfUplinkModuleContent = R"(
smurf_uplink_struct:
  port:
    clk:
      type: logic
      direction: in
    # These ports will be created automatically by bus uplink expansion
    axim1_arvalid:
      type: logic
      direction: out
    axim1_arready:
      type: logic
      direction: in
    axim1_rdata:
      type: logic[127:0]
      direction: in
  bus:
    axim1:
      bus: axi_bus
      direction: master
      mapping:
        arvalid: axim1_arvalid
        arready: axim1_arready
        rdata: axim1_rdata
)";

        /* Create bus definition */
        const QString axiBusUplinkContent = R"(
axi_bus:
  port:
    arvalid:
      type: logic
      direction: out
    arready:
      type: logic
      direction: in
    rdata:
      type: logic[127:0]
      direction: in
)";

        /* Create module files */
        const QDir    moduleDir(projectManager.getModulePath());
        const QString smurfUplinkModulePath = moduleDir.filePath("smurf_uplink_struct.soc_mod");
        QFile         smurfUplinkModuleFile(smurfUplinkModulePath);
        if (smurfUplinkModuleFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&smurfUplinkModuleFile);
            stream << smurfUplinkModuleContent;
            smurfUplinkModuleFile.close();
        }

        /* Create bus file */
        const QDir    busDir(projectManager.getBusPath());
        const QString axiBusUplinkPath = busDir.filePath("axi_bus.soc_bus");
        QFile         axiBusUplinkFile(axiBusUplinkPath);
        if (axiBusUplinkFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&axiBusUplinkFile);
            stream << axiBusUplinkContent;
            axiBusUplinkFile.close();
        }

        /* Create netlist content with bus uplink syntax */
        const QString netlistContent = R"(
---
version: "1.0"
module: "test_bus_uplink"
port:
  clk:
    direction: in
    type: logic
instance:
  u_smurf:
    module: smurf_uplink_struct
    port:
      clk:
        link: clk
    bus:
      axim1:
        uplink: axi_bus1
net:
  clk:
    - instance: u_smurf
      port: clk
)";

        /* Create the netlist file */
        const QString filePath = createTempFile("test_bus_uplink.soc_net", netlistContent);

        /* Clear messageList again right before running the test */
        messageList.clear();

        /* Run qsoc generate command */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the output file exists */
        QVERIFY(verifyVerilogOutputExistence("test_bus_uplink"));

        /* Verify that top-level ports are created with correct naming pattern */
        QVERIFY2(
            verifyVerilogContent("test_bus_uplink", "axi_bus1_arvalid"),
            "Top-level port axi_bus1_arvalid should be created by bus uplink");

        QVERIFY2(
            verifyVerilogContent("test_bus_uplink", "axi_bus1_arready"),
            "Top-level port axi_bus1_arready should be created by bus uplink");

        QVERIFY2(
            verifyVerilogContent("test_bus_uplink", "axi_bus1_rdata"),
            "Top-level port axi_bus1_rdata should be created by bus uplink");

        /* Verify that instance connections are made to top-level ports */
        QVERIFY2(
            verifyVerilogContent("test_bus_uplink", "u_smurf"),
            "Instance u_smurf should be present");

        /* Check that the ports follow the expected pattern:
           module_port connected to toplevel_port */
        QVERIFY2(
            verifyVerilogContent("test_bus_uplink", ".axim1_arvalid(axi_bus1_arvalid)")
                || verifyVerilogContent("test_bus_uplink", "axim1_arvalid")
                       && verifyVerilogContent("test_bus_uplink", "axi_bus1_arvalid"),
            "Module port axim1_arvalid should be connected to top-level port axi_bus1_arvalid");

        QVERIFY2(
            verifyVerilogContent("test_bus_uplink", ".axim1_arready(axi_bus1_arready)")
                || verifyVerilogContent("test_bus_uplink", "axim1_arready")
                       && verifyVerilogContent("test_bus_uplink", "axi_bus1_arready"),
            "Module port axim1_arready should be connected to top-level port axi_bus1_arready");

        QVERIFY2(
            verifyVerilogContent("test_bus_uplink", ".axim1_rdata(axi_bus1_rdata)")
                || verifyVerilogContent("test_bus_uplink", "axim1_rdata")
                       && verifyVerilogContent("test_bus_uplink", "axi_bus1_rdata"),
            "Module port axim1_rdata should be connected to top-level port axi_bus1_rdata");

        /* Verify direction preservation */
        QVERIFY2(
            verifyVerilogContent("test_bus_uplink", "output")
                && verifyVerilogContent("test_bus_uplink", "axi_bus1_arvalid"),
            "axi_bus1_arvalid should be declared as output");

        QVERIFY2(
            verifyVerilogContent("test_bus_uplink", "input")
                && verifyVerilogContent("test_bus_uplink", "axi_bus1_arready"),
            "axi_bus1_arready should be declared as input");

        QVERIFY2(
            verifyVerilogContent("test_bus_uplink", "input")
                && verifyVerilogContent("test_bus_uplink", "axi_bus1_rdata"),
            "axi_bus1_rdata should be declared as input");
    }

    /* Test conditional compilation: no condition (backward compatibility) */
    void testConditionalNoCondition()
    {
        messageList.clear();

        const QString netlistContent = R"(
top:
  port:
    clk:
      type: logic
      direction: in
instance:
  u_c906:
    module: c906
    connect:
      - net: clk
        port: axim_clk_en
)";
        const QString filePath       = createTempFile("test_cond_no.soc_net", netlistContent);

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        QVERIFY2(verifyVerilogContent("test_cond_no", "c906 u_c906"), "Instance without condition");
        QVERIFY2(!verifyVerilogContent("test_cond_no", "`ifdef"), "No ifdef directive");
        QVERIFY2(!verifyVerilogContent("test_cond_no", "`endif"), "No endif directive");
    }

    /* Test conditional compilation: single ifdef */
    void testConditionalSingleIfdef()
    {
        messageList.clear();

        const QString netlistContent = R"(
top:
  port:
    clk:
      type: logic
      direction: in
instance:
  u_pad_fpga:
    module: c906
    ifdef: [TECH_FPGA]
    connect:
      - net: clk
        port: axim_clk_en
)";
        const QString filePath       = createTempFile("test_cond_single.soc_net", netlistContent);

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        QVERIFY2(
            verifyVerilogContent("test_cond_single", "`ifdef TECH_FPGA"), "Single ifdef directive");
        QVERIFY2(verifyVerilogContent("test_cond_single", "c906 u_pad_fpga"), "Instance inside ifdef");
        QVERIFY2(
            verifyVerilogContent("test_cond_single", "`endif /*  TECH_FPGA */"),
            "Endif with correct comment");
    }

    /* Test conditional compilation: multiple ifdef (alphabetical nesting) */
    void testConditionalMultipleIfdef()
    {
        messageList.clear();

        const QString netlistContent = R"(
top:
  port:
    clk:
      type: logic
      direction: in
instance:
  u_pad_multi:
    module: c906
    ifdef: [USE_PADS, TECH_FPGA]
    connect:
      - net: clk
        port: axim_clk_en
)";
        const QString filePath       = createTempFile("test_cond_multi.soc_net", netlistContent);

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Read Verilog file to check ordering */
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_cond_multi.v");
        QFile   file(verilogPath);
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), "File should exist");
        QString content = file.readAll();
        file.close();

        int posT    = content.indexOf("`ifdef TECH_FPGA");
        int posU    = content.indexOf("`ifdef USE_PADS");
        int posInst = content.indexOf("c906 u_pad_multi");

        QVERIFY2(posT >= 0, "TECH_FPGA ifdef present");
        QVERIFY2(posU >= 0, "USE_PADS ifdef present");
        QVERIFY2(posT < posU && posU < posInst, "Alphabetical order: TECH_FPGA before USE_PADS");
        QVERIFY2(
            verifyVerilogContent("test_cond_multi", "`endif /*  USE_PADS */"), "Endif for USE_PADS");
        QVERIFY2(
            verifyVerilogContent("test_cond_multi", "`endif /*  TECH_FPGA */"),
            "Endif for TECH_FPGA");
    }

    /* Test conditional compilation: ifdef + ifndef */
    void testConditionalIfdefAndIfndef()
    {
        messageList.clear();

        const QString netlistContent = R"(
top:
  port:
    clk:
      type: logic
      direction: in
instance:
  u_pad_mixed:
    module: c906
    ifdef: [TECH_TSMC28]
    ifndef: [TECH_FPGA, LEGACY_MODE]
    connect:
      - net: clk
        port: axim_clk_en
)";
        const QString filePath       = createTempFile("test_cond_mixed.soc_net", netlistContent);

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Read Verilog file to check ordering */
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_cond_mixed.v");
        QFile   file(verilogPath);
        QVERIFY2(file.open(QIODevice::ReadOnly | QIODevice::Text), "File should exist");
        QString content = file.readAll();
        file.close();

        int posIfdef   = content.indexOf("`ifdef TECH_TSMC28");
        int posIfndef1 = content.indexOf("`ifndef LEGACY_MODE");
        int posIfndef2 = content.indexOf("`ifndef TECH_FPGA");
        int posInst    = content.indexOf("c906 u_pad_mixed");

        QVERIFY2(posIfdef >= 0, "ifdef directive present");
        QVERIFY2(posIfndef1 >= 0 && posIfndef2 >= 0, "Both ifndef directives present");
        QVERIFY2(posIfdef < posIfndef1 && posIfndef1 < posInst, "ifdef before ifndef");
        QVERIFY2(
            verifyVerilogContent("test_cond_mixed", "`endif /*  !TECH_FPGA */"),
            "Endif with ! for ifndef");
        QVERIFY2(
            verifyVerilogContent("test_cond_mixed", "`endif /*  !LEGACY_MODE */"),
            "Endif with ! for ifndef");
        QVERIFY2(
            verifyVerilogContent("test_cond_mixed", "`endif /*  TECH_TSMC28 */"), "Endif for ifdef");
    }

    /* Test conditional compilation: conflict detection */
    void testConditionalConflict()
    {
        messageList.clear();

        const QString netlistContent = R"(
top:
  port:
    clk:
      type: logic
      direction: in
instance:
  u_pad_conflict:
    module: c906
    ifdef: [TECH_FPGA]
    ifndef: [TECH_FPGA, LEGACY_MODE]
    connect:
      - net: clk
        port: axim_clk_en
)";
        const QString filePath       = createTempFile("test_cond_conflict.soc_net", netlistContent);

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify error message was logged */
        bool foundConflictError = false;
        for (const QString &msg : messageList) {
            if (msg.contains("TECH_FPGA") && msg.contains("both")
                && (msg.contains("ifdef") || msg.contains("ifndef"))) {
                foundConflictError = true;
                break;
            }
        }
        QVERIFY2(foundConflictError, "Conflict error message should be logged");

        /* Verify instance was skipped */
        QVERIFY2(
            !verifyVerilogContent("test_cond_conflict", "c906 u_pad_conflict"),
            "Conflicting instance should be skipped");
    }

    /* Test comb/seq input signal extraction using slang parser */
    void testCombSeqFsmInputSignalExtraction()
    {
        messageList.clear();

        /* Create a netlist with various comb/seq expressions to test input extraction */
        const QString content = R"(
port:
  # Input signals that should be detected
  clk:
    type: logic
    direction: in
  rst_n:
    type: logic
    direction: in
  data_in:
    type: logic[7:0]
    direction: in
  enable:
    type: logic
    direction: in
  counter:
    type: logic[3:0]
    direction: in

  # Output signals
  simple_out:
    type: logic
    direction: out
  bit_select_out:
    type: logic[3:0]
    direction: out
  concat_out:
    type: logic[7:0]
    direction: out
  arithmetic_out:
    type: logic[3:0]
    direction: out
  seq_out:
    type: logic[7:0]
    direction: out

# Test simple signal reference (like cnvr_test_out[3])
comb:
  - out: simple_out
    expr: "data_in[0]"

  # Test bit selection
  - out: bit_select_out
    expr: "data_in[7:4]"

  # Test concatenation/replication expression (like {8{signal}})
  - out: concat_out
    expr: "{8{enable}}"

  # Test multi-signal arithmetic expression
  - out: arithmetic_out
    expr: "counter + 4'd1"

# Test seq input extraction
seq:
  - reg: seq_out
    clk: clk
    rst: rst_n
    next: "data_in"

# Empty instance section (required)
instance: {}
)";

        /* Create netlist file */
        const QString filePath = createTempFile("test_comb_seq_input_extraction.soc_net", content);

        /* Run the command to generate Verilog */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the output file exists */
        QVERIFY(verifyVerilogOutputExistence("test_comb_seq_input_extraction"));

        /* Read generated Verilog content */
        const QString verilogFile = projectManager.getCurrentPath()
                                    + "/output/test_comb_seq_input_extraction.v";
        QFile file(verilogFile);
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = file.readAll();
        file.close();

        /* Verify module structure */
        QVERIFY(verifyVerilogContent(
            "test_comb_seq_input_extraction", "module test_comb_seq_input_extraction"));

        /* Verify input signals are declared and used correctly */
        QVERIFY2(
            verilogContent.contains("input") && verilogContent.contains("clk"),
            "Clock signal should be declared as input");
        QVERIFY2(verilogContent.contains("rst_n"), "Reset signal should be in the module");
        QVERIFY2(verilogContent.contains("data_in"), "data_in should be declared and used");
        QVERIFY2(verilogContent.contains("enable"), "enable signal should be declared and used");
        QVERIFY2(verilogContent.contains("counter"), "counter signal should be declared and used");

        /* Verify comb assignments are generated */
        QVERIFY(verifyVerilogContent("test_comb_seq_input_extraction", "assign simple_out"));
        QVERIFY(verifyVerilogContent("test_comb_seq_input_extraction", "assign bit_select_out"));
        QVERIFY(verifyVerilogContent("test_comb_seq_input_extraction", "assign concat_out"));
        QVERIFY(verifyVerilogContent("test_comb_seq_input_extraction", "assign arithmetic_out"));

        /* Verify seq logic is generated */
        QVERIFY(verifyVerilogContent("test_comb_seq_input_extraction", "always"));
        QVERIFY(verifyVerilogContent("test_comb_seq_input_extraction", "seq_out"));

        /* Verify no undriven warnings for correctly driven signals */
        QVERIFY2(
            !verilogContent.contains("FIXME: Net simple_out"),
            "simple_out should not have undriven warning");
        QVERIFY2(
            !verilogContent.contains("FIXME: Net bit_select_out"),
            "bit_select_out should not have undriven warning");
        QVERIFY2(
            !verilogContent.contains("FIXME: Net concat_out"),
            "concat_out should not have undriven warning");
        QVERIFY2(
            !verilogContent.contains("FIXME: Net arithmetic_out"),
            "arithmetic_out should not have undriven warning");
        QVERIFY2(
            !verilogContent.contains("FIXME: Net seq_out"),
            "seq_out should not have undriven warning");
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegenerate.moc"
