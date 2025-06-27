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

    void testSimpleAssignComb()
    {
        QString netlistContent = R"(
# Test netlist with simple assign combinational logic
port:
  clk:
    direction: input
    type: logic
  a:
    direction: input
    type: logic
  b:
    direction: input  
    type: logic
  y:
    direction: output
    type: logic

instance: {}

net: {}

comb:
  - out: y
    expr: "a & b"
)";

        QString netlistPath = createTempFile("test_simple_assign.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;

            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        /* Check if Verilog file was generated - use the base name without extension */
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_simple_assign.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify assign statement is generated */
        QVERIFY(verilogContent.contains("assign y = a & b;"));
        QVERIFY(verilogContent.contains("/* Combinational logic */"));
    }

    void testConditionalComb()
    {
        QString netlistContent = R"(
# Test netlist with conditional combinational logic
port:
  sel:
    direction: input
    type: logic[1:0]
  a:
    direction: input
    type: logic[31:0]
  b:
    direction: input  
    type: logic[31:0]
  result:
    direction: output
    type: logic[31:0]

instance: {}

net: {}

comb:
  - out: result
    if:
      - cond: "sel == 2'b00"
        then: "a"
      - cond: "sel == 2'b01"  
        then: "b"
    default: "32'b0"
)";

        QString netlistPath = createTempFile("test_conditional.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_conditional.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify always block is generated */
        QVERIFY(verilogContent.contains("always @(*) begin"));
        QVERIFY(verilogContent.contains("result = 32'b0;"));
        QVERIFY(verilogContent.contains("if (sel == 2'b00)"));
        QVERIFY(verilogContent.contains("result = a;"));
        QVERIFY(verilogContent.contains("else if (sel == 2'b01)"));
        QVERIFY(verilogContent.contains("result = b;"));
        QVERIFY(verilogContent.contains("end"));
    }

    void testCaseComb()
    {
        QString netlistContent = R"(
# Test netlist with case combinational logic
port:
  funct:
    direction: input
    type: logic[5:0]
  alu_op:
    direction: output
    type: logic[3:0]

instance: {}

net: {}

comb:
  - out: alu_op
    case: funct
    cases:
      "6'b100000": "4'b0001"
      "6'b100010": "4'b0010"
      "6'b100100": "4'b0011"
    default: "4'b0000"
)";

        QString netlistPath = createTempFile("test_case.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_case.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify case statement is generated */
        QVERIFY(verilogContent.contains("always @(*) begin"));
        QVERIFY(verilogContent.contains("alu_op = 4'b0000;"));
        QVERIFY(verilogContent.contains("case (funct)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "6'b100000: alu_op = 4'b0001;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "6'b100010: alu_op = 4'b0010;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "6'b100100: alu_op = 4'b0011;"));
        QVERIFY(verilogContent.contains("default:") && verilogContent.contains("alu_op = 4'b0000;"));
        QVERIFY(verilogContent.contains("endcase"));
        QVERIFY(verilogContent.contains("end"));
    }

    void testMultipleComb()
    {
        QString netlistContent = R"(
# Test netlist with multiple combinational logic blocks
port:
  a:
    direction: input
    type: logic
  b:
    direction: input
    type: logic
  sel:
    direction: input
    type: logic
  and_out:
    direction: output
    type: logic
  mux_out:
    direction: output
    type: logic

instance: {}

net: {}

comb:
  - out: and_out
    expr: "a & b"
  - out: mux_out
    if:
      - cond: "sel"
        then: "a"
    default: "b"
)";

        QString netlistPath = createTempFile("test_multiple.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_multiple.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify both combinational logic blocks are generated */
        QVERIFY(verilogContent.contains("assign and_out = a & b;"));
        QVERIFY(verilogContent.contains("always @(*) begin"));
        QVERIFY(verilogContent.contains("mux_out = b;"));
        QVERIFY(verilogContent.contains("if (sel)"));
        QVERIFY(verilogContent.contains("mux_out = a;"));
    }

    void testInvalidComb()
    {
        QString netlistContent = R"(
# Test netlist with invalid combinational logic
port:
  y:
    direction: output
    type: logic

instance: {}

net: {}

comb:
  - out: y
    # Missing logic specification - should generate warning
)";

        QString netlistPath = createTempFile("test_invalid.soc_net", netlistContent);
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
        QVERIFY(allMessages.contains("has no logic specification"));
    }

    void testNestedIfCaseComb()
    {
        QString netlistContent = R"(
# Test netlist with nested if + case combinational logic
port:
  opcode:
    direction: input
    type: logic[5:0]
  funct:
    direction: input
    type: logic[5:0]
  alu_op:
    direction: output
    type: logic[3:0]

instance: {}

net: {}

comb:
  - out: alu_op
    if:
      - cond: "opcode == 6'b000000"
        then:
          case: funct
          cases:
            "6'b100000": "4'b0001"
            "6'b100010": "4'b0010"
          default: "4'b1111"
      - cond: "opcode == 6'b001000"
        then: "4'b0101"
    default: "4'b0000"
)";

        QString netlistPath = createTempFile("test_nested.soc_net", netlistContent);
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
        QString verilogPath = QDir(projectManager.getOutputPath()).filePath("test_nested.v");
        QVERIFY(QFile::exists(verilogPath));

        /* Read generated Verilog content */
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        /* Verify nested structure is generated correctly */
        QVERIFY(verilogContent.contains("always @(*) begin"));
        QVERIFY(verilogContent.contains("alu_op = 4'b0000;")); /* Default value */
        QVERIFY(verilogContent.contains("if (opcode == 6'b000000) begin"));
        QVERIFY(verilogContent.contains("case (funct)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "6'b100000: alu_op = 4'b0001;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "6'b100010: alu_op = 4'b0010;"));
        QVERIFY(verilogContent.contains("default:") && verilogContent.contains("alu_op = 4'b1111;"));
        QVERIFY(verilogContent.contains("endcase"));
        QVERIFY(verilogContent.contains("end")); /* end of if */
        QVERIFY(verilogContent.contains("else if (opcode == 6'b001000) begin"));
        QVERIFY(verilogContent.contains("alu_op = 4'b0101;"));
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)
#include "test_qsoccliparsegeneratecomblogic.moc"
