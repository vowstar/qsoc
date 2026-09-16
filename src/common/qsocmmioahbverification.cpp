// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocmmioahbverification.h"

#include <algorithm>
#include <QSet>

namespace {
QString literal(quint32 width, quint64 value)
{
    return QString("%1'h%2").arg(width).arg(value, 0, 16);
}

QString range(quint32 width)
{
    return width == 1 ? QString() : QString("[%1:0] ").arg(width - 1);
}

QString bitOf(const QString &name, quint32 width, quint32 bit)
{
    return width == 1 ? name : QString("%1[%2]").arg(name).arg(bit);
}

QString prefixFor(const QSocMmioPlan &plan)
{
    QString    prefix = "ahb_model_";
    const auto ports  = QSocMmioGenerator::describePorts(plan);
    while (std::any_of(ports.cbegin(), ports.cend(), [&](const auto &port) {
        return port.name.startsWith(prefix);
    })) {
        prefix += '_';
    }
    return prefix;
}

QList<quint64> addresses(const QSocMmioPlan &plan)
{
    QSet<quint64> result;
    const quint32 bytes = plan.dataWidth / 8;
    for (const auto &reg : plan.registers) {
        result.insert(reg.byteOffset);
        for (const auto &field : reg.fields) {
            for (quint32 bit = 0; bit < field.width; ++bit) {
                const quint64 address = reg.byteOffset + (field.lsb + bit) / 8;
                result.insert(address - address % bytes);
            }
        }
    }
    auto sorted = result.values();
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

void appendSignals(QStringList *lines, const QSocMmioPlan &plan, bool formal)
{
    for (const auto &port : QSocMmioGenerator::describePorts(plan)) {
        if (port.name == "clk_i" || (formal && port.name == "rst_ni"))
            continue;
        if (!formal && port.name == "s_ahb_hready") {
            lines->append("wire s_ahb_hready = s_ahb_hreadyout;");
        } else if (port.direction == "output") {
            lines->append("wire " + range(port.width) + port.name + ";");
        } else {
            lines->append(
                (formal ? "(* anyseq *) reg " : "logic ") + range(port.width) + port.name
                + (formal ? ";" : " = '0;"));
        }
    }
}

void dut(QStringList *lines, const QSocMmioPlan &plan, const QString &bus)
{
    lines->append(QString("%1 %2dut (").arg(plan.moduleName, prefixFor(plan)));
    QStringList connections;
    for (const auto &port : QSocMmioGenerator::describePorts(plan)) {
        connections.append(QString("    .%1(%2%1)").arg(port.name, bus));
    }
    lines->append(connections.join(",\n"));
    lines->append(");");
}

void check(QStringList *lines, const QString &name, const QString &condition, bool formal)
{
    lines->append(
        formal ? QString("        %1: assert(%2);").arg(name, condition)
               : QString("        if (!(%1)) `uvm_error(\"%2\", \"AHB mismatch\")")
                     .arg(condition, name));
}

void model(QStringList *lines, const QSocMmioPlan &plan, const QString &p, bool formal)
{
    const quint32 bytes = plan.dataWidth / 8;
    const auto    words = addresses(plan);
    lines->append("reg " + p + "active, " + p + "write, " + p + "bad, " + p + "wait;");
    lines->append("reg " + range(plan.addressWidth) + p + "address;");
    lines->append("reg [2:0] " + p + "size;");
    lines->append("reg " + range(plan.dataWidth) + p + "read;");
    lines->append("function " + p + "invalid;");
    lines->append("    input " + range(plan.addressWidth) + "address;");
    lines->append("    input [2:0] size;");
    lines->append(
        QString("    reg [%1:0] count, end_address;\n    reg mapped;").arg(plan.addressWidth + 7));
    lines->append("    begin\n        count = 1; count = count << size;");
    lines->append("        end_address = address; end_address = end_address + count;");
    lines->append(
        "        mapped = "
        + (plan.zeroFillBytes == 0 ? QString("0;")
                                   : QString("end_address <= %1;")
                                         .arg(literal(plan.addressWidth + 8, plan.zeroFillBytes))));
    lines->append(QString("        case (address / %1)").arg(bytes));
    for (quint64 word : words) {
        lines->append(
            QString("            %1: mapped = 1;").arg(literal(plan.addressWidth, word / bytes)));
    }
    lines->append("            default: begin end\n        endcase");
    QString invalid = QString("!mapped || count > %1 || address % count != 0 || end_address > %2")
                          .arg(bytes)
                          .arg(literal(plan.addressWidth + 8, quint64(1) << plan.addressWidth));
    if (plan.zeroFillBytes != 0) {
        invalid += " || end_address > " + literal(plan.addressWidth + 8, plan.zeroFillBytes);
    }
    lines->append(QString("        %1invalid = %2;\n    end\nendfunction").arg(p, invalid));
    qsizetype   index = 0;
    QStringList read, reset, updates, outputs;
    for (const auto &reg : plan.registers) {
        for (const auto &field : reg.fields) {
            const QString name   = p + "field_" + QString::number(index++);
            const bool    stored = qsocMmioHasStorage(field.access);
            lines->append(
                (stored ? "reg " : "wire ") + range(field.width) + name
                + (stored ? ";"
                          : " = "
                                + (field.constantValue.has_value()
                                       ? literal(field.width, *field.constantValue)
                                       : field.inputPort)
                                + ";"));
            if (stored)
                reset.append(
                    "        " + name + " <= " + literal(field.width, field.resetValue.value_or(0))
                    + ";");
            for (quint32 bit = 0; bit < field.width; ++bit) {
                const quint64 byte     = reg.byteOffset + (field.lsb + bit) / 8;
                const quint32 lane     = (byte % bytes) * 8 + (field.lsb + bit) % 8;
                const QString position = literal(plan.addressWidth + 8, byte);
                const QString value    = bitOf(name, field.width, bit);
                read.append(QString("    if (%1address / %2 == %3) %1read[%4] = %5;")
                                .arg(p)
                                .arg(bytes)
                                .arg(byte / bytes)
                                .arg(lane)
                                .arg(value));
                if (!stored)
                    continue;
                const QString data       = QString("s_ahb_hwdata[%1]").arg(lane);
                const QString writeValue = field.access == QSocMmioAccess::WriteOneClear
                                               ? value + " & ~" + data
                                               : data;
                updates.append(QString(
                                   "        if (%1active && %1write && !%1bad && s_ahb_hready"
                                   " && %2 >= %1address && (%2 - %1address) < (128'd1 << %1size))\n"
                                   "            %3 <= %4;")
                                   .arg(p, position, value, writeValue));
                if (field.access == QSocMmioAccess::WriteOneClear) {
                    updates.append(QString("        if (%1) %2 <= 1'b1;")
                                       .arg(bitOf(field.inputPort, field.width, bit), value));
                }
            }
            if (!field.outputPort.isEmpty()) {
                check(
                    &outputs,
                    p + "FIELD_" + QString::number(index - 1),
                    field.outputPort + " == " + name,
                    formal);
            }
        }
    }
    lines->append("always @* begin\n    " + p + "read = 0;");
    lines->append(read);
    lines->append("end");
    lines->append("always @(posedge clk_i) begin\n    if (!rst_ni) begin");
    for (const QString &field : {"active", "write", "bad", "wait", "address", "size"}) {
        lines->append("        " + p + field + " <= 0;");
    }
    lines->append(reset);
    lines->append("    end else begin");
    lines->append(updates);
    lines->append(QString(R"(
        if (%1wait) %1wait <= 0;
        if (s_ahb_hready) begin
            %1active <= s_ahb_hsel && s_ahb_htrans[1];
            if (s_ahb_hsel && s_ahb_htrans[1]) begin
                %1address <= s_ahb_haddr;
                %1write <= s_ahb_hwrite;
                %1size <= s_ahb_hsize;
                %1bad <= %1invalid(s_ahb_haddr, s_ahb_hsize);
                %1wait <= %1invalid(s_ahb_haddr, s_ahb_hsize);
            end
        end
    end
end
reg %1past = 0;
always @(posedge clk_i) begin
    %1past <= 1;
    if (%1past) begin
)")
                      .arg(p));
    check(
        lines,
        p + "READY",
        "s_ahb_hreadyout == (!rst_ni || !" + p + "active || !" + p + "wait)",
        formal);
    check(lines, p + "RESPONSE", "s_ahb_hresp == (rst_ni && " + p + "active && " + p + "bad)", formal);
    lines->append(
        "        if (rst_ni && " + p + "active && !" + p + "write && !" + p
        + "bad && s_ahb_hready) begin");
    check(lines, p + "READBACK", "s_ahb_hrdata == " + p + "read", formal);
    lines->append("        end");
    lines->append("        if (rst_ni) begin");
    lines->append(outputs);
    lines->append("        end\n    end\nend");
}
} // namespace

QString QSocMmioAhbVerification::formal(const QSocMmioPlan &plan)
{
    const QString p = prefixFor(plan);
    QStringList   lines
        = {QString("module %1_formal(input wire clk_i").arg(plan.moduleName),
           "`ifdef FORMAL_EXTERNAL_RESET\n    , input wire formal_reset_ni\n`endif\n);",
           "`ifdef FORMAL_EXTERNAL_RESET\nwire rst_ni = formal_reset_ni;\n`else",
           "reg [1:0] " + p + "reset = 0;",
           "(* anyseq *) reg " + p + "reset_request;",
           "always @(posedge clk_i) " + p + "reset <= {" + p + "reset[0], " + p + "reset_request};",
           "wire rst_ni = " + p + "reset[1];\n`endif"};
    appendSignals(&lines, plan, true);
    dut(&lines, plan, "");
    model(&lines, plan, p, true);
    lines.append(QString(R"(
always @(posedge clk_i) begin
    if (!rst_ni) assume(s_ahb_htrans == 0);
    if (%1past && rst_ni) begin
        if (%1active) assume(s_ahb_hready == !%1wait);
        cover(%1active && %1write && !%1bad && s_ahb_hready);
        cover(%1active && !%1write && !%1bad && s_ahb_hready);
        cover(%1active && %1bad && %1wait);
        cover(%1active && %1bad && !%1wait);
        cover(s_ahb_hsel && !s_ahb_hready && s_ahb_htrans[1]);
        cover(s_ahb_hready && s_ahb_htrans == 1);
    end
    if (%1past && $past(rst_ni && %1active)) cover(!rst_ni);
end
endmodule
)")
                     .arg(p));
    return lines.join('\n') + '\n';
}

QSocMmioUvmCollateral QSocMmioAhbVerification::uvm(const QSocMmioPlan &plan)
{
    QSocMmioUvmCollateral result;
    const QString         p             = prefixFor(plan);
    const QString         interfaceName = plan.moduleName + "_uvm_if";
    const QString         test          = plan.moduleName + "_test";
    QStringList           lines
        = {"`include \"uvm_macros.svh\"",
           "interface " + interfaceName + "(input logic clk_i);",
           "import uvm_pkg::*;"};
    appendSignals(&lines, plan, false);
    model(&lines, plan, p, false);
    lines.append("endinterface");
    result.interfaceSource = lines.join('\n') + '\n';
    QString     source     = R"(`include "uvm_macros.svh"
package @M@_uvm_pkg;
import uvm_pkg::*;
class @M@_test extends uvm_test;
    `uvm_component_utils(@M@_test)
    virtual @M@_uvm_if vif;
    function new(string name, uvm_component parent); super.new(name, parent); endfunction
    function void build_phase(uvm_phase phase);
        super.build_phase(phase);
        if (!uvm_config_db #(virtual @M@_uvm_if)::get(this, "", "vif", vif))
            `uvm_fatal("VIF", "interface missing")
    endfunction
    task access(bit wr, bit [@A@-1:0] address, int size, bit [@D@-1:0] data);
        @(negedge vif.clk_i);
        vif.s_ahb_hsel = 1; vif.s_ahb_htrans = 2;
        vif.s_ahb_haddr = address; vif.s_ahb_hwrite = wr; vif.s_ahb_hsize = 3'(size);
        @(posedge vif.clk_i);
        @(negedge vif.clk_i);
        vif.s_ahb_hsel = 0; vif.s_ahb_htrans = 0; vif.s_ahb_hwdata = data;
        do @(posedge vif.clk_i); while (!vif.s_ahb_hready);
        @(negedge vif.clk_i);
    endtask
    task run_phase(uvm_phase phase);
        phase.raise_objection(this);
        repeat (2) begin
            vif.rst_ni = 0;
            repeat (2) @(negedge vif.clk_i);
            vif.rst_ni = 1;
@INPUTS@
            repeat (2) @(negedge vif.clk_i);
@CLEAR@
@ACCESS@
        end
        repeat (3) @(negedge vif.clk_i);
        phase.drop_objection(this);
    endtask
endclass
endpackage
)";
    QStringList inputs, clear, accesses;
    for (const auto &reg : plan.registers) {
        for (const auto &field : reg.fields) {
            if (field.inputPort.isEmpty())
                continue;
            inputs.append("            vif." + field.inputPort + " = '1;");
            clear.append("            vif." + field.inputPort + " = '0;");
        }
    }
    const auto words = addresses(plan);
    for (quint64 word : words) {
        accesses.append(QString("            access(0, %1, $clog2(@B@), '0);")
                            .arg(literal(plan.addressWidth, word)));
        for (quint32 lane = 0; lane < plan.dataWidth / 8; ++lane) {
            const QString addr = literal(plan.addressWidth, word + lane);
            accesses.append(
                QString(
                    "            access(1, %1, 0, '1);\n            access(0, %1, 0, '0);\n"
                    "            access(1, %1, 0, '0);\n            access(0, %1, 0, '0);")
                    .arg(addr));
        }
    }
    quint64 hole = plan.zeroFillBytes;
    while (words.contains(hole))
        hole += plan.dataWidth / 8;
    if (hole < (quint64(1) << plan.addressWidth)) {
        accesses.append(
            QString("            access(0, %1, 0, '0);\n            access(1, %1, 0, '1);")
                .arg(literal(plan.addressWidth, hole)));
    }
    if (plan.dataWidth > 8)
        accesses.append("            access(1, 1, 1, '1);");
    source.replace("@INPUTS@", inputs.join('\n'));
    source.replace("@CLEAR@", clear.join('\n'));
    source.replace("@ACCESS@", accesses.join('\n'));
    source.replace("@M@", plan.moduleName);
    source.replace("@A@", QString::number(plan.addressWidth));
    source.replace("@D@", QString::number(plan.dataWidth));
    source.replace("@B@", QString::number(plan.dataWidth / 8));
    result.packageSource = source;
    lines
        = {"`include \"uvm_macros.svh\"",
           "module " + plan.moduleName + "_uvm_tb;",
           "import uvm_pkg::*;",
           "import " + plan.moduleName + "_uvm_pkg::*;",
           "logic clk_i = 0; always #5 clk_i = ~clk_i;",
           interfaceName + " bus(clk_i);"};
    dut(&lines, plan, "bus.");
    lines.append(QString(
                     "initial begin\n    uvm_config_db #(virtual %1)::set(null, \"*\", \"vif\", "
                     "bus);\n    run_test(\"%2\");\nend")
                     .arg(interfaceName, test));
    lines.append("initial begin #1000000; $fatal(1, \"TIMEOUT\"); end");
    lines.append(
        "final begin\n    if (uvm_report_server::get_server().get_severity_count(UVM_ERROR) != 0\n"
        "        || uvm_report_server::get_server().get_severity_count(UVM_FATAL) != 0) $fatal(1, "
        "\"UVM failed\");\nend\nendmodule");
    result.testbenchSource = lines.join('\n') + '\n';
    result.fileList
        = QString("%1.v\n%1_uvm_if.sv\n%1_uvm_pkg.sv\n%1_uvm_tb.sv\n").arg(plan.moduleName);
    return result;
}
