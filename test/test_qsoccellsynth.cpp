// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoccellsynth.h"
#include "qsoc_test.h"

#include <QtCore>
#include <QtTest>

#include <algorithm>
#include <ctime>

namespace {

QSocCellSynthCell cell(
    const QString &name, const QStringList &input, const QString &output, int table)
{
    return {name, input, output, static_cast<quint8>(table)};
}

const QSocCellSynthCell kInv   = cell("inv", {"I"}, "ZN", 0b01);
const QSocCellSynthCell kNand2 = cell("nand2", {"A1", "A2"}, "ZN", 0b0111);
const QSocCellSynthCell kNor2  = cell("nor2", {"A1", "A2"}, "ZN", 0b0001);
const QSocCellSynthCell kAnd2  = cell("and2", {"A1", "A2"}, "Z", 0b1000);
const QSocCellSynthCell kOr2   = cell("or2", {"A1", "A2"}, "Z", 0b1110);
const QSocCellSynthCell kXor2  = cell("xor2", {"A1", "A2"}, "Z", 0b0110);

QStringList rolePort(QSocCellSynthRole role)
{
    switch (role) {
    case QSocCellSynthRole::Buf:
    case QSocCellSynthRole::Inv:
        return {"clk_in"};
    case QSocCellSynthRole::Mux2:
        return {"clk0", "clk1", "sel"};
    case QSocCellSynthRole::Or2:
    case QSocCellSynthRole::Xor2:
        return {"clk_a", "clk_b"};
    }
    return {};
}

QString roleExpression(QSocCellSynthRole role)
{
    switch (role) {
    case QSocCellSynthRole::Buf:
        return "clk_in";
    case QSocCellSynthRole::Inv:
        return "~clk_in";
    case QSocCellSynthRole::Mux2:
        return "sel ? clk1 : clk0";
    case QSocCellSynthRole::Or2:
        return "clk_a | clk_b";
    case QSocCellSynthRole::Xor2:
        return "clk_a ^ clk_b";
    }
    return {};
}

/* Behavioral model of a basis cell, generated from its declared table. */
QString cellModel(const QSocCellSynthCell &c)
{
    QStringList reversed;
    for (auto it = c.input.crbegin(); it != c.input.crend(); ++it) {
        reversed.append(*it);
    }
    const int rows = 1 << c.input.size();
    QString   bits;
    for (int m = rows - 1; m >= 0; --m) {
        bits.append(((c.table >> m) & 1) != 0 ? '1' : '0');
    }
    QStringList port;
    for (const auto &pin : c.input) {
        port.append("input wire " + pin);
    }
    port.append("output wire " + c.output);
    return QStringLiteral(
               "module %1 (%2);\n  localparam [%3:0] TT = %4'b%5;\n  assign %6 = TT[{%7}];\n"
               "endmodule\n")
        .arg(c.name, port.join(", "))
        .arg(rows - 1)
        .arg(rows)
        .arg(bits, c.output, reversed.join(", "));
}

/* Structural wrapper of a netlist, gate i named g<i>, the last one driving y. */
QString wrapper(
    const QSocCellSynthNetlist     &net,
    const QList<QSocCellSynthCell> &basis,
    const QString                  &module,
    const QStringList              &port)
{
    QStringList head;
    for (const auto &p : port) {
        head.append("input wire " + p);
    }
    QString text = QStringLiteral("module %1 (%2, output wire y);\n").arg(module, head.join(", "));
    for (int i = 0; i < net.gate.size(); ++i) {
        const QSocCellSynthGate &gate = net.gate[i];
        const auto  it = std::find_if(basis.cbegin(), basis.cend(), [&](const auto &c) {
            return c.name == gate.cell;
        });
        QStringList connect;
        for (int j = 0; j < gate.pin.size(); ++j) {
            const QSocCellSynthSource &src    = gate.pin[j];
            const QString              signal = src.kind == QSocCellSynthSource::Kind::Constant
                                                    ? QStringLiteral("1'b%1").arg(src.index)
                                                : src.kind == QSocCellSynthSource::Kind::Input
                                                    ? port.value(src.index)
                                                    : QStringLiteral("n%1").arg(src.index);
            connect.append(QStringLiteral(".%1(%2)").arg(it->input[j], signal));
        }
        const QString out = i + 1 == net.gate.size() ? QStringLiteral("y")
                                                     : QStringLiteral("n%1").arg(i);
        if (out != "y") {
            text += "  wire " + out + ";\n";
        }
        connect.append(QStringLiteral(".%1(%2)").arg(it->output, out));
        text += QStringLiteral("  %1 g%2 (%3);\n").arg(it->name).arg(i).arg(connect.join(", "));
    }
    return text + "endmodule\n";
}

double cpuSeconds()
{
    return static_cast<double>(std::clock()) / CLOCKS_PER_SEC;
}

class Test : public QObject
{
    Q_OBJECT

private:
    QSocCellSynthResult run(
        QSocCellSynthRole role, const QList<QSocCellSynthCell> &basis, bool tie = true)
    {
        QSocCellSynthRequest request;
        request.role                     = role;
        request.basis                    = basis;
        request.constantTie              = tie;
        const double              start  = cpuSeconds();
        const QSocCellSynthResult result = QSocCellSynth::synthesize(request);
        QStringList               name;
        for (const auto &c : basis) {
            name.append(c.name);
        }
        const QString verdict = result.status == QSocCellSynthStatus::Found
                                    ? QSocCellSynth::report(role, result.netlist)
                                    : QSocCellSynth::roleName(role) + ": " + result.reason;
        qInfo().noquote() << QStringLiteral(
                                 "%1 from {%2}%3: cpu %4 s, rlimit %7, %5 candidates, %6 hazardous")
                                 .arg(QSocCellSynth::roleName(role), name.join(", "))
                                 .arg(tie ? "" : " without ties")
                                 .arg(cpuSeconds() - start, 0, 'f', 3)
                                 .arg(result.candidate)
                                 .arg(result.hazardous)
                                 .arg(result.resourceUsed)
                          << "|" << verdict;
        return result;
    }

    void checkFound(
        QSocCellSynthRole               role,
        const QList<QSocCellSynthCell> &basis,
        int                             depth,
        int                             count,
        QSocCellSynthNetlist           *out = nullptr)
    {
        const QSocCellSynthResult result = run(role, basis);
        QCOMPARE(result.status, QSocCellSynthStatus::Found);
        QCOMPARE(result.netlist.depth, depth);
        QCOMPARE(result.netlist.gate.size(), count);
        const int  inputs = QSocCellSynth::roleInputCount(role);
        const auto table  = QSocCellSynth::roleTable(role);
        for (int m = 0; m < (1 << inputs); ++m) {
            QCOMPARE(QSocCellSynth::evaluate(result.netlist, basis, m), ((table >> m) & 1) != 0);
        }
        if (out != nullptr) {
            *out = result.netlist;
        }
    }

    /* Prove wrapper == role with a SAT miter over the flattened models. */
    void proveEquivalent(
        QSocCellSynthRole               role,
        const QList<QSocCellSynthCell> &basis,
        const QSocCellSynthNetlist     &net)
    {
        const QString yosys = QStandardPaths::findExecutable("yosys");
        if (yosys.isEmpty()) {
            QSOC_TEST_MISSING_DEPENDENCY("yosys");
        }
        const QStringList port = rolePort(role);
        QStringList       gold;
        for (const auto &p : port) {
            gold.append("input wire " + p);
        }
        QString text = wrapper(net, basis, "qsoc_role", port)
                           .replace("output wire y", "output wire clk_out")
                           .replace("(y)", "(clk_out)");
        for (const auto &c : basis) {
            text += cellModel(c);
        }
        text += QStringLiteral(
                    "module qsoc_gold (%1, output wire clk_out);\n"
                    "  assign clk_out = %2;\nendmodule\n")
                    .arg(gold.join(", "), roleExpression(role));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile file(dir.filePath("design.v"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write(text.toUtf8());
        file.close();
        QProcess process;
        process.setWorkingDirectory(dir.path());
        process.start(
            yosys,
            {"-q",
             "-p",
             "read_verilog design.v; proc; flatten; "
             "miter -equiv -flatten -make_assert qsoc_gold qsoc_role qsoc_miter; "
             "hierarchy -top qsoc_miter; sat -verify -prove-asserts qsoc_miter"});
        QVERIFY(process.waitForFinished(60000));
        QVERIFY2(
            process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
            qPrintable(process.readAllStandardOutput() + process.readAllStandardError()));
    }

private slots:
    void muxFromNandInv();
    void muxFromNorInv();
    void muxFromAndOrInv();
    void orFromNandInv();
    void orFromAndInv();
    void orFromNorInv();
    void xorFromNand();
    void invFromNand();
    void muxFromXorAnd();
    void deterministicAcrossRunsAndOrder();
    void asymmetricCellKeepsPinOrder();
    void emittedWrappersAreEquivalent();
    void bufFromInv();
    void tieBreakPrefersUntiedCells();
    void hazardousHandNetworkRejected();
    void budgetExceeded();
    void cancelled();
    void invalidBasis();
};

void Test::muxFromNandInv()
{
    QSocCellSynthNetlist net;
    checkFound(QSocCellSynthRole::Mux2, {kNand2, kInv}, 3, 4, &net);
    using K = QSocCellSynthSource::Kind;
    const QList<QSocCellSynthGate> expect{
        {"inv", {{K::Input, 2}}},
        {"nand2", {{K::Input, 1}, {K::Input, 2}}},
        {"nand2", {{K::Input, 0}, {K::Gate, 0}}},
        {"nand2", {{K::Gate, 1}, {K::Gate, 2}}},
    };
    QCOMPARE(net.gate, expect);
    QCOMPARE(
        QSocCellSynth::report(QSocCellSynthRole::Mux2, net),
        QString("ck_mux2: depth 3, 4 cells (inv x1, nand2 x3)"));
}

void Test::muxFromNorInv()
{
    checkFound(QSocCellSynthRole::Mux2, {kNor2, kInv}, 3, 4);
}

void Test::muxFromAndOrInv()
{
    checkFound(QSocCellSynthRole::Mux2, {kAnd2, kOr2, kInv}, 3, 4);
}

void Test::orFromNandInv()
{
    checkFound(QSocCellSynthRole::Or2, {kNand2, kInv}, 2, 3);
}

void Test::orFromAndInv()
{
    checkFound(QSocCellSynthRole::Or2, {kAnd2, kInv}, 3, 4);
}

void Test::orFromNorInv()
{
    checkFound(QSocCellSynthRole::Or2, {kNor2, kInv}, 2, 2);
}

void Test::xorFromNand()
{
    /* The four-gate form glitches under skewed input paths; tied inverters fix it. */
    QSocCellSynthNetlist net;
    checkFound(QSocCellSynthRole::Xor2, {kNand2}, 3, 5, &net);
    int ties = 0;
    for (const auto &gate : net.gate) {
        for (const auto &pin : gate.pin) {
            ties += pin.kind == QSocCellSynthSource::Kind::Constant ? 1 : 0;
        }
    }
    QCOMPARE(ties, 2);
}

void Test::invFromNand()
{
    QSocCellSynthNetlist net;
    checkFound(QSocCellSynthRole::Inv, {kNand2}, 1, 1, &net);
    using K = QSocCellSynthSource::Kind;
    QCOMPARE(net.gate.first().pin, (QList<QSocCellSynthSource>{{K::Constant, 1}, {K::Input, 0}}));
}

void Test::muxFromXorAnd()
{
    /* Without ties every function of xor2 and and2 maps all-zero to zero and
     * the select cannot be inverted to mask the data input. */
    const QSocCellSynthResult none = run(QSocCellSynthRole::Mux2, {kXor2, kAnd2}, false);
    QCOMPARE(none.status, QSocCellSynthStatus::NoSolution);
    /* A tie makes xor2 an inverter: (a & ~s) ^ (b & s), whose terms never overlap. */
    checkFound(QSocCellSynthRole::Mux2, {kXor2, kAnd2}, 3, 4);
}

void Test::deterministicAcrossRunsAndOrder()
{
    const QList<QSocCellSynthCell> basis{kAnd2, kOr2, kInv, kNand2, kNor2};
    QList<QSocCellSynthCell>       reversed(basis.crbegin(), basis.crend());
    const auto                     first  = run(QSocCellSynthRole::Mux2, basis);
    const auto                     second = run(QSocCellSynthRole::Mux2, basis);
    const auto                     third  = run(QSocCellSynthRole::Mux2, reversed);
    QCOMPARE(first.status, QSocCellSynthStatus::Found);
    QCOMPARE(second.netlist, first.netlist);
    QCOMPARE(third.netlist, first.netlist);
    const QStringList port = rolePort(QSocCellSynthRole::Mux2);
    QCOMPARE(wrapper(third.netlist, reversed, "m", port), wrapper(first.netlist, basis, "m", port));

    const auto xorA = run(QSocCellSynthRole::Xor2, {kNand2, kInv, kNor2});
    const auto xorB = run(QSocCellSynthRole::Xor2, {kNor2, kNand2, kInv});
    QCOMPARE(xorA.status, QSocCellSynthStatus::Found);
    QCOMPARE(xorB.netlist, xorA.netlist);
}

void Test::asymmetricCellKeepsPinOrder()
{
    /* andn2 = A & ~B is not symmetric: the solver must pick which pin gets what. */
    const QSocCellSynthCell andn = cell("andn2", {"A", "B"}, "Z", 0b0010);
    QSocCellSynthNetlist    net;
    checkFound(QSocCellSynthRole::Mux2, {andn, kOr2}, 2, 3, &net);
    proveEquivalent(QSocCellSynthRole::Mux2, {andn, kOr2}, net);
}

void Test::emittedWrappersAreEquivalent()
{
    const QList<QPair<QSocCellSynthRole, QList<QSocCellSynthCell>>> cases{
        {QSocCellSynthRole::Mux2, {kNand2, kInv}},
        {QSocCellSynthRole::Mux2, {kNor2, kInv}},
        {QSocCellSynthRole::Mux2, {kAnd2, kOr2, kInv}},
        {QSocCellSynthRole::Mux2, {kXor2, kAnd2}},
        {QSocCellSynthRole::Or2, {kNand2, kInv}},
        {QSocCellSynthRole::Or2, {kAnd2, kInv}},
        {QSocCellSynthRole::Xor2, {kNand2}},
        {QSocCellSynthRole::Inv, {kNand2}},
    };
    for (const auto &entry : cases) {
        QSocCellSynthRequest request;
        request.role                     = entry.first;
        request.basis                    = entry.second;
        const QSocCellSynthResult result = QSocCellSynth::synthesize(request);
        QCOMPARE(result.status, QSocCellSynthStatus::Found);
        proveEquivalent(entry.first, entry.second, result.netlist);
    }
}

void Test::bufFromInv()
{
    QSocCellSynthNetlist net;
    checkFound(QSocCellSynthRole::Buf, {kInv}, 2, 2, &net);
    proveEquivalent(QSocCellSynthRole::Buf, {kInv}, net);
    checkFound(QSocCellSynthRole::Buf, {kNand2}, 2, 2);
}

void Test::tieBreakPrefersUntiedCells()
{
    /* Either name order, the untied inverter pair wins over tied NANDs. */
    const QSocCellSynthCell aNand = cell("a_nand2", {"A1", "A2"}, "ZN", 0b0111);
    const QSocCellSynthCell zInv  = cell("z_inv", {"I"}, "ZN", 0b01);
    for (const QList<QSocCellSynthCell> &basis :
         {QList<QSocCellSynthCell>{kNand2, kInv}, QList<QSocCellSynthCell>{aNand, zInv}}) {
        QSocCellSynthNetlist net;
        checkFound(QSocCellSynthRole::Buf, basis, 2, 2, &net);
        for (const auto &gate : net.gate) {
            QCOMPARE(gate.pin.size(), 1);
            QCOMPARE(gate.pin.first().kind == QSocCellSynthSource::Kind::Constant, false);
        }
    }
    /* With equal ties, fewer cell inputs win: inv over a tied nand2. */
    QSocCellSynthNetlist net;
    checkFound(QSocCellSynthRole::Inv, {aNand, zInv}, 1, 1, &net);
    QCOMPARE(net.gate.first().cell, QString("z_inv"));
    /* xor2 tied as an inverter loses to a real inverter in a mux. */
    checkFound(QSocCellSynthRole::Mux2, {kXor2, kAnd2, kOr2, kInv}, 3, 4, &net);
    for (const auto &gate : net.gate) {
        for (const auto &pin : gate.pin)
            QVERIFY(pin.kind != QSocCellSynthSource::Kind::Constant);
    }
}

void Test::hazardousHandNetworkRejected()
{
    using K = QSocCellSynthSource::Kind;
    /* The textbook four-NAND XOR: a change of a reaches the output through
     * n0 and directly, so skewed paths can pulse the output twice. */
    QSocCellSynthNetlist xorFour;
    xorFour.gate = {
        {"nand2", {{K::Input, 0}, {K::Input, 1}}},
        {"nand2", {{K::Input, 0}, {K::Gate, 0}}},
        {"nand2", {{K::Input, 1}, {K::Gate, 0}}},
        {"nand2", {{K::Gate, 1}, {K::Gate, 2}}},
    };
    xorFour.depth = 3;
    for (int m = 0; m < 4; ++m) {
        QCOMPARE(QSocCellSynth::evaluate(xorFour, {kNand2}, m), ((0b0110 >> m) & 1) != 0);
    }
    QVERIFY(!QSocCellSynth::hazardFree(QSocCellSynthRole::Xor2, xorFour, {kNand2}));

    /* The found five-cell form passes the same check. */
    const auto found = run(QSocCellSynthRole::Xor2, {kNand2});
    QVERIFY(QSocCellSynth::hazardFree(QSocCellSynthRole::Xor2, found.netlist, {kNand2}));

    /* a | (a ^ b) is an OR, but with b = 1 it is a | ~a: when a toggles, the
     * two paths of a can both read low for a moment and pulse the output. */
    QSocCellSynthNetlist orXor;
    orXor.gate = {
        {"xor2", {{K::Input, 0}, {K::Input, 1}}},
        {"or2", {{K::Input, 0}, {K::Gate, 0}}},
    };
    for (int m = 0; m < 4; ++m) {
        QCOMPARE(QSocCellSynth::evaluate(orXor, {kXor2, kOr2}, m), ((0b1110 >> m) & 1) != 0);
    }
    QVERIFY(!QSocCellSynth::hazardFree(QSocCellSynthRole::Or2, orXor, {kXor2, kOr2}));

    /* Malformed netlists never pass. */
    QSocCellSynthNetlist forward;
    forward.gate = {{"inv", {{K::Gate, 0}}}};
    QVERIFY(!QSocCellSynth::hazardFree(QSocCellSynthRole::Inv, forward, {kInv}));
    QVERIFY(!QSocCellSynth::hazardFree(QSocCellSynthRole::Inv, {}, {kInv}));
}

void Test::budgetExceeded()
{
    QSocCellSynthRequest request;
    request.role                     = QSocCellSynthRole::Mux2;
    request.basis                    = {kXor2, kAnd2};
    request.constantTie              = false;
    request.resourceLimit            = 1000;
    const QSocCellSynthResult result = QSocCellSynth::synthesize(request);
    QCOMPARE(result.status, QSocCellSynthStatus::BudgetExceeded);
}

void Test::cancelled()
{
    std::stop_source source;
    source.request_stop();
    QSocCellSynthRequest request;
    request.role  = QSocCellSynthRole::Mux2;
    request.basis = {kNand2, kInv};
    QCOMPARE(
        QSocCellSynth::synthesize(request, source.get_token()).status,
        QSocCellSynthStatus::Cancelled);
}

void Test::invalidBasis()
{
    QSocCellSynthRequest request;
    request.role = QSocCellSynthRole::Mux2;
    QCOMPARE(QSocCellSynth::synthesize(request).status, QSocCellSynthStatus::Invalid);
    request.basis = {kInv, kInv};
    QCOMPARE(QSocCellSynth::synthesize(request).status, QSocCellSynthStatus::Invalid);
    request.basis = {cell("bad", {"A"}, "Z", 0b100)};
    QCOMPARE(QSocCellSynth::synthesize(request).status, QSocCellSynthStatus::Invalid);
    request.basis    = {kNand2};
    request.maxCells = 9;
    QCOMPARE(QSocCellSynth::synthesize(request).status, QSocCellSynthStatus::Invalid);
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoccellsynth.moc"
