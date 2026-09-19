// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmbinding.h"
#include "common/qsocprcmreader.h"

#include <QSet>

namespace {

using QSocPrcmDetail::Context;
using QSocPrcmDetail::Reader;

[[noreturn]] void conflict(const Reader &field, const QString &message, const QSocPrcmSource &other)
{
    throw QSocPrcmDiagnostic{"PRCM_RESOURCE_CONFLICT", message, {field.position(), other}};
}

void unsupported(const Reader &field, const QString &message)
{
    field.fail("PRCM_RESOURCE_UNSUPPORTED", message);
}

Reader controller(const Reader &root, const QString &kind, const Reader &reference)
{
    const auto                    wanted = reference.name();
    const auto                    table  = root.member(kind);
    QMap<QString, QSocPrcmSource> found;
    std::optional<Reader>         result;
    for (std::size_t i = 0; i < table.size(); ++i) {
        const auto item = table.item(i);
        item.keys();
        const auto name = item.member("name").name();
        if (found.contains(name)) {
            conflict(item.member("name"), "Controller is declared twice: " + name, found.value(name));
        }
        found.insert(name, item.member("name").position());
        if (name == wanted) {
            result.emplace(item);
        }
    }
    if (!result) {
        reference.fail("PRCM_RESOURCE_REFERENCE", "Unknown " + kind + " controller: " + wanted);
    }
    return *result;
}

void checkClockDeclaration(const Reader &node)
{
    node.fields({"name", "input", "target"}, {"test_enable", "ref_clock"});
    if (node.has("test_enable") || node.has("ref_clock")) {
        unsupported(
            node,
            "PRCM clock binding currently requires a gate without a test or reference clock port.");
    }
    const auto input = node.member("input");
    for (const auto &name : input.table()) {
        input.member(name).fields({}, {"freq", "duty", "comment"});
    }
    const auto target = node.member("target");
    for (const auto &name : target.table()) {
        const auto item = target.member(name);
        if (input.has(name)) {
            conflict(
                item,
                "Clock target conflicts with an input: " + name,
                input.member(name).position());
        }
        item.fields(
            {"icg", "link"},
            {"freq", "comment", "div", "inv", "select", "reset", "test_enable", "test_clock"});
        for (const auto &field : {"div", "inv", "select", "reset", "test_enable", "test_clock"}) {
            if (item.has(field)) {
                unsupported(
                    item.member(field),
                    "PRCM clock binding currently requires a direct target gate.");
            }
        }
        const auto gate = item.member("icg");
        gate.fields({"enable", "reset"}, {"polarity", "clock_on_reset"});
        gate.member("enable").name();
        gate.member("reset").name();
        for (const auto &control : {"enable", "reset"}) {
            const auto field  = gate.member(control);
            const auto signal = field.name();
            if (target.has(signal)) {
                conflict(
                    field,
                    "Clock control conflicts with an output: " + signal,
                    target.member(signal).position());
            }
        }
        if (gate.has("polarity") && gate.member("polarity").text() != "high") {
            unsupported(
                gate.member("polarity"),
                "PRCM clock binding currently requires a positive-edge clock.");
        }
        if (gate.has("clock_on_reset") && gate.member("clock_on_reset").choice("true", "false")) {
            unsupported(
                gate.member("clock_on_reset"),
                "PRCM clock binding currently requires clock_on_reset to be false.");
        }
        const auto link  = item.member("link");
        const auto names = link.table();
        if (names.size() != 1) {
            unsupported(link, "PRCM clock binding currently requires one direct clock input.");
        }
        link.member(names[0]).fields({});
    }
}

void checkResetDeclaration(const Reader &node)
{
    node.fields({"name", "source", "target"}, {"test_enable", "reason"});
    if (node.has("test_enable") || node.has("reason")) {
        unsupported(
            node,
            "PRCM reset binding currently requires a synchronizer without test or reason ports.");
    }
    const auto source = node.member("source");
    for (const auto &name : source.table()) {
        const auto item = source.member(name);
        item.fields({"active"});
        item.member("active").choice("low", "high");
    }
    const auto    target = node.member("target");
    QSet<QString> linked;
    for (const auto &name : target.table()) {
        const auto item = target.member(name);
        item.fields({"active", "async", "link"}, {"sync", "count"});
        if (item.has("sync") || item.has("count")) {
            unsupported(
                item,
                "Use one async reset stage at this target. Multiple processing stages at one point "
                "are not supported.");
        }
        item.member("active").choice("low", "high");
        const auto sync = item.member("async");
        sync.fields({"clock", "stage"});
        sync.member("clock").name();
        if (sync.member("stage").number(std::numeric_limits<int>::max()) < 2) {
            sync.member("stage").fail(
                "PRCM_RESOURCE_VALUE",
                "Reset release requires at least two synchronization stages.");
        }
        const auto link = item.member("link");
        for (const auto &input : link.table()) {
            linked.insert(input);
            link.member(input).fields({});
            if (!source.has(input)) {
                link.member(input).fail("PRCM_RESOURCE_REFERENCE", "Declare reset source: " + input);
            }
        }
    }
    for (const auto &name : target.table()) {
        const auto field  = target.member(name).member("async").member("clock");
        const auto signal = field.name();
        if (target.has(signal)) {
            conflict(
                field,
                "Reset clock conflicts with a target: " + signal,
                target.member(signal).position());
        }
        if (linked.contains(signal)) {
            conflict(
                field,
                "Reset clock conflicts with a reset source: " + signal,
                source.member(signal).position());
        }
    }
}

template<typename Port>
void requirePort(const QList<Port> &ports, const QString &name, bool isInput, const Reader &field)
{
    for (const auto &port : ports) {
        if (port.name == name && port.isInput == isInput && port.width == 1) {
            return;
        }
    }
    field.fail(
        "PRCM_RESOURCE_ROLE",
        "Expected a one-bit " + QString(isInput ? "input: " : "output: ") + name);
}

class Binding
{
public:
    Binding(QSocPrcmBindingPlan &value, const Reader &rootNode, Context &state)
        : plan(value)
        , root(rootNode)
        , context(state)
        , clock(controller(
              root,
              "clock",
              root.member("prcm").member("controller").member("clock").member("controller")))
        , reset(controller(
              root,
              "reset",
              root.member("prcm").member("controller").member("reset").member("controller")))
    {}

    void run()
    {
        checkClockDeclaration(clock);
        checkResetDeclaration(reset);
        QSocClockPrimitive clockGenerator;
        QSocResetPrimitive resetGenerator;
        plan.clock = clockGenerator.parseClockConfig(clock.value());
        plan.reset = resetGenerator.parseResetConfig(reset.value());
        if (!plan.clock.valid) {
            clock.fail("PRCM_RESOURCE", "Clock declaration is invalid.");
        }
        if (!plan.reset.valid) {
            reset.fail("PRCM_RESOURCE", "Reset declaration is invalid.");
        }
        if (plan.clock.moduleName == plan.reset.moduleName) {
            conflict(
                reset.member("name"),
                "Clock and reset modules need distinct names.",
                clock.member("name").position());
        }
        QSocMmioPlan mmio;
        mmio.bus          = plan.input.bus;
        mmio.dataWidth    = plan.input.dataWidth;
        mmio.addressWidth = plan.input.addressWidth;
        QStringList errors;
        if (!QSocMmioGenerator::validateInterface(mmio, &errors)) {
            root.member("prcm")
                .member("mmio")
                .fail("PRCM_MMIO", errors.join('\n').replace("generator.", "prcm.mmio."));
        }
        checkManagement();
        bindOutput();
        checkExternal();
        for (auto domain = plan.input.domain.cbegin(); domain != plan.input.domain.cend();
             ++domain) {
            bindDomain(domain.key(), domain.value());
        }
        requirePort(
            QSocResetPrimitive::describePorts(plan.reset),
            plan.input.resetSource,
            true,
            root.member("prcm").member("controller").member("reset"));
        plan.input.source = context.source;
    }

private:
    Reader domainField(const QString &name) const
    {
        return root.member("prcm").member("domain").member(name);
    }

    void claim(const QString &id, const Reader &field)
    {
        if (owner.contains(id)) {
            conflict(field, "Resource already has a controller: " + id, owner.value(id));
        }
        owner.insert(id, field.position());
    }

    void input(const QString &name, const Reader &field)
    {
        if (output.contains(name)) {
            conflict(field, "Feedback must not use a request output: " + name, output.value(name));
        }
        if (!external.contains(name)) {
            external.insert(name, field.position());
        }
    }

    void drive(const QString &name, const Reader &field)
    {
        if (external.contains(name)) {
            conflict(field, "Control output conflicts with an input: " + name, external.value(name));
        }
        if (output.contains(name)) {
            conflict(field, "Control output is assigned twice: " + name, output.value(name));
        }
        output.insert(name, field.position());
    }

    void feedback(const QSocPrcmFeedback &value, const Reader &field)
    {
        if (value.sampleClock != plan.input.clockInput) {
            unsupported(
                field.member("sample_clock"),
                "Feedback must use the controller clock. An asynchronous feedback contract is not "
                "supported yet.");
        }
        if (value.signal == plan.input.clockInput || value.signal == plan.input.resetSource) {
            field.member("signal").fail(
                "PRCM_RESOURCE_ROLE",
                "A clock or reset source cannot serve as completion feedback.");
        }
        input(value.signal, field.member("signal"));
    }

    void checkManagement()
    {
        const auto field  = root.member("prcm").member("controller");
        const auto supply = plan.input.supplyTable.constFind(plan.input.supply);
        if (supply == plan.input.supplyTable.cend() || !supply->alwaysOn) {
            field.member("supply")
                .fail("PRCM_RESOURCE_REFERENCE", "The controller needs a declared always-on supply.");
        }
        if (!clock.member("input").has(plan.input.clockInput)) {
            field.member("clock").member("input").fail(
                "PRCM_RESOURCE_REFERENCE",
                "Unknown controller clock input: " + plan.input.clockInput);
        }
        if (!reset.member("source").has(plan.input.resetSource)) {
            field.member("reset").member("source").fail(
                "PRCM_RESOURCE_REFERENCE",
                "Unknown controller reset source: " + plan.input.resetSource);
        }
        if (plan.input.clockInput == plan.input.resetSource) {
            conflict(
                field.member("reset").member("source"),
                "Clock and reset inputs need distinct signals.",
                field.member("clock").member("input").position());
        }
        input(plan.input.clockInput, field.member("clock").member("input"));
        input(plan.input.resetSource, field.member("reset").member("source"));
        requirePort(plan.clock.ports, plan.input.clockInput, true, field.member("clock"));
        for (const auto &source : plan.clock.inputs) {
            input(source.name, clock.member("input").member(source.name));
        }
        checkManagementReset();
    }

    void checkManagementReset()
    {
        if (plan.input.resetTarget.isEmpty())
            return;
        const auto field = root.member("prcm").member("controller").member("reset").member("target");
        const auto target = reset.member("target");
        if (!target.has(plan.input.resetTarget)) {
            field.fail(
                "PRCM_RESOURCE_REFERENCE",
                "Unknown management reset target: " + plan.input.resetTarget);
        }
        const auto selected = target.member(plan.input.resetTarget);
        if (selected.member("async").member("clock").name() != plan.input.clockInput) {
            selected.member("async").member("clock").fail(
                "PRCM_RESOURCE_REFERENCE", "Management reset must use the controller clock input.");
        }
        const auto link = selected.member("link");
        if (!link.has(plan.input.resetSource)) {
            link.fail(
                "PRCM_RESOURCE_REFERENCE", "Management reset must include the cold reset source.");
        }
        requirePort(
            QSocResetPrimitive::describePorts(plan.reset), plan.input.resetTarget, false, field);
        claim("reset." + plan.reset.name + "." + plan.input.resetTarget, field);
        for (const auto &name : link.table())
            input(name, link.member(name));
    }

    void bindOutput()
    {
        for (const auto &port : plan.clock.ports) {
            if (!port.isInput) {
                drive(port.name, clock.member("target").member(port.name));
            }
        }
        for (const auto &port : QSocResetPrimitive::describePorts(plan.reset)) {
            if (!port.isInput) {
                drive(port.name, reset.member("target").member(port.name));
            }
        }
    }

    void checkExternal()
    {
        const auto prcm = root.member("prcm");
        for (auto supply = plan.input.supplyTable.cbegin(); supply != plan.input.supplyTable.cend();
             ++supply) {
            if (!supply->alwaysOn) {
                const auto field = prcm.member("supply").member(supply.key());
                drive(supply->request, field.member("request"));
                feedback(supply->valid, field.member("valid"));
            }
        }
        for (auto domain = plan.input.domain.cbegin(); domain != plan.input.domain.cend();
             ++domain) {
            const auto field = domainField(domain.key());
            drive(domain->quiesce.request, field.member("quiesce").member("request"));
            feedback(domain->quiesce.completion, field.member("quiesce").member("ack"));
            drive(domain->isolation.request, field.member("isolation").member("request"));
            feedback(domain->isolation.completion, field.member("isolation").member("active"));
        }
    }

    void bindDomain(const QString &name, const QSocPrcmDomain &domain)
    {
        const auto field = domainField(name);
        if (!plan.input.supplyTable.contains(domain.supply)) {
            field.member("supply")
                .fail("PRCM_RESOURCE_REFERENCE", "Unknown supply: " + domain.supply);
        }
        if (domain.clock.controller != plan.clock.name
            || domain.reset.controller != plan.reset.name) {
            controller(root, "clock", field.member("clock").member("controller"));
            controller(root, "reset", field.member("reset").member("controller"));
            unsupported(
                field,
                "PRCM currently binds each domain through its management clock and reset "
                "controllers.");
        }
        if (domain.clock.stage != "target.icg") {
            unsupported(
                field.member("clock").member("stage"), "Supported clock control stage: target.icg.");
        }
        if (!clock.member("target").has(domain.clock.target)) {
            field.member("clock")
                .member("target")
                .fail("PRCM_RESOURCE_REFERENCE", "Unknown clock target: " + domain.clock.target);
        }
        if (!reset.member("target").has(domain.reset.target)
            || !reset.member("source").has(domain.reset.source)) {
            field.member("reset").fail("PRCM_RESOURCE_REFERENCE", "Unknown reset source or target.");
        }
        const auto clockTarget = clock.member("target").member(domain.clock.target);
        const auto resetTarget = reset.member("target").member(domain.reset.target);
        const auto gate        = clockTarget.member("icg");
        const auto resetLink   = resetTarget.member("link");
        if (clockTarget.member("link").table() != QStringList{plan.input.clockInput}) {
            unsupported(
                clockTarget.member("link"),
                "The controlled gate must use the management clock directly.");
        }
        if (gate.member("reset").name() != plan.input.resetSource) {
            gate.member("reset").fail(
                "PRCM_RESOURCE_REFERENCE", "The controlled gate must use the management reset.");
        }
        if (reset.member("source").member(plan.input.resetSource).member("active").text() != "low") {
            unsupported(
                reset.member("source").member(plan.input.resetSource),
                "The gate reset port requires an active-low management reset.");
        }
        if (domain.reset.source == plan.input.resetSource) {
            conflict(
                field.member("reset").member("source"),
                "A domain must not drive the management reset.",
                reset.member("source").member(plan.input.resetSource).position());
        }
        if (!resetLink.has(domain.reset.source) || !resetLink.has(plan.input.resetSource)) {
            resetLink.fail(
                "PRCM_RESOURCE_REFERENCE",
                "The reset target must include its domain request and management reset.");
        }
        if (resetTarget.member("async").member("clock").name() != domain.clock.target) {
            resetTarget.member("async").member("clock").fail(
                "PRCM_RESOURCE_REFERENCE",
                "Reset release must use the domain clock target: " + domain.clock.target);
        }
        const auto enable = gate.member("enable").name();
        requirePort(plan.clock.ports, enable, true, gate.member("enable"));
        requirePort(plan.clock.ports, domain.clock.target, false, field.member("clock"));
        const auto resetPorts = QSocResetPrimitive::describePorts(plan.reset);
        requirePort(resetPorts, domain.reset.source, true, field.member("reset"));
        requirePort(resetPorts, domain.reset.target, false, field.member("reset"));
        claim("clock." + plan.clock.name + "." + domain.clock.target, field.member("clock"));
        claim("clock." + plan.clock.name + "." + enable, gate.member("enable"));
        claim("reset." + plan.reset.name + "." + domain.reset.target, field.member("reset"));
        claim(
            "reset." + plan.reset.name + "." + domain.reset.source,
            field.member("reset").member("source"));
        checkFanout(domain, enable, gate, field);
        drive(enable, gate.member("enable"));
        drive(domain.reset.source, field.member("reset").member("source"));
        plan.domain.insert(
            name,
            {enable,
             reset.member("source").member(domain.reset.source).member("active").text() == "low",
             resetTarget.member("active").text() == "low"});
    }

    void checkFanout(
        const QSocPrcmDomain &domain, const QString &enable, const Reader &gate, const Reader &field)
    {
        for (const auto &source : plan.clock.inputs) {
            if (source.name == enable) {
                conflict(
                    gate.member("enable"),
                    "A domain must not drive a clock input.",
                    clock.member("input").member(source.name).position());
            }
        }
        for (const auto &target : plan.clock.targets) {
            if (target.name != domain.clock.target && target.icg.enable == enable) {
                conflict(
                    gate.member("enable"),
                    "Clock enable also controls another target.",
                    clock.member("target")
                        .member(target.name)
                        .member("icg")
                        .member("enable")
                        .position());
            }
            if (target.icg.reset == enable) {
                conflict(
                    gate.member("enable"),
                    "Clock enable also serves as a reset input.",
                    clock.member("target")
                        .member(target.name)
                        .member("icg")
                        .member("reset")
                        .position());
            }
        }
        for (const auto &target : plan.reset.targets) {
            for (const auto &link : target.links) {
                if (target.name != domain.reset.target && link.source == domain.reset.source) {
                    conflict(
                        reset.member("target").member(target.name).member("link").member(link.source),
                        "Domain reset also controls another target.",
                        field.member("reset").position());
                }
            }
        }
    }

    QSocPrcmBindingPlan          &plan;
    const Reader                 &root;
    Context                      &context;
    Reader                        clock;
    Reader                        reset;
    QMap<QString, QSocPrcmSource> owner;
    QMap<QString, QSocPrcmSource> external;
    QMap<QString, QSocPrcmSource> output;
};

} // namespace

QSocPrcmBindingResult QSocPrcmBinding::resolve(
    const YAML::Node &netlist, const QString &file, const QMap<QString, QSocPrcmSource> &origin)
{
    QSocPrcmBindingResult result;
    auto                  parsed = QSocPrcmParser::parse(netlist, file, origin);
    if (!parsed.input) {
        result.diagnostic = std::move(parsed.diagnostic);
        return result;
    }
    Context context{file, parsed.input->source, origin};
    try {
        const Reader root(netlist, {}, context);
        if (root.has("power") && root.member("power").size() != 0) {
            unsupported(
                root.member("power"),
                "A composite power controller cannot share this PRCM binding yet.");
        }
        QSocPrcmBindingPlan plan;
        plan.input = std::move(*parsed.input);
        Binding(plan, root, context).run();
        result.plan = std::move(plan);
    } catch (const QSocPrcmDiagnostic &diagnostic) {
        result.diagnostic.append(diagnostic);
    } catch (const YAML::Exception &error) {
        result.diagnostic.append(
            {"PRCM_YAML",
             QString::fromStdString(error.msg),
             {{file, {}, error.mark.line + 1, error.mark.column + 1}}});
    }
    return result;
}
