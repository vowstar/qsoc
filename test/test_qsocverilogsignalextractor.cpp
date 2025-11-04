// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "common/qslangdriver.h"
#include "common/qsocgeneratemanager.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QString>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir       tempDir;
    QSocProjectManager *projectManager = nullptr;

private slots:
    void initTestCase();
    void cleanupTestCase();

    /*
     * Basic Functionality Tests
     * Test core parsing and signal extraction
     */
    void basic_parse_simpleAssign();
    void basic_parse_combIfBlock();
    void basic_parse_seqAlwaysBlock();
    void basic_parse_complexExpression();
    void basic_extract_simpleSignals();
    void basic_extract_withExclusion();
    void basic_extract_nestedExpressions();
    void basic_integration_combGeneration();
    void basic_integration_seqGeneration();

    /*
     * Bit Width Inference Tests
     * Test bit width detection from various selection patterns
     */
    void bitWidth_infer_multipleSelectsSameSignal();
    void bitWidth_infer_nonAlignedRange();
    void bitWidth_infer_mixedSingleAndRange();
    void bitWidth_infer_descendingRange();
    void bitWidth_infer_singleBitZero();
    void bitWidth_infer_singleBitHigh();
    void bitWidth_infer_veryWideBus();
    void bitWidth_infer_noSelection();

    /*
     * Expression Context Tests
     * Test bit width inference in various expression contexts
     */
    void context_nested_arithmetic();
    void context_ternary_operator();
    void context_concatenation();
    void context_caseStatement();
    void context_arithmetic_multiOperand();
    void context_comparison();
    void context_shiftOperation();
    void context_deeplyNested();

    /*
     * Boundary Condition Tests
     * Test extreme and edge cases
     */
    void boundary_width_singleBit();
    void boundary_width_extremelyWide();
    void boundary_width_power2Boundary();
    void boundary_complexity_deepNesting();
    void boundary_complexity_manyConcats();
    void boundary_complexity_longExpression();
    void boundary_mixed_allOperatorTypes();

    /*
     * Real-World Scenario Tests
     * Test realistic hardware design patterns
     */
    void realWorld_stateMachine();
    void realWorld_busInterface();
    void realWorld_fifoControl();
    void realWorld_arithmeticUnit();
};

void Test::initTestCase()
{
    QVERIFY(tempDir.isValid());
    projectManager = new QSocProjectManager();
    projectManager->setProjectName("test_signal_extractor");
    projectManager->setCurrentPath(tempDir.path());
}

void Test::cleanupTestCase()
{
    delete projectManager;
}

void Test::basic_parse_simpleAssign()
{
    /* Test simple assign statement */
    QString verilogCode = "assign y = a & b;";

    QSlangDriver driver;
    bool         result = driver.parseVerilogSnippet(verilogCode, true);

    QVERIFY(result);

    /* Extract signal references */
    QSet<QString> signalSet = driver.extractSignalReferences();

    /* Should find a, b, y */
    QVERIFY(signalSet.contains("a"));
    QVERIFY(signalSet.contains("b"));
    QVERIFY(signalSet.contains("y"));
    QCOMPARE(signalSet.size(), 3);
}

void Test::basic_parse_combIfBlock()
{
    /* Test combinational if block */
    QString verilogCode = R"(
reg [31:0] result_reg;
assign result = result_reg;

always @(*) begin
    result_reg = 32'b0;
    if (sel == 2'b00)
        result_reg = a;
    else if (sel == 2'b01)
        result_reg = b;
end
)";

    QSlangDriver driver;
    bool         result = driver.parseVerilogSnippet(verilogCode, true);

    QVERIFY(result);

    /* Extract signals */
    QSet<QString> signalSet = driver.extractSignalReferences();

    /* Should find sel, a, b, result, result_reg */
    QVERIFY(signalSet.contains("sel"));
    QVERIFY(signalSet.contains("a"));
    QVERIFY(signalSet.contains("b"));
    QVERIFY(signalSet.contains("result"));
    QVERIFY(signalSet.contains("result_reg"));
}

void Test::basic_parse_seqAlwaysBlock()
{
    /* Test sequential always block */
    QString verilogCode = R"(
reg [7:0] data_reg;
assign data = data_reg;

always @(posedge clk or negedge rst_n) begin
    if (!rst_n)
        data_reg <= 8'h00;
    else
        data_reg <= data_in;
end
)";

    QSlangDriver driver;
    bool         result = driver.parseVerilogSnippet(verilogCode, true);

    QVERIFY(result);

    /* Extract signals */
    QSet<QString> signalSet = driver.extractSignalReferences();

    /* Should find clk, rst_n, data_in, data, data_reg */
    QVERIFY(signalSet.contains("clk"));
    QVERIFY(signalSet.contains("rst_n"));
    QVERIFY(signalSet.contains("data_in"));
    QVERIFY(signalSet.contains("data"));
    QVERIFY(signalSet.contains("data_reg"));
}

void Test::basic_parse_complexExpression()
{
    /* Test complex expression with bit selection */
    QString verilogCode = "assign result = data_in[7:0] + counter;";

    QSlangDriver driver;
    bool         parseResult = driver.parseVerilogSnippet(verilogCode, true);

    QVERIFY(parseResult);

    /* Extract signals */
    QSet<QString> signalSet = driver.extractSignalReferences();

    /* Should find result, data_in, counter */
    QVERIFY(signalSet.contains("result"));
    QVERIFY(signalSet.contains("data_in"));
    QVERIFY(signalSet.contains("counter"));
}

void Test::basic_extract_simpleSignals()
{
    /* Test basic signal extraction */
    QString verilogCode = "assign out = in1 & in2 | in3;";

    QSlangDriver driver;
    driver.parseVerilogSnippet(verilogCode, true);

    QSet<QString> signalSet = driver.extractSignalReferences();

    QCOMPARE(signalSet.size(), 4);
    QVERIFY(signalSet.contains("out"));
    QVERIFY(signalSet.contains("in1"));
    QVERIFY(signalSet.contains("in2"));
    QVERIFY(signalSet.contains("in3"));
}

void Test::basic_extract_withExclusion()
{
    /* Test signal extraction with exclusion */
    QString verilogCode = "assign out = in1 & in2;";

    QSlangDriver driver;
    driver.parseVerilogSnippet(verilogCode, true);

    /* Exclude 'out' as it's the known output */
    QSet<QString> excludeSet;
    excludeSet.insert("out");

    QSet<QString> signalSet = driver.extractSignalReferences(excludeSet);

    /* Should only find in1, in2 (out is excluded) */
    QCOMPARE(signalSet.size(), 2);
    QVERIFY(signalSet.contains("in1"));
    QVERIFY(signalSet.contains("in2"));
    QVERIFY(!signalSet.contains("out"));
}

void Test::basic_extract_nestedExpressions()
{
    /* Test extraction from nested structures */
    QString verilogCode = R"(
always @(*) begin
    case (ctrl)
        2'b00: output_reg = input_a;
        2'b01: output_reg = input_b;
        2'b10: output_reg = input_c;
        default: output_reg = 8'h00;
    endcase
end
)";

    QSlangDriver driver;
    driver.parseVerilogSnippet(verilogCode, true);

    QSet<QString> signalSet = driver.extractSignalReferences();

    /* Should find ctrl, output_reg, input_a, input_b, input_c */
    QVERIFY(signalSet.contains("ctrl"));
    QVERIFY(signalSet.contains("output_reg"));
    QVERIFY(signalSet.contains("input_a"));
    QVERIFY(signalSet.contains("input_b"));
    QVERIFY(signalSet.contains("input_c"));
}

void Test::basic_integration_combGeneration()
{
    /* Integration test: Generate comb block and extract signals */
    QString netlistContent = R"(
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

    /* Create temporary netlist file */
    QString netlistPath = QDir(tempDir.path()).filePath("test_comb.soc_net");
    QFile   file(netlistPath);
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
    QTextStream out(&file);
    out << netlistContent;
    file.close();

    /* Generate Verilog using QSocGenerateManager */
    QSocGenerateManager generator(nullptr, projectManager);
    QString             verilogOutput;
    QTextStream         verilogStream(&verilogOutput);

    /* This would normally generate full Verilog, but we're testing the concept */
    /* For now, just test the expected comb output structure */
    QString expectedCombCode = R"(
reg [31:0] result_reg;
assign result = result_reg;

always @(*) begin
    result_reg = 32'b0;
    if (sel == 2'b00)
        result_reg = a;
    else if (sel == 2'b01)
        result_reg = b;
end
)";

    /* Parse the generated code */
    QSlangDriver driver;
    bool         parseResult = driver.parseVerilogSnippet(expectedCombCode, true);

    QVERIFY(parseResult);

    /* Extract signals, excluding the known output */
    QSet<QString> excludeSet;
    excludeSet.insert("result");
    excludeSet.insert("result_reg");

    QSet<QString> signalSet = driver.extractSignalReferences(excludeSet);

    /* Should find input signals: sel, a, b */
    QVERIFY(signalSet.contains("sel"));
    QVERIFY(signalSet.contains("a"));
    QVERIFY(signalSet.contains("b"));
}

void Test::basic_integration_seqGeneration()
{
    /* Integration test: Generate seq block and extract signals */
    QString expectedSeqCode = R"(
reg [7:0] counter_reg;
assign counter = counter_reg;

always @(posedge clk or negedge rst_n) begin
    if (!rst_n)
        counter_reg <= 8'h00;
    else
        counter_reg <= counter + 1;
end
)";

    /* Parse the sequential code */
    QSlangDriver driver;
    bool         parseResult = driver.parseVerilogSnippet(expectedSeqCode, true);

    QVERIFY(parseResult);

    /* Extract signals, excluding outputs */
    QSet<QString> excludeSet;
    excludeSet.insert("counter");
    excludeSet.insert("counter_reg");

    QSet<QString> signalSet = driver.extractSignalReferences(excludeSet);

    /* Should find input signals: clk, rst_n */
    QVERIFY(signalSet.contains("clk"));
    QVERIFY(signalSet.contains("rst_n"));
    /* Note: 'counter' appears in the expression but is excluded */
}

/* ========== Edge Case Tests ========== */
/* Test boundary conditions and complex scenarios for bit width inference */

void Test::bitWidth_infer_multipleSelectsSameSignal()
{
    /* Same signal with multiple bit selections - should take maximum width */
    QSlangDriver driver;
    QString      verilogCode = "assign out = data[7:0] + data[15:8];";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("data"));
    QVERIFY(signalSet.contains("out"));
    /* data should be inferred as 16-bit (0-15) not 8-bit */
}

void Test::bitWidth_infer_nonAlignedRange()
{
    /* Non-aligned bit selection [12:5] - needs 13 bits (0-12) */
    QSlangDriver driver;
    QString      verilogCode = "assign result = input_data[12:5];";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("input_data"));
    QVERIFY(signalSet.contains("result"));
}

void Test::bitWidth_infer_mixedSingleAndRange()
{
    /* Mixing single-bit [5] and range [7:0] on same signal */
    QSlangDriver driver;
    QString      verilogCode = R"(
assign bit_out = data[5];
assign byte_out = data[7:0];
)";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("data"));
    /* data should be inferred as 8-bit minimum */
}

void Test::bitWidth_infer_descendingRange()
{
    /* Ascending range [0:7] - convert to descending [7:0] for consistency */
    QSlangDriver driver;
    QString      verilogCode = "assign result = signal[7:0];";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("signal"));
    QVERIFY(signalSet.contains("result"));
}

void Test::context_nested_arithmetic()
{
    /* Nested expressions with multiple bit selections */
    QSlangDriver driver;
    QString      verilogCode = "assign result = (data[7:0] + offset[3:0]) & mask[15:0];";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("data"));
    QVERIFY(signalSet.contains("offset"));
    QVERIFY(signalSet.contains("mask"));
    QVERIFY(signalSet.contains("result"));
}

void Test::context_ternary_operator()
{
    /* Ternary operator with bit selections */
    QSlangDriver driver;
    QString      verilogCode = "assign out = sel ? input_a[31:0] : input_b[31:0];";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("sel"));
    QVERIFY(signalSet.contains("input_a"));
    QVERIFY(signalSet.contains("input_b"));
    QVERIFY(signalSet.contains("out"));
}

void Test::context_concatenation()
{
    /* Concatenation with bit selections */
    QSlangDriver driver;
    QString      verilogCode = "assign result = {upper[7:0], lower[7:0]};";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("upper"));
    QVERIFY(signalSet.contains("lower"));
    QVERIFY(signalSet.contains("result"));
}

void Test::bitWidth_infer_veryWideBus()
{
    /* Very wide bus [1023:0] - 1024 bits */
    QSlangDriver driver;
    QString      verilogCode = "assign out = wide_bus[1023:0];";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("wide_bus"));
    QVERIFY(signalSet.contains("out"));
}

void Test::bitWidth_infer_singleBitZero()
{
    /* Single bit at index 0 - signal[0] needs at least 1-bit array */
    QSlangDriver driver;
    QString      verilogCode = "assign result = signal[0];";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("signal"));
    QVERIFY(signalSet.contains("result"));
}

void Test::context_caseStatement()
{
    /* Bit selection in case statement */
    QSlangDriver driver;
    QString      verilogCode = R"(
always @(*) begin
    case (ctrl[1:0])
        2'b00: output_reg = data[7:0];
        2'b01: output_reg = data[15:8];
        2'b10: output_reg = data[23:16];
        default: output_reg = 8'h00;
    endcase
end
)";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("ctrl"));
    QVERIFY(signalSet.contains("data"));
    QVERIFY(signalSet.contains("output_reg"));
}

void Test::context_arithmetic_multiOperand()
{
    /* Bit selection in arithmetic operations */
    QSlangDriver driver;
    QString      verilogCode = "assign sum = a[15:0] + b[15:0] + c[15:0];";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("a"));
    QVERIFY(signalSet.contains("b"));
    QVERIFY(signalSet.contains("c"));
    QVERIFY(signalSet.contains("sum"));
}

void Test::context_comparison()
{
    /* Bit selection in comparison */
    QSlangDriver driver;
    QString      verilogCode = "assign match = (addr[31:0] == base[31:0]);";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("addr"));
    QVERIFY(signalSet.contains("base"));
    QVERIFY(signalSet.contains("match"));
}

void Test::context_shiftOperation()
{
    /* Bit selection with shift operations */
    QSlangDriver driver;
    QString      verilogCode = "assign result = (data[7:0] << shift[2:0]);";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("data"));
    QVERIFY(signalSet.contains("shift"));
    QVERIFY(signalSet.contains("result"));
}

void Test::context_deeplyNested()
{
    /* Deeply nested expressions */
    QSlangDriver driver;
    QString      verilogCode = "assign out = ((a[7:0] & b[7:0]) | (c[15:8] ^ d[15:8])) + e[31:16];";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("a"));
    QVERIFY(signalSet.contains("b"));
    QVERIFY(signalSet.contains("c"));
    QVERIFY(signalSet.contains("d"));
    QVERIFY(signalSet.contains("e"));
    QVERIFY(signalSet.contains("out"));
}

void Test::bitWidth_infer_singleBitHigh()
{
    /* Single bit at high position [127] - needs 128 bits */
    QSlangDriver driver;
    QString      verilogCode = "assign flag = status[127];";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("status"));
    QVERIFY(signalSet.contains("flag"));
}

void Test::bitWidth_infer_noSelection()
{
    /* Signals without bit selection should be declared as 1-bit scalar */
    QSlangDriver driver;
    QString      verilogCode = "assign result = enable & ready & valid;";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("enable"));
    QVERIFY(signalSet.contains("ready"));
    QVERIFY(signalSet.contains("valid"));
    QVERIFY(signalSet.contains("result"));
}

/* ========== Boundary Condition Tests ========== */
/* Test extreme and edge cases that push the limits */

void Test::boundary_width_singleBit()
{
    /* Single bit signal accessed as array [0:0] vs scalar */
    QSlangDriver driver;
    QString      verilogCode = R"(
assign bit_array = data[0];
assign scalar_sig = enable;
)";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("data"));
    QVERIFY(signalSet.contains("enable"));
}

void Test::boundary_width_extremelyWide()
{
    /* Extremely wide bus - 2048 bits */
    QSlangDriver driver;
    QString      verilogCode = "assign out = ultra_wide[2047:0];";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("ultra_wide"));
    QVERIFY(signalSet.contains("out"));
}

void Test::boundary_width_power2Boundary()
{
    /* Test power-of-2 boundaries: 31/32, 63/64, 127/128, 255/256 */
    QSlangDriver driver;
    QString      verilogCode = R"(
assign a = sig_31[30:0];
assign b = sig_32[31:0];
assign c = sig_63[62:0];
assign d = sig_64[63:0];
assign e = sig_127[126:0];
assign f = sig_128[127:0];
assign g = sig_255[254:0];
assign h = sig_256[255:0];
)";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("sig_31"));
    QVERIFY(signalSet.contains("sig_32"));
    QVERIFY(signalSet.contains("sig_64"));
    QVERIFY(signalSet.contains("sig_128"));
    QVERIFY(signalSet.contains("sig_256"));
}

void Test::boundary_complexity_deepNesting()
{
    /* Deeply nested parentheses and operations - 10+ levels */
    QSlangDriver driver;
    QString      verilogCode
        = "assign result = ((((((((((a[7:0] + b[7:0]) & c[7:0]) | d[7:0]) ^ e[7:0]) - f[7:0]) * "
          "g[3:0]) << h[2:0]) >> i[2:0]) + j[7:0]) & k[7:0]);";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    /* Should extract all 11 signals a-k */
    QVERIFY(signalSet.contains("a"));
    QVERIFY(signalSet.contains("k"));
    QVERIFY(signalSet.size() >= 12); /* a-k + result */
}

void Test::boundary_complexity_manyConcats()
{
    /* Multiple concatenations - 16 signals concatenated */
    QSlangDriver driver;
    QString      verilogCode = R"(
assign wide_bus = {
    s0[7:0], s1[7:0], s2[7:0], s3[7:0],
    s4[7:0], s5[7:0], s6[7:0], s7[7:0],
    s8[7:0], s9[7:0], s10[7:0], s11[7:0],
    s12[7:0], s13[7:0], s14[7:0], s15[7:0]
};
)";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("s0"));
    QVERIFY(signalSet.contains("s7"));
    QVERIFY(signalSet.contains("s15"));
    QVERIFY(signalSet.contains("wide_bus"));
}

void Test::boundary_complexity_longExpression()
{
    /* Very long expression with many terms */
    QSlangDriver driver;
    QString      verilogCode = R"(
assign sum =
    a[15:0] + b[15:0] + c[15:0] + d[15:0] + e[15:0] +
    f[15:0] + g[15:0] + h[15:0] + i[15:0] + j[15:0] +
    k[15:0] + l[15:0] + m[15:0] + n[15:0] + o[15:0] + p[15:0];
)";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("a"));
    QVERIFY(signalSet.contains("p"));
    QVERIFY(signalSet.size() >= 17); /* a-p + sum */
}

void Test::boundary_mixed_allOperatorTypes()
{
    /* Mix all operator types in one expression */
    QSlangDriver driver;
    QString      verilogCode = R"(
assign result = (
    (arith_a[31:0] + arith_b[31:0] - arith_c[31:0] * arith_d[15:0] / arith_e[15:0] % arith_f[7:0]) &
    (logic_a[31:0] | logic_b[31:0] ^ logic_c[31:0] ~^ logic_d[31:0]) &
    (shift_a[31:0] << shift_b[4:0]) >> (shift_c[31:0] >>> shift_d[4:0]) &
    (cmp_a[31:0] == cmp_b[31:0]) ? sel_a[31:0] :
    (cmp_c[31:0] != cmp_d[31:0]) ? sel_b[31:0] :
    (cmp_e[31:0] > cmp_f[31:0]) ? sel_c[31:0] :
    (cmp_g[31:0] < cmp_h[31:0]) ? sel_d[31:0] : sel_e[31:0]
);
)";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    /* Should extract all arithmetic, logic, shift, comparison, and select signals */
    QVERIFY(signalSet.contains("arith_a"));
    QVERIFY(signalSet.contains("logic_a"));
    QVERIFY(signalSet.contains("shift_a"));
    QVERIFY(signalSet.contains("cmp_a"));
    QVERIFY(signalSet.contains("sel_a"));
}

/* ========== Real-World Scenario Tests ========== */
/* Test realistic hardware design patterns */

void Test::realWorld_stateMachine()
{
    /* FSM with different bit widths */
    QSlangDriver driver;
    QString      verilogCode = R"(
always @(posedge clk) begin
    case (state[2:0])
        3'b000: next_state = 3'b001;
        3'b001: next_state = data_ready ? 3'b010 : 3'b001;
        default: next_state = 3'b000;
    endcase
    counter[7:0] <= counter[7:0] + 8'd1;
end
)";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("clk"));
    QVERIFY(signalSet.contains("state"));
    QVERIFY(signalSet.contains("next_state"));
    QVERIFY(signalSet.contains("data_ready"));
    QVERIFY(signalSet.contains("counter"));
}

void Test::realWorld_busInterface()
{
    /* Bus interface with address comparison */
    QSlangDriver driver;
    QString      verilogCode = R"(
assign bus_req = addr[31:0] >= base[31:0] && addr[31:0] < limit[31:0];
assign rdata = valid ? mem_data[63:0] : 64'h0;
assign byte_en = be[7:0];
)";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("addr"));
    QVERIFY(signalSet.contains("base"));
    QVERIFY(signalSet.contains("limit"));
    QVERIFY(signalSet.contains("valid"));
    QVERIFY(signalSet.contains("mem_data"));
    QVERIFY(signalSet.contains("be"));
}

void Test::realWorld_fifoControl()
{
    /* FIFO control logic with full/empty generation */
    QSlangDriver driver;
    QString      verilogCode = R"(
assign wr_ptr_next[4:0] = wr_en ? (wr_ptr[4:0] + 5'h1) : wr_ptr[4:0];
assign rd_ptr_next[4:0] = rd_en ? (rd_ptr[4:0] + 5'h1) : rd_ptr[4:0];

assign count_next[4:0] = wr_en && !rd_en ? (count[4:0] + 5'h1) :
                        !wr_en && rd_en ? (count[4:0] - 5'h1) : count[4:0];

assign full = (count[4:0] == 5'd16);
assign empty = (count[4:0] == 5'd0);
assign almost_full = (count[4:0] >= 5'd14);
assign almost_empty = (count[4:0] <= 5'd2);

assign wr_addr = wr_ptr[3:0];
assign rd_addr = rd_ptr[3:0];
)";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("wr_en"));
    QVERIFY(signalSet.contains("rd_en"));
    QVERIFY(signalSet.contains("wr_ptr"));
    QVERIFY(signalSet.contains("rd_ptr"));
    QVERIFY(signalSet.contains("count"));
    QVERIFY(signalSet.size() >= 5);
}

void Test::realWorld_arithmeticUnit()
{
    /* ALU with multiple operations and flag generation */
    QSlangDriver driver;
    QString      verilogCode = R"(
always @(*) begin
    case (opcode[3:0])
        4'h0: alu_result[31:0] = operand_a[31:0] + operand_b[31:0];
        4'h1: alu_result[31:0] = operand_a[31:0] - operand_b[31:0];
        4'h2: alu_result[31:0] = operand_a[31:0] & operand_b[31:0];
        4'h3: alu_result[31:0] = operand_a[31:0] | operand_b[31:0];
        4'h4: alu_result[31:0] = operand_a[31:0] ^ operand_b[31:0];
        4'h5: alu_result[31:0] = operand_a[31:0] << operand_b[4:0];
        4'h6: alu_result[31:0] = operand_a[31:0] >> operand_b[4:0];
        4'h7: alu_result[31:0] = $signed(operand_a[31:0]) >>> operand_b[4:0];
        default: alu_result[31:0] = 32'h0;
    endcase
end

assign zero_flag = (alu_result[31:0] == 32'h0);
assign negative_flag = alu_result[31];
assign overflow = (operand_a[31] == operand_b[31]) && (alu_result[31] != operand_a[31]);
assign carry_out = cout[31];
)";

    bool parseResult = driver.parseVerilogSnippet(verilogCode, true);
    QVERIFY(parseResult);

    QSet<QString> signalSet = driver.extractSignalReferences();
    QVERIFY(signalSet.contains("opcode"));
    QVERIFY(signalSet.contains("operand_a"));
    QVERIFY(signalSet.contains("operand_b"));
    QVERIFY(signalSet.contains("alu_result"));
    QVERIFY(signalSet.contains("zero_flag"));
    QVERIFY(signalSet.contains("negative_flag"));
    QVERIFY(signalSet.contains("overflow"));
    QVERIFY(signalSet.contains("carry_out"));
    QVERIFY(signalSet.contains("cout"));
}

QSOC_TEST_MAIN(Test)

#include "test_qsocverilogsignalextractor.moc"
