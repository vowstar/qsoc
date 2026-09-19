// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmgenerator.h"
#include "common/qsocprcmsequenceplan.h"
#include "common/qsocprcmsequencertl.h"
#include "common/qsocverilogutils.h"

#include <algorithm>
#include <bit>
#include <QRegularExpression>
#include <QSet>

namespace {

[[noreturn]] void reject(
    const QSocPrcmInput &input,
    const QString       &path,
    const QString       &message,
    const QString       &other = {})
{
    QList<QSocPrcmSource> source{input.source.value(path, {{}, path, 0, 0})};
    if (!other.isEmpty())
        source.append(input.source.value(other, {{}, other, 0, 0}));
    throw QSocPrcmDiagnostic{"PRCM_GENERATE", message, source};
}

QString range(quint32 width)
{
    return width == 1 ? QString() : QString("[%1:0] ").arg(width - 1);
}

QString literal(quint32 width, quint64 value)
{
    return QString("%1'h%2").arg(width).arg(value, 0, 16);
}

QSocMmioPlan registerPlan(
    const QSocPrcmInput &input, const QSocPrcmSequencePlan &sequence, const QString &name)
{
    const auto width = qMax(1U, static_cast<unsigned>(std::bit_width(sequence.mode.lastKey())));
    if (width + 3 > input.dataWidth) {
        const auto &mode = input.domain.constFind(sequence.domain)->mode;
        const auto  widest
            = std::max_element(mode.cbegin(), mode.cend(), [](const auto &a, const auto &b) {
                  return a.code < b.code;
              });
        reject(
            input,
            "prcm.domain." + sequence.domain + ".mode." + widest.key() + ".code",
            "Mode code and three status bits must fit one MMIO data word.",
            "prcm.mmio.data_width");
    }
    QSocMmioPlan plan;
    plan.moduleName   = name;
    plan.bus          = input.bus;
    plan.dataWidth    = input.dataWidth;
    plan.addressWidth = input.addressWidth;
    plan.clearPort    = "clear_i";
    QSocMmioFieldPlan field;
    field.name       = "mode";
    field.width      = width;
    field.access     = QSocMmioAccess::ReadWrite;
    field.resetValue = sequence.resetCode;
    field.outputPort = "mode_o";
    plan.registers.append({"REQUEST", {}, 0, {field}});
    field           = {};
    field.name      = "request";
    field.width     = width;
    field.inputPort = "request_i";
    QSocMmioRegisterPlan status{"STATUS", {}, input.dataWidth / 8, {field}};
    quint32              bit = width;
    for (const auto &fieldName : {"done", "invalid_mode", "fault"}) {
        field           = {};
        field.name      = fieldName;
        field.lsb       = bit++;
        field.inputPort = QString(fieldName) + "_i";
        status.fields.append(field);
    }
    plan.registers.append(status);
    field            = {};
    field.name       = "power_lost";
    field.access     = QSocMmioAccess::WriteOneClear;
    field.resetValue = 0;
    field.inputPort  = "power_lost_i";
    plan.registers.append({"EVENT", {}, input.dataWidth / 4, {field}});
    QStringList error;
    if (!QSocMmioGenerator::canonicalizePlan(&plan, &error))
        reject(input, "prcm.mmio", error.join('\n'));
    return plan;
}

class Assembly
{
public:
    Assembly(
        const QSocPrcmBindingPlan  &binding,
        const QSocPrcmSequencePlan &sequence,
        const QSocMmioPlan         &registers,
        const QString              &name,
        int                         stage)
        : plan(binding)
        , action(sequence)
        , mmio(registers)
        , moduleName(name)
        , sampleStage(stage)
        , domain(plan.input.domain[action.domain])
        , resource(plan.domain[action.domain])
        , supply(plan.input.supplyTable[domain.supply])
    {}

    QString generate()
    {
        collectPort();
        QString     text;
        QTextStream out(&text);
        out << "module " << moduleName << " (\n";
        QStringList declaration;
        for (const auto &port : portTable)
            declaration.append("    " + port.direction + " wire " + range(port.width) + port.name);
        out << declaration.join(",\n") << "\n);\n\n";
        out << "wire " << resource.clockEnable << ";\n";
        out << "wire " << domain.reset.source << ";\n";
        emitReset(out);
        emitTarget(out);
        emitAction(out);
        emitInstance(out, mmio.moduleName, prefix + "register_inst", registerConnection());
        QMap<QString, QString> connection;
        for (const auto &port : plan.clock.ports)
            connection.insert(port.name, port.name);
        emitInstance(out, moduleName + "_clock", prefix + "clock_inst", connection);
        connection.clear();
        for (const auto &port : QSocResetPrimitive::describePorts(plan.reset))
            connection.insert(port.name, port.name);
        emitInstance(out, moduleName + "_reset", prefix + "reset_inst", connection);
        out << "endmodule\n";
        out.flush();
        return text;
    }

private:
    void collectPort()
    {
        auto add = [&](const QString &name, bool input, quint32 width) {
            if (portTable.contains(name)) {
                auto &old = portTable[name];
                if (old.width != width || (!input && old.direction == "output"))
                    reject(plan.input, "prcm.controller", "Conflicting resource port: " + name);
                if (!input)
                    old.direction = "output";
            } else {
                portTable.insert(name, {name, input ? "input" : "output", width});
            }
        };
        for (const auto &port : plan.clock.ports)
            add(port.name, port.isInput, static_cast<quint32>(port.width));
        for (const auto &port : QSocResetPrimitive::describePorts(plan.reset))
            add(port.name, port.isInput, static_cast<quint32>(port.width));
        add(supply.request, false, 1);
        add(supply.valid.signal, true, 1);
        add(domain.quiesce.request, false, 1);
        add(domain.quiesce.completion.signal, true, 1);
        add(domain.isolation.request, false, 1);
        add(domain.isolation.completion.signal, true, 1);
        QSet<QString> used;
        for (const auto &name : portTable.keys())
            used.insert(name);
        auto busPlan = mmio;
        busPlan.registers.clear();
        busPlan.clearPort.clear();
        for (const auto &port : QSocMmioGenerator::describePorts(busPlan)) {
            if (port.name == "clk_i" || port.name == "rst_ni")
                continue;
            if (used.contains(port.name))
                reject(
                    plan.input,
                    "prcm.mmio",
                    "Bus port conflicts with resource signal: " + port.name);
            portTable.insert(port.name, port);
            used.insert(port.name);
        }
        while (std::any_of(used.cbegin(), used.cend(), [&](const QString &name) {
            return name.startsWith(prefix);
        }))
            prefix += '_';
        portTable.remove(resource.clockEnable);
        portTable.remove(domain.reset.source);
    }

    static void emitInstance(
        QTextStream                  &out,
        const QString                &module,
        const QString                &name,
        const QMap<QString, QString> &connection)
    {
        QStringList port;
        for (auto it = connection.cbegin(); it != connection.cend(); ++it)
            port.append("    ." + it.key() + "(" + it.value() + ")");
        out << '\n' << module << ' ' << name << " (\n" << port.join(",\n") << "\n);\n";
    }

    void emitReset(QTextStream &out) const
    {
        const auto clock = plan.input.clockInput;
        const auto cold  = prefix + "cold_n";
        out << "wire " << cold << ";\n";
        out << "qsoc_rst_sync #(.STAGE(" << sampleStage << ")) " << prefix << "cold_inst (\n"
            << "    .clk(" << clock << "), .rst_in_n(" << plan.input.resetSource
            << "), .test_enable(1'b0), .rst_out_n(" << cold << ")\n);\n";
        QString management = "1'b0";
        for (const auto &target : plan.reset.targets) {
            if (target.name == plan.input.resetTarget)
                management = (target.active == "low" ? "!" : "") + target.name;
        }
        const auto reset = (resource.resetTargetActiveLow ? "!" : "") + domain.reset.target;
        for (const auto &item :
             {qMakePair(QString("clear"), management), qMakePair(QString("reset"), reset)}) {
            const auto sample = prefix + item.first + "_sample";
            out << "reg [" << sampleStage - 1 << ":0] " << sample << ";\n"
                << "always @(posedge " << clock << " or negedge " << cold << ") begin\n"
                << "    if (!" << cold << ") " << sample << " <= {" << sampleStage << "{1'b1}};\n"
                << "    else " << sample << " <= {" << sample << '[' << sampleStage - 2 << ":0], "
                << item.second << "};\nend\n"
                << "wire " << prefix << item.first << " = " << sample << '[' << sampleStage - 1
                << "];\n";
        }
    }

    void emitTarget(QTextStream &out) const
    {
        const auto width = mmio.registers[0].fields[0].width;
        out << "wire " << range(width) << prefix << "mode;\n"
            << "reg [1:0] " << prefix << "decoded;\n"
            << "reg " << prefix << "valid;\n"
            << "reg [1:0] " << prefix << "target_q;\n"
            << "always @* begin\n"
            << "    " << prefix << "valid = 1'b1;\n"
            << "    " << prefix << "decoded = 2'd0;\n"
            << "    case (" << prefix << "mode)\n";
        for (auto mode = action.mode.cbegin(); mode != action.mode.cend(); ++mode)
            out << "        " << literal(width, mode.key()) << ": " << prefix << "decoded = 2'd"
                << static_cast<int>(mode.value()) << ";\n";
        out << "        default: " << prefix << "valid = 1'b0;\n"
            << "    endcase\nend\n"
            << "always @(posedge " << plan.input.clockInput << " or negedge " << prefix
            << "cold_n) begin\n"
            << "    if (!" << prefix << "cold_n) " << prefix << "target_q <= 2'd0;\n"
            << "    else if (" << prefix << "valid) " << prefix << "target_q <= " << prefix
            << "decoded;\nend\n"
            << "wire [1:0] " << prefix << "target = " << prefix << "valid ? " << prefix
            << "decoded : " << prefix << "target_q;\n";
    }

    void emitAction(QTextStream &out) const
    {
        for (const auto &name : {"off", "held", "run", "fault", "watch", "reset_request"})
            out << "wire " << prefix << name << ";\n";
        emitInstance(
            out,
            "qsoc_prcm_domain",
            prefix + "action_inst",
            {{"clk_i", plan.input.clockInput},
             {"rst_ni", prefix + "cold_n"},
             {"target_off_i", prefix + "target == 2'd0"},
             {"target_reset_i", prefix + "target == 2'd1"},
             {"target_run_i", prefix + "target == 2'd2"},
             {"power_i", supply.valid.signal},
             {"reset_i", prefix + "reset"},
             {"isolation_i", domain.isolation.completion.signal},
             {"idle_i", domain.quiesce.completion.signal},
             {"power_o", supply.request},
             {"clock_o", resource.clockEnable},
             {"reset_o", prefix + "reset_request"},
             {"isolation_o", domain.isolation.request},
             {"quiesce_o", domain.quiesce.request},
             {"state_off_o", prefix + "off"},
             {"state_reset_o", prefix + "held"},
             {"state_run_o", prefix + "run"},
             {"fault_o", prefix + "fault"},
             {"power_watch_o", prefix + "watch"}});
        const auto power     = supply.valid.signal;
        const auto isolation = domain.isolation.completion.signal;
        const auto idle      = domain.quiesce.completion.signal;
        out << "assign " << domain.reset.source << " = "
            << (resource.resetSourceActiveLow ? "!" : "") << prefix << "reset_request;\n"
            << "wire " << prefix << "done = " << prefix << "valid && (\n"
            << "    (" << prefix << "decoded == 2'd0 && " << prefix << "off && !" << power << " && "
            << prefix << "reset && " << isolation << " && " << idle << ") ||\n"
            << "    (" << prefix << "decoded == 2'd1 && " << prefix << "held && " << power << " && "
            << prefix << "reset && " << isolation << " && " << idle << ") ||\n"
            << "    (" << prefix << "decoded == 2'd2 && " << prefix << "run && " << power << " && !"
            << prefix << "reset && !" << isolation << " && !" << idle << "));\n";
    }

    QMap<QString, QString> registerConnection() const
    {
        QMap<QString, QString> connection;
        for (const auto &port : QSocMmioGenerator::describePorts(mmio))
            connection.insert(port.name, port.name);
        connection["clk_i"]          = plan.input.clockInput;
        connection["rst_ni"]         = prefix + "cold_n";
        connection["clear_i"]        = prefix + "clear";
        connection["mode_o"]         = prefix + "mode";
        connection["request_i"]      = prefix + "mode";
        connection["done_i"]         = prefix + "done";
        connection["invalid_mode_i"] = "!" + prefix + "valid";
        connection["fault_i"]        = prefix + "fault";
        connection["power_lost_i"]   = prefix + "watch && !" + supply.valid.signal;
        return connection;
    }

    const QSocPrcmBindingPlan             &plan;
    const QSocPrcmSequencePlan            &action;
    const QSocMmioPlan                    &mmio;
    QString                                moduleName;
    int                                    sampleStage;
    QSocPrcmDomain                         domain;
    QSocPrcmDomainResource                 resource;
    QSocPrcmSupply                         supply;
    QString                                prefix = "prcm_";
    QMap<QString, QSocMmioPortDescription> portTable;
};

} // namespace

QSocPrcmGenerateResult QSocPrcmGenerator::generate(
    const QSocPrcmBindingPlan &binding, const QString &moduleName, int sampleStage)
{
    QSocPrcmGenerateResult result;
    const auto             sequence = QSocPrcmSequencePlanner::build(binding.input);
    if (!sequence.plan) {
        result.diagnostic = sequence.diagnostic;
        return result;
    }
    try {
        if (!QSocVerilogUtils::isValidVerilogIdentifier(moduleName))
            reject(binding.input, "prcm", "Module name must be a Verilog identifier.");
        if (sampleStage < 2)
            reject(binding.input, "prcm.controller", "Reset sampling requires at least two stages.");
        if (!binding.domain.contains(sequence.plan->domain))
            reject(
                binding.input,
                "prcm.domain",
                "Bind the domain resources before circuit generation.");
        const auto                           domain = binding.input.domain[sequence.plan->domain];
        const auto                           path   = "prcm.domain." + sequence.plan->domain;
        const QList<QPair<QString, QString>> feedback{
            {binding.input.supplyTable[domain.supply].valid.signal,
             "prcm.supply." + domain.supply + ".valid.signal"},
            {domain.quiesce.completion.signal, path + ".quiesce.ack.signal"},
            {domain.isolation.completion.signal, path + ".isolation.active.signal"}};
        QMap<QString, QString> feedbackOwner;
        for (const auto &value : feedback) {
            if (feedbackOwner.contains(value.first))
                reject(
                    binding.input,
                    value.second,
                    "This template needs distinct feedback signals: " + value.first,
                    feedbackOwner[value.first]);
            feedbackOwner.insert(value.first, value.second);
        }
        QSocPrcmCircuit circuit;
        circuit.mmio     = registerPlan(binding.input, *sequence.plan, moduleName + "_register");
        auto clock       = binding.clock;
        auto reset       = binding.reset;
        clock.moduleName = moduleName + "_clock";
        reset.moduleName = moduleName + "_reset";
        QSocClockPrimitive       clockGenerator;
        QSocResetPrimitive       resetGenerator;
        QSet<QString>            declared;
        const QRegularExpression module(
            "^\\s*module\\s+([A-Za-z_][A-Za-z_0-9$]*)\\s", QRegularExpression::MultilineOption);
        auto add = [&](const QString &name, const QString &rtl) {
            if (rtl.isEmpty() || circuit.rtl.contains(name))
                reject(binding.input, "prcm", "Cannot emit a unique RTL file: " + name);
            auto match = module.globalMatch(rtl);
            while (match.hasNext()) {
                const auto value = match.next().captured(1);
                if (declared.contains(value))
                    reject(binding.input, "prcm", "Generated module name is used twice: " + value);
                declared.insert(value);
            }
            circuit.rtl.insert(name, rtl);
        };
        add(clock.moduleName + ".v", clockGenerator.generateControllerVerilog(clock));
        add(reset.moduleName + ".v", resetGenerator.generateControllerVerilog(reset));
        add("clock_cell.v", clockGenerator.generateCellVerilog());
        add("reset_cell.v", resetGenerator.generateCellVerilog());
        add("qsoc_prcm_domain.v", QSocPrcmSequenceRtl::generate());
        add(circuit.mmio.moduleName + ".v", QSocMmioGenerator::generateVerilog(circuit.mmio));
        add(moduleName + ".v",
            Assembly(binding, *sequence.plan, circuit.mmio, moduleName, sampleStage).generate());
        result.circuit = std::move(circuit);
    } catch (const QSocPrcmDiagnostic &diagnostic) {
        result.diagnostic.append(diagnostic);
    }
    return result;
}
