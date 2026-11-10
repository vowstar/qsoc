// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprimitivereport.h"
#include "common/qsocconsole.h"
#include "common/qsocpaths.h"
#include "common/qsocsibling.h"

#include <QDir>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QProcess>
#include <QSaveFile>

namespace {
void detail(QJsonArray &rows, const QString &name, const QString &value)
{
    if (!value.isEmpty())
        rows.append(QJsonObject{{"name", name}, {"value", value}});
}

QString fallback(const QString &signal, const QString &value)
{
    return signal.isEmpty() ? value : signal;
}

QJsonObject component(const QString &kind, const QString &label, const QString &note = {})
{
    return {{"kind", kind}, {"label", label}, {"note", note}};
}

void guide(
    QJsonObject                             &part,
    QJsonArray                              &rows,
    const QSocClockPrimitive::ClockSTAGuide &config,
    const QString                           &owner)
{
    if (!config.configured)
        return;
    part["guide"] = true;
    detail(
        rows,
        owner + " STA guide",
        QString("cell: %1; in: %2; out: %3; instance: %4")
            .arg(
                fallback(config.cell, "qsoc_ck_buf"),
                config.cell.isEmpty() ? "clk_in" : config.in,
                config.cell.isEmpty() ? "clk_out" : config.out,
                fallback(config.instance, "automatic")));
}

QJsonArray clockSteps(
    const QSocClockPrimitive::ClockGate     &gate,
    const QSocClockPrimitive::ClockDivider  &div,
    const QSocClockPrimitive::ClockInverter &inv,
    QJsonArray                              &rows,
    const QString                           &owner,
    bool                                     branch)
{
    QJsonArray steps;
    if (gate.configured && (!branch || !gate.enable.isEmpty())) {
        QJsonObject part  = component("gate", "ICG");
        part["control"]   = fallback(gate.enable, "1'b1");
        part["activeLow"] = gate.polarity == "low";
        detail(
            rows,
            owner + " ICG",
            QString("enable: %1; polarity: %2; reset: %3; test: %4; clock during reset: %5")
                .arg(
                    gate.enable,
                    gate.polarity,
                    fallback(gate.reset, "1'b1"),
                    fallback(gate.test_enable, "1'b0"),
                    gate.clock_on_reset ? "yes" : "no"));
        guide(part, rows, gate.sta_guide, owner + " ICG");
        steps.append(part);
    }
    if (div.configured && (!branch || div.default_value > 1 || !div.value.isEmpty())) {
        QJsonObject part = component(
            "divider",
            div.value.isEmpty() ? QString::fromUtf8("÷%1").arg(div.default_value)
                                : QString::fromUtf8("÷N"));
        part["control"] = div.value;
        detail(
            rows,
            owner + " DIV",
            QString(
                "default: %1; width: %2; reset: %3; enable: %4; test: %5; clock during reset: %6")
                .arg(div.default_value)
                .arg(div.width)
                .arg(
                    fallback(div.reset, "1'b1"),
                    fallback(div.enable, "1'b1"),
                    fallback(div.test_enable, "1'b0"),
                    div.clock_on_reset ? "yes" : "no"));
        detail(
            rows,
            owner + " DIV interface",
            div.value.isEmpty() ? "Static division; count: " + fallback(div.count, "unconnected")
                                : QString("value: %1; valid: %2; ready: %3; count: %4")
                                      .arg(
                                          div.value,
                                          fallback(div.valid, "automatic update"),
                                          fallback(div.ready, "unconnected"),
                                          fallback(div.count, "unconnected")));
        guide(part, rows, div.sta_guide, owner + " DIV");
        steps.append(part);
    }
    if (inv.configured) {
        QJsonObject part = component("inverter", "INV");
        guide(part, rows, inv.sta_guide, owner + " INV");
        steps.append(part);
    }
    return steps;
}

template<typename T>
QJsonArray resetSteps(const T &config, QJsonArray &rows, const QString &owner)
{
    QJsonArray steps;
    if (!config.async.clock.isEmpty()) {
        auto part     = component("reset", "ASYNC", QString("%1 FF").arg(config.async.stage));
        part["clock"] = config.async.clock;
        steps.append(part);
        detail(rows, owner + " ASYNC", "clock: " + config.async.clock);
    } else if (!config.sync.clock.isEmpty()) {
        auto part     = component("reset", "SYNC", QString("%1 FF").arg(config.sync.stage));
        part["clock"] = config.sync.clock;
        steps.append(part);
        detail(rows, owner + " SYNC", "clock: " + config.sync.clock);
    } else if (!config.count.clock.isEmpty()) {
        auto part     = component("reset", "COUNT", QString("%1 cycles").arg(config.count.cycle));
        part["clock"] = config.count.clock;
        steps.append(part);
        detail(rows, owner + " COUNT", "clock: " + config.count.clock);
    }
    return steps;
}

class Graph
{
public:
    QJsonArray          nodes;
    QJsonArray          edges;
    QHash<QString, int> ports;

    int node(const QJsonObject &value)
    {
        nodes.append(value);
        return nodes.size() - 1;
    }
    void edge(int from, int to, int input = 0, int output = 0, const QString &style = {})
    {
        edges.append(
            QJsonObject{
                {"from", from}, {"to", to}, {"input", input}, {"output", output}, {"style", style}});
    }
    int source(const QString &signal, const QString &note = {}, bool separate = false)
    {
        if (separate)
            return node(component("input", signal, note));
        if (!ports.contains(signal))
            ports.insert(signal, node(component("input", signal, note)));
        return ports.value(signal);
    }
    int afterGuide(int from, bool enabled)
    {
        if (!enabled)
            return from;
        auto value       = component("buffer", "STA");
        value["guide"]   = true;
        const int buffer = node(value);
        edge(from, buffer);
        return buffer;
    }
    int chain(int from, const QJsonArray &parts)
    {
        for (const auto &part : parts) {
            auto       value    = part.toObject();
            const bool hasGuide = value.take("guide").toBool();
            const int  next     = node(value);
            edge(from, next);
            from = afterGuide(next, hasGuide);
        }
        return from;
    }
};

QJsonObject buildGraph(const QJsonArray &panels, const QJsonArray &sources)
{
    Graph      graph;
    QList<int> outputs;
    for (const auto &value : panels) {
        const auto panel  = value.toObject();
        const int  output = graph.node(
            component("output", panel["output"].toString(), panel["outputNote"].toString()));
        outputs.append(output);
        graph.ports.insert(panel["identity"].toString(panel["title"].toString()), output);
    }
    for (const auto &value : sources) {
        const auto source = value.toObject();
        graph.source(source["signal"].toString(), source["note"].toString());
    }
    for (qsizetype i = 0; i < panels.size(); ++i) {
        const auto  panel    = panels[i].toObject();
        const auto  branches = panel["branches"].toArray();
        QJsonObject merge    = panel["merge"].toObject();
        merge["inputs"]      = branches.size();
        const bool hasGuide  = merge.take("guide").toBool();
        const bool direct    = merge["kind"] == "wire";
        const int  join      = direct ? -1 : graph.node(merge);
        int        current   = -1;
        for (qsizetype j = 0; j < branches.size(); ++j) {
            const auto branch = branches[j].toObject();
            current           = graph.chain(
                graph.source(branch["source"].toString(), branch["note"].toString()),
                branch["steps"].toArray());
            if (!direct)
                graph.edge(current, join, j, 0, branch["style"].toString());
        }
        if (!direct)
            current = join;
        if (!merge["testClock"].toString().isEmpty()) {
            auto mux         = component("mux", "TEST");
            mux["inputs"]    = 2;
            mux["test"]      = true;
            mux["select"]    = merge["testEnable"];
            const int bypass = graph.node(mux);
            graph.edge(current, bypass);
            graph.edge(graph.source(merge["testClock"].toString(), {}, true), bypass, 1, 0, "test");
            current = bypass;
        }
        current        = graph.afterGuide(current, hasGuide);
        const int tail = graph.chain(current, panel["tail"].toArray());
        graph.edge(tail, outputs[i]);
        for (const auto &value : panel["exits"].toArray()) {
            const auto exit   = value.toObject();
            const int  target = graph.node(component("output", exit["signal"].toString()));
            graph.edge(join, target, 0, exit["port"].toInt());
        }
        for (const auto &value : panel["followers"].toArray()) {
            const auto follower = value.toObject();
            auto       pipe
                = component("reset", "RST PIPE", QString("%1 FF").arg(follower["stage"].toInt()));
            pipe["clock"]    = follower["clock"];
            const int sync   = graph.node(pipe);
            const int target = graph.node(
                component("output", follower["reset"].toString(), follower["clock"].toString()));
            graph.edge(join, sync, 0, 2);
            graph.edge(sync, target);
        }
    }
    for (const auto &value : graph.edges) {
        const int from = value.toObject()["from"].toInt();
        auto      node = graph.nodes[from].toObject();
        if (node["kind"] == "output") {
            node["kind"]      = "net";
            graph.nodes[from] = node;
        }
    }
    return {{"nodes", graph.nodes}, {"edges", graph.edges}};
}

QJsonObject report(
    const QString    &kind,
    const QString    &name,
    const QJsonArray &panels,
    const QJsonArray &sources = {})
{
    return {
        {"version", 1},
        {"kind", kind},
        {"name", name},
        {"panels", panels},
        {"sources", sources},
        {"graph", buildGraph(panels, sources)}};
}
} // namespace

QJsonObject QSocPrimitiveReport::graph(const QJsonArray &panels)
{
    auto result       = buildGraph(panels, {});
    result["circuit"] = true;
    return result;
}

QJsonObject QSocPrimitiveReport::clock(const QSocClockPrimitive::ClockControllerConfig &config)
{
    QJsonArray panels;
    for (const auto &target : config.targets) {
        QJsonArray rows, branches, tail;
        detail(rows, "Target", target.name);
        detail(rows, "Output frequency", target.freq);
        detail(rows, "Comment", target.comment);
        for (qsizetype i = 0; i < target.links.size(); ++i) {
            const auto      &link  = target.links[i];
            const QString    owner = QString("Input %1").arg(i);
            const QJsonArray steps = clockSteps(link.icg, link.div, link.inv, rows, owner, true);
            detail(rows, owner + " source", link.source);
            QString frequency;
            for (const auto &input : config.inputs) {
                if (input.name != link.source)
                    continue;
                frequency = input.freq;
                detail(rows, owner + " comment", input.comment);
                detail(rows, owner + " frequency", input.freq);
                detail(rows, owner + " duty", input.duty);
            }
            branches.append(
                QJsonObject{{"source", link.source}, {"note", frequency}, {"steps", steps}});
        }
        QJsonObject merge = component("wire", "");
        if (target.links.size() > 1) {
            const bool gf       = target.mux.type == QSocClockPrimitive::GF_MUX;
            merge               = component("mux", gf ? "GF" : "STD");
            merge["select"]     = target.select;
            merge["testEnable"] = fallback(target.test_enable, "1'b0");
            if (gf && !target.test_clock.isEmpty())
                merge["testClock"] = target.test_clock;
            detail(rows, "MUX select", target.select + QString(" [%1 bits]").arg(target.select_width));
            if (gf) {
                detail(rows, "MUX reset", fallback(target.reset, "1'b1"));
                detail(rows, "MUX test clock", fallback(target.test_clock, "1'b0"));
                detail(rows, "MUX test enable", fallback(target.test_enable, "1'b0"));
            }
            guide(merge, rows, target.mux.sta_guide, "MUX");
        }
        const QJsonArray outputSteps
            = clockSteps(target.icg, target.div, target.inv, rows, "Output", false);
        for (const auto &step : outputSteps)
            tail.append(step);
        panels.append(
            QJsonObject{
                {"title", target.name},
                {"subtitle", "Clock signal path"},
                {"branches", branches},
                {"merge", merge},
                {"tail", tail},
                {"output", target.name},
                {"outputNote", target.freq},
                {"details", rows}});
    }
    QJsonArray sources;
    for (const auto &input : config.inputs)
        sources.append(QJsonObject{{"signal", input.name}, {"note", input.freq}});
    return report("Clock", config.moduleName, panels, sources);
}

QJsonObject QSocPrimitiveReport::reset(const QSocResetPrimitive::ResetControllerConfig &config)
{
    QJsonArray panels;
    for (const auto &target : config.targets) {
        QJsonArray rows, branches;
        detail(rows, "Test enable", fallback(config.testEnable, "1'b0"));
        detail(rows, "Target", target.name);
        detail(rows, "Output polarity", "Active " + target.active);
        for (const auto &link : target.links) {
            QString active = "low";
            for (const auto &source : config.sources) {
                if (source.name == link.source)
                    active = source.active;
            }
            detail(rows, "Source (active " + active + ")", link.source);
            QJsonArray steps;
            if (active == "high")
                steps.append(component("inverter", "INV"));
            for (const auto &step : resetSteps(link, rows, link.source))
                steps.append(step);
            branches.append(
                QJsonObject{{"source", link.source}, {"note", "active " + active}, {"steps", steps}});
        }
        if (branches.isEmpty())
            branches.append(
                QJsonObject{{"source", "1'b1"}, {"note", "inactive"}, {"steps", QJsonArray()}});
        QJsonArray tail = resetSteps(target, rows, "Output");
        if (target.active != "low")
            tail.append(component("inverter", "INV"));
        if (config.reason.enabled) {
            detail(
                rows,
                "Reset reason",
                QString("output: %1; valid: %2; clear: %3; clock: %4; root: %5")
                    .arg(
                        config.reason.output,
                        config.reason.valid,
                        config.reason.clear,
                        config.reason.clock,
                        config.reason.rootReset));
            for (qsizetype i = 0; i < config.reason.sourceOrder.size(); ++i)
                detail(rows, QString("Reason bit %1").arg(i), config.reason.sourceOrder[i]);
        }
        panels.append(
            QJsonObject{
                {"title", target.name},
                {"subtitle", "Reset assertion and release"},
                {"branches", branches},
                {"merge", component(target.links.size() > 1 ? "and" : "wire", "AND")},
                {"tail", tail},
                {"output", target.name},
                {"outputNote", "active " + target.active},
                {"details", rows}});
    }
    QJsonArray sources;
    for (const auto &source : config.sources)
        sources.append(QJsonObject{{"signal", source.name}, {"note", "active " + source.active}});
    return report("Reset", config.moduleName, panels, sources);
}

QJsonObject QSocPrimitiveReport::power(const QSocPowerPrimitive::PowerControllerConfig &config)
{
    using Kind = QSocPowerPrimitive::DomainKind;
    QJsonArray panels;
    for (const auto &domain : config.domains) {
        const bool    ao   = domain.kind == Kind::AlwaysOn;
        const QString kind = ao                          ? "AO / always on"
                             : domain.kind == Kind::Root ? "ROOT / controllable"
                                                         : "DEPENDENT";
        QJsonArray    rows, branches;
        detail(rows, "Host clock", config.host_clock);
        detail(rows, "Host reset", config.host_reset);
        detail(rows, "Test enable", fallback(config.test_enable, "1'b0"));
        detail(rows, "Domain", domain.name);
        detail(rows, "Domain kind", kind);
        if (domain.v_mv > 0)
            detail(rows, "Voltage", QString::number(domain.v_mv) + " mV");
        detail(rows, "Control enable", ao ? "1'b1" : "en_" + domain.name);
        detail(rows, "Fault clear", ao ? "1'b0" : "clr_" + domain.name);
        detail(rows, "Power good", fallback(domain.pgood, "1'b1"));
        detail(
            rows,
            "Timing",
            QString("wait_dep: %1; settle_on: %2; settle_off: %3 host clock cycles")
                .arg(domain.wait_dep)
                .arg(domain.settle_on)
                .arg(domain.settle_off));
        for (const auto &dep : domain.depends) {
            if (dep.type != "hard" && dep.type != "soft") {
                detail(rows, "Ignored dependency", dep.name + " (type: " + dep.type + ")");
                continue;
            }
            branches.append(
                QJsonObject{
                    {"source", "domain:" + dep.name}, {"style", dep.type}, {"steps", QJsonArray()}});
            detail(rows, dep.type + " dependency", "rdy_" + dep.name);
        }
        if (branches.isEmpty())
            branches.append(
                QJsonObject{
                    {"source", ao ? "1'b1" : "en_" + domain.name},
                    {"note", "enable"},
                    {"steps", QJsonArray()}});
        detail(
            rows,
            "Dependency policy",
            "Hard dependencies block start and fault on timeout. Soft dependencies can time out "
            "with a fault flag.");
        detail(
            rows,
            "Power-on order",
            "OFF, WAIT_DEP, TURN_ON, CLK_ON, ON. Clock enable precedes reset release.");
        detail(
            rows,
            "Power-off order",
            "RST_ASSERT, TURN_OFF, OFF. Reset assertion precedes clock disable and switch off.");
        detail(
            rows,
            "Fault recovery",
            "FAULT retries after cooldown when hard dependencies are ready.");
        detail(
            rows,
            "Outputs",
            QString("%1icg_en_%2; rst_gate_%2_n; rdy_%2; flt_%2")
                .arg(ao ? QString() : "sw_" + domain.name + "; ", domain.name));
        for (const auto &follow : domain.follow_entries)
            detail(
                rows,
                "Follow reset",
                QString("%1: qsoc_rst_pipe, %2 stages on %3")
                    .arg(follow.reset)
                    .arg(follow.stage)
                    .arg(follow.clock));
        QJsonArray exits, followers;
        for (const auto &entry : domain.follow_entries)
            followers.append(
                QJsonObject{{"clock", entry.clock}, {"reset", entry.reset}, {"stage", entry.stage}});
        if (followers.isEmpty())
            exits.append(QJsonObject{{"signal", "rst_gate_" + domain.name + "_n"}, {"port", 2}});
        exits.append(QJsonObject{{"signal", "icg_en_" + domain.name}, {"port", 1}});
        exits.append(QJsonObject{{"signal", "flt_" + domain.name}, {"port", 4}});
        if (!ao)
            exits.append(QJsonObject{{"signal", "sw_" + domain.name}, {"port", 3}});
        QJsonObject fsm = component(
            "power",
            domain.name,
            ao                          ? "AO"
            : domain.kind == Kind::Root ? "ROOT"
                                        : "SWITCHED");
        fsm["outputs"] = 5;
        panels.append(
            QJsonObject{
                {"title", domain.name},
                {"identity", "domain:" + domain.name},
                {"subtitle", kind},
                {"branches", branches},
                {"merge", fsm},
                {"tail", QJsonArray()},
                {"output", "rdy_" + domain.name},
                {"followers", followers},
                {"exits", exits},
                {"details", rows}});
    }
    return report("Power", config.moduleName, panels);
}

bool QSocPrimitiveReport::stage(
    const QString &directory, const QString &name, const QJsonObject &data)
{
    const auto artifact = QSocPaths::resolveArtifactPath(directory, name + ".report.json");
    if (!artifact.isValid()) {
        QSocConsole::warn() << artifact.error;
        return false;
    }
    QSaveFile        file(artifact.path);
    const QByteArray bytes = QJsonDocument(data).toJson(QJsonDocument::Compact);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        QSocConsole::warn() << "Cannot stage primitive report:" << file.errorString();
        return false;
    }
    return true;
}

void QSocPrimitiveReport::render(const QString &directory)
{
    const QString program = QSocSibling::path("qsoc-gui");
    if (program.isEmpty()) {
        if (!QDir(directory).entryList({"*.report.json"}, QDir::Files).isEmpty())
            QSocConsole::warn() << QSocSibling::missingMessage("qsoc-gui");
        return;
    }
    for (const QFileInfo &request : QDir(directory).entryInfoList({"*.report.json"}, QDir::Files)) {
        const QString base = request.filePath().chopped(QString(".report.json").size());
        QProcess      process;
        process.setProgram(program);
        process.setArguments({"--render-primitive-report", request.filePath(), base});
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start();
        const int  timeout  = 60000 + static_cast<int>(qMin<qint64>(240000, request.size() / 8));
        const bool started  = process.waitForStarted(5000);
        const bool finished = started && process.waitForFinished(timeout);
        if (finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0)
            continue;
        const QString reason = started && !finished
                                   ? QString("Timed out after %1 seconds").arg(timeout / 1000)
                                   : process.errorString();
        process.kill();
        process.waitForFinished();
        QFile::remove(base + ".html");
        QFile::remove(base + ".pdf");
        QSocConsole::warn() << "Primitive report export failed for" << request.completeBaseName()
                            << reason << QString::fromUtf8(process.readAll());
    }
}
