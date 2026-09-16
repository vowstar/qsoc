// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocmmioapbverification.h"

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
    QString    prefix = "apb_model_";
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
            const quint64 first = reg.byteOffset + field.lsb / 8;
            const quint64 last  = reg.byteOffset + (field.lsb + field.width - 1) / 8;
            for (quint64 address = first - first % bytes; address <= last; address += bytes) {
                result.insert(address);
            }
        }
    }
    auto sorted = result.values();
    std::sort(sorted.begin(), sorted.end());
    return sorted;
}

QString expectedName(const QString &prefix, qsizetype index)
{
    return prefix + "value_" + QString::number(index);
}

void appendSignals(QStringList *lines, const QSocMmioPlan &plan, bool formal)
{
    for (const auto &port : QSocMmioGenerator::describePorts(plan)) {
        if (port.name == "clk_i" || (formal && port.name == "rst_ni")) {
            continue;
        }
        if (port.direction == "input") {
            const QString qualifier = formal ? "(* anyseq *) reg " : "logic ";
            lines->append(qualifier + range(port.width) + port.name + (formal ? ";" : " = '0;"));
        } else {
            lines->append("wire " + range(port.width) + port.name + ";");
        }
    }
}

void appendDut(
    QStringList *lines, const QSocMmioPlan &plan, const QString &instance, const QString &bus)
{
    lines->append(QString("%1 %2 (").arg(plan.moduleName, instance));
    QStringList connections;
    for (const auto &port : QSocMmioGenerator::describePorts(plan)) {
        connections.append(QString("    .%1(%2%1)").arg(port.name, bus));
    }
    lines->append(connections.join(",\n"));
    lines->append(");");
}

void appendModel(QStringList *lines, const QSocMmioPlan &plan, const QString &prefix)
{
    qsizetype index = 0;
    for (const auto &reg : plan.registers) {
        for (const auto &field : reg.fields) {
            const QString name = expectedName(prefix, index++);
            if (qsocMmioHasStorage(field.access)) {
                lines->append("reg " + range(field.width) + name + ";");
            } else {
                const QString value = field.constantValue.has_value()
                                          ? literal(field.width, *field.constantValue)
                                          : field.inputPort;
                lines->append("wire " + range(field.width) + name + " = " + value + ";");
            }
        }
    }
    lines->append("function " + prefix + "mapped;");
    lines->append(QString("    input [%1:0] address;").arg(plan.addressWidth - 1));
    lines->append("    begin");
    const QString fallback = plan.zeroFillBytes == 0
                                 ? "1'b0"
                                 : (plan.zeroFillBytes == (quint64(1) << plan.addressWidth)
                                        ? "1'b1"
                                        : "address < "
                                              + literal(plan.addressWidth, plan.zeroFillBytes));
    lines->append("        " + prefix + "mapped = " + fallback + ";");
    for (quint64 address : addresses(plan)) {
        lines->append(QString("        if (address == %1) %2mapped = 1'b1;")
                          .arg(literal(plan.addressWidth, address), prefix));
    }
    lines->append("    end");
    lines->append("endfunction");
    lines->append(QString("function [%1:0] %2read;").arg(plan.dataWidth - 1).arg(prefix));
    lines->append(QString("    input [%1:0] address;").arg(plan.addressWidth - 1));
    lines->append("    begin");
    lines->append("        " + prefix + "read = '0;");
    index               = 0;
    const quint32 bytes = plan.dataWidth / 8;
    for (const auto &reg : plan.registers) {
        for (const auto &field : reg.fields) {
            for (quint32 bit = 0; bit < field.width; ++bit) {
                const quint64 address = reg.byteOffset + (field.lsb + bit) / 8;
                const quint32 busBit  = quint32(address % bytes) * 8 + (field.lsb + bit) % 8;
                lines->append(QString("        if (address == %1) %2read[%3] = %4;")
                                  .arg(literal(plan.addressWidth, address - address % bytes), prefix)
                                  .arg(busBit)
                                  .arg(bitOf(expectedName(prefix, index), field.width, bit)));
            }
            ++index;
        }
    }
    lines->append("    end");
    lines->append("endfunction");
    lines->append("always @(posedge clk_i or negedge rst_ni) begin");
    lines->append("    if (!rst_ni) begin");
    index = 0;
    for (const auto &reg : plan.registers) {
        for (const auto &field : reg.fields) {
            if (qsocMmioHasStorage(field.access)) {
                lines->append(
                    QString("        %1 <= %2;")
                        .arg(expectedName(prefix, index), literal(field.width, *field.resetValue)));
            }
            ++index;
        }
    }
    lines->append("    end else begin");
    index = 0;
    for (const auto &reg : plan.registers) {
        for (const auto &field : reg.fields) {
            if (qsocMmioHasStorage(field.access)) {
                for (quint32 bit = 0; bit < field.width; ++bit) {
                    const quint64 address = reg.byteOffset + (field.lsb + bit) / 8;
                    const quint32 busBit  = quint32(address % bytes) * 8 + (field.lsb + bit) % 8;
                    const QString target  = bitOf(expectedName(prefix, index), field.width, bit);
                    QString       value   = QString("s_apb_pwdata[%1]").arg(busBit);
                    if (field.access == QSocMmioAccess::WriteOneClear) {
                        value = target + " & ~" + value;
                    }
                    lines->append(QString(
                                      "        if (s_apb_pselx && s_apb_penable && s_apb_pwrite && "
                                      "s_apb_pready\n"
                                      "            && s_apb_paddr == %1 && %2)")
                                      .arg(
                                          literal(plan.addressWidth, address - address % bytes),
                                          bitOf("s_apb_pstrb", bytes, busBit / 8)));
                    lines->append(QString("            %1 <= %2;").arg(target, value));
                }
                if (field.access == QSocMmioAccess::WriteOneClear) {
                    lines->append(QString("        if (%1) %2 <= 1'b1;")
                                      .arg(field.inputPort, expectedName(prefix, index)));
                }
            }
            ++index;
        }
    }
    lines->append("    end");
    lines->append("end");
}

void appendChecks(QStringList *lines, const QSocMmioPlan &plan, const QString &prefix, bool formal)
{
    const auto check = [&](const QString &condition, const QString &name) {
        lines->append(
            formal
                ? QString("        %1: assert(%2);").arg(prefix + name, condition)
                : QString("        if (!(%1)) `uvm_error(\"%2\", \"register contract mismatch\")")
                      .arg(condition, name));
    };
    lines->append("always @(posedge clk_i) begin");
    lines->append("    if (rst_ni) begin");
    check("s_apb_pready", "READY");
    check(
        QString("!(s_apb_pselx && s_apb_penable) || (s_apb_pslverr == !%1mapped(s_apb_paddr))")
            .arg(prefix),
        "RESPONSE");
    check(
        QString(
            "!(s_apb_pselx && s_apb_penable && !s_apb_pwrite) || (s_apb_prdata == "
            "%1read(s_apb_paddr))")
            .arg(prefix),
        "READBACK");
    qsizetype index = 0;
    for (const auto &reg : plan.registers) {
        for (const auto &field : reg.fields) {
            if (!field.outputPort.isEmpty()) {
                check(
                    field.outputPort + " == " + expectedName(prefix, index),
                    "FIELD_" + QString::number(index));
            }
            ++index;
        }
    }
    lines->append("    end");
    lines->append("end");
}
} // namespace

QString QSocMmioApbVerification::formal(const QSocMmioPlan &plan)
{
    const QString prefix = prefixFor(plan);
    QStringList   lines;
    lines.append(QString("module %1_formal (input wire clk_i").arg(plan.moduleName));
    lines.append("`ifdef FORMAL_EXTERNAL_RESET\n    , input wire formal_reset_ni\n`endif\n);");
    lines.append("`ifdef FORMAL_EXTERNAL_RESET\nwire rst_ni = formal_reset_ni;\n`else");
    lines.append("reg [1:0] " + prefix + "reset = 0;");
    lines.append("(* anyseq *) reg " + prefix + "reset_request;");
    lines.append(
        "always @(posedge clk_i) " + prefix + "reset <= {" + prefix + "reset[0], " + prefix
        + "reset_request};");
    lines.append("wire rst_ni = " + prefix + "reset[1];\n`endif");
    appendSignals(&lines, plan, true);
    appendDut(&lines, plan, prefix + "dut", "");
    appendModel(&lines, plan, prefix);
    appendChecks(&lines, plan, prefix, true);
    lines.append("reg " + prefix + "past = 0;");
    lines.append("always @(posedge clk_i) begin");
    lines.append("    " + prefix + "past <= 1;");
    lines.append("    if (rst_ni) begin");
    lines.append("        assume(!s_apb_penable || s_apb_pselx);");
    lines.append("        assume(s_apb_pwrite || s_apb_pstrb == 0);");
    lines.append("        if (!" + prefix + "past || !$past(rst_ni)) assume(!s_apb_penable);");
    lines.append("        else begin");
    lines.append("            if ($past(s_apb_pselx && !s_apb_penable)) begin");
    lines.append("                assume(s_apb_pselx && s_apb_penable);");
    for (const QString &signal :
         {"s_apb_paddr", "s_apb_pwrite", "s_apb_pwdata", "s_apb_pstrb", "s_apb_pprot"}) {
        lines.append(QString("                assume(%1 == $past(%1));").arg(signal));
    }
    lines.append("            end else assume(!s_apb_penable);");
    lines.append("        end");
    lines.append("        cover(s_apb_pselx && s_apb_penable && s_apb_pwrite && s_apb_pstrb != 0);");
    lines.append("        cover(s_apb_pselx && s_apb_penable && !s_apb_pwrite);");
    lines.append("    end\nend\nendmodule");
    return lines.join('\n') + '\n';
}

QSocMmioUvmCollateral QSocMmioApbVerification::uvm(const QSocMmioPlan &plan)
{
    QSocMmioUvmCollateral result;
    const QString         prefix        = prefixFor(plan);
    const QString         interfaceName = plan.moduleName + "_uvm_if";
    const QString         testName      = plan.moduleName + "_test";
    QStringList           lines;
    lines.append("`include \"uvm_macros.svh\"");
    lines.append(QString("interface %1(input logic clk_i);").arg(interfaceName));
    lines.append("import uvm_pkg::*;");
    appendSignals(&lines, plan, false);
    appendModel(&lines, plan, prefix);
    appendChecks(&lines, plan, prefix, false);
    lines.append("endinterface");
    result.interfaceSource = lines.join('\n') + '\n';
    lines.clear();
    lines.append("`include \"uvm_macros.svh\"");
    lines.append(QString("package %1_uvm_pkg;\nimport uvm_pkg::*;").arg(plan.moduleName));
    lines.append(QString("class %1 extends uvm_test;\n    `uvm_component_utils(%1)").arg(testName));
    lines.append(QString("    virtual %1 vif;").arg(interfaceName));
    lines.append(
        "    function new(string name, uvm_component parent); super.new(name, parent); "
        "endfunction");
    lines.append(
        "    function void build_phase(uvm_phase phase);\n        super.build_phase(phase);");
    lines.append(QString(
                     "        if (!uvm_config_db #(virtual %1)::get(this, \"\", \"vif\", vif))\n   "
                     "         `uvm_fatal(\"VIF\", \"interface missing\")\n    endfunction")
                     .arg(interfaceName));
    lines.append(
        QString("    task access(bit wr, bit [%1:0] address, bit [%2:0] data, bit [%3:0] mask);")
            .arg(plan.addressWidth - 1)
            .arg(plan.dataWidth - 1)
            .arg(plan.dataWidth / 8 - 1));
    lines.append(
        "        @(negedge vif.clk_i);\n        vif.s_apb_pselx = 1; vif.s_apb_penable = 0;\n      "
        "  vif.s_apb_paddr = address; vif.s_apb_pwrite = wr;\n        vif.s_apb_pwdata = data; "
        "vif.s_apb_pstrb = wr ? mask : '0;\n        @(negedge vif.clk_i); vif.s_apb_penable = 1;\n "
        "       @(posedge vif.clk_i);\n        @(negedge vif.clk_i); vif.s_apb_pselx = 0; "
        "vif.s_apb_penable = 0;\n    endtask");
    lines.append(
        "    task run_phase(uvm_phase phase);\n        phase.raise_objection(this);\n        "
        "repeat (2) @(negedge vif.clk_i);\n        vif.rst_ni = 1;");
    for (const auto &reg : plan.registers) {
        for (const auto &field : reg.fields) {
            if (!field.inputPort.isEmpty()) {
                lines.append(QString("        vif.%1 = '1;").arg(field.inputPort));
            }
        }
    }
    lines.append("        repeat (2) @(negedge vif.clk_i);");
    for (quint64 address : addresses(plan)) {
        lines.append(
            QString("        access(0, %1, '0, '0);").arg(literal(plan.addressWidth, address)));
    }
    for (const auto &reg : plan.registers) {
        for (const auto &field : reg.fields) {
            if (!field.inputPort.isEmpty()) {
                lines.append(QString("        vif.%1 = '0;").arg(field.inputPort));
            }
        }
    }
    for (quint64 address : addresses(plan)) {
        const QString addr = literal(plan.addressWidth, address);
        lines.append(QString("        access(0, %1, '0, '0);").arg(addr));
        for (quint32 lane = 0; lane < plan.dataWidth / 8; ++lane) {
            const QString mask = literal(plan.dataWidth / 8, quint64(1) << lane);
            lines.append(QString(
                             "        access(1, %1, '1, %2);\n        access(0, %1, '0, '0);\n     "
                             "   access(1, %1, '0, %2);\n        access(0, %1, '0, '0);")
                             .arg(addr, mask));
        }
        lines.append(
            QString("        access(1, %1, '1, '0);\n        access(0, %1, '0, '0);").arg(addr));
    }
    const auto mapped   = addresses(plan);
    quint64    unmapped = plan.zeroFillBytes;
    for (quint64 address : mapped) {
        if (address == unmapped) {
            unmapped += plan.dataWidth / 8;
        }
    }
    if (unmapped < (quint64(1) << plan.addressWidth)) {
        const QString addr = literal(plan.addressWidth, unmapped);
        lines.append(
            QString("        access(0, %1, '0, '0);\n        access(1, %1, '1, '1);").arg(addr));
    }
    if (plan.dataWidth > 8 && !mapped.isEmpty()) {
        const QString addr = literal(plan.addressWidth, mapped.first() + 1);
        lines.append(
            QString("        access(0, %1, '0, '0);\n        access(1, %1, '1, '1);").arg(addr));
    }
    lines.append(
        "        @(negedge vif.clk_i); vif.rst_ni = 0;\n        repeat (2) @(negedge vif.clk_i); "
        "vif.rst_ni = 1;");
    for (quint64 address : addresses(plan)) {
        lines.append(
            QString("        access(0, %1, '0, '0);").arg(literal(plan.addressWidth, address)));
    }
    lines.append("        phase.drop_objection(this);\n    endtask\nendclass\nendpackage");
    result.packageSource = lines.join('\n') + '\n';
    lines.clear();
    lines.append(QString(
                     "module %1_uvm_tb;\n    timeunit 1ns; timeprecision 1ps;\n    import "
                     "uvm_pkg::*;\n    import %1_uvm_pkg::*;")
                     .arg(plan.moduleName));
    lines.append("reg clk_i = 0;\nalways #5ns clk_i = !clk_i;");
    lines.append(interfaceName + " bus(clk_i);");
    appendDut(&lines, plan, "dut", "bus.");
    lines.append(QString(
                     "initial begin\n    uvm_config_db #(virtual %1)::set(null, \"*\", \"vif\", "
                     "bus);\n    run_test(\"%2\");\nend")
                     .arg(interfaceName, testName));
    lines.append("initial begin #1ms; $fatal(1, \"QSOC_UVM_TIMEOUT\"); end");
    lines.append(
        "final begin\n    if (uvm_report_server::get_server().get_severity_count(UVM_ERROR) != 0\n "
        "       || uvm_report_server::get_server().get_severity_count(UVM_FATAL) != 0)\n        "
        "$fatal(1, \"QSOC_UVM_FAILED\");\nend\nendmodule");
    result.testbenchSource = lines.join('\n') + '\n';
    result.fileList
        = QString("%1.v\n%1_uvm_if.sv\n%1_uvm_pkg.sv\n%1_uvm_tb.sv\n").arg(plan.moduleName);
    return result;
}
