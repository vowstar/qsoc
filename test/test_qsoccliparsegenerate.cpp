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
        QDir    moduleDir(projectManager.getModulePath());
        QString modulePath = moduleDir.filePath("c906.soc_mod");
        QFile   moduleFile(modulePath);
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
                QRegularExpression      re("Successfully generated Verilog code: (.+\\.v)");
                QRegularExpressionMatch match = re.match(msg);
                if (match.hasMatch()) {
                    QString filePath = match.captured(1);
                    if (QFile::exists(filePath)) {
                        return true;
                    }
                }
            }
        }

        /* Check the project output directory */
        QString projectOutputPath = projectManager.getOutputPath();
        QString projectFilePath   = QDir(projectOutputPath).filePath(baseFileName + ".v");
        if (QFile::exists(projectFilePath)) {
            return true;
        }

        return false;
    }

    /* Get Verilog content and check if it contains specific text */
    bool verifyVerilogContent(const QString &baseFileName, const QString &contentToVerify)
    {
        if (baseFileName.isNull() || contentToVerify.isNull()) {
            qDebug() << "Error: baseFileName or contentToVerify is null";
            return false;
        }

        QString verilogContent;
        QString filePath;

        /* Debug output */
        qDebug() << "\n=== [DEBUG] Verifying Verilog Content ===";
        qDebug() << "Looking for file:" << baseFileName + ".v";
        qDebug() << "Content to verify:" << contentToVerify;
        qDebug() << "Project output path:" << projectManager.getOutputPath();

        /* First try from message logs */
        qDebug() << "\nChecking message logs:";
        for (const QString &msg : messageList) {
            if (msg.isNull()) {
                qDebug() << "Found null message in messageList";
                continue;
            }
            qDebug() << "Message:" << msg;
            if (msg.contains("Successfully generated Verilog code:")
                && msg.contains(baseFileName + ".v")) {
                QRegularExpression      re("Successfully generated Verilog code: (.+\\.v)");
                QRegularExpressionMatch match = re.match(msg);
                if (match.hasMatch()) {
                    filePath = match.captured(1);
                    qDebug() << "Found file path from logs:" << filePath;
                    if (!filePath.isNull() && QFile::exists(filePath)) {
                        QFile file(filePath);
                        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                            verilogContent = file.readAll();
                            file.close();
                            if (!verilogContent.isNull()) {
                                qDebug() << "Successfully read file from logs:" << filePath;
                                qDebug() << "File content (first 500 chars):\n"
                                         << verilogContent.left(500);
                                break;
                            } else {
                                qDebug() << "File content is null from logs:" << filePath;
                            }
                        } else {
                            qDebug() << "Failed to open file from logs:" << filePath;
                        }
                    } else {
                        qDebug() << "File from logs does not exist:" << filePath;
                    }
                }
            }
        }

        /* If not found from logs, check the project output directory */
        if (verilogContent.isEmpty()) {
            QString projectOutputPath = projectManager.getOutputPath();
            if (!projectOutputPath.isNull()) {
                filePath = QDir(projectOutputPath).filePath(baseFileName + ".v");
                qDebug() << "\nTrying project output path:" << filePath;
                if (!filePath.isNull() && QFile::exists(filePath)) {
                    QFile file(filePath);
                    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                        verilogContent = file.readAll();
                        file.close();
                        if (!verilogContent.isNull()) {
                            qDebug() << "Successfully read file from project output:" << filePath;
                            qDebug() << "File content (first 500 chars):\n"
                                     << verilogContent.left(500);
                        } else {
                            qDebug() << "File content is null from project output:" << filePath;
                        }
                    } else {
                        qDebug() << "Failed to open file from project output:" << filePath;
                    }
                } else {
                    qDebug() << "File does not exist in project output:" << filePath;
                }
            } else {
                qDebug() << "Project output path is null";
            }
        }

        /* Empty content check */
        if (verilogContent.isEmpty()) {
            qDebug() << "\nCould not find or read Verilog file for" << baseFileName;
            return false;
        }

        /* Check if the content contains the text we're looking for */
        bool result = verilogContent.contains(contentToVerify);
        qDebug() << "\n=== [DEBUG] Search result ===";
        qDebug() << "Looking for:" << contentToVerify;
        qDebug() << "Found:" << result;
        if (!result) {
            qDebug() << "Content not found. File content preview:\n" << verilogContent;
        }
        return result;
    }

private slots:
    void initTestCase()
    {
        TestApp::instance();
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
    cpu0:
      port: "biu_pad_rdata"
    cpu1:
      port: "pad_biu_rdata"
    cpu2:
      port: "pad_tdt_dm_rdata"
instance:
  cpu0:
    module: "c906"
  cpu1:
    module: "c906"
  cpu2:
    module: "c906"
)";
        QString       filePath = createTempFile("max_width_test.soc_net", content);

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
        QString       filePath = createTempFile("tie_overflow_test.soc_net", content);

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Print all messages for debugging */
        qDebug() << "Message list contents:";
        for (const QString &msg : messageList) {
            qDebug() << msg;
        }

        /* Verify that the Verilog file was generated */
        QVERIFY(verifyVerilogOutputExistence("tie_overflow_test"));

        /* Verify the module name */
        QVERIFY(verifyVerilogContent("tie_overflow_test", "module tie_overflow_test"));

        /* Verify CPU instance */
        QVERIFY(verifyVerilogContent("tie_overflow_test", "c906 cpu0"));

        /* Verify the tie values are correctly formatted in the output */
        QVERIFY(verifyVerilogContent("tie_overflow_test", "128'hDEADBEEFDEADBEEFDEADBEEFDEADBEEF"));
        QVERIFY(verifyVerilogContent("tie_overflow_test", "100'h12345678901234567890"));

        /* Check both possible formats for large decimal value */
        bool hasLargeDecimal = verifyVerilogContent("tie_overflow_test", "18446744073709551616");
        if (!hasLargeDecimal) {
            hasLargeDecimal = verifyVerilogContent("tie_overflow_test", "128'd18446744073709551616");
        }
        QVERIFY(hasLargeDecimal);

        /* Verify 64-bit limit values */
        QVERIFY(verifyVerilogContent("tie_overflow_test", "64'hFFFFFFFFFFFFFFFF"));
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
        QString       filePath = createTempFile("tie_format_test.soc_net", content);

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Print all messages for debugging */
        qDebug() << "Message list contents:";
        for (const QString &msg : messageList) {
            qDebug() << msg;
        }

        /* Verify that the Verilog file was generated */
        QVERIFY(verifyVerilogOutputExistence("tie_format_test"));

        /* Verify the module name */
        QVERIFY(verifyVerilogContent("tie_format_test", "module tie_format_test"));

        /* Verify CPU instance */
        QVERIFY(verifyVerilogContent("tie_format_test", "c906 cpu0"));

        /* Verify binary format preserved */
        QVERIFY(verifyVerilogContent("tie_format_test", "1'b0"));

        /* Verify decimal format preserved */
        QVERIFY(verifyVerilogContent("tie_format_test", "1'd1"));

        /* Verify binary format preserved */
        QVERIFY(verifyVerilogContent("tie_format_test", "8'b10101010"));

        /* Verify binary format preserved */
        QVERIFY(verifyVerilogContent("tie_format_test", "64'b101010"));

        /* Verify 8-bit hex value - note that it appears as lowercase in the file */
        QVERIFY(verifyVerilogContent("tie_format_test", "8'haa"));

        /* Verify truncated hex value */
        QVERIFY(verifyVerilogContent("tie_format_test", "8'hff"));

        /* Verify decimal value */
        QVERIFY(verifyVerilogContent("tie_format_test", "40'd42"));

        /* Verify octal format is preserved */
        QVERIFY(verifyVerilogContent("tie_format_test", "128'o77"));
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
    cpu0:
      port: pll_core_cpuclk
    cpu1:
      port: pll_core_cpuclk
  reset_net:
    cpu0:
      port: pad_cpu_rst_b
    cpu1:
      port: pad_cpu_rst_b
  arvalid_net:
    cpu0:
      port: biu_pad_arvalid
    cpu1:
      port: biu_pad_arvalid
)";
        QString       filePath = createTempFile("invert_test.soc_net", content);

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
        QVERIFY(verifyVerilogContent("invert_test", ".axim_clk_en     (1'd0)"));
        QVERIFY(verifyVerilogContent("invert_test", ".biu_pad_arvalid (~arvalid_net)"));

        /* Verify invert logic for cpu1 */
        QVERIFY(verifyVerilogContent("invert_test", "cpu1"));
        QVERIFY(verifyVerilogContent("invert_test", ".axim_clk_en     (~(1'd1"));
        QVERIFY(verifyVerilogContent("invert_test", ".biu_pad_arvalid (~arvalid_net)"));

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
        QString       filePath = createTempFile("tie_width_test.soc_net", content);

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
        QVERIFY(verifyVerilogContent("tie_width_test", ".axim_clk_en     (1'b0)"));

        /* Verify truncation for 8-bit to 1-bit - should show FIXME comment */
        QVERIFY(verifyVerilogContent(
            "tie_width_test", "FIXME: Value 8'b10101010 wider than port width 1 bits"));
        QVERIFY(verifyVerilogContent("tie_width_test", ".sys_apb_rst_b   (1'b0"));

        /* Verify zero extension for 1-bit to 8-bit (not visible in output as port is missing) */

        /* Verify truncation for large decimal to 8-bit */
        QVERIFY(verifyVerilogContent(
            "tie_width_test", "FIXME: Value 16'd300 wider than port width 8 bits"));
        QVERIFY(verifyVerilogContent("tie_width_test", ".pad_biu_bid     (8'd44"));
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
        QString       filePath = createTempFile("tie_format_input_test.soc_net", content);

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
        QVERIFY(verifyVerilogContent("tie_format_input_test", ".axim_clk_en     (1'b0)"));
        QVERIFY(verifyVerilogContent("tie_format_input_test", ".sys_apb_rst_b   (1'b1)"));

        /* Verify hex format handling (note: usually lowercase in output) */
        QVERIFY(verifyVerilogContent("tie_format_input_test", ".pad_biu_bid     (8'hff)"));
        QVERIFY(verifyVerilogContent("tie_format_input_test", ".pad_biu_rid     (8'haa)"));

        /* Verify octal format handling */
        QVERIFY(verifyVerilogContent("tie_format_input_test", ".pad_cpu_rvba    (40'o77)"));

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
        QString       filePath = createTempFile("complex_tie_test.soc_net", content);

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
        QVERIFY(verifyVerilogContent("complex_tie_test", ".axim_clk_en     (1'b0)"));
        QVERIFY(verifyVerilogContent("complex_tie_test", ".sys_apb_rst_b   (1'd1"));

        /* Verify the tie+invert combination */
        QVERIFY(verifyVerilogContent("complex_tie_test", ".pad_biu_bid     (~(8'hff))"));

        /* Verify truncation with warning comment */
        QVERIFY(verifyVerilogContent(
            "complex_tie_test", "FIXME: Value 16'habcd wider than port width 8 bits"));
        QVERIFY(verifyVerilogContent("complex_tie_test", ".pad_biu_rid     (8'hcd"));

        /* Verify cpu1 ties are different from cpu0 */
        QVERIFY(verifyVerilogContent("complex_tie_test", "cpu1"));
        QVERIFY(verifyVerilogContent("complex_tie_test", ".axim_clk_en     (1'b1)"));
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
        QString       filePath1 = createTempFile("example1.soc_net", content1);
        QString       filePath2 = createTempFile("example2.soc_net", content2);

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

        /* Verify example2 module content */
        QVERIFY(verifyVerilogContent("example2", "module example2"));
        QVERIFY(verifyVerilogContent("example2", "c906 cpu0"));
        QVERIFY(verifyVerilogContent("example2", "input clk"));
        QVERIFY(verifyVerilogContent("example2", "input rst_n"));
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegenerate.moc"
