// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsociomuxformal.h"

#include <QRegularExpression>

#include "common/qsociomuxgenerator.h"

#include <algorithm>
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
    const QSocPadSafePlan &safe = plan.padClass(pin).safe;
    if (plan.padModel.safe) {
        const quint8 value = role == QSocIomuxRole::InputEnable   ? safe.inputEnable
                             : role == QSocIomuxRole::OutputValue ? safe.outputValue
                                                                  : safe.outputEnable;
        expression         = QString("pad_force_i ? 1'b%1 : (%2)").arg(value).arg(expression);
    }
    return expression;
}

/**
 * @brief Every select net a plan declares, in route order.
 */
QStringList selectPortNames(const QSocIomuxPlan &plan)
{
    QStringList names;
    for (const QSocIomuxRoutePlan &route : plan.routes) {
        if (route.pullSelect.linked()) {
            names.append(
                QSocIomuxGenerator::selectPortName(route.pin, route.slot, QStringLiteral("pull")));
        }
        for (auto it = route.control.cbegin(); it != route.control.cend(); ++it) {
            if (it.value().select.linked()) {
                names.append(QSocIomuxGenerator::selectPortName(route.pin, route.slot, it.key()));
            }
        }
    }
    return names;
}

/**
 * @brief The expected code of one slot: a constant, or on and off under its net.
 */
QString expectedCode(
    const QSocIomuxPlan         &plan,
    const QSocIomuxRoutePlan    &route,
    const QString               &group,
    const QSocIomuxEndpointPlan &link,
    quint32                      width,
    int                          fixed,
    int                          on,
    int                          off)
{
    if (link.link.isEmpty()) {
        return QString("%1'd%2").arg(width).arg(fixed);
    }
    QString net = QSocIomuxGenerator::selectPortName(route.pin, route.slot, group)
                  + (link.invert ? QStringLiteral(" ^ 1'b1") : QString());
    if (plan.option.invert) {
        net += QString(" ^ pin_%1_%2_inv_i").arg(route.pin).arg(group);
    }
    return QString("(%1 ? %2'd%3 : %2'd%4)").arg(net).arg(width).arg(on).arg(off);
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
    const QSocPadModel    &model    = plan.padModel;
    const QSocPadEncoding  encoding = QSocIomuxGenerator::padEncoding(plan.padClass(pin), model);
    const QSocPadSafePlan &safe     = plan.padClass(pin).safe;
    const auto             expect   = [&](const char    *name,
                                          const char    *reg,
                                          quint32        width,
                                          const char    *src,
                                          int            safeCode,
                                          const QString &value) {
        const QString chosen
            = plan.option.padControl
                  ? QString("pin_%1_%2_i ? pin_%1_%3_i : %4").arg(pin).arg(src, reg, value)
                  : value;
        const QString forced
            = model.safe
                  ? QString("pad_force_i ? %1'd%2 : (%3)").arg(width).arg(safeCode).arg(chosen)
                  : chosen;
        lines->append(QString("        assert (pad_%1_w%2 == %3);")
                          .arg(
                              name,
                              QSocIomuxGenerator::padLane(pin),
                              QSocIomuxGenerator::padLaneValue(width, "(" + forced + ")")));
    };
    const auto expectZero = [&](const QString &name) {
        lines->append(QString("        assert (pad_%1_w%2 == %3'd0);")
                          .arg(name, QSocIomuxGenerator::padLane(pin))
                          .arg(QSocIomuxGenerator::kPadLane));
    };
    if (model.hasPull() && !encoding.hasPull()) {
        expectZero(QStringLiteral("pull_mode"));
        if (model.upSelWidth > 0) {
            expectZero(QStringLiteral("up_sel"));
        }
        if (model.downSelWidth > 0) {
            expectZero(QStringLiteral("down_sel"));
        }
    } else if (model.hasPull()) {
        expect(
            "pull_mode",
            "pull_mode",
            model.modeWidth,
            "pull_src",
            encoding.requestMode(safe.pull),
            route ? expectedCode(
                        plan,
                        *route,
                        "pull",
                        route->pullSelect.link,
                        model.modeWidth,
                        encoding.routeMode(*route),
                        encoding.requestMode(route->pullSelect.on),
                        encoding.requestMode(route->pullSelect.off))
                  : QString("%1'd0").arg(model.modeWidth));
        if (model.upSelWidth > 0) {
            expect(
                "up_sel",
                "up_sel",
                model.upSelWidth,
                "pull_src",
                encoding.requestUpSel(safe.pull),
                route ? expectedCode(
                            plan,
                            *route,
                            "pull",
                            route->pullSelect.link,
                            model.upSelWidth,
                            encoding.routeUpSel(*route),
                            encoding.requestUpSel(route->pullSelect.on),
                            encoding.requestUpSel(route->pullSelect.off))
                      : QString("%1'd0").arg(model.upSelWidth));
        }
        if (model.downSelWidth > 0) {
            expect(
                "down_sel",
                "down_sel",
                model.downSelWidth,
                "pull_src",
                encoding.requestDownSel(safe.pull),
                route ? expectedCode(
                            plan,
                            *route,
                            "pull",
                            route->pullSelect.link,
                            model.downSelWidth,
                            encoding.routeDownSel(*route),
                            encoding.requestDownSel(route->pullSelect.on),
                            encoding.requestDownSel(route->pullSelect.off))
                      : QString("%1'd0").arg(model.downSelWidth));
        }
    }
    for (qsizetype index = 0; index < encoding.control.size(); ++index) {
        const QSocPadEncoding::Control &item       = encoding.control.at(index);
        const quint32                   fieldWidth = model.control.at(index).width;
        if (fieldWidth == 0) {
            continue;
        }
        if (item.cellIndex < 0) {
            expectZero(item.name + "_select");
            continue;
        }
        const QByteArray out  = (item.name + "_select").toUtf8();
        const QByteArray name = item.name.toUtf8();
        const QByteArray src  = (item.name + "_src").toUtf8();
        expect(
            out.constData(),
            name.constData(),
            fieldWidth,
            src.constData(),
            encoding.controlCodeOrDefault(index, safe.control.value(item.name)),
            route ? expectedCode(
                        plan,
                        *route,
                        item.name,
                        route->control.value(item.name).select.link,
                        fieldWidth,
                        encoding.routeControlCode(*route, index),
                        encoding.controlCodeOrDefault(
                            index, route->control.value(item.name).select.on.mode),
                        encoding.controlCodeOrDefault(
                            index, route->control.value(item.name).select.off.mode))
                  : QString("%1'd%2").arg(fieldWidth).arg(item.defaultCode));
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
    lines.append(QString("module %1_hs_formal #(").arg(plan.moduleName));
    lines.append("    parameter integer PIN_LO = 0,");
    lines.append(QString("    parameter integer PIN_HI = %1").arg(plan.pinCount - 1));
    lines.append(") (");
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
    for (const QString &name : selectPortNames(plan)) {
        declarations.append(QString("    input logic %1").arg(name));
    }
    if (plan.padModel.safe) {
        declarations.append(QStringLiteral("    input logic pad_force_i"));
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
    for (const QString &name : selectPortNames(plan)) {
        coreConnections.append(QString("    .%1(%1)").arg(name));
    }
    if (plan.padModel.safe) {
        coreConnections.append(QStringLiteral("    .pad_force_i(pad_force_i)"));
    }
    lines.append(QString("%1_core u_core (").arg(plan.moduleName));
    for (qsizetype index = 0; index < coreConnections.size(); ++index) {
        const QString suffix = index + 1 == coreConnections.size() ? QString() : QString(",");
        lines.append(coreConnections.at(index) + suffix);
    }
    lines.append(");");
    lines.append(QString());

    for (quint32 pin = 0; pin < plan.pinCount; ++pin) {
        /* One block per pin, so a job can take a range of pins and the
         * synthesis drops everything the other pins feed. */
        lines.append(
            QString("generate if (PIN_LO <= %1 && %1 <= PIN_HI) begin : g_pin_%1").arg(pin));
        lines.append("always_comb begin");
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
            const bool plain = !plan.option.gpio && !plan.option.invert && !plan.padModel.safe;
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
        for (const QSocIomuxRoutePlan &route : plan.routes) {
            const QSocIomuxEndpointPlan &endpoint = route.inputValue;
            if (route.pin != pin || endpoint.link.isEmpty()) {
                continue;
            }
            const QString name = QSocIomuxGenerator::endpointPortName(
                route.pin, route.slot, QSocIomuxRole::InputValue);
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
        lines.append("end");
        lines.append("end endgenerate");
        lines.append(QString());
    }
    lines.append("always_comb begin");
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

/**
 * @brief The job file: one bank of pins per prove and bmc task.
 *
 * The cost of one job grows faster than the pin count, so a whole design in
 * one task never closes past a few dozen pins while a bank of sixteen takes
 * minutes. The proof is per pin by construction, so a task sets `PIN_LO` and
 * `PIN_HI` and the tasks run in parallel. Cover looks at the first and last
 * selector only and takes bank 0.
 */
QString buildSby(const QSocIomuxPlan &plan, quint32 bankPins)
{
    const quint32 banks = (plan.pinCount + bankPins - 1) / bankPins;
    QString       tasks;
    QString       params;
    if (banks > 1) {
        for (quint32 bank = 0; bank < banks; ++bank) {
            tasks += QString("prove_b%1 prove b%1\nbmc_b%1 bmc b%1\n").arg(bank);
            params += QString("b%1: chparam -set PIN_LO %2 -set PIN_HI %3 %4_hs_formal\n")
                          .arg(bank)
                          .arg(bank * bankPins)
                          .arg(std::min(plan.pinCount, (bank + 1) * bankPins) - 1)
                          .arg(plan.moduleName);
        }
        tasks += QStringLiteral("cover b0\n");
    } else {
        tasks = QStringLiteral("prove\nbmc\ncover\n");
    }
    return QStringLiteral(
               "[tasks]\n"
               "%2"
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
               "%3"
               "prep -top %1_hs_formal\n"
               "\n"
               "[files]\n"
               "%1.v\n"
               "%1_conn.v\n"
               "%1_hs_formal.sv\n")
        .arg(plan.moduleName, tasks, params);
}

} // namespace

QSocIomuxFormalCollateral QSocIomuxFormal::generate(const QSocIomuxPlan &plan, quint32 bankPins)
{
    QSocIomuxFormalCollateral collateral;
    if (plan.pinCount == 0 || plan.hsSlots == 0 || bankPins == 0) {
        return collateral;
    }
    collateral.systemVerilog = buildSystemVerilog(plan);
    collateral.sby           = buildSby(plan, bankPins);
    return collateral;
}

QString QSocIomuxFormal::generateFileList(const QSocIomuxPlan &plan)
{
    if (plan.pinCount == 0 || plan.hsSlots == 0) {
        return QString();
    }
    QStringList files
        = {plan.moduleName + "_regs.v", plan.moduleName + "_conn.v", plan.moduleName + ".v"};
    bool anyConstraint = false;
    for (const QSocPadCellPlan &cell : plan.padCells) {
        anyConstraint = anyConstraint || !cell.constraint.isEmpty();
    }
    if (plan.hasPadCell()) {
        files.append(QSocIomuxGenerator::ioModuleName(plan.moduleName) + ".v");
    }
    files.append(plan.moduleName + "_regs_formal.sv");
    files.append(plan.moduleName + "_hs_formal.sv");
    if (anyConstraint) {
        files.append(QSocIomuxGenerator::ioModuleName(plan.moduleName) + "_formal.sv");
    }
    return files.join('\n') + "\n";
}

QSocIomuxFormalCollateral QSocIomuxFormal::generatePad(const QSocIomuxPlan &plan)
{
    bool anyConstraint = false;
    for (const QSocPadCellPlan &cell : plan.padCells) {
        anyConstraint = anyConstraint || !cell.constraint.isEmpty();
    }
    if (plan.pinCount == 0 || !anyConstraint) {
        return {};
    }
    const QString name  = plan.moduleName;
    const QString shell = QSocIomuxGenerator::ioModuleName(name);
    QStringList   sv;
    sv.append("// Generated by QSoC. Do not edit.");
    sv.append("`default_nettype none");
    sv.append(QString());

    /* A stub of every cell the shell instantiates. Directions come from the
     * module library, so a harness that elaborates is one more check that
     * the declarations named real ports. Physical cells have no ports. */
    struct Stub
    {
        QMap<QString, QString> ports;
        const QSocPadCellPlan *cell = nullptr; /**< The class whose constraints it carries */
    };
    QMap<QString, Stub> stubs;
    for (const QSocPadCellPlan &cell : plan.padCells) {
        QStringList modules = {cell.cell};
        if (plan.ioLib.contains(cell.cell)) {
            for (const QString &variant : plan.ioLib.value(cell.cell).variant) {
                modules.append(variant);
            }
        }
        for (const QString &module : modules) {
            stubs[module] = {cell.cellPorts, &cell};
        }
    }
    for (const QSocIoRingDirect &direct : plan.ioRing.direct) {
        QStringList modules = {direct.cell};
        if (plan.ioLib.contains(direct.cell)) {
            for (const QString &variant : plan.ioLib.value(direct.cell).variant) {
                modules.append(variant);
            }
        }
        for (const QString &module : modules) {
            if (!stubs.contains(module)) {
                stubs[module] = {direct.cellPorts, nullptr};
            }
        }
    }
    if (plan.ioRing.declared) {
        QStringList physical;
        if (!plan.ioRing.corner.isEmpty()) {
            physical.append(plan.ioRing.corner);
        }
        for (const QString &cell : plan.ioRing.power) {
            physical.append(cell);
        }
        for (const QString &side : QSocIomuxGenerator::ringSides()) {
            for (const QSocIoRingItem &item : plan.ioRing.side.value(side)) {
                if (item.kind == QSocIoRingItem::Cell) {
                    physical.append(item.name);
                }
            }
        }
        for (const QSocIoLibCell &cell : plan.ioLib) {
            if (cell.kind == "fill") {
                physical.append(cell.name);
            }
        }
        for (const QString &base : physical) {
            QStringList modules = {base};
            if (plan.ioLib.contains(base)) {
                for (const QString &variant : plan.ioLib.value(base).variant) {
                    modules.append(variant);
                }
            }
            for (const QString &module : modules) {
                if (!stubs.contains(module)) {
                    stubs[module] = {{}, nullptr};
                }
            }
        }
    }
    for (auto it = stubs.cbegin(); it != stubs.cend(); ++it) {
        const Stub &stub = it.value();
        QStringList stubPorts;
        for (auto port = stub.ports.cbegin(); port != stub.ports.cend(); ++port) {
            const QString dir = port.value() == "out"     ? QStringLiteral("output")
                                : port.value() == "inout" ? QStringLiteral("inout")
                                                          : QStringLiteral("input");
            stubPorts.append(QString("    %1 wire %2").arg(dir, port.key()));
        }
        sv.append(QString("module %1 (").arg(it.key()));
        sv.append(stubPorts.join(",\n"));
        sv.append(");");
        /* An empty body reads as a blackbox and the flow refuses it. Every
         * output takes a free value instead, which is the honest model of a
         * receiver the proof knows nothing about. */
        for (auto port = stub.ports.cbegin(); port != stub.ports.cend(); ++port) {
            if (port.value() == "out") {
                sv.append(QString("(* anyseq *) reg %1_any;").arg(port.key()));
                sv.append(QString("assign %1 = %1_any;").arg(port.key()));
            }
        }
        if (stub.ports.isEmpty()) {
            sv.append("(* keep *) wire formal_keep = 1'b1;");
        }
        if (stub.cell != nullptr) {
            /* The constraints are written over the cell's own pins, so the
             * stub is their natural home: the shell instantiates it once per
             * pin of the class and every pin gets its copy, named by its
             * instance. Temporal properties clock on the formal global clock
             * because the pad has no clock; the open engine has $past, $rose
             * and $stable but no SVA sequences, so an implication is written
             * as a boolean. */
            bool anyTemporal = false;
            for (const QSocPadConstraint &item : stub.cell->constraint) {
                anyTemporal = anyTemporal || item.temporal;
            }
            if (anyTemporal) {
                sv.append("(* gclk *) wire formal_clk;");
            }
            for (const QSocPadConstraint &item : stub.cell->constraint) {
                const QString verb = item.assume ? QStringLiteral("assume")
                                                 : QStringLiteral("assert");
                sv.append(QString("%1 %2_%3: %2(%4);")
                              .arg(
                                  item.temporal ? QStringLiteral("always @(posedge formal_clk)")
                                                : QStringLiteral("always @(*)"),
                                  verb,
                                  item.name,
                                  item.body));
            }
        }
        sv.append("endmodule");
        sv.append(QString());
    }

    /* The harness: every input of the shell is free, every output is a wire,
     * and the direct nets of the ring are ports of the harness itself. */
    const QString    range = QString("[%1:0]").arg(plan.pinCount - 1);
    QStringList      harnessPorts;
    QStringList      connections = {"    .pad_io(pad_io)"};
    QStringList      wires       = {QString("wire %1 pad_io;").arg(range)};
    const YAML::Node view        = QSocIomuxGenerator::describeIoModuleYaml(plan)["port"];
    for (const auto &entry : view) {
        const QString portName  = QString::fromStdString(entry.first.as<std::string>());
        const QString direction = QString::fromStdString(
            entry.second["direction"].as<std::string>());
        const QString type  = QString::fromStdString(entry.second["type"].as<std::string>());
        const QString width = type == "logic" ? QString() : type.mid(5) + " ";
        if (portName == "pad_io") {
            continue;
        }
        if (direction == "input") {
            harnessPorts.append(QString("    input wire %1%2").arg(width, portName));
        } else if (direction == "inout") {
            harnessPorts.append(QString("    inout wire %1%2").arg(width, portName));
        } else {
            wires.append(QString("wire %1%2;").arg(width, portName));
        }
        connections.append(QString("    .%1(%1)").arg(portName));
    }
    sv.append(QString("module %1_formal (").arg(shell));
    sv.append(harnessPorts.join(",\n"));
    sv.append(");");
    sv.append(wires.join('\n'));
    sv.append(QString("%1 u_io (").arg(shell));
    sv.append(connections.join(",\n"));
    sv.append(");");
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
                            "read -formal -sv %1.v %1_formal.sv\n"
                            "prep -top %1_formal -flatten\n"
                            "\n"
                            "[files]\n"
                            "%1.v\n"
                            "%1_formal.sv\n")
                            .arg(shell);
    return {sv.join('\n'), sby};
}
