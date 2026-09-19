// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocmmioapb.h"
#include "qsocmmioregisters.h"

namespace {
QString literal(quint32 width, quint64 value)
{
    return QString("%1'h%2").arg(width).arg(value, 0, 16);
}
} // namespace

QList<QSocMmioPortDescription> QSocMmioApb::ports(const QSocMmioPlan &plan)
{
    return {
        {"clk_i", "input", 1},
        {"rst_ni", "input", 1},
        {"s_apb_paddr", "input", plan.addressWidth},
        {"s_apb_pselx", "input", 1},
        {"s_apb_penable", "input", 1},
        {"s_apb_pwrite", "input", 1},
        {"s_apb_pwdata", "input", plan.dataWidth},
        {"s_apb_pstrb", "input", plan.dataWidth / 8},
        {"s_apb_pprot", "input", 3},
        {"s_apb_prdata", "output", plan.dataWidth},
        {"s_apb_pready", "output", 1},
        {"s_apb_pslverr", "output", 1}};
}

QString QSocMmioApb::generate(const QSocMmioPlan &plan)
{
    QStringList lines;
    lines.append(QString("module %1 (").arg(plan.moduleName));
    QStringList declarations;
    for (const auto &port : QSocMmioGenerator::describePorts(plan)) {
        const QString range = port.width == 1 ? QString() : QString("[%1:0] ").arg(port.width - 1);
        declarations.append(QString("    %1 wire %2%3").arg(port.direction, range, port.name));
    }
    lines.append(declarations.join(",\n"));
    lines.append(");");
    lines.append(QString());
    const auto registers
        = QSocMmioRegisters::generate(plan, "s_apb_paddr", "s_apb_pwdata", "s_apb_pstrb");
    lines.append(registers.storage);
    lines.append(registers.decode);
    lines.append("wire write_fire = s_apb_pselx && s_apb_penable && s_apb_pwrite && s_apb_pready;");
    lines.append(
        "assign s_apb_pready = rst_ni"
        + (plan.clearPort.isEmpty() ? QString() : " && !" + plan.clearPort) + ";");
    lines.append("assign s_apb_pslverr = rst_ni && s_apb_pselx && s_apb_penable");
    lines.append("                      && !address_is_mapped(s_apb_paddr);");
    lines.append("assign s_apb_prdata = rst_ni && s_apb_pselx && s_apb_penable && !s_apb_pwrite");
    lines.append(
        "                     ? read_register(s_apb_paddr) : " + literal(plan.dataWidth, 0) + ";");
    lines.append(QString());
    lines.append(registers.write);
    lines.append("endmodule");
    return lines.join('\n') + '\n';
}
