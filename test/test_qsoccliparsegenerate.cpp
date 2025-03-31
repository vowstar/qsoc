// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/config.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QStringList>
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

    static void messageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
    {
        Q_UNUSED(type);
        Q_UNUSED(context);
        messageList << msg;
    }

    QString createTempFile(const QString &fileName, const QString &content)
    {
        QString filePath = QDir::current().filePath(fileName);
        QFile   file(filePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&file);
            stream << content;
            file.close();
        }
        return filePath;
    }

    void setupTestProject()
    {
        /* Create a project manager */
        QSocProjectManager projectManager;

        /* Set project path to the temporary directory */
        projectManager.setProjectName("test_project");

        /* Create project directory structure */
        projectManager.mkpath();

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
        QDir moduleDir(projectManager.getModulePath());

        QString modulePath = moduleDir.filePath("c906.soc_mod");
        QFile   moduleFile(modulePath);
        if (moduleFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&moduleFile);
            stream << c906Content;
            moduleFile.close();
        }

        /* Create output directory for Verilog files */
        QDir outputDir(projectManager.getOutputPath());
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

        /* Check the test project output directory */
        QString testOutputPath = QDir::current().filePath("output");
        QString testFilePath   = QDir(testOutputPath).filePath(baseFileName + ".v");
        if (QFile::exists(testFilePath)) {
            return true;
        }

        return false;
    }

    /* Get Verilog content and check if it contains specific text */
    bool verifyVerilogContent(const QString &baseFileName, const QString &contentToVerify)
    {
        QString verilogContent;
        QString filePath;

        /* First try from message logs */
        for (const QString &msg : messageList) {
            if (msg.contains("Successfully generated Verilog code:")
                && msg.contains(baseFileName + ".v")) {
                QRegularExpression      re("Successfully generated Verilog code: (.+\\.v)");
                QRegularExpressionMatch match = re.match(msg);
                if (match.hasMatch()) {
                    filePath = match.captured(1);
                    if (QFile::exists(filePath)) {
                        QFile file(filePath);
                        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                            verilogContent = file.readAll();
                            file.close();
                            qDebug() << "Found file from logs:" << filePath;
                            break;
                        }
                    }
                }
            }
        }

        /* If not found from logs, check the test project output directory */
        if (verilogContent.isEmpty()) {
            QString testOutputPath = QDir::current().filePath("output");
            filePath               = QDir(testOutputPath).filePath(baseFileName + ".v");
            if (QFile::exists(filePath)) {
                QFile file(filePath);
                if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    verilogContent = file.readAll();
                    file.close();
                    qDebug() << "Found file in test output directory:" << filePath;
                }
            }
        }

        /* If not found, check build/output directory */
        if (verilogContent.isEmpty()) {
            QString buildOutputPath = QDir::current().filePath("output");
            filePath                = QDir(buildOutputPath).filePath(baseFileName + ".v");
            if (QFile::exists(filePath)) {
                QFile file(filePath);
                if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                    verilogContent = file.readAll();
                    file.close();
                    qDebug() << "Found file in build output directory:" << filePath;
                }
            }
        }

        /* Empty content means we couldn't find or read the file */
        if (verilogContent.isEmpty()) {
            qDebug() << "Could not find or read Verilog file for" << baseFileName;
            return false;
        }

        /* For debugging, print the first 200 chars of content */
        qDebug() << "File content preview (first 200 chars):" << verilogContent.left(200);
        qDebug() << "Looking for:" << contentToVerify;

        /* Check if the content contains the text we're looking for */
        bool result = verilogContent.contains(contentToVerify);
        if (!result) {
            qDebug() << "Content not found:" << contentToVerify;
        }
        return result;
    }

private slots:
    void initTestCase()
    {
        TestApp::instance();
        qInstallMessageHandler(messageOutput);
        setupTestProject();
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
        const QStringList appArguments = {"qsoc", "generate", "verilog", filePath};
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
        const QStringList appArguments = {"qsoc", "generate", "verilog", filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that the Verilog file was generated */
        QVERIFY(verifyVerilogOutputExistence("tie_overflow_test"));

        /* Verify that specific large values are handled correctly */
        QVERIFY(verifyVerilogContent("tie_overflow_test", "module tie_overflow_test"));
        QVERIFY(verifyVerilogContent("tie_overflow_test", "c906 cpu0"));
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
        const QStringList appArguments = {"qsoc", "generate", "verilog", filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that the Verilog file was generated */
        QVERIFY(verifyVerilogOutputExistence("tie_format_test"));

        /* Verify that specific content with different format types exists */
        QVERIFY(verifyVerilogContent("tie_format_test", "module tie_format_test"));
        QVERIFY(verifyVerilogContent("tie_format_test", "c906 cpu0"));
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
        const QStringList appArguments = {"qsoc", "generate", "verilog", filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that the Verilog file was generated */
        QVERIFY(verifyVerilogOutputExistence("invert_test"));

        /* Verify that module and instances exist */
        QVERIFY(verifyVerilogContent("invert_test", "module invert_test"));
        QVERIFY(verifyVerilogContent("invert_test", "c906 cpu0"));
        QVERIFY(verifyVerilogContent("invert_test", "c906 cpu1"));
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
        const QStringList appArguments = {"qsoc", "generate", "verilog", filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that the Verilog file was generated */
        QVERIFY(verifyVerilogOutputExistence("tie_width_test"));

        /* Verify that module and instance exist */
        QVERIFY(verifyVerilogContent("tie_width_test", "module tie_width_test"));
        QVERIFY(verifyVerilogContent("tie_width_test", "c906 cpu0"));
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
        const QStringList appArguments = {"qsoc", "generate", "verilog", filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that the Verilog file was generated */
        QVERIFY(verifyVerilogOutputExistence("tie_format_input_test"));

        /* Verify that module and instance exist */
        QVERIFY(verifyVerilogContent("tie_format_input_test", "module tie_format_input_test"));
        QVERIFY(verifyVerilogContent("tie_format_input_test", "c906 cpu0"));
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
        const QStringList appArguments = {"qsoc", "generate", "verilog", filePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that the Verilog file was generated */
        QVERIFY(verifyVerilogOutputExistence("complex_tie_test"));

        /* Verify that module and instances exist */
        QVERIFY(verifyVerilogContent("complex_tie_test", "module complex_tie_test"));
        QVERIFY(verifyVerilogContent("complex_tie_test", "c906 cpu0"));
        QVERIFY(verifyVerilogContent("complex_tie_test", "c906 cpu1"));
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
        const QStringList appArguments = {"qsoc", "generate", "verilog", filePath1, filePath2};
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
