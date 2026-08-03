// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/config.h"
#include "common/qslangdriver.h"
#include "common/qsocconsole.h"
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

namespace {

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
        qInstallMessageHandler(messageOutput);
        QSocConsole::setTeeToMessageHandler(true);
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
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign y = a & b;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "/* Combinational logic */"));
    }

    /* `expr` wins over `if` and `case` at emission. Counting the item as a
       process anyway declared a register nothing writes and drove the output
       from both that register and the expression. */
    void testMixedFormCombDrivesOutputOnce()
    {
        const QString netlistContent = R"(
port:
  a:
    direction: input
    type: logic[7:0]
  b:
    direction: input
    type: logic[7:0]
  sel:
    direction: input
    type: logic
  y:
    direction: output
    type: logic[7:0]

comb:
  - out: y
    expr: a
    if:
      - cond: sel
        then: b
    default: a
)";
        const QString netlistPath    = createTempFile("test_mixed_form.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;
            socCliWorker.setup(args, false);
            socCliWorker.run();
        }

        const QString verilogPath
            = QDir(projectManager.getOutputPath()).filePath("test_mixed_form.v");
        QVERIFY(QFile::exists(verilogPath));
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString verilogContent = verilogFile.readAll();
        verilogFile.close();

        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign y = a;"));
        QCOMPARE(verilogContent.count("assign y ="), 1);
        QVERIFY(!verilogContent.contains("y_reg"));
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

        /* Verify always block is generated with internal reg pattern */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "always @(*) begin"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "result_reg = 32'b0;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "if (sel == 2'b00)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "result_reg = a;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "else if (sel == 2'b01)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "result_reg = b;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "end"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign result = result_reg;"));
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

        /* Verify case statement is generated with internal reg pattern */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "always @(*) begin"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "alu_op_reg = 4'b0000;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "case (funct)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "6'b100000: alu_op_reg = 4'b0001;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "6'b100010: alu_op_reg = 4'b0010;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "6'b100100: alu_op_reg = 4'b0011;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "default: alu_op_reg = 4'b0000;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "endcase"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "end"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign alu_op = alu_op_reg;"));
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
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign and_out = a & b;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "always @(*) begin"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "mux_out_reg = b;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "if (sel)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "mux_out_reg = a;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign mux_out = mux_out_reg;"));
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

        /* Verify nested structure is generated correctly with internal reg pattern */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "always @(*) begin"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "alu_op_reg = 4'b0000;")); /* Default value */
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "if (opcode == 6'b000000) begin"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "case (funct)"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "6'b100000: alu_op_reg = 4'b0001;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "6'b100010: alu_op_reg = 4'b0010;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "default: alu_op_reg = 4'b1111;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "endcase"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "end")); /* end of if */
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "else if (opcode == 6'b001000) begin"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "alu_op_reg = 4'b0101;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign alu_op = alu_op_reg;"));
    }

    void testProcessTargetsPreserveSlices()
    {
        const QString netlistContent = R"(
port:
  sel:
    direction: input
    type: logic
  mode:
    direction: input
    type: logic
  low:
    direction: input
    type: logic[3:0]
  high:
    direction: input
    type: logic[3:0]
  data:
    direction: output
    type: logic[7:0]
  status:
    direction: output
    type: logic

comb:
  - out: data[3:0]
    if:
      - cond: sel
        then:
          case: mode
          cases:
            "1'b0": low
          default: 4'hf
    default: 4'h0
  - out: status
    if:
      - cond: sel
        then: 1'b1
    default: 1'b0
  - out: data[0]
    bits: "[7:4]"
    case: sel
    cases:
      "1'b1": high
    default: 4'h0
)";

        const QString netlistPath = createTempFile("test_process_slices.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        QSocCliWorker socCliWorker;
        socCliWorker.setup(
            {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), netlistPath},
            false);
        socCliWorker.run();

        const QString verilogPath
            = QDir(projectManager.getOutputPath()).filePath("test_process_slices.v");
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString verilogContent = verilogFile.readAll();

        QSlangDriver driver;
        QVERIFY2(
            driver
                .parseFileList("", {verilogPath}, {}, {}, QSlangDriver::UnknownModulePolicy::Reject),
            qPrintable("Generated Verilog did not elaborate:\n" + verilogContent));
        QCOMPARE(verilogContent.count("reg [7:0] data_reg;"), 1);
        QCOMPARE(verilogContent.count("reg status_reg;"), 1);
        QVERIFY(
            verilogContent.indexOf("reg [7:0] data_reg;")
            < verilogContent.indexOf("reg status_reg;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign data[3:0] = data_reg[3:0];"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign data[7:4] = data_reg[7:4];"));
        QVERIFY(
            verilogContent.indexOf("assign data[3:0]") < verilogContent.indexOf("assign data[7:4]"));
        QVERIFY(
            verilogContent.indexOf("assign data[7:4]")
            < verilogContent.indexOf("assign status = status_reg;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "data_reg[3:0] = low;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "data_reg[7:4] = high;"));
        QVERIFY(!verilogContent.contains("]_reg"));
        QVERIFY(!verifyVerilogContentNormalized(verilogContent, "assign data = data_reg;"));
    }

    void testProcessTargetsUseCommonResolution()
    {
        const QString netlistContent = R"(
port:
  sel:
    direction: input
    type: logic
  value:
    direction: input
    type: logic[3:0]
  out_if:
    direction: output
    type: logic[7:0]
    connect: if_net
  out_case:
    direction: output
    type: logic[7:0]
    connect: case_net
  connected_expr_in:
    direction: input
    type: logic[3:0]
    connect: expr_input_net
  direct_expr_in:
    direction: input
    type: logic[3:0]
  implicit_expr:
    type: logic[3:0]
  sideways_expr:
    direction: sideways
    type: logic[3:0]
  connected_process_in:
    direction: input
    type: logic[3:0]
    connect: process_input_net
  direct_process_in:
    direction: input
    type: logic[3:0]
  implicit_process:
    type: logic[3:0]
  sideways_process:
    direction: sideways
    type: logic[3:0]

comb:
  - out: if_net[3:0]
    if:
      - cond: sel
        then: value
    default: 4'h0
  - out: case_net[0]
    bits: "[7:4]"
    case: sel
    cases:
      "1'b1": value
    default: 4'h0
  - out: expr_input_net
    expr: value
  - out: direct_expr_in
    expr: value
  - out: implicit_expr
    expr: value
  - out: sideways_expr
    expr: value
  - out: process_input_net
    if:
      - cond: sel
        then: value
    default: 4'h0
  - out: direct_process_in
    case: sel
    cases:
      "1'b1": value
    default: 4'h0
  - out: implicit_process
    if:
      - cond: sel
        then: value
    default: 4'h0
  - out: sideways_process
    case: sel
    cases:
      "1'b1": value
    default: 4'h0
)";

        const QString netlistPath
            = createTempFile("test_process_resolution.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        QSocCliWorker socCliWorker;
        socCliWorker.setup(
            {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), netlistPath},
            false);
        socCliWorker.run();

        const QString verilogPath
            = QDir(projectManager.getOutputPath()).filePath("test_process_resolution.v");
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString verilogContent = verilogFile.readAll();

        QSlangDriver driver;
        QVERIFY2(
            driver
                .parseFileList("", {verilogPath}, {}, {}, QSlangDriver::UnknownModulePolicy::Reject),
            qPrintable("Generated Verilog did not elaborate:\n" + verilogContent));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "assign out_if[3:0] = out_if_reg[3:0];"));
        QVERIFY(verifyVerilogContentNormalized(
            verilogContent, "assign out_case[7:4] = out_case_reg[7:4];"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "out_if_reg[3:0] = value;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "out_case_reg[7:4] = value;"));
        QVERIFY(!verilogContent.contains("if_net_reg"));
        QVERIFY(!verilogContent.contains("case_net_reg"));

        const QStringList skippedExprNames
            = {"connected_expr_in", "direct_expr_in", "implicit_expr", "sideways_expr"};
        for (const QString &name : skippedExprNames) {
            QCOMPARE(verilogContent.count("FIXME: comb tried to drive top-level input " + name), 1);
            QVERIFY(!verifyVerilogContentNormalized(verilogContent, "assign " + name + " = value;"));
        }

        /* A process form on a top-level input is the same illegal driver as
           an expression on one; both skip emission behind a FIXME. */
        const QStringList skippedProcessNames
            = {"connected_process_in", "direct_process_in", "implicit_process", "sideways_process"};
        for (const QString &name : skippedProcessNames) {
            QCOMPARE(verilogContent.count("FIXME: comb tried to drive top-level input " + name), 1);
            QVERIFY(!verilogContent.contains(name + "_reg"));
        }
        QVERIFY(!verilogContent.contains("process_input_net_reg"));
        QCOMPARE(verilogContent.count("always @(*)"), 2);

        QMap<QString, int> warningCounts;
        for (const QString &message : messageList) {
            if (!message.contains("comb writes to top-level input port")) {
                continue;
            }
            for (const QString &name : skippedExprNames + skippedProcessNames) {
                if (message.contains(name)) {
                    ++warningCounts[name];
                }
            }
        }
        for (const QString &name : skippedExprNames + skippedProcessNames) {
            QCOMPARE(warningCounts.value(name), 1);
        }
    }

    void testSharedConnectUsesFirstDeclaredPort()
    {
        const QString netlistContent = R"(
port:
  sel:
    direction: input
    type: logic
  low:
    direction: input
    type: logic[3:0]
  high:
    direction: input
    type: logic[3:0]
  m_alias:
    direction: output
    type: logic[7:0]
    connect: shared_net
  a_primary:
    direction: output
    type: logic[7:0]
    connect: shared_net
  z_alias:
    direction: output
    type: logic[7:0]
    connect: shared_net

comb:
  - out: shared_net[3:0]
    if:
      - cond: sel
        then: low
    default: 4'h0
  - out: shared_net[0]
    bits: "[7:4]"
    case: sel
    cases:
      "1'b1": high
    default: 4'h0
)";

        const QString netlistPath = createTempFile("test_shared_connect.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        QSocCliWorker socCliWorker;
        socCliWorker.setup(
            {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), netlistPath},
            false);
        socCliWorker.run();

        const QString verilogPath
            = QDir(projectManager.getOutputPath()).filePath("test_shared_connect.v");
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString verilogContent = verilogFile.readAll();

        QSlangDriver driver;
        QVERIFY2(
            driver
                .parseFileList("", {verilogPath}, {}, {}, QSlangDriver::UnknownModulePolicy::Reject),
            qPrintable("Generated Verilog did not elaborate:\n" + verilogContent));
        QCOMPARE(verilogContent.count("reg [7:0] m_alias_reg;"), 1);
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "assign m_alias[3:0] = m_alias_reg[3:0];"));
        QVERIFY(
            verifyVerilogContentNormalized(verilogContent, "assign m_alias[7:4] = m_alias_reg[7:4];"));
        QCOMPARE(verilogContent.count("assign a_primary = m_alias;"), 1);
        QCOMPARE(verilogContent.count("assign z_alias = m_alias;"), 1);
        QVERIFY(!verilogContent.contains("shared_net"));
        QVERIFY(!verilogContent.contains("a_primary_reg"));
        QVERIFY(!verilogContent.contains("z_alias_reg"));
        QVERIFY(!verilogContent.contains("multiple drivers"));
    }

    void testOverlappingProcessesAreRejected()
    {
        const QString netlistContent = R"(
port:
  sel:
    direction: input
    type: logic
  low_first:
    direction: input
    type: logic[3:0]
  low_second:
    direction: input
    type: logic[3:0]
  high:
    direction: input
    type: logic[3:0]
  result:
    direction: output
    type: logic[7:0]
  whole_first:
    direction: output
    type: logic[7:0]
  slice_first:
    direction: output
    type: logic[7:0]

comb:
  - out: result[3:0]
    if:
      - cond: sel
        then: low_first
    default: 4'h0
  - out: result[3:0]
    if:
      - cond: sel
        then: low_second
    default: 4'h1
  - out: result[7:4]
    if:
      - cond: sel
        then: high
    default: 4'h2
  - out: whole_first
    if:
      - cond: sel
        then: "8'hA5"
    default: 8'h00
  - out: whole_first[3:0]
    if:
      - cond: sel
        then: low_second
    default: 4'h3
  - out: slice_first[3:0]
    if:
      - cond: sel
        then: low_first
    default: 4'h4
  - out: slice_first
    if:
      - cond: sel
        then: "8'h5A"
    default: 8'h00
)";

        const QString netlistPath
            = createTempFile("test_overlapping_processes.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        const QString verilogPath
            = QDir(projectManager.getOutputPath()).filePath("test_overlapping_processes.v");
        QVERIFY(QFile::remove(verilogPath) || !QFile::exists(verilogPath));

        QSocCliWorker worker;
        worker.setup(
            {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), netlistPath},
            false);
        worker.run();

        /* Superseded pin: keep-first emission for overlapping drivers was
           replaced by rejection under the two-form ruling; one driver may
           own a bit range. */
        const QString messages = messageList.join('\n');
        QVERIFY2(
            !messages.contains("Successfully generated Verilog code: " + verilogPath),
            qPrintable(messages));
        QVERIFY(!QFile::exists(verilogPath));
        QVERIFY(messages.contains("comb has overlapping process driver for result[3:0]"));
    }

    void testFullWidthProcessesUseDeclaredSignalWidths()
    {
        const QString moduleContent = R"(
comb_width_sink:
  port:
    if_data:
      type: logic[7:0]
      direction: in
    case_data:
      type: logic[15:0]
      direction: in
)";
        const QString modulePath = createTempFile("module/comb_width_sink.soc_mod", moduleContent);
        QVERIFY(!modulePath.isEmpty());

        const QString netlistContent = R"(
parameter:
  WIDTH:
    type: integer
    value: 12

port:
  sel:
    direction: input
    type: logic
  data8:
    direction: input
    type: logic[7:0]
  data16:
    direction: input
    type: logic[15:0]
  param_data:
    direction: input
    type: logic[WIDTH-1:0]
  param_out:
    direction: output
    type: logic[WIDTH-1:0]
  packed_data:
    direction: input
    type: logic[1:0][3:0]
  packed_out:
    direction: output
    type: logic[1:0][3:0]

instance:
  u_sink:
    module: comb_width_sink

net:
  if_bus:
    - instance: u_sink
      port: if_data
  case_bus:
    - instance: u_sink
      port: case_data

comb:
  - out: if_bus
    if:
      - cond: sel
        then: data8
    default: 8'h00
  - out: case_bus
    case: sel
    cases:
      "1'b1": data16
    default: 16'h0000
  - out: param_out
    if:
      - cond: sel
        then: param_data
    default: "'0"
  - out: packed_out
    case: sel
    cases:
      "1'b1": packed_data
    default: 8'h00
)";

        const QString netlistPath
            = createTempFile("test_internal_process_width.soc_net", netlistContent);
        QVERIFY(!netlistPath.isEmpty());

        QSocCliWorker socCliWorker;
        socCliWorker.setup(
            {"qsoc", "generate", "verilog", "-d", projectManager.getCurrentPath(), netlistPath},
            false);
        socCliWorker.run();

        const QString verilogPath
            = QDir(projectManager.getOutputPath()).filePath("test_internal_process_width.v");
        QFile verilogFile(verilogPath);
        QVERIFY(verilogFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString verilogContent = verilogFile.readAll();

        QCOMPARE(verilogContent.count("wire [7:0] if_bus;"), 1);
        QCOMPARE(verilogContent.count("wire [15:0] case_bus;"), 1);
        QCOMPARE(verilogContent.count("reg [7:0] if_bus_reg;"), 1);
        QCOMPARE(verilogContent.count("reg [15:0] case_bus_reg;"), 1);
        QCOMPARE(verilogContent.count("reg [WIDTH-1:0] param_out_reg;"), 1);
        QCOMPARE(verilogContent.count("reg [1:0][3:0] packed_out_reg;"), 1);
        QVERIFY(!verilogContent.contains("\n    reg if_bus_reg;"));
        QVERIFY(!verilogContent.contains("\n    reg case_bus_reg;"));
        QVERIFY(!verilogContent.contains("\n    reg param_out_reg;"));
        QVERIFY(!verilogContent.contains("\n    reg packed_out_reg;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign if_bus = if_bus_reg;"));
        QVERIFY(verifyVerilogContentNormalized(verilogContent, "assign case_bus = case_bus_reg;"));

        const QString stubContent = R"(
module comb_width_sink (
    input wire [7:0] if_data,
    input wire [15:0] case_data
);
endmodule
)";
        const QString stubPath    = createTempFile("comb_width_sink.v", stubContent);
        QVERIFY(!stubPath.isEmpty());

        QSlangDriver driver;
        QVERIFY2(
            driver.parseFileList(
                "", {stubPath, verilogPath}, {}, {}, QSlangDriver::UnknownModulePolicy::Reject),
            qPrintable("Generated Verilog did not elaborate:\n" + verilogContent));
        QVERIFY2(
            driver.parseArgs(QString(
                                 "slang --single-unit --timescale 1ns/10ps --error-limit=0 "
                                 "-Werror=width-trunc -Werror=width-expand \"%1\" \"%2\"")
                                 .arg(stubPath, verilogPath)),
            qPrintable("Generated Verilog changed signal width:\n" + verilogContent));
    }
    /* Multi-form items and escaped slices reject the run. */
    void testMalformedCombFormsAreRejected_data()
    {
        QTest::addColumn<QString>("stem");
        QTest::addColumn<QString>("netlist");
        QTest::addColumn<QString>("fragment");

        QTest::newRow("expr-and-if") << "rej_multi_form" << QString(R"(
port:
  a:
    direction: input
    type: logic
  y:
    direction: output
    type: logic

comb:
  - out: y
    expr: a
    if:
      - cond: a
        then: "1'b0"
    default: "1'b1"
)") << "carries more than one of expr, if, and case";

        QTest::newRow("slice-escapes-binding") << "rej_slice_escape" << QString(R"(
port:
  a:
    direction: input
    type: logic[3:0]
  y:
    direction: output
    type: logic[15:0]

net:
  part:
    - instance: top
      port: y
      bits: "[7:4]"

comb:
  - out: part
    bits: "[5]"
    expr: a[0]
)") << "exceeds the 4 bits bound by";
    }

    void testMalformedCombFormsAreRejected()
    {
        QFETCH(QString, stem);
        QFETCH(QString, netlist);
        QFETCH(QString, fragment);
        messageList.clear();
        const QString netlistPath = createTempFile(stem + ".soc_net", netlist);
        QVERIFY(!netlistPath.isEmpty());
        const QString verilogPath = QDir(projectManager.getOutputPath()).filePath(stem + ".v");
        QFile::remove(verilogPath);
        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;
            socCliWorker.setup(args, false);
            socCliWorker.run();
        }
        QVERIFY(!QFile::exists(verilogPath));
        QVERIFY2(
            messageList.join('\n').contains(fragment),
            qPrintable(fragment + " | " + messageList.join('\n').right(600)));
    }
};

QStringList Test::messageList;

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoccliparsegeneratecomblogic.moc"
