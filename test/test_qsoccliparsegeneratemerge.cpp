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

    void testMergeCombSections()
    {
        /* Create first netlist file with comb section */
        QString netlist1Content = R"(
# First netlist with comb logic
port:
  a:
    direction: input
    type: logic
  b:
    direction: input
    type: logic
  y1:
    direction: output
    type: logic
  y2:
    direction: output
    type: logic

instance: {}

net: {}

comb:
  - out: y1
    expr: "a & b"
  - out: y2
    expr: "a | b"
)";

        /* Create second netlist file with additional comb section */
        QString netlist2Content = "# Second netlist with more comb logic\n"
                                  "port:\n"
                                  "  c:\n"
                                  "    direction: input\n"
                                  "    type: logic\n"
                                  "  d:\n"
                                  "    direction: input\n"
                                  "    type: logic\n"
                                  "  y3:\n"
                                  "    direction: output\n"
                                  "    type: logic\n"
                                  "  y4:\n"
                                  "    direction: output\n"
                                  "    type: logic\n"
                                  "\n"
                                  "instance: {}\n"
                                  "\n"
                                  "net: {}\n"
                                  "\n"
                                  "comb:\n"
                                  "  - out: y3\n"
                                  "    expr: \"c ^ d\"\n"
                                  "  - out: y4\n"
                                  "    expr: \"~(c & d)\"\n";

        QString netlist1Path = createTempFile("test_merge1.soc_net", netlist1Content);
        QString netlist2Path = createTempFile("test_merge2.soc_net", netlist2Content);
        QVERIFY(!netlist1Path.isEmpty());
        QVERIFY(!netlist2Path.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "--merge" << "-d"
                 << projectManager.getCurrentPath() << netlist1Path << netlist2Path;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        /* Check if Verilog file was generated - uses first file's basename */
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_merge1.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        qDebug() << "Generated Verilog content:" << verilogContent;

        /* Verify all comb logic from both files is present */
        QVERIFY(verilogContent.contains("assign y1 = a & b;"));    /* From first file */
        QVERIFY(verilogContent.contains("assign y2 = a | b;"));    /* From first file */
        QVERIFY(verilogContent.contains("assign y3 = c ^ d;"));    /* From second file */
        QVERIFY(verilogContent.contains("assign y4 = ~(c & d);")); /* From second file */

        /* Verify all ports from both files are present */
        QVERIFY(verilogContent.contains("input  a"));
        QVERIFY(verilogContent.contains("input  b"));
        QVERIFY(verilogContent.contains("input  c"));
        QVERIFY(verilogContent.contains("input  d"));
        QVERIFY(verilogContent.contains("output y1"));
        QVERIFY(verilogContent.contains("output y2"));
        QVERIFY(verilogContent.contains("output y3"));
        QVERIFY(verilogContent.contains("output y4"));
    }

    void testMergeSeqSections()
    {
        /* Create first netlist file with seq section */
        QString netlist1Content = R"(
# First netlist with sequential logic
port:
  clk:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  data_in1:
    direction: input
    type: logic[7:0]
  data_in2:
    direction: input
    type: logic[7:0]
  reg1:
    direction: output
    type: logic[7:0]
  reg2:
    direction: output
    type: logic[7:0]

instance: {}

net: {}

seq:
  - reg: reg1
    clk: clk
    rst: rst_n
    rst_val: "8'h00"
    next: data_in1
  - reg: reg2
    clk: clk
    rst: rst_n
    rst_val: "8'hFF"
    next: data_in2
)";

        /* Create second netlist file with additional seq section */
        QString netlist2Content = R"(
# Second netlist with more sequential logic
port:
  enable:
    direction: input
    type: logic
  data_in3:
    direction: input
    type: logic[15:0]
  reg3:
    direction: output
    type: logic[15:0]

instance: {}

net: {}

seq:
  - reg: reg3
    clk: clk
    rst: rst_n
    rst_val: "16'h0000"
    enable: enable
    next: data_in3
)";

        QString netlist1Path = createTempFile("test_seq_merge1.soc_net", netlist1Content);
        QString netlist2Path = createTempFile("test_seq_merge2.soc_net", netlist2Content);
        QVERIFY(!netlist1Path.isEmpty());
        QVERIFY(!netlist2Path.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "--merge" << "-d"
                 << projectManager.getCurrentPath() << netlist1Path << netlist2Path;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        /* Check if Verilog file was generated */
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_seq_merge1.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        qDebug() << "Generated sequential Verilog content:" << verilogContent;

        /* Verify all sequential logic from both files is present */
        /* Should have 3 always blocks total */
        int alwaysBlockCount = verilogContent.count("always @(posedge clk");
        QVERIFY(alwaysBlockCount == 3);

        /* Verify register assignments from first file */
        QVERIFY(verilogContent.contains("reg1 <= 8'h00;"));
        QVERIFY(verilogContent.contains("reg1 <= data_in1;"));
        QVERIFY(verilogContent.contains("reg2 <= 8'hFF;"));
        QVERIFY(verilogContent.contains("reg2 <= data_in2;"));

        /* Verify register assignment from second file */
        QVERIFY(verilogContent.contains("reg3 <= 16'h0000;"));
        QVERIFY(verilogContent.contains("reg3 <= data_in3;"));
        QVERIFY(verilogContent.contains("if (enable) begin"));

        /* Verify all ports from both files are present using normalized whitespace */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "input clk"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "input rst_n"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "input enable"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "input [7:0] data_in1"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "input [7:0] data_in2"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "input [15:0] data_in3"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "output [7:0] reg1"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "output [7:0] reg2"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "output [15:0] reg3"));
    }

    void testMergeMixedSections()
    {
        /* Create first netlist file with both comb and seq */
        QString netlist1Content = R"(
# First netlist with mixed logic
port:
  clk:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  sel:
    direction: input
    type: logic
  a:
    direction: input
    type: logic[7:0]
  b:
    direction: input
    type: logic[7:0]
  mux_out:
    direction: output
    type: logic[7:0]
  reg_out:
    direction: output
    type: logic[7:0]

instance: {}

net: {}

comb:
  - out: mux_out
    if:
      - cond: "sel"
        then: "a"
    default: "b"

seq:
  - reg: reg_out
    clk: clk
    rst: rst_n
    rst_val: "8'h00"
    next: mux_out
)";

        /* Create second netlist file with additional mixed logic */
        QString netlist2Content = R"(
# Second netlist with more mixed logic
port:
  c:
    direction: input
    type: logic[7:0]
  and_out:
    direction: output
    type: logic[7:0]
  shift_reg:
    direction: output
    type: logic[7:0]

instance: {}

net: {}

comb:
  - out: and_out
    expr: "a & c"

seq:
  - reg: shift_reg
    clk: clk
    rst: rst_n
    rst_val: "8'hAA"
    next: "shift_reg << 1"
)";

        QString netlist1Path = createTempFile("test_mixed_merge1.soc_net", netlist1Content);
        QString netlist2Path = createTempFile("test_mixed_merge2.soc_net", netlist2Content);
        QVERIFY(!netlist1Path.isEmpty());
        QVERIFY(!netlist2Path.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "--merge" << "-d"
                 << projectManager.getCurrentPath() << netlist1Path << netlist2Path;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        /* Check if Verilog file was generated */
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_mixed_merge1.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        qDebug() << "Generated mixed logic Verilog content:" << verilogContent;

        /* Verify combinational logic from both files */
        QVERIFY(verilogContent.contains("always @(*) begin"));       /* Conditional logic */
        QVERIFY(verilogContent.contains("assign and_out = a & c;")); /* Simple assign */
        QVERIFY(verilogContent.contains("if (sel)"));
        QVERIFY(verilogContent.contains("mux_out = a;"));
        QVERIFY(verilogContent.contains("mux_out = b;"));

        /* Verify sequential logic from both files */
        int alwaysSeqCount = verilogContent.count("always @(posedge clk");
        QVERIFY(alwaysSeqCount == 2);
        QVERIFY(verilogContent.contains("reg_out <= 8'h00;"));
        QVERIFY(verilogContent.contains("reg_out <= mux_out;"));
        QVERIFY(verilogContent.contains("shift_reg <= 8'hAA;"));
        QVERIFY(verilogContent.contains("shift_reg <= shift_reg << 1;"));

        /* Verify all ports are present */
        QVERIFY(verilogContent.contains("input  [7:0] a"));
        QVERIFY(verilogContent.contains("input  [7:0] b"));
        QVERIFY(verilogContent.contains("input  [7:0] c"));
        QVERIFY(verilogContent.contains("output [7:0] mux_out"));
        QVERIFY(verilogContent.contains("output [7:0] and_out"));
        QVERIFY(verilogContent.contains("output [7:0] reg_out"));
        QVERIFY(verilogContent.contains("output [7:0] shift_reg"));
    }

    void testMergeThreeFiles()
    {
        /* Test merging three files to ensure order preservation */
        QString netlist1Content = R"(
port:
  in1:
    direction: input
    type: logic

instance: {}
net: {}

comb:
  - out: out1
    expr: "in1"
)";

        QString netlist2Content = R"(
port:
  in2:
    direction: input
    type: logic

instance: {}
net: {}

comb:
  - out: out2
    expr: "in2"
)";

        QString netlist3Content = R"(
port:
  in3:
    direction: input
    type: logic

instance: {}
net: {}

comb:
  - out: out3
    expr: "in3"
)";

        QString netlist1Path = createTempFile("test_three1.soc_net", netlist1Content);
        QString netlist2Path = createTempFile("test_three2.soc_net", netlist2Content);
        QString netlist3Path = createTempFile("test_three3.soc_net", netlist3Content);
        QVERIFY(!netlist1Path.isEmpty());
        QVERIFY(!netlist2Path.isEmpty());
        QVERIFY(!netlist3Path.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "--merge" << "-d"
                 << projectManager.getCurrentPath() << netlist1Path << netlist2Path << netlist3Path;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        /* Check if Verilog file was generated */
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_three1.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify all assignments are present in order */
        QVERIFY(verilogContent.contains("assign out1 = in1;"));
        QVERIFY(verilogContent.contains("assign out2 = in2;"));
        QVERIFY(verilogContent.contains("assign out3 = in3;"));

        /* Verify order is preserved - out1 should come before out2, out2 before out3 */
        int pos1 = verilogContent.indexOf("assign out1 = in1;");
        int pos2 = verilogContent.indexOf("assign out2 = in2;");
        int pos3 = verilogContent.indexOf("assign out3 = in3;");

        QVERIFY(pos1 >= 0 && pos2 >= 0 && pos3 >= 0);
        QVERIFY(pos1 < pos2);
        QVERIFY(pos2 < pos3);
    }

    void testMergeInstanceSections()
    {
        /* Test that instance sections are also properly merged */
        QString netlist1Content = R"(
port:
  clk:
    direction: input
    type: logic

instance:
  inst1:
    module: dummy_module
    port:
      clk:
        link: clk

net: {}
comb: []
)";

        QString netlist2Content = R"(
port:
  data:
    direction: input
    type: logic[7:0]

instance:
  inst2:
    module: another_module
    port:
      data_in:
        link: data

net: {}
comb: []
)";

        QString netlist1Path = createTempFile("test_inst1.soc_net", netlist1Content);
        QString netlist2Path = createTempFile("test_inst2.soc_net", netlist2Content);
        QVERIFY(!netlist1Path.isEmpty());
        QVERIFY(!netlist2Path.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "--merge" << "-d"
                 << projectManager.getCurrentPath() << netlist1Path << netlist2Path;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        /* Check if Verilog file was generated */
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_inst1.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify both instances are present */
        QVERIFY(verilogContent.contains("inst1"));
        QVERIFY(verilogContent.contains("inst2"));
        QVERIFY(verilogContent.contains("dummy_module"));
        QVERIFY(verilogContent.contains("another_module"));

        /* Verify all ports are present using normalized whitespace */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "input clk"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "input [7:0] data"));
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)
#include "test_qsoccliparsegeneratemerge.moc"
