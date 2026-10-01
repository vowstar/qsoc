// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoccelltable.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

namespace {

QSocCellPorts cellPorts(const QString &cell)
{
    if (cell == "nand2") {
        return {{"A1", "in"}, {"A2", "in"}, {"ZN", "out"}};
    }
    if (cell == "inv") {
        return {{"I", "input"}, {"ZN", "output"}};
    }
    if (cell == "mux2") {
        return {{"I0", "in"}, {"I1", "in"}, {"S", "in"}, {"Z", "out"}};
    }
    if (cell == "rmux") {
        return {{"A", "in"}, {"B", "in"}, {"SEL", "in"}, {"Y", "out"}};
    }
    if (cell == "ha") {
        return {{"A", "in"}, {"B", "in"}, {"CO", "out"}, {"S", "out"}};
    }
    if (cell == "wide") {
        return {{"I", "in"}, {"T", QSocCellPort("in", 2)}, {"ZN", "out"}};
    }
    if (cell == "nine") {
        QSocCellPorts ports = {{"Z", "out"}};
        for (int i = 0; i < 9; ++i) {
            ports.insert(QStringLiteral("I%1").arg(i), "in");
        }
        return ports;
    }
    return {
        {"PAD", "inout"},
        {"I", "in"},
        {"OEN", "in"},
        {"C", "out"},
        {"PE", "in"},
        {"PS", "in"},
        {"DS", "in"}};
}

QSocCellTable cellTable(const QString &cell, const QString &yaml)
{
    return QSocCellTable::parse(YAML::Load(yaml.toStdString()), cellPorts(cell));
}

QString bits(const QSocCellTable::Truth &truth, int inputs)
{
    QString text;
    for (int m = 0; m < (1 << inputs); ++m) {
        text += truth.test(m) ? '1' : '0';
    }
    return text;
}

const QString nand2 = QStringLiteral("[{A1: 0, ZN: 1}, {A2: 0, ZN: 1}, {A1: 1, A2: 1, ZN: 0}]");
const QString mux2  = QStringLiteral("[{S: 0, Z: I0}, {S: 1, Z: I1}]");
const QString pad   = QStringLiteral(
    "[{PE: 0, pull: none}, {PE: 1, PS: 1, pull: up}, {PE: 1, PS: 0, pull: down},"
    " {DS: 0, drive: low}, {DS: 1, drive: high}]");

bool writeText(const QString &path, const QString &text)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(text.toUtf8()) == text.toUtf8().size();
}

/* Exhaustive self-checking bench: expected holds one bit per minterm. */
QString bench(const QSocCellTable &table, const QString &module)
{
    const QStringList in  = table.inputs();
    const QStringList out = table.outputs();
    QStringList       connections;
    for (qsizetype i = 0; i < in.size(); ++i) {
        connections.append(QStringLiteral(".%1(in[%2])").arg(in[i]).arg(i));
    }
    QString checks;
    for (qsizetype o = 0; o < out.size(); ++o) {
        QString expected = bits(table.truth(out[o]), int(in.size()));
        std::reverse(expected.begin(), expected.end());
        connections.append(QStringLiteral(".%1(out[%2])").arg(out[o]).arg(o));
        checks += QStringLiteral(
                      "        if (out[%1] !== ((%2'b%3 >> in) & 1'b1)) "
                      "$display(\"TEST_FAIL %4 %b\", in);\n")
                      .arg(o)
                      .arg(expected.size())
                      .arg(expected, out[o]);
    }
    return QStringLiteral(
               "module tb;\n"
               "    reg  [%1:0] in;\n"
               "    wire [%2:0] out;\n"
               "    integer m;\n"
               "    %3 dut (%4);\n"
               "    initial begin\n"
               "        for (m = 0; m < %5; m = m + 1) begin\n"
               "        in = m; #1;\n"
               "%6"
               "        end\n"
               "        $display(\"TEST_PASS\");\n"
               "    end\n"
               "endmodule\n")
        .arg(in.size() - 1)
        .arg(out.size() - 1)
        .arg(module, connections.join(", "))
        .arg(1 << in.size())
        .arg(checks);
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void truthTables_data();
    void truthTables();
    void rejects_data();
    void rejects();
    void omittedInputIsX();
    void evaluatesWithUnknownInputs();
    void attributesBecomePositionalPatterns();
    void patternsRejectPinsOutsideTheOrder();
    void rowsCheckAllowsRepeatedPatterns();
    void matchesUnderPermutation_data();
    void matchesUnderPermutation();
    void emitsVerilog_data();
    void emitsVerilog();
    void modelRejectsNonCombinationalCell();
    void modelElaboratesAndSimulates_data();
    void modelElaboratesAndSimulates();
    void readsLibraryPorts();
};

void Test::truthTables_data()
{
    QTest::addColumn<QString>("cell");
    QTest::addColumn<QString>("yaml");
    QTest::addColumn<QString>("output");
    QTest::addColumn<QString>("truth");

    QTest::newRow("nand2 rows") << "nand2" << nand2 << "ZN" << "1110";
    QTest::newRow("inv single map") << "inv" << "{ZN: '!I'}" << "ZN" << "10";
    QTest::newRow("inv bitwise") << "inv" << "{ZN: '~I'}" << "ZN" << "10";
    QTest::newRow("mux2 pin value") << "mux2" << mux2 << "Z" << "01010011";
    QTest::newRow("mux2 ternary") << "mux2" << "{Z: 'S ? I1 : I0'}" << "Z" << "01010011";
    QTest::newRow("nand2 expression") << "nand2" << "{ZN: '~(A1 & A2)'}" << "ZN" << "1110";
    QTest::newRow("logical ops") << "nand2" << "{ZN: '!(A1 && A2) || 1''b0'}" << "ZN"
                                 << "1110";
    QTest::newRow("xor") << "ha" << "{S: 'A ^ B', CO: 'A & B'}" << "S" << "0110";
    QTest::newRow("or of carry") << "ha" << "{S: 'A ^ B', CO: 'A & B'}" << "CO" << "0001";
    QTest::newRow("constant rows") << "inv" << "[{I: 0, ZN: 1}, {I: 1, ZN: 1'b0}]" << "ZN"
                                   << "10";
    QTest::newRow("overlap agrees") << "nand2"
                                    << "[{A1: 0, ZN: 1}, {A1: 0, A2: 0, ZN: 1},"
                                       " {A1: 1, ZN: '!A2'}]"
                                    << "ZN" << "1110";
    QTest::newRow("wide input ignored") << "wide" << "{ZN: '!I'}" << "ZN" << "10";
}

void Test::truthTables()
{
    QFETCH(QString, cell);
    QFETCH(QString, yaml);
    QFETCH(QString, output);
    QFETCH(QString, truth);

    const QSocCellTable table = cellTable(cell, yaml);
    QVERIFY2(table.isValid(), qPrintable(table.errors().join('\n')));
    QVERIFY(table.outputs().contains(output));
    QCOMPARE(bits(table.truth(output), int(table.inputs().size())), truth);
}

void Test::rejects_data()
{
    QTest::addColumn<QString>("cell");
    QTest::addColumn<QString>("yaml");
    QTest::addColumn<QString>("error");

    QTest::newRow("typo pin") << "nand2" << "[{A3: 0, ZN: 1}]"
                              << "function[0].A3: 'A3' is not a port of the cell";
    QTest::newRow("wide pin") << "wide" << "{T: 1, ZN: 1}"
                              << "function.T: pin T is 2 bits wide, a table pin is 1 bit";
    QTest::newRow("input word") << "inv" << "[{I: high, ZN: 0}, {I: 0, ZN: 1}]"
                                << "function[0].I: input pin takes 0, 1 or x, not 'high'";
    QTest::newRow("output x") << "inv" << "{ZN: x}" << "function.ZN: an output pin cannot be x";
    QTest::newRow("inout pin") << "pad" << "[{PAD: 1, pull: up}, {pull: down}]"
                               << "function[0].PAD: inout pin PAD cannot appear in a table";
    QTest::newRow("attribute name") << "pad" << "[{PE: 0, 'pull-up': a}, {PE: 1, 'pull-up': b}]"
                                    << "function[0].pull-up: attribute name 'pull-up' is not an "
                                       "identifier";
    QTest::newRow("label words") << "pad" << "[{PE: 0, pull: weak up}, {PE: 1, pull: none}]"
                                 << "function[0].pull: label 'weak up' is not a single word";
    QTest::newRow("no output") << "nand2" << "[{A1: 0}, {ZN: '~(A1 & A2)'}]"
                               << "function[0]: row sets no output pin or attribute";
    QTest::newRow("conflict") << "nand2" << "[{A1: 0, ZN: 1}, {A2: 0, ZN: 0}, {A1: 1, ZN: 1}]"
                              << "function[0] and function[1]: disagree on ZN when A1=0 A2=0";
    QTest::newRow("label conflict") << "pad" << "[{PE: 1, pull: up}, {PS: 1, pull: down}]"
                                    << "function[0] and function[1]: disagree on pull when PE=1 "
                                       "PS=1";
    QTest::newRow("coverage gap") << "nand2" << "[{A1: 0, ZN: 1}, {A1: 1, A2: 1, ZN: 0}]"
                                  << "function: ZN is undefined when A1=1 A2=0";
    QTest::newRow("single value") << "pad" << "[{PE: 0, pull: up}, {PE: 1, pull: up}]"
                                  << "function.pull: the only value is 'up', a constant is a tie";
    QTest::newRow("unknown identifier") << "inv" << "{ZN: '!J'}"
                                        << "function.ZN: 'J' is not an input pin";
    QTest::newRow("output identifier") << "ha" << "{S: 'A ^ B', CO: 'S & A'}"
                                       << "function.CO: 'S' is not an input pin";
    QTest::newRow("arithmetic") << "nand2" << "{ZN: 'A1 + A2'}"
                                << "function.ZN: 'A1 + A2' is not one of ! ~ & | ^ && || ?:";
    QTest::newRow("xnor") << "nand2" << "{ZN: 'A1 ~^ A2'}"
                          << "function.ZN: 'A1 ~^ A2' is not one of ! ~ & | ^ && || ?:";
    QTest::newRow("wide literal") << "inv" << "{ZN: 'I & 2''b1'}"
                                  << "function.ZN: literal '2'b1' is not 0, 1, 1'b0 or 1'b1";
    QTest::newRow("macro") << "inv" << "{ZN: '`FOO'}"
                           << "function.ZN: '`FOO' is not a plain expression";
    QTest::newRow("trailing text") << "inv" << "{ZN: 'I I'}"
                                   << "function.ZN: 'I I' is not a valid expression";
    QTest::newRow("scalar node") << "inv" << "7" << "function: expected a row or a list of rows";
    QTest::newRow("nested value") << "inv" << "{ZN: [1]}" << "function.ZN: value must be a scalar";
    QTest::newRow("nine inputs") << "nine" << "{Z: I0}"
                                 << "function.Z: 9 input pins exceed the limit of 8 for a "
                                    "Boolean output";
}

void Test::rejects()
{
    QFETCH(QString, cell);
    QFETCH(QString, yaml);
    QFETCH(QString, error);

    const QSocCellTable table = cellTable(cell, yaml);
    QVERIFY(!table.isValid());
    QVERIFY2(table.errors().contains(error), qPrintable(table.errors().join('\n')));
}

void Test::omittedInputIsX()
{
    const QSocCellTable omitted = cellTable("nand2", nand2);
    const QSocCellTable spelled
        = cellTable("nand2", "[{A1: 0, A2: x, ZN: 1}, {A1: x, A2: 0, ZN: 1}, {A1: 1, A2: 1, ZN: 0}]");
    QVERIFY2(spelled.isValid(), qPrintable(spelled.errors().join('\n')));
    QCOMPARE(spelled.truth("ZN"), omitted.truth("ZN"));
    QCOMPARE(spelled.rows().at(0).when, (QMap<QString, bool>{{"A1", false}}));
}

void Test::evaluatesWithUnknownInputs()
{
    const QSocCellTable table = cellTable("nand2", nand2);
    QCOMPARE(table.evaluate({{"A1", true}, {"A2", true}}), (QMap<QString, QString>{{"ZN", "0"}}));
    QCOMPARE(table.evaluate({{"A1", false}}), (QMap<QString, QString>{{"ZN", "1"}}));
    QCOMPARE(table.evaluate({{"A1", true}}), (QMap<QString, QString>{}));

    const QSocCellTable pads = cellTable("pad", pad);
    QVERIFY2(pads.isValid(), qPrintable(pads.errors().join('\n')));
    QCOMPARE(
        pads.evaluate({{"PE", true}, {"PS", false}, {"DS", true}}),
        (QMap<QString, QString>{{"pull", "down"}, {"drive", "high"}}));
    QCOMPARE(pads.evaluate({{"PE", true}}), (QMap<QString, QString>{}));
}

void Test::attributesBecomePositionalPatterns()
{
    const QSocCellTable table = cellTable("pad", pad);
    QVERIFY2(table.isValid(), qPrintable(table.errors().join('\n')));
    QCOMPARE(table.attributes(), (QStringList{"pull", "drive"}));
    QVERIFY(table.outputs().isEmpty());
    const QList<QSocCellTable::Pattern> expected = {{"0x", "none"}, {"11", "up"}, {"10", "down"}};
    QCOMPARE(table.patterns("pull", {"PE", "PS"}), expected);
    QCOMPARE(
        table.patterns("drive", {"PS", "DS"}),
        (QList<QSocCellTable::Pattern>{{"x0", "low"}, {"x1", "high"}}));
}

void Test::patternsRejectPinsOutsideTheOrder()
{
    const QSocCellTable table = cellTable("pad", pad);
    QString             error;
    QVERIFY(table.patterns("pull", {"PE"}, &error).isEmpty());
    QCOMPARE(error, QStringLiteral("pull: pin PS is not in the pin order"));
}

void Test::rowsCheckAllowsRepeatedPatterns()
{
    /* Two labels on one pattern, an overlap through x, and a lone label. */
    const YAML::Node node = YAML::Load(
        "[{PE: 0, pull: none}, {PE: 1, PS: 1, pull: up}, {PE: 1, PS: 1, pull: hold},"
        " {PE: 0, PS: 1, pull: keeper}, {DS: 1, drive: high}]");
    QVERIFY(!QSocCellTable::parse(node, cellPorts("pad")).isValid());
    const QSocCellTable rows = QSocCellTable::parse(
        node, cellPorts("pad"), QStringLiteral("function"), QSocCellTable::Check::Rows);
    QVERIFY2(rows.isValid(), qPrintable(rows.errors().join('\n')));
    QCOMPARE(
        rows.patterns("pull", {"PE", "PS"}),
        (QList<QSocCellTable::Pattern>{
            {"0x", "none"}, {"11", "up"}, {"11", "hold"}, {"01", "keeper"}}));

    /* Each row is still checked on its own. */
    const QSocCellTable bad = QSocCellTable::parse(
        YAML::Load("[{PE: 2, pull: up}]"),
        cellPorts("pad"),
        QStringLiteral("function"),
        QSocCellTable::Check::Rows);
    QCOMPARE(bad.errors(), QStringList{"function[0].PE: input pin takes 0, 1 or x, not '2'"});
}

void Test::matchesUnderPermutation_data()
{
    QTest::addColumn<QString>("cell");
    QTest::addColumn<QString>("yaml");
    QTest::addColumn<QString>("otherCell");
    QTest::addColumn<QString>("otherYaml");
    QTest::addColumn<QStringList>("maps");

    QTest::newRow("nand symmetric") << "nand2" << nand2 << "nand2" << "{ZN: '!(A2 & A1)'}"
                                    << QStringList{"A1>A1 A2>A2 ZN>ZN", "A1>A2 A2>A1 ZN>ZN"};
    QTest::newRow("mux select") << "mux2" << mux2 << "rmux" << "{Y: 'SEL ? B : A'}"
                                << QStringList{"I0>A I1>B S>SEL Z>Y"};
    QTest::newRow("mux swapped data")
        << "mux2" << mux2 << "rmux" << "{Y: 'SEL ? A : B'}" << QStringList{"I0>B I1>A S>SEL Z>Y"};
    QTest::newRow("half adder outputs")
        << "ha" << "{S: 'A ^ B', CO: 'A & B'}" << "ha" << "{S: 'A & B', CO: 'A ^ B'}"
        << QStringList{"A>A B>B CO>S S>CO", "A>B B>A CO>S S>CO"};
    QTest::newRow("nand is not nor")
        << "nand2" << nand2 << "nand2" << "{ZN: '!(A1 | A2)'}" << QStringList{};
}

void Test::matchesUnderPermutation()
{
    QFETCH(QString, cell);
    QFETCH(QString, yaml);
    QFETCH(QString, otherCell);
    QFETCH(QString, otherYaml);
    QFETCH(QStringList, maps);

    const QSocCellTable table = cellTable(cell, yaml);
    const QSocCellTable other = cellTable(otherCell, otherYaml);
    QVERIFY2(other.isValid(), qPrintable(other.errors().join('\n')));
    QStringList found;
    for (const QSocCellTable::PinMap &map : table.matches(other)) {
        QStringList pairs;
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            pairs.append(it.key() + '>' + it.value());
        }
        found.append(pairs.join(' '));
    }
    QCOMPARE(found, maps);
}

void Test::emitsVerilog_data()
{
    QTest::addColumn<QString>("cell");
    QTest::addColumn<QString>("yaml");
    QTest::addColumn<QString>("assigns");

    QTest::newRow("nand2") << "nand2" << nand2 << "    assign ZN = ~A1 | ~A2;\n";
    QTest::newRow("inv") << "inv" << "{ZN: '!I'}" << "    assign ZN = !I;\n";
    QTest::newRow("mux2") << "mux2" << mux2 << "    assign Z = ~S & (I0) | S & (I1);\n";
    QTest::newRow("ternary kept") << "mux2" << "{Z: 'S ? I1 : I0'}"
                                  << "    assign Z = S ? I1 : I0;\n";
    QTest::newRow("all zero") << "inv" << "[{I: 0, ZN: 0}, {I: 1, ZN: 0}]"
                              << "    assign ZN = 1'b0;\n";
}

void Test::emitsVerilog()
{
    QFETCH(QString, cell);
    QFETCH(QString, yaml);
    QFETCH(QString, assigns);

    QString       error;
    const QString text = cellTable(cell, yaml).verilog(cell.toUpper(), &error);
    QVERIFY2(!text.isEmpty(), qPrintable(error));
    QVERIFY2(text.startsWith(QStringLiteral("module %1 (\n").arg(cell.toUpper())), qPrintable(text));
    QVERIFY2(text.contains(assigns), qPrintable(text));
    QVERIFY(text.endsWith("endmodule\n"));
}

void Test::modelRejectsNonCombinationalCell()
{
    QString error;
    QVERIFY(cellTable("pad", pad).verilog("PAD", &error).isEmpty());
    QCOMPARE(error, QStringLiteral("output C has no function"));
    QVERIFY(cellTable("ha", "{S: 'A ^ B'}").verilog("HA", &error).isEmpty());
    QCOMPARE(error, QStringLiteral("output CO has no function"));
}

void Test::modelElaboratesAndSimulates_data()
{
    QTest::addColumn<QString>("cell");
    QTest::addColumn<QString>("yaml");

    QTest::newRow("nand2") << "nand2" << nand2;
    QTest::newRow("mux2") << "mux2" << mux2;
    QTest::newRow("mixed rows") << "mux2"
                                << "[{S: 0, Z: 'I0 && 1'}, {S: 1, I1: 1, Z: 1'b1},"
                                   " {S: 1, I1: 0, Z: '!1''b1'}]";
    QTest::newRow("half adder") << "ha" << "{S: 'A ^ B', CO: 'A ? B : 1''b0'}";
    QTest::newRow("wide input") << "wide" << "{ZN: '~I'}";
}

void Test::modelElaboratesAndSimulates()
{
    QFETCH(QString, cell);
    QFETCH(QString, yaml);

    const QString compiler = QStandardPaths::findExecutable("iverilog");
    const QString runtime  = QStandardPaths::findExecutable("vvp");
    if (compiler.isEmpty() || runtime.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY(QStringLiteral("iverilog and vvp"));
    }
    const QSocCellTable table = cellTable(cell, yaml);
    QString             error;
    const QString       model = table.verilog("CELL", &error);
    QVERIFY2(!model.isEmpty(), qPrintable(error));

    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const QDir dir(directory.path());
    QVERIFY(writeText(dir.filePath("cell.v"), model));
    QVERIFY(writeText(dir.filePath("tb.v"), bench(table, "CELL")));

    QProcess process;
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(
        compiler,
        {"-g2005",
         "-s",
         "tb",
         "-o",
         dir.filePath("sim"),
         dir.filePath("cell.v"),
         dir.filePath("tb.v")});
    QVERIFY(process.waitForFinished());
    QVERIFY2(process.exitCode() == 0, process.readAll().constData());
    process.start(runtime, {dir.filePath("sim")});
    QVERIFY(process.waitForFinished());
    const QByteArray output = process.readAll();
    QVERIFY2(output.contains("TEST_PASS") && !output.contains("TEST_FAIL"), output.constData());
}

void Test::readsLibraryPorts()
{
    const YAML::Node node = YAML::Load(
        "{A: {direction: input, type: logic}, B: {direction: in, type: 'logic[3:0]'},"
        " Y: {direction: output, type: 'logic[1:0][3:0]'}, P: {direction: inout}}");
    const QSocCellPorts expected
        = {{"A", "in"}, {"B", QSocCellPort("in", 4)}, {"Y", QSocCellPort("out", 8)}, {"P", "inout"}};
    QCOMPARE(QSocCellTable::portsOf(node), expected);
    QVERIFY(QSocCellTable::portsOf(YAML::Node()).isEmpty());
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoccelltable.moc"
