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

    void testSimpleSequential()
    {
        QString netlistContent = R"(
# Test netlist with simple sequential logic
port:
  clk:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  data_in:
    direction: input
    type: logic[7:0]
  data_reg:
    direction: output
    type: logic[7:0]

instance: {}

net: {}

seq:
  - reg: data_reg
    clk: clk
    rst: rst_n
    rst_val: "8'h00"
    next: data_in
)";

        QString netlistPath = createTempFile("test_simple_seq.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_simple_seq.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify sequential logic is generated */
        QVERIFY(verilogContent.contains("/* Sequential logic */"));
        QVERIFY(verilogContent.contains("always @(posedge clk or negedge rst_n) begin"));
        QVERIFY(verilogContent.contains("if (!rst_n) begin"));
        QVERIFY(verilogContent.contains("data_reg <= 8'h00;"));
        QVERIFY(verilogContent.contains("end else begin"));
        QVERIFY(verilogContent.contains("data_reg <= data_in;"));
        QVERIFY(verilogContent.contains("end"));
    }

    void testSequentialWithEnable()
    {
        QString netlistContent = R"(
# Test netlist with sequential logic and enable
port:
  clk:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  enable:
    direction: input
    type: logic
  data_in:
    direction: input
    type: logic[15:0]
  counter:
    direction: output
    type: logic[15:0]

instance: {}

net: {}

seq:
  - reg: counter
    clk: clk
    rst: rst_n
    rst_val: "16'h0000"
    enable: enable
    next: "counter + 1"
)";

        QString netlistPath = createTempFile("test_seq_enable.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_seq_enable.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify sequential logic with enable is generated */
        QVERIFY(verilogContent.contains("always @(posedge clk or negedge rst_n) begin"));
        QVERIFY(verilogContent.contains("if (!rst_n) begin"));
        QVERIFY(verilogContent.contains("counter <= 16'h0000;"));
        QVERIFY(verilogContent.contains("end else begin"));
        QVERIFY(verilogContent.contains("if (enable) begin"));
        QVERIFY(verilogContent.contains("counter <= counter + 1;"));
        QVERIFY(verilogContent.contains("end"));
    }

    void testSequentialWithConditional()
    {
        QString netlistContent = R"(
# Test netlist with sequential logic using conditional logic
port:
  clk:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  mode:
    direction: input
    type: logic[1:0]
  data_in:
    direction: input
    type: logic[7:0]
  state_reg:
    direction: output
    type: logic[7:0]

instance: {}

net: {}

seq:
  - reg: state_reg
    clk: clk
    rst: rst_n
    rst_val: "8'h00"
    if:
      - cond: "mode == 2'b00"
        then: "8'h01"
      - cond: "mode == 2'b01"
        then: "data_in"
      - cond: "mode == 2'b10"
        then: "state_reg + 1"
    default: "state_reg"
)";

        QString netlistPath = createTempFile("test_seq_conditional.soc_net", netlistContent);
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
            = QDir(projectManager.getOutputPath()).filePath("test_seq_conditional.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify conditional sequential logic is generated */
        QVERIFY(verilogContent.contains("always @(posedge clk or negedge rst_n) begin"));
        QVERIFY(verilogContent.contains("if (!rst_n) begin"));
        QVERIFY(verilogContent.contains("state_reg <= 8'h00;"));
        QVERIFY(verilogContent.contains("end else begin"));
        QVERIFY(verilogContent.contains("state_reg <= state_reg;"));
        QVERIFY(verilogContent.contains("if (mode == 2'b00)"));
        QVERIFY(verilogContent.contains("state_reg <= 8'h01;"));
        QVERIFY(verilogContent.contains("else if (mode == 2'b01)"));
        QVERIFY(verilogContent.contains("state_reg <= data_in;"));
        QVERIFY(verilogContent.contains("else if (mode == 2'b10)"));
        QVERIFY(verilogContent.contains("state_reg <= state_reg + 1;"));
    }

    void testSequentialNegativeEdge()
    {
        QString netlistContent = R"(
# Test netlist with negative edge sequential logic
port:
  clk:
    direction: input
    type: logic
  data_in:
    direction: input
    type: logic
  q:
    direction: output
    type: logic

instance: {}

net: {}

seq:
  - reg: q
    clk: clk
    edge: neg
    next: data_in
)";

        QString netlistPath = createTempFile("test_seq_negedge.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_seq_negedge.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify negative edge sequential logic is generated */
        QVERIFY(verilogContent.contains("always @(negedge clk) begin"));
        QVERIFY(verilogContent.contains("q <= data_in;"));
    }

    void testMultipleSequential()
    {
        QString netlistContent = R"(
# Test netlist with multiple sequential logic blocks
port:
  clk:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  enable:
    direction: input
    type: logic
  data_in:
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
    next: data_in
  - reg: reg2
    clk: clk
    rst: rst_n
    rst_val: "8'hFF"
    enable: enable
    next: "reg1"
)";

        QString netlistPath = createTempFile("test_multiple_seq.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_multiple_seq.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify multiple sequential logic blocks are generated */
        int alwaysBlockCount = verilogContent.count("always @(posedge clk or negedge rst_n) begin");
        QVERIFY(alwaysBlockCount == 2);
        QVERIFY(verilogContent.contains("reg1 <= 8'h00;"));
        QVERIFY(verilogContent.contains("reg1 <= data_in;"));
        QVERIFY(verilogContent.contains("reg2 <= 8'hFF;"));
        QVERIFY(verilogContent.contains("if (enable) begin"));
        QVERIFY(verilogContent.contains("reg2 <= reg1;"));
    }

    void testInvalidSequential()
    {
        QString netlistContent = R"(
# Test netlist with invalid sequential logic
port:
  clk:
    direction: input
    type: logic
  q:
    direction: output
    type: logic

instance: {}

net: {}

seq:
  - reg: q
    # Missing clock signal - should generate warning
    next: "1'b0"
)";

        QString netlistPath = createTempFile("test_invalid_seq.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        } /* Should still succeed but with warnings */

        /* Check if warning message was generated */
        QString allMessages = messageList.join(" ");
        QVERIFY(allMessages.contains("has no 'clk' field"));
    }

    void testSequentialWithNestedCase()
    {
        QString netlistContent = R"(
# Test netlist with nested case statements in sequential logic
port:
  clk:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  ctrl:
    direction: input
    type: logic[1:0]
  sub_ctrl:
    direction: input
    type: logic[1:0]
  data_in:
    direction: input
    type: logic[7:0]
  state_machine:
    direction: output
    type: logic[7:0]

instance: {}

net: {}

seq:
  - reg: state_machine
    clk: clk
    rst: rst_n
    rst_val: "8'h00"
    if:
      - cond: "ctrl == 2'b00"
        then: "8'h01"
      - cond: "ctrl == 2'b01"
        then:
          case: sub_ctrl
          cases:
            "2'b00": "8'h10"
            "2'b01": "8'h20"
            "2'b10": "8'h30"
          default: "8'h0F"
      - cond: "ctrl == 2'b10"
        then: "data_in"
    default: "state_machine"
)";

        QString netlistPath = createTempFile("test_seq_nested.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_seq_nested.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify nested sequential logic is generated */
        QVERIFY(verilogContent.contains("always @(posedge clk or negedge rst_n) begin"));
        QVERIFY(verilogContent.contains("if (!rst_n) begin"));
        QVERIFY(verilogContent.contains("state_machine <= 8'h00;"));
        QVERIFY(verilogContent.contains("end else begin"));

        /* Verify default assignment */
        QVERIFY(verilogContent.contains("state_machine <= state_machine;"));

        /* Verify if-else structure with begin/end blocks */
        QVERIFY(verilogContent.contains("if (ctrl == 2'b00) begin"));
        QVERIFY(verilogContent.contains("state_machine <= 8'h01;"));
        QVERIFY(verilogContent.contains("else if (ctrl == 2'b01) begin"));

        /* Verify nested case statement using normalized whitespace */
        QVERIFY(verilogContent.contains("case (sub_ctrl)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "2'b00: state_machine <= 8'h10;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "2'b01: state_machine <= 8'h20;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "2'b10: state_machine <= 8'h30;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "default: state_machine <= 8'h0F;"));
        QVERIFY(verilogContent.contains("endcase"));

        /* Verify final if condition */
        QVERIFY(verilogContent.contains("else if (ctrl == 2'b10) begin"));
        QVERIFY(verilogContent.contains("state_machine <= data_in;"));

        /* Verify proper end statements */
        int beginCount = verilogContent.count(" begin");
        int endCount   = verilogContent.count("end");
        QVERIFY(beginCount > 0 && endCount >= beginCount); /* Allow for endcase and end statements */
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)
#include "test_qsoccliparsegenerateseqlogic.moc"
