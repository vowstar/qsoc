// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmshared.h"
#include "common/qsocprcmcomposition.h"
#include "common/qsocprcmsequencertl.h"
#include "common/qsocverilogutils.h"

#include <algorithm>
#include <bit>
#include <QJsonArray>
#include <QRegularExpression>

namespace {

[[noreturn]] void reject(const QSocPrcmInput &input, const QString &path, const QString &message)
{
    throw QSocPrcmDiagnostic{"PRCM_GENERATE", message, {input.source.value(path, {{}, path, 0, 0})}};
}

QString range(quint32 width)
{
    return width == 1 ? QString() : QString("[%1:0] ").arg(width - 1);
}

QString literal(quint32 width, quint64 value)
{
    return QString("%1'h%2").arg(width).arg(value, 0, 16);
}

QString disjunction(const QStringList &value)
{
    return value.isEmpty() ? "1'b0" : '(' + value.join(" || ") + ')';
}

struct Domain
{
    QString    prefix;
    quint32    width = 1;
    QList<int> incoming;
    QList<int> outgoing;
};

class Assembly
{
public:
    Assembly(
        const QSocPrcmBindingPlan     &binding,
        const QSocPrcmCompositionPlan &composition,
        const QString                 &name,
        int                            stage)
        : binding(binding)
        , input(binding.input)
        , plan(composition)
        , name(name)
        , stage(stage)
    {}

    QSocPrcmCircuit generate()
    {
        collectPort();
        selectDomain();
        buildRegister();
        addResource();
        QString     text;
        QTextStream out(&text);
        QStringList declaration;
        for (const auto &port : result.port)
            declaration.append("    " + port.direction + " wire " + range(port.width) + port.name);
        out << "module " << name << " (\n" << declaration.join(",\n") << "\n);\n\n";
        emitReset(out);
        emitChip(out);
        for (auto domain = node.cbegin(); domain != node.cend(); ++domain)
            emitDomain(out, domain.key(), domain.value());
        for (qsizetype index = 0; index < plan.service.size(); ++index)
            emitService(out, index);
        emitChipStatus(out);
        emitInstance(out, result.mmio.moduleName, prefix + "register_inst", registerPort);
        QMap<QString, QString> port;
        for (const auto &item : binding.clock.ports)
            port.insert(item.name, item.name);
        emitInstance(out, name + "_clock", prefix + "clock_inst", port);
        port.clear();
        for (const auto &item : QSocResetPrimitive::describePorts(binding.reset))
            port.insert(item.name, item.name);
        emitInstance(out, name + "_reset", prefix + "reset_inst", port);
        out << "endmodule\n";
        out.flush();
        addRtl(name + ".v", text);
        result.binding
            = {{"module", name},
               {"instance", instance},
               {"receiver", receiver},
               {"domain", domainBinding},
               {"service", serviceBinding}};
        return result;
    }

private:
    void addPort(const QString &signal, bool incoming, quint32 width)
    {
        auto found = result.port.find(signal);
        if (found == result.port.end()) {
            result.port.insert(signal, {signal, incoming ? "input" : "output", width});
            return;
        }
        if (found->width != width || (!incoming && found->direction == "output"))
            reject(input, "prcm.controller", "Conflicting resource port: " + signal);
        if (!incoming)
            found->direction = "output";
    }

    void collectPort()
    {
        for (const auto &port : binding.clock.ports)
            addPort(port.name, port.isInput, static_cast<quint32>(port.width));
        for (const auto &port : QSocResetPrimitive::describePorts(binding.reset))
            addPort(port.name, port.isInput, static_cast<quint32>(port.width));
        for (auto domain = input.domain.cbegin(); domain != input.domain.cend(); ++domain) {
            if (!binding.domain.contains(domain.key()))
                reject(
                    input,
                    "prcm.domain." + domain.key(),
                    "Bind domain resources before generation.");
            const auto supply = input.supplyTable.value(domain->supply);
            addPort(supply.request, false, 1);
            addPort(supply.valid.signal, true, 1);
            addPort(domain->quiesce.request, false, 1);
            addPort(domain->quiesce.completion.signal, true, 1);
            addPort(domain->isolation.request, false, 1);
            addPort(domain->isolation.completion.signal, true, 1);
        }
        QSocMmioPlan bus;
        bus.bus          = input.bus;
        bus.dataWidth    = input.dataWidth;
        bus.addressWidth = input.addressWidth;
        for (const auto &port : QSocMmioGenerator::describePorts(bus)) {
            if (port.name == "clk_i" || port.name == "rst_ni")
                continue;
            if (result.port.contains(port.name))
                reject(input, "prcm.mmio", "Bus port conflicts with resource signal: " + port.name);
            result.port.insert(port.name, port);
            registerPort.insert(port.name, port.name);
        }
        const auto used = result.port.keys();
        while (std::any_of(used.cbegin(), used.cend(), [&](const auto &port) {
            return port.startsWith(prefix);
        }))
            prefix += '_';
        for (auto domain = input.domain.cbegin(); domain != input.domain.cend(); ++domain) {
            result.port.remove(binding.domain[domain.key()].clockEnable);
            result.port.remove(domain->reset.source);
        }
        registerPort.insert("clk_i", input.clockInput);
        registerPort.insert("rst_ni", prefix + "cold_n");
        registerPort.insert("clear_i", prefix + "clear");
    }

    void selectDomain()
    {
        for (auto domain = plan.domain.cbegin(); domain != plan.domain.cend(); ++domain) {
            Domain item;
            item.prefix = prefix + "d" + QString::number(node.size()) + '_';
            item.width  = qMax(1U, static_cast<unsigned>(std::bit_width(domain->mode.lastKey())));
            node.insert(domain.key(), item);
        }
        for (qsizetype i = 0; i < plan.service.size(); ++i) {
            node[plan.service[i].consumer].outgoing.append(static_cast<int>(i));
            node[plan.service[i].provider].incoming.append(static_cast<int>(i));
        }
    }

    QString servicePrefix(qsizetype index) const
    {
        return prefix + "s" + QString::number(index) + '_';
    }

    void addRegister(const QString &name, const QList<QSocMmioFieldPlan> &field)
    {
        result.mmio.registers.append(
            {name, {}, quint64(result.mmio.registers.size()) * (input.dataWidth / 8), field});
    }

    QSocMmioFieldPlan field(
        const QString &name,
        quint32        width,
        quint32        bit,
        const QString &port,
        const QString &signal,
        QSocMmioAccess access = QSocMmioAccess::ReadOnly,
        quint64        reset  = 0)
    {
        QSocMmioFieldPlan entry;
        entry.name   = name;
        entry.width  = width;
        entry.lsb    = bit;
        entry.access = access;
        if (access == QSocMmioAccess::ReadWrite)
            entry.outputPort = port;
        else
            entry.inputPort = port;
        if (access != QSocMmioAccess::ReadOnly)
            entry.resetValue = reset;
        registerPort.insert(port, signal);
        return entry;
    }

    void buildRegister()
    {
        auto &mmio        = result.mmio;
        mmio.moduleName   = name + "_register";
        mmio.bus          = input.bus;
        mmio.dataWidth    = input.dataWidth;
        mmio.addressWidth = input.addressWidth;
        mmio.clearPort    = "clear_i";
        if (!plan.chip.isEmpty()) {
            chipWidth = qMax(1U, static_cast<unsigned>(std::bit_width(plan.chip.lastKey())));
            if (chipWidth + 2 > input.dataWidth)
                reject(
                    input,
                    "prcm.chip.mode",
                    "Chip mode code and two status bits must fit one MMIO word.");
            addRegister(
                "CHIP_REQUEST",
                {field(
                    "mode",
                    chipWidth,
                    0,
                    "chip_mode_o",
                    prefix + "chip_mode",
                    QSocMmioAccess::ReadWrite,
                    plan.resetCode)});
            addRegister(
                "CHIP_STATUS",
                {field("request", chipWidth, 0, "chip_request_i", prefix + "chip_mode"),
                 field("done", 1, chipWidth, "chip_done_i", prefix + "chip_done"),
                 field(
                     "invalid_mode",
                     1,
                     chipWidth + 1,
                     "chip_invalid_i",
                     "!" + prefix + "chip_valid")});
        }
        for (auto item = node.cbegin(); item != node.cend(); ++item) {
            const auto &p = item->prefix;
            const auto  w = item->width;
            addRegister(
                "DOMAIN_" + item.key() + "_REQUEST",
                {field(
                    "mode",
                    w,
                    0,
                    p + "mode_o",
                    p + "mode",
                    QSocMmioAccess::ReadWrite,
                    plan.domain[item.key()].resetCode)});
            QList<QSocMmioFieldPlan> status{field("request", w, 0, p + "request_i", p + "mode")};
            QStringList              flags{"done", "invalid_mode", "fault"};
            if (!plan.chip.isEmpty())
                flags.append("blocked_by_chip");
            if (!item->incoming.isEmpty())
                flags.append("in_use");
            if (!item->outgoing.isEmpty())
                flags.append("wait_service");
            if (w + flags.size() > input.dataWidth)
                reject(
                    input,
                    "prcm.domain." + item.key() + ".mode",
                    "Mode code and status flags must fit one MMIO word.");
            quint32 bit = w;
            for (const auto &flag : flags)
                status.append(field(
                    flag,
                    1,
                    bit++,
                    p + flag + "_i",
                    flag == "invalid_mode" ? "!" + p + "valid" : p + flag));
            addRegister("DOMAIN_" + item.key() + "_STATUS", status);
            const auto power = input.supplyTable[input.domain[item.key()].supply].valid.signal;
            QList<QSocMmioFieldPlan> event{field(
                "power_lost",
                1,
                0,
                p + "power_lost_i",
                p + "watch && !" + power,
                QSocMmioAccess::WriteOneClear)};
            if (!item->outgoing.isEmpty())
                event.append(field(
                    "service_lost",
                    1,
                    1,
                    p + "service_lost_i",
                    p + "service_failure",
                    QSocMmioAccess::WriteOneClear));
            addRegister("DOMAIN_" + item.key() + "_EVENT", event);
        }
        QStringList errors;
        if (!QSocMmioGenerator::canonicalizePlan(&mmio, &errors))
            reject(input, "prcm.mmio", errors.join('\n'));
    }

    void addRtl(const QString &file, const QString &rtl)
    {
        if (rtl.isEmpty() || result.rtl.contains(file))
            reject(input, "prcm", "Cannot emit a unique RTL file: " + file);
        const QRegularExpression module(
            "^\\s*module\\s+([A-Za-z_][A-Za-z_0-9$]*)\\s", QRegularExpression::MultilineOption);
        auto match = module.globalMatch(rtl);
        while (match.hasNext()) {
            const auto moduleName = match.next().captured(1);
            if (declared.contains(moduleName))
                reject(input, "prcm", "Generated module name is used twice: " + moduleName);
            declared.append(moduleName);
        }
        result.rtl.insert(file, rtl);
    }

    void addResource()
    {
        auto clock       = binding.clock;
        auto reset       = binding.reset;
        clock.moduleName = name + "_clock";
        reset.moduleName = name + "_reset";
        QSocClockPrimitive c;
        QSocResetPrimitive r;
        addRtl(clock.moduleName + ".v", c.generateControllerVerilog(clock));
        addRtl(reset.moduleName + ".v", r.generateControllerVerilog(reset));
        addRtl("clock_cell.v", c.generateCellVerilog());
        addRtl("reset_cell.v", r.generateCellVerilog());
        addRtl("qsoc_prcm_domain_service.v", QSocPrcmSequenceRtl::generateService());
        if (!plan.service.isEmpty())
            addRtl("qsoc_prcm_service.v", QSocPrcmSequenceRtl::generateHandshake());
        addRtl(result.mmio.moduleName + ".v", QSocMmioGenerator::generateVerilog(result.mmio));
    }

    void emitInstance(
        QTextStream                  &out,
        const QString                &module,
        const QString                &name,
        const QMap<QString, QString> &port,
        const QJsonObject            &parameter = {})
    {
        QStringList connection;
        QJsonObject connectionMap;
        for (auto item = port.cbegin(); item != port.cend(); ++item) {
            connection.append("    ." + item.key() + '(' + item.value() + ')');
            connectionMap.insert(item.key(), item.value());
        }
        instance.insert(
            name,
            QJsonObject{{"module", module}, {"port", connectionMap}, {"parameter", parameter}});
        out << '\n' << module;
        if (!parameter.isEmpty()) {
            QStringList value;
            for (auto item = parameter.constBegin(); item != parameter.constEnd(); ++item)
                value.append('.' + item.key() + '(' + QString::number(item.value().toInt()) + ')');
            out << " #(" << value.join(", ") << ')';
        }
        out << ' ' << name << " (\n" << connection.join(",\n") << "\n);\n";
    }

    void sample(QTextStream &out, const QString &signal, const QString &expression)
    {
        const auto reg = signal + "_sample";
        receiver.insert(
            reg,
            QJsonObject{
                {"clock", input.clockInput},
                {"edge", "rise"},
                {"stage", stage},
                {"input", expression},
                {"reset", QJsonObject{{"signal", prefix + "cold_n"}, {"active", "low"}}}});
        out << "reg [" << stage - 1 << ":0] " << reg << ";\n"
            << "always @(posedge " << input.clockInput << " or negedge " << prefix
            << "cold_n) begin\n"
            << "    if (!" << prefix << "cold_n) " << reg << " <= {" << stage << "{1'b1}};\n"
            << "    else " << reg << " <= {" << reg << '[' << stage - 2 << ":0], " << expression
            << "};\nend\n"
            << "wire " << signal << " = " << reg << '[' << stage - 1 << "];\n";
    }

    void emitReset(QTextStream &out)
    {
        out << "wire " << prefix << "cold_n;\n";
        emitInstance(
            out,
            "qsoc_rst_sync",
            prefix + "cold_inst",
            {{"clk", input.clockInput},
             {"rst_in_n", input.resetSource},
             {"test_enable", "1'b0"},
             {"rst_out_n", prefix + "cold_n"}},
            {{"STAGE", stage}});
        QString clear = "1'b0";
        for (const auto &target : binding.reset.targets)
            if (target.name == input.resetTarget)
                clear = (target.active == "low" ? "!" : "") + target.name;
        sample(out, prefix + "clear", clear);
    }

    void emitChip(QTextStream &out)
    {
        if (plan.chip.isEmpty())
            return;
        out << "wire " << range(chipWidth) << prefix << "chip_mode;\n"
            << "reg " << prefix << "chip_valid;\n"
            << "reg " << range(chipWidth) << prefix << "chip_last;\n"
            << "always @* begin\n    " << prefix << "chip_valid = 1'b1;\n    case (" << prefix
            << "chip_mode)\n";
        for (auto mode = plan.chip.cbegin(); mode != plan.chip.cend(); ++mode)
            out << "        " << literal(chipWidth, mode.key()) << ": begin end\n";
        out << "        default: " << prefix << "chip_valid = 1'b0;\n    endcase\nend\n"
            << "always @(posedge " << input.clockInput << " or negedge " << prefix
            << "cold_n) begin\n"
            << "    if (!" << prefix << "cold_n) " << prefix
            << "chip_last <= " << literal(chipWidth, plan.resetCode) << ";\n"
            << "    else if (" << prefix << "chip_valid) " << prefix << "chip_last <= " << prefix
            << "chip_mode;\nend\n"
            << "wire " << range(chipWidth) << prefix << "chip_target = " << prefix
            << "chip_valid ? " << prefix << "chip_mode : " << prefix << "chip_last;\n";
    }

    void emitTarget(QTextStream &out, const QString &domain, const Domain &item)
    {
        const auto &p = item.prefix;
        out << "wire " << range(item.width) << p << "mode;\nreg [1:0] " << p << "decoded;\nreg "
            << p << "valid;\nreg [1:0] " << p << "last;\n"
            << "always @* begin\n    " << p << "valid = 1'b1;\n    " << p
            << "decoded = 2'd0;\n    case (" << p << "mode)\n";
        const auto &mode = plan.domain[domain].mode;
        for (auto value = mode.cbegin(); value != mode.cend(); ++value)
            out << "        " << literal(item.width, value.key()) << ": " << p << "decoded = 2'd"
                << int(value.value()) << ";\n";
        out << "        default: " << p << "valid = 1'b0;\n    endcase\nend\n"
            << "always @(posedge " << input.clockInput << " or negedge " << prefix
            << "cold_n) begin\n"
            << "    if (!" << prefix << "cold_n) " << p << "last <= 2'd0;\n"
            << "    else if (" << p << "valid) " << p << "last <= " << p << "decoded;\nend\n"
            << "wire [1:0] " << p << "desired = " << p << "valid ? " << p << "decoded : " << p
            << "last;\n"
            << "reg [1:0] " << p << "base;\n"
            << "always @* begin\n    " << p << "base = " << p << "desired;\n";
        if (!plan.chip.isEmpty()) {
            out << "    case (" << prefix << "chip_target)\n";
            for (auto policy = plan.chip.cbegin(); policy != plan.chip.cend(); ++policy) {
                const auto target = policy.value()[domain];
                out << "        " << literal(chipWidth, policy.key()) << ": ";
                if (target)
                    out << p << "base = 2'd" << int(*target) << ";\n";
                else
                    out << "begin end\n";
            }
            out << "        default: begin end\n    endcase\n";
        }
        out << "end\nwire " << p << "blocked_by_chip = " << p << "base != " << p << "desired;\n";
    }

    void emitDomain(QTextStream &out, const QString &name, const Domain &item)
    {
        const auto &p        = item.prefix;
        const auto  domain   = input.domain[name];
        const auto  resource = binding.domain[name];
        const auto  supply   = input.supplyTable[domain.supply];
        out << "wire " << resource.clockEnable << ", " << domain.reset.source << ";\n";
        sample(out, p + "reset", (resource.resetTargetActiveLow ? "!" : "") + domain.reset.target);
        emitTarget(out, name, item);
        for (const auto &signal : {"off", "held", "run", "fault", "watch", "reset_request"})
            out << "wire " << p << signal << ";\n";
        QStringList incoming, failure, missing;
        for (int id : item.incoming)
            incoming.append(servicePrefix(id) + "request");
        for (int id : item.outgoing) {
            failure.append(servicePrefix(id) + "failure");
            missing.append('!' + servicePrefix(id) + "permission");
        }
        out << "wire " << p << "in_use = " << disjunction(incoming) << ";\n"
            << "wire " << p << "service_failure = " << disjunction(failure) << ";\n"
            << "wire [1:0] " << p << "wanted = " << p << "in_use ? 2'd2 : " << p << "base;\n"
            << "wire " << p << "wait_service = (" << p << "wanted == 2'd2) && "
            << disjunction(missing) << ";\n"
            << "wire [1:0] " << p << "target = " << p << "wait_service && !" << p << "fault ? ("
            << supply.request << " ? 2'd1 : 2'd0) : " << p << "wanted;\n";
        emitInstance(
            out,
            "qsoc_prcm_domain_service",
            p + "action_inst",
            {{"clk_i", input.clockInput},
             {"rst_ni", prefix + "cold_n"},
             {"target_off_i", p + "target == 2'd0"},
             {"target_reset_i", p + "target == 2'd1"},
             {"target_run_i", p + "target == 2'd2"},
             {"power_i", supply.valid.signal},
             {"reset_i", p + "reset"},
             {"isolation_i", domain.isolation.completion.signal},
             {"idle_i", domain.quiesce.completion.signal},
             {"service_fault_i", p + "service_failure"},
             {"power_o", supply.request},
             {"clock_o", resource.clockEnable},
             {"reset_o", p + "reset_request"},
             {"isolation_o", domain.isolation.request},
             {"quiesce_o", domain.quiesce.request},
             {"state_off_o", p + "off"},
             {"state_reset_o", p + "held"},
             {"state_run_o", p + "run"},
             {"fault_o", p + "fault"},
             {"power_watch_o", p + "watch"}});
        const auto power = supply.valid.signal;
        const auto iso   = domain.isolation.completion.signal;
        const auto idle  = domain.quiesce.completion.signal;
        out << "assign " << domain.reset.source << " = "
            << (resource.resetSourceActiveLow ? "!" : "") << p << "reset_request;\n"
            << "wire " << p << "ready_off = " << p << "off && !" << power << " && " << p
            << "reset && " << iso << " && " << idle << ";\n"
            << "wire " << p << "ready_reset = " << p << "held && " << power << " && " << p
            << "reset && " << iso << " && " << idle << ";\n"
            << "wire " << p << "ready_run = " << p << "run && " << power << " && !" << p
            << "reset && !" << iso << " && !" << idle << ";\n"
            << "wire " << p << "released = " << p << "ready_off || " << p << "ready_reset || (" << p
            << "fault && !" << supply.request << " && !" << power << " && " << p << "reset && "
            << iso << " && " << idle << ");\n"
            << "wire " << p << "done = " << p << "valid && !" << p << "blocked_by_chip && ((" << p
            << "decoded == 2'd0 && " << p << "ready_off) || (" << p << "decoded == 2'd1 && " << p
            << "ready_reset) || (" << p << "decoded == 2'd2 && " << p << "ready_run));\n";
        domainBinding.insert(
            name,
            QJsonObject{
                {"prefix", p},
                {"action", p + "action_inst"},
                {"request", p + "mode"},
                {"target", p + "target"},
                {"reset", p + "reset"}});
    }

    void emitService(QTextStream &out, qsizetype index)
    {
        const auto &edge = plan.service[index];
        const auto  s    = servicePrefix(index);
        const auto  c    = node[edge.consumer].prefix;
        const auto  p    = node[edge.provider].prefix;
        out << "wire " << s << "request, " << s << "hold;\nreg " << s << "grant;\n"
            << "wire " << s << "failure = " << s << "request && (" << s << "hold ? (!" << s
            << "grant || !" << p << "ready_run) : " << p << "fault);\n"
            << "wire " << s << "permission = " << s << "hold && " << s << "grant && " << p
            << "ready_run && !" << p << "fault;\n"
            << "always @(posedge " << input.clockInput << " or negedge " << prefix
            << "cold_n) begin\n"
            << "    if (!" << prefix << "cold_n) " << s << "grant <= 1'b0;\n"
            << "    else " << s << "grant <= " << s << "request && " << p << "ready_run;\nend\n";
        emitInstance(
            out,
            "qsoc_prcm_service",
            s + "handshake_inst",
            {{"clk_i", input.clockInput},
             {"rst_ni", prefix + "cold_n"},
             {"need_i", c + "wanted == 2'd2 && !" + c + "fault"},
             {"grant_i", s + "grant"},
             {"fault_i", s + "failure"},
             {"release_i", c + "released"},
             {"request_o", s + "request"},
             {"hold_o", s + "hold"}});
        QJsonArray source;
        for (const auto &path : edge.source)
            source.append(path);
        serviceBinding.append(
            QJsonObject{
                {"consumer", edge.consumer},
                {"provider", edge.provider},
                {"instance", s + "handshake_inst"},
                {"request", s + "request"},
                {"grant", s + "grant"},
                {"source", source}});
    }

    void emitChipStatus(QTextStream &out)
    {
        if (plan.chip.isEmpty())
            return;
        out << "reg " << prefix << "chip_done;\nalways @* begin\n    " << prefix
            << "chip_done = 1'b0;\n    case (" << prefix << "chip_mode)\n";
        const QStringList state{"off", "reset", "run"};
        for (auto policy = plan.chip.cbegin(); policy != plan.chip.cend(); ++policy) {
            QStringList complete;
            for (auto item = node.cbegin(); item != node.cend(); ++item) {
                const auto &p      = item->prefix;
                const auto  target = policy.value()[item.key()];
                if (target) {
                    complete.append(
                        '(' + p + "wanted == 2'd" + QString::number(int(*target)) + " && !" + p
                        + "wait_service && " + p + "ready_" + state[int(*target)] + ')');
                } else {
                    QStringList allowed;
                    for (const auto &mode : plan.domain[item.key()].mode)
                        if (!allowed.contains(p + "ready_" + state[int(mode)]))
                            allowed.append(p + "ready_" + state[int(mode)]);
                    complete.append(disjunction(allowed));
                }
            }
            out << "        " << literal(chipWidth, policy.key()) << ": " << prefix
                << "chip_done = " << complete.join(" && ") << ";\n";
        }
        out << "        default: begin end\n    endcase\nend\n";
    }

    const QSocPrcmBindingPlan     &binding;
    const QSocPrcmInput           &input;
    const QSocPrcmCompositionPlan &plan;
    QString                        name;
    int                            stage;
    QString                        prefix    = "prcm_";
    quint32                        chipWidth = 1;
    QMap<QString, Domain>          node;
    QMap<QString, QString>         registerPort;
    QSocPrcmCircuit                result;
    QStringList                    declared;
    QJsonObject                    instance, receiver, domainBinding;
    QJsonArray                     serviceBinding;
};

} // namespace

QSocPrcmGenerateResult QSocPrcmShared::generate(
    const QSocPrcmBindingPlan &binding, const QString &moduleName, int sampleStage)
{
    QSocPrcmGenerateResult result;
    const auto             plan = QSocPrcmComposition::build(binding.input);
    if (!plan.plan) {
        result.diagnostic = plan.diagnostic;
        return result;
    }
    try {
        if (!QSocVerilogUtils::isValidVerilogIdentifier(moduleName))
            reject(binding.input, "prcm", "Module name must be a Verilog identifier.");
        if (sampleStage < 2)
            reject(binding.input, "prcm.controller", "Reset sampling requires at least two stages.");
        result.circuit = Assembly(binding, *plan.plan, moduleName, sampleStage).generate();
    } catch (const QSocPrcmDiagnostic &diagnostic) {
        result.diagnostic.append(diagnostic);
    }
    return result;
}
