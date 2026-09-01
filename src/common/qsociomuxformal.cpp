// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsociomuxformal.h"

#include <QRegularExpression>

#include "common/qsociomuxgenerator.h"

#include <utility>
#include <QStringList>

namespace {

quint32 selectorWidth(quint32 hsSlots)
{
    if (hsSlots <= 2) {
        return 1;
    }
    if (hsSlots <= 4) {
        return 2;
    }
    return 3;
}

const QSocIomuxEndpointPlan &routeRole(const QSocIomuxRoutePlan &route, QSocIomuxRole role)
{
    switch (role) {
    case QSocIomuxRole::InputValue:
        return route.inputValue;
    case QSocIomuxRole::InputEnable:
        return route.inputEnable;
    case QSocIomuxRole::OutputValue:
        return route.outputValue;
    case QSocIomuxRole::OutputEnable:
        break;
    }
    return route.outputEnable;
}

const QSocIomuxRoutePlan *findRoute(const QSocIomuxPlan &plan, quint32 pin, quint32 slot)
{
    for (const QSocIomuxRoutePlan &route : plan.routes) {
        if (route.pin == pin && route.slot == slot) {
            return &route;
        }
    }
    return nullptr;
}

QString expectedRoleExpression(
    const QSocIomuxPlan &plan, quint32 pin, quint32 slot, QSocIomuxRole role)
{
    const QSocIomuxRoutePlan *route = findRoute(plan, pin, slot);
    if (route == nullptr) {
        return QStringLiteral("1'b0");
    }
    const QSocIomuxEndpointPlan &endpoint = routeRole(*route, role);
    if (!endpoint.link.isEmpty()) {
        const QString name = QSocIomuxGenerator::endpointPortName(pin, slot, role);
        return endpoint.invert ? name + " ^ 1'b1" : name;
    }
    if (endpoint.constant.has_value() && *endpoint.constant == 1) {
        return QStringLiteral("1'b1");
    }
    return QStringLiteral("1'b0");
}

/**
 * @brief What the core must drive on one pad control for one selected slot.
 *
 * `slotIe`, `slotOv` and `slotOe` are the bundle the slot carries. With no
 * option on this is that bundle; the gpio source fields and the inversion
 * bank are layered on exactly as the core layers them.
 */
QString expectedPadExpression(
    const QSocIomuxPlan &plan,
    quint32              pin,
    QSocIomuxRole        role,
    const QString       &slotIe,
    const QString       &slotOv,
    const QString       &slotOe)
{
    QString expression;
    QString roleName;
    switch (role) {
    case QSocIomuxRole::InputEnable:
        roleName   = QStringLiteral("input_enable");
        expression = plan.option.gpio
                         ? QString("pin_%1_input_enable_src_i ? pin_%1_input_enable_i : (%2)")
                               .arg(pin)
                               .arg(slotIe)
                         : slotIe;
        break;
    case QSocIomuxRole::OutputValue:
        roleName   = QStringLiteral("output_value");
        expression = plan.option.gpio
                         ? QString(
                               "pin_%1_output_value_src_i == 2'd1 ? pin_%1_output_value_i"
                               " : pin_%1_output_value_src_i == 2'd2 ? (%2)"
                               " : pin_%1_output_value_src_i == 2'd3 ? (%3) : (%4)")
                               .arg(pin)
                               .arg(slotIe, slotOe, slotOv)
                         : slotOv;
        break;
    case QSocIomuxRole::InputValue:
        return QString();
    case QSocIomuxRole::OutputEnable:
        roleName   = QStringLiteral("output_enable");
        expression = plan.option.gpio
                         ? QString(
                               "pin_%1_output_enable_src_i == 2'd1 ? pin_%1_output_enable_i"
                               " : pin_%1_output_enable_src_i == 2'd2 ? (%2)"
                               " : pin_%1_output_enable_src_i == 2'd3 ? 1'b0 : (%3)")
                               .arg(pin)
                               .arg(slotOv, slotOe)
                         : slotOe;
        break;
    }
    if (plan.option.invert) {
        expression = QString("(%1) ^ pin_%2_%3_inv_i").arg(expression).arg(pin).arg(roleName);
    }
    return expression;
}

/**
 * @brief The pad selector code assertions for one pin under one selector value.
 *
 * `route` is the route the selector picks, or null for an unrouted or invalid
 * code, where every constant is zero.
 */
void appendPadCodeAssertions(
    QStringList *lines, const QSocIomuxPlan &plan, quint32 pin, const QSocIomuxRoutePlan *route)
{
    const QSocPadEncoding encoding = QSocIomuxGenerator::padEncoding(plan.integration.padCell);
    const auto            expect =
        [&](const char *name, const char *reg, quint32 width, const char *src, int constant) {
            const QString value = QString("%1'd%2").arg(width).arg(constant);
            lines->append(
                QString("        assert (pad_%1_w[%2:%3] == (%4));")
                    .arg(name)
                    .arg((pin + 1) * width - 1)
                    .arg(pin * width)
                    .arg(
                        plan.option.padControl
                            ? QString("pin_%1_%2_i ? pin_%1_%3_i : %4").arg(pin).arg(src, reg, value)
                            : value));
        };
    if (encoding.hasPull()) {
        expect(
            "pull_mode",
            "pull_mode",
            encoding.modeWidth,
            "pull_src",
            route ? encoding.routeMode(*route) : 0);
        if (encoding.upSelWidth > 0) {
            expect(
                "up_sel",
                "up_sel",
                encoding.upSelWidth,
                "pull_src",
                route ? encoding.routeUpSel(*route) : 0);
        }
        if (encoding.downSelWidth > 0) {
            expect(
                "down_sel",
                "down_sel",
                encoding.downSelWidth,
                "pull_src",
                route ? encoding.routeDownSel(*route) : 0);
        }
    }
    if (encoding.hasDrive()) {
        expect(
            "drive_select",
            "drive",
            encoding.driveWidth,
            "drive_src",
            route ? encoding.routeDriveCode(*route) : 0);
    }
}

QString buildSystemVerilog(const QSocIomuxPlan &plan)
{
    const quint32 width = selectorWidth(plan.hsSlots);
    const quint64 dense = quint64(plan.hsSlots) * plan.pinCount;

    QStringList txPorts;
    QStringList rxPorts;
    for (const QSocIomuxRoutePlan &route : plan.routes) {
        for (const QSocIomuxRole role :
             {QSocIomuxRole::InputValue,
              QSocIomuxRole::InputEnable,
              QSocIomuxRole::OutputValue,
              QSocIomuxRole::OutputEnable}) {
            const QSocIomuxEndpointPlan &endpoint = routeRole(route, role);
            if (endpoint.link.isEmpty()) {
                continue;
            }
            const QString name = QSocIomuxGenerator::endpointPortName(route.pin, route.slot, role);
            if (role == QSocIomuxRole::InputValue) {
                rxPorts.append(name);
            } else {
                txPorts.append(name);
            }
        }
    }

    QStringList lines;
    lines.append("// Generated by QSoC. Do not edit.");
    lines.append(QString("module %1_hs_formal (").arg(plan.moduleName));
    QStringList declarations;
    declarations.append(QString("    input logic [%1:0] pad_input_value_i").arg(plan.pinCount - 1));
    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        declarations.append(
            QString("    input logic [%1:0] pin_%2_select_i").arg(width - 1).arg(pin));
        /* Every option register is a free input, so the proof covers each
         * source and inversion setting rather than the reset one alone. */
        for (const QSocIomuxCorePort &port : QSocIomuxGenerator::corePinOptionPorts(plan, pin)) {
            declarations.append(
                port.width == 1
                    ? QString("    input logic %1_i").arg(port.name)
                    : QString("    input logic [%1:0] %2_i").arg(port.width - 1).arg(port.name));
        }
    }
    for (const QString &name : txPorts) {
        declarations.append(QString("    input logic %1").arg(name));
    }
    for (qsizetype index = 0; index < declarations.size(); ++index) {
        const QString suffix = index + 1 == declarations.size() ? QString() : QString(",");
        lines.append(declarations.at(index) + suffix);
    }
    lines.append(");");
    lines.append(QString());
    lines.append(QString("logic [%1:0] tx_input_enable_w;").arg(dense - 1));
    lines.append(QString("logic [%1:0] tx_output_value_w;").arg(dense - 1));
    lines.append(QString("logic [%1:0] tx_output_enable_w;").arg(dense - 1));
    lines.append(QString("logic [%1:0] rx_input_value_w;").arg(dense - 1));
    lines.append(QString("logic [%1:0] pad_input_enable_w;").arg(plan.pinCount - 1));
    lines.append(QString("logic [%1:0] pad_output_value_w;").arg(plan.pinCount - 1));
    lines.append(QString("logic [%1:0] pad_output_enable_w;").arg(plan.pinCount - 1));
    for (const QSocIomuxCorePort &port : QSocIomuxGenerator::corePadSelectPorts(plan)) {
        lines.append(QString("logic [%1:0] %2_w;").arg(port.width - 1).arg(port.name));
    }
    for (const QString &name : rxPorts) {
        lines.append(QString("logic %1;").arg(name));
    }
    lines.append(QString());

    QStringList connConnections
        = {"    .tx_input_enable_o(tx_input_enable_w)",
           "    .tx_output_value_o(tx_output_value_w)",
           "    .tx_output_enable_o(tx_output_enable_w)",
           "    .rx_input_value_i(rx_input_value_w)"};
    for (const QSocIomuxRoutePlan &route : plan.routes) {
        for (const QSocIomuxRole role :
             {QSocIomuxRole::InputValue,
              QSocIomuxRole::InputEnable,
              QSocIomuxRole::OutputValue,
              QSocIomuxRole::OutputEnable}) {
            if (routeRole(route, role).link.isEmpty()) {
                continue;
            }
            const QString name = QSocIomuxGenerator::endpointPortName(route.pin, route.slot, role);
            connConnections.append(QString("    .%1(%1)").arg(name));
        }
    }
    lines.append(QString("%1_conn u_conn (").arg(plan.moduleName));
    for (qsizetype index = 0; index < connConnections.size(); ++index) {
        const QString suffix = index + 1 == connConnections.size() ? QString() : QString(",");
        lines.append(connConnections.at(index) + suffix);
    }
    lines.append(");");
    lines.append(QString());

    QStringList coreConnections
        = {"    .pad_input_value_i(pad_input_value_i)",
           "    .pad_input_enable_o(pad_input_enable_w)",
           "    .pad_output_value_o(pad_output_value_w)",
           "    .pad_output_enable_o(pad_output_enable_w)",
           "    .tx_input_enable_i(tx_input_enable_w)",
           "    .tx_output_value_i(tx_output_value_w)",
           "    .tx_output_enable_i(tx_output_enable_w)",
           "    .rx_input_value_o(rx_input_value_w)"};
    for (const QSocIomuxCorePort &port : QSocIomuxGenerator::corePadSelectPorts(plan)) {
        coreConnections.append(QString("    .%1_o(%1_w)").arg(port.name));
    }
    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        coreConnections.append(QString("    .pin_%1_select_i(pin_%1_select_i)").arg(pin));
        for (const QSocIomuxCorePort &port : QSocIomuxGenerator::corePinOptionPorts(plan, pin)) {
            coreConnections.append(QString("    .%1_i(%1_i)").arg(port.name));
        }
    }
    lines.append(QString("%1_core u_core (").arg(plan.moduleName));
    for (qsizetype index = 0; index < coreConnections.size(); ++index) {
        const QString suffix = index + 1 == coreConnections.size() ? QString() : QString(",");
        lines.append(coreConnections.at(index) + suffix);
    }
    lines.append(");");
    lines.append(QString());

    lines.append("always_comb begin");
    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        for (quint32 slot = 0; slot < plan.hsSlots; ++slot) {
            const QString slotIe
                = expectedRoleExpression(plan, pin, slot, QSocIomuxRole::InputEnable);
            const QString slotOv
                = expectedRoleExpression(plan, pin, slot, QSocIomuxRole::OutputValue);
            const QString slotOe
                = expectedRoleExpression(plan, pin, slot, QSocIomuxRole::OutputEnable);
            lines.append(
                QString("    if (pin_%1_select_i == %2'd%3) begin").arg(pin).arg(width).arg(slot));
            lines.append(QString("        assert (pad_input_enable_w[%1] == (%2));")
                             .arg(pin)
                             .arg(expectedPadExpression(
                                 plan, pin, QSocIomuxRole::InputEnable, slotIe, slotOv, slotOe)));
            lines.append(QString("        assert (pad_output_value_w[%1] == (%2));")
                             .arg(pin)
                             .arg(expectedPadExpression(
                                 plan, pin, QSocIomuxRole::OutputValue, slotIe, slotOv, slotOe)));
            lines.append(QString("        assert (pad_output_enable_w[%1] == (%2));")
                             .arg(pin)
                             .arg(expectedPadExpression(
                                 plan, pin, QSocIomuxRole::OutputEnable, slotIe, slotOv, slotOe)));
            appendPadCodeAssertions(&lines, plan, pin, findRoute(plan, pin, slot));
            lines.append("    end");
        }
        if (plan.hsSlots < (quint32(1) << width)) {
            lines.append(QString("    if (pin_%1_select_i >= %2'd%3) begin")
                             .arg(pin)
                             .arg(width)
                             .arg(plan.hsSlots));
            const bool plain = !plan.option.gpio && !plan.option.invert;
            for (const auto &[name, role] :
                 {std::pair{"input_enable", QSocIomuxRole::InputEnable},
                  std::pair{"output_value", QSocIomuxRole::OutputValue},
                  std::pair{"output_enable", QSocIomuxRole::OutputEnable}}) {
                lines.append(
                    plain
                        ? QString("        assert (pad_%1_w[%2] == 1'b0);").arg(QString(name)).arg(pin)
                        : QString("        assert (pad_%1_w[%2] == (%3));")
                              .arg(QString(name))
                              .arg(pin)
                              .arg(expectedPadExpression(plan, pin, role, "1'b0", "1'b0", "1'b0")));
            }
            appendPadCodeAssertions(&lines, plan, pin, nullptr);
            lines.append("    end");
        }
    }
    for (const QSocIomuxRoutePlan &route : plan.routes) {
        const QSocIomuxEndpointPlan &endpoint = route.inputValue;
        if (endpoint.link.isEmpty()) {
            continue;
        }
        const QString name
            = QSocIomuxGenerator::endpointPortName(route.pin, route.slot, QSocIomuxRole::InputValue);
        QString expression = QString("pad_input_value_i[%1]").arg(route.pin);
        if (plan.option.rxOverride) {
            expression = QString("pin_%1_rx_src_s%2_i ? pin_%1_rx_value_s%2_i : %3")
                             .arg(route.pin)
                             .arg(route.slot)
                             .arg(expression);
        }
        if (plan.option.invert) {
            expression = QString("(%1) ^ pin_%2_rx_inv_s%3_i")
                             .arg(expression)
                             .arg(route.pin)
                             .arg(route.slot);
        }
        if (endpoint.invert) {
            expression += QStringLiteral(" ^ 1'b1");
        }
        lines.append(QString("    assert (%1 == (%2));").arg(name, expression));
    }
    for (quint32 slot = 0; slot < plan.hsSlots; ++slot) {
        lines.append(QString("    cover (pin_0_select_i == %1'd%2);").arg(width).arg(slot));
        lines.append(QString("    cover (pin_%1_select_i == %2'd%3);")
                         .arg(plan.pinCount - 1)
                         .arg(width)
                         .arg(slot));
    }
    lines.append("end");
    lines.append(QString());
    lines.append("endmodule");
    lines.append(QString());
    return lines.join('\n');
}

QString buildSby(const QSocIomuxPlan &plan)
{
    return QStringLiteral(
               "[tasks]\n"
               "prove\n"
               "bmc\n"
               "cover\n"
               "\n"
               "[options]\n"
               "prove: mode prove\n"
               "prove: depth 4\n"
               "prove: aigsmt none\n"
               "bmc: mode bmc\n"
               "bmc: depth 4\n"
               "cover: mode cover\n"
               "cover: depth 4\n"
               "\n"
               "[engines]\n"
               "prove: abc pdr\n"
               "bmc: smtbmc z3\n"
               "cover: smtbmc z3\n"
               "\n"
               "[script]\n"
               "read -formal -sv %1.v %1_conn.v %1_hs_formal.sv\n"
               "prep -top %1_hs_formal\n"
               "\n"
               "[files]\n"
               "%1.v\n"
               "%1_conn.v\n"
               "%1_hs_formal.sv\n")
        .arg(plan.moduleName);
}

} // namespace

QSocIomuxFormalCollateral QSocIomuxFormal::generate(const QSocIomuxPlan &plan)
{
    QSocIomuxFormalCollateral collateral;
    if (plan.pinCount == 0 || plan.hsSlots == 0) {
        return collateral;
    }
    collateral.systemVerilog = buildSystemVerilog(plan);
    collateral.sby           = buildSby(plan);
    return collateral;
}

QSocIomuxFormalCollateral QSocIomuxFormal::generatePad(const QSocIomuxPlan &plan)
{
    const QSocPadCellPlan &cell = plan.integration.padCell;
    if (plan.pinCount == 0 || !cell.declared() || cell.constraint.isEmpty()) {
        return {};
    }
    const QString name = plan.moduleName;
    QStringList   sv;
    sv.append("// Generated by QSoC. Do not edit.");
    sv.append("`default_nettype none");
    sv.append(QString());

    /* A stub of the cell. Directions come from the module library, so a harness
     * that elaborates is one more check that the declaration named real ports. */
    QStringList stubPorts;
    for (auto it = cell.cellPorts.cbegin(); it != cell.cellPorts.cend(); ++it) {
        const QString dir = it.value() == "out"     ? QStringLiteral("output")
                            : it.value() == "inout" ? QStringLiteral("inout")
                                                    : QStringLiteral("input");
        stubPorts.append(QString("    %1 wire %2").arg(dir, it.key()));
    }
    sv.append(QString("module %1 (").arg(cell.cell));
    sv.append(stubPorts.join(",\n"));
    sv.append(");");
    /* An empty body reads as a blackbox and the flow refuses it. Every output
     * takes a free value instead, which is the honest model of a receiver the
     * proof knows nothing about. */
    for (auto it = cell.cellPorts.cbegin(); it != cell.cellPorts.cend(); ++it) {
        if (it.value() == "out") {
            sv.append(QString("(* anyseq *) reg %1_any;").arg(it.key()));
            sv.append(QString("assign %1 = %1_any;").arg(it.key()));
        }
    }
    sv.append("endmodule");
    sv.append(QString());

    /* Free inputs of the pad module become free inputs of the harness. */
    const QString range = QString("[%1:0]").arg(plan.pinCount - 1);
    QStringList   harnessPorts;
    QStringList   padConnections = {"    .pad_io(pad_io)"};
    sv.append(QString("module %1_pad_formal (").arg(name));
    if (!cell.portInputEnable.isEmpty()) {
        harnessPorts.append(QString("    input wire %1 pad_input_enable_i").arg(range));
        padConnections.append("    .pad_input_enable_i(pad_input_enable_i)");
    }
    if (!cell.portOutputValue.isEmpty()) {
        harnessPorts.append(QString("    input wire %1 pad_output_value_i").arg(range));
        padConnections.append("    .pad_output_value_i(pad_output_value_i)");
    }
    if (!cell.portOutputEnable.isEmpty()) {
        harnessPorts.append(QString("    input wire %1 pad_output_enable_i").arg(range));
        padConnections.append("    .pad_output_enable_i(pad_output_enable_i)");
    }
    for (const QSocIomuxCorePort &port : QSocIomuxGenerator::corePadSelectPorts(plan)) {
        harnessPorts.append(
            QString("    input wire [%1:0] %2_i").arg(port.width - 1).arg(port.name));
        padConnections.append(QString("    .%1_i(%1_i)").arg(port.name));
    }
    sv.append(harnessPorts.join(",\n"));
    sv.append(");");
    sv.append(QString("wire %1 pad_io;").arg(range));
    if (!cell.portInputValue.isEmpty()) {
        sv.append(QString("wire %1 pad_input_value_o;").arg(range));
        padConnections.append("    .pad_input_value_o(pad_input_value_o)");
    }
    sv.append(QString("%1_pad u_pad (").arg(name));
    sv.append(padConnections.join(",\n"));
    sv.append(");");
    sv.append(QString());

    sv.append(QString());
    sv.append("endmodule");
    sv.append("`default_nettype wire");
    sv.append(QString());

    const QString sby = QStringLiteral(
                            "[tasks]\n"
                            "prove\n"
                            "bmc\n"
                            "\n"
                            "[options]\n"
                            "multiclock on\n"
                            "prove: mode prove\n"
                            "prove: depth 4\n"
                            "bmc: mode bmc\n"
                            "bmc: depth 4\n"
                            "\n"
                            "[engines]\n"
                            "prove: smtbmc z3\n"
                            "bmc: smtbmc z3\n"
                            "\n"
                            "[script]\n"
                            "read -formal -sv %1_pad.v %1_pad_formal.sv\n"
                            "prep -top %1_pad_formal -flatten\n"
                            "\n"
                            "[files]\n"
                            "%1_pad.v\n"
                            "%1_pad_formal.sv\n")
                            .arg(name);
    return {sv.join('\n'), sby};
}
