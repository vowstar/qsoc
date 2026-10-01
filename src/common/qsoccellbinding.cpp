// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoccellbinding.h"
#include "common/qsoccelllibrary.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"

namespace {

using Cell    = QSocCellBinding::Cell;
using Binding = QSocCellBinding::Binding;

struct RoleSpec
{
    const char *role;
    const char *function; /**< clk_out of a combinational role, empty otherwise */
    const char *ports;
};

const RoleSpec kRoles[] = {
    {"qsoc_ck_buf", "clk_in", "clk_in clk_out"},
    {"qsoc_ck_inv", "!clk_in", "clk_in clk_out"},
    {"qsoc_ck_or2", "clk_in0 | clk_in1", "clk_in0 clk_in1 clk_out"},
    {"qsoc_ck_xor2", "clk_in0 ^ clk_in1", "clk_in0 clk_in1 clk_out"},
    {"qsoc_ck_mux2", "clk_sel ? clk_in1 : clk_in0", "clk_in0 clk_in1 clk_sel clk_out"},
    {"qsoc_ck_icg_pos", "", "clk en test_en clk_out"},
    {"qsoc_ck_icg_neg", "", "clk en test_en clk_out"},
    {"qsoc_sync", "", "clk rst_n d q"},
};

/* Sequential type to its role, and template key to role port. */
const QMap<QString, QString> kTypeRole
    = {{"icg_pos", "qsoc_ck_icg_pos"}, {"icg_neg", "qsoc_ck_icg_neg"}, {"sync", "qsoc_sync"}};
const QMap<QString, QString> kGatePort
    = {{"clock", "clk"}, {"enable", "en"}, {"test", "test_en"}, {"output", "clk_out"}};
const QMap<QString, QString> kSyncPort
    = {{"clock", "clk"}, {"data", "d"}, {"reset", "rst_n"}, {"output", "q"}};

QString scalar(const YAML::Node &node)
{
    return node && node.IsScalar() ? QString::fromStdString(node.Scalar()) : QString();
}

bool isGate(const QString &type)
{
    return type == "icg_pos" || type == "icg_neg";
}

QSocCellTable roleTable(const RoleSpec &spec)
{
    QSocCellPorts ports;
    for (const QString &port : QString(spec.ports).split(' '))
        ports.insert(port, port == "clk_out" ? "out" : "in");
    YAML::Node row;
    row["clk_out"] = spec.function;
    return QSocCellTable::parse(row, ports, spec.role);
}

/* One instance with named connections in pin order. */
QString instance(
    const QString                &module,
    const QString                &name,
    const QMap<QString, QString> &connect,
    const QString                &indent,
    bool                          keep)
{
    QStringList lines;
    for (auto it = connect.constBegin(); it != connect.constEnd(); ++it)
        lines.append(QString("%1    .%2(%3)").arg(indent, it.key(), it.value()));
    return (keep ? indent + "(* dont_touch = \"true\" *)\n" : QString())
           + QString("%1%2 %3 (\n%4\n%1);\n").arg(indent, module, name, lines.join(",\n"));
}

QMap<QString, QString> withTies(const Cell &cell, QMap<QString, QString> connect)
{
    for (auto it = cell.tie.constBegin(); it != cell.tie.constEnd(); ++it)
        connect.insert(it.key(), it.value() ? "1'b1" : "1'b0");
    return connect;
}

/* A gate role from the other polarity between two inverters. */
QString composedGate(const QString &inner)
{
    return "    wire clk_n;\n    wire gated_n;\n"
           + instance(
               "qsoc_ck_inv", "u_inv_in", {{"clk_in", "clk"}, {"clk_out", "clk_n"}}, "    ", false)
           + instance(
               inner,
               "u_icg",
               {{"clk", "clk_n"}, {"en", "en"}, {"test_en", "test_en"}, {"clk_out", "gated_n"}},
               "    ",
               false)
           + instance(
               "qsoc_ck_inv",
               "u_inv_out",
               {{"clk_in", "gated_n"}, {"clk_out", "clk_out"}},
               "    ",
               false);
}

/* STAGES flops: whole sync cells first, then plain flops for the rest. */
QString syncChain(const Cell &cell)
{
    const QMap<QString, QString> connect = withTies(
        cell,
        {{cell.pin["clock"], "clk"},
         {cell.pin["reset"], "rst_n"},
         {cell.pin["data"], "link[i]"},
         {cell.pin["output"], "link[i + 1]"}});
    return QString(
               "\n"
               "    /* Whole %1 cells of %2 stages, then plain flops up to STAGES */\n"
               "    localparam integer CELLS = STAGES / %2;\n"
               "    localparam integer EXTRA = STAGES - CELLS * %2;\n"
               "\n"
               "    /* Reset clears the flops, so RESET_VALUE is applied around the chain */\n"
               "    wire [CELLS:0] link;\n"
               "    assign link[0] = d ^ RESET_VALUE;\n"
               "\n"
               "    genvar i;\n"
               "    generate\n"
               "        for (i = 0; i < CELLS; i = i + 1) begin : g_cell\n"
               "%3"
               "        end\n"
               "        if (EXTRA == 0) begin : g_exact\n"
               "            assign q = link[CELLS] ^ RESET_VALUE;\n"
               "        end else begin : g_extra\n"
               "            reg [EXTRA-1:0] tail;\n"
               "            always @(posedge clk or negedge rst_n) begin\n"
               "                if (!rst_n) tail <= {EXTRA{1'b0}};\n"
               "                else        tail <= (tail << 1) | link[CELLS];\n"
               "            end\n"
               "            assign q = tail[EXTRA-1] ^ RESET_VALUE;\n"
               "        end\n"
               "    endgenerate\n")
        .arg(cell.name)
        .arg(cell.stages)
        .arg(instance(cell.name, "u_cell", connect, "            ", true));
}

QString modelHeader(const QString &name)
{
    return QString(
               "/**\n"
               " * @file %1.v\n"
               " * @brief Behavioral model of %1, from its declaration.\n"
               " * @details For simulation and formal only.\n"
               " */\n\n"
               "`timescale 1ns / 1ps\n\n")
        .arg(name);
}

} // namespace

QStringList QSocCellBinding::roleNames()
{
    QStringList names;
    for (const RoleSpec &spec : kRoles)
        names.append(spec.role);
    return names;
}

QStringList QSocCellBinding::rolePorts(const QString &role)
{
    for (const RoleSpec &spec : kRoles) {
        if (role == spec.role)
            return QString(spec.ports).split(' ');
    }
    return {};
}

QSocCellBinding QSocCellBinding::resolve(const YAML::Node &project, const YAML::Node &modules)
{
    QSocCellBinding result;
    result.readTarget(project);
    if (modules && modules.IsMap()) {
        QMap<QString, YAML::Node> entries;
        for (const auto &entry : modules) {
            const YAML::Node &node = entry.second;
            if (node.IsMap() && (node["function"] || node["sequential"] || node["tie"]))
                entries.insert(scalar(entry.first), node);
        }
        for (auto it = entries.constBegin(); it != entries.constEnd(); ++it)
            result.declare(it.key(), it.value());
    }
    if (!result.problems.isEmpty())
        return result;
    result.bind();
    result.compose();
    if (!result.isAsic()) {
        result.notices.clear();
        return result;
    }
    if (!result.bound.contains("qsoc_ck_icg_pos") && !result.bound.contains("qsoc_ck_icg_neg"))
        result.problems.append(
            "cell target asic: no declared cell implements a clock gate, declare an icg_pos or "
            "icg_neg cell");
    const QStringList missing = result.unresolved();
    if (!missing.isEmpty())
        result.notices.prepend(
            "cell roles without a declared cell, elaboration fails where they are used: "
            + missing.join(", "));
    return result;
}

QSocCellBinding QSocCellBinding::fromProject(QSocProjectManager *project, QSocModuleManager *modules)
{
    return resolve(
        project ? YAML::Clone(project->getProjectYaml()) : YAML::Node(),
        modules ? modules->getModuleYamls() : YAML::Node());
}

void QSocCellBinding::readTarget(const YAML::Node &project)
{
    const YAML::Node cell = project && project.IsMap() ? project["cell"] : YAML::Node();
    if (!cell)
        return;
    if (!cell.IsMap()) {
        problems.append("project cell: must be a map");
        return;
    }
    for (const auto &entry : cell) {
        if (scalar(entry.first) != "target")
            problems.append(QString("project cell.%1: unknown key").arg(scalar(entry.first)));
    }
    if (!cell["target"])
        return;
    const QString text = scalar(cell["target"]);
    if (text == "asic")
        mode = Target::Asic;
    else if (text != "generic")
        problems.append(QString("project cell.target: must be generic or asic, not '%1'").arg(text));
}

void QSocCellBinding::declare(const QString &name, const YAML::Node &entry)
{
    const QString    path       = "module." + name;
    const YAML::Node function   = entry["function"];
    const YAML::Node sequential = entry["sequential"];
    if (!function && !sequential) {
        problems.append(path + ".tie: a tie needs a function or a sequential template");
        return;
    }
    if (function && sequential) {
        problems.append(path + ": declare a function or a sequential template, not both");
        return;
    }
    if (QSocCellLibrary::isReserved(name)) {
        problems.append(path + ": the name is reserved for QSoC cells");
        return;
    }
    Cell cell;
    cell.name  = name;
    cell.ports = QSocCellTable::portsOf(entry["port"]);
    if (cell.ports.isEmpty()) {
        problems.append(path + ".port: a declared cell needs its ports");
        return;
    }
    bool valid = true;
    for (auto it = cell.ports.constBegin(); it != cell.ports.constEnd(); ++it) {
        if (it->direction != "in" && it->direction != "out") {
            problems.append(QString("%1.port.%2: a declared cell has no %3 pin")
                                .arg(path, it.key(), it->direction));
            valid = false;
        } else if (it->width != 1) {
            problems.append(QString("%1.port.%2: %3 bits wide, a declared cell pin is 1 bit")
                                .arg(path, it.key())
                                .arg(it->width));
            valid = false;
        }
    }
    valid = readTies(&cell, entry["tie"], path + ".tie") && valid;
    if (!valid)
        return;
    if (sequential) {
        if (readTemplate(&cell, sequential, path + ".sequential"))
            declared.append(cell);
        return;
    }
    cell.table = QSocCellTable::parse(function, cell.ports, path + ".function");
    if (!cell.table.isValid()) {
        problems.append(cell.table.errors());
        return;
    }
    for (const QString &attribute : cell.table.attributes()) {
        problems.append(QString("%1.function: %2 is not a pin of the cell").arg(path, attribute));
        valid = false;
    }
    for (auto it = cell.ports.constBegin(); it != cell.ports.constEnd(); ++it) {
        if (it->direction == "out" && !cell.table.outputs().contains(it.key())) {
            problems.append(QString("%1.function: output %2 has no function").arg(path, it.key()));
            valid = false;
        } else if (
            it->direction == "in" && !cell.tie.contains(it.key())
            && !cell.table.dependsOn(it.key())) {
            problems.append(
                QString("%1.function: input %2 changes no output, tie it").arg(path, it.key()));
            valid = false;
        }
    }
    if (valid)
        declared.append(cell);
}

bool QSocCellBinding::readTies(Cell *cell, const YAML::Node &node, const QString &path)
{
    if (!node)
        return true;
    if (!node.IsMap()) {
        problems.append(path + ": must map input pins to 0 or 1");
        return false;
    }
    bool valid = true;
    for (const auto &entry : node) {
        const QString pin   = scalar(entry.first);
        const QString value = scalar(entry.second);
        const auto    port  = cell->ports.constFind(pin);
        if (port == cell->ports.constEnd() || port->direction != "in") {
            problems.append(QString("%1.%2: not an input pin of the cell").arg(path, pin));
            valid = false;
        } else if (value != "0" && value != "1") {
            problems.append(QString("%1.%2: takes 0 or 1, not '%3'").arg(path, pin, value));
            valid = false;
        } else {
            cell->tie.insert(pin, value == "1");
        }
    }
    return valid;
}

bool QSocCellBinding::readTemplate(Cell *cell, const YAML::Node &node, const QString &path)
{
    if (!node.IsMap()) {
        problems.append(path + ": must be a map");
        return false;
    }
    const QString type = scalar(node["type"]);
    if (!kTypeRole.contains(type)) {
        problems.append(
            QString("%1.type: must be icg_pos, icg_neg or sync, not '%2'").arg(path, type));
        return false;
    }
    const QMap<QString, QString> &ports = isGate(type) ? kGatePort : kSyncPort;
    bool                          valid = true;
    for (const auto &entry : node) {
        const QString key = scalar(entry.first);
        if (key == "type" || (key == "stages" && !isGate(type)))
            continue;
        if (!ports.contains(key)) {
            problems.append(QString("%1.%2: not a key of a %3 template").arg(path, key, type));
            valid = false;
            continue;
        }
        const QString pin       = scalar(entry.second);
        const QString direction = key == "output" ? "out" : "in";
        const auto    port      = cell->ports.constFind(pin);
        if (port == cell->ports.constEnd() || port->direction != direction) {
            problems.append(QString("%1.%2: '%3' is not an %4 pin of the cell")
                                .arg(path, key, pin, direction == "in" ? "input" : "output"));
            valid = false;
        } else if (cell->tie.contains(pin) || cell->pin.values().contains(pin)) {
            problems.append(QString("%1.%2: pin %3 has another use").arg(path, key, pin));
            valid = false;
        } else {
            cell->pin.insert(key, pin);
        }
    }
    for (auto it = ports.constBegin(); it != ports.constEnd(); ++it) {
        if (it.key() != "test" && !node[it.key().toStdString()]) {
            problems.append(QString("%1: type %2 needs %3").arg(path, type, it.key()));
            valid = false;
        }
    }
    if (!isGate(type)) {
        bool      number = false;
        const int stages = scalar(node["stages"]).toInt(&number);
        if (!number || stages < 1) {
            problems.append(path + ".stages: a sync cell needs its flop count, at least 1");
            valid = false;
        }
        cell->stages = stages;
    }
    if (!valid)
        return false;
    for (auto it = cell->ports.constBegin(); it != cell->ports.constEnd(); ++it) {
        const bool used = cell->pin.values().contains(it.key()) || cell->tie.contains(it.key());
        if (!used && it->direction == "in") {
            problems.append(
                QString("%1: input %2 is not in the template, tie it").arg(path, it.key()));
            valid = false;
        } else if (!used) {
            problems.append(QString("%1: output %2 is not the template output").arg(path, it.key()));
            valid = false;
        }
    }
    cell->type = type;
    return valid;
}

void QSocCellBinding::bind()
{
    for (const RoleSpec &spec : kRoles) {
        QList<int>     cells;
        QList<Binding> bindings;
        if (*spec.function) {
            const QSocCellTable role = roleTable(spec);
            for (int i = 0; i < declared.size(); ++i) {
                const Cell &cell = declared.at(i);
                if (!cell.type.isEmpty())
                    continue;
                const auto maps = cell.table.tied(cell.tie).matches(role);
                if (maps.isEmpty())
                    continue;
                cells.append(i);
                bindings.append({i, maps.first(), {}});
            }
        } else {
            for (int i = 0; i < declared.size(); ++i) {
                const Cell &cell = declared.at(i);
                if (kTypeRole.value(cell.type) != spec.role)
                    continue;
                const QMap<QString, QString> &ports = isGate(cell.type) ? kGatePort : kSyncPort;
                Binding                       binding{i, {}, {}};
                for (auto it = cell.pin.constBegin(); it != cell.pin.constEnd(); ++it)
                    binding.pin.insert(it.value(), ports.value(it.key()));
                cells.append(i);
                bindings.append(binding);
            }
        }
        claim(spec.role, cells, bindings);
    }
}

void QSocCellBinding::claim(
    const QString &role, const QList<int> &cells, const QList<Binding> &bindings)
{
    if (cells.size() == 1) {
        bound.insert(role, bindings.first());
        return;
    }
    if (cells.isEmpty())
        return;
    QStringList names;
    for (const int cell : cells)
        names.append(declared.at(cell).name);
    problems.append(QString("cell role %1: %2 all implement it, keep one declaration")
                        .arg(role, names.join(" and ")));
}

void QSocCellBinding::compose()
{
    const auto gate = [this](const QString &missing, const QString &other) {
        if (bound.contains(missing) || !bound.contains(other) || !bound.contains("qsoc_ck_inv"))
            return;
        bound.insert(missing, {-1, {}, {"qsoc_ck_inv", other, "qsoc_ck_inv"}});
        notices.append(
            QString(
                "cell role %1: composed as qsoc_ck_inv, %2, qsoc_ck_inv, two extra inverter "
                "delays")
                .arg(missing, other));
    };
    gate("qsoc_ck_icg_neg", "qsoc_ck_icg_pos");
    gate("qsoc_ck_icg_pos", "qsoc_ck_icg_neg");
}

QStringList QSocCellBinding::unresolved() const
{
    QStringList missing;
    for (const QString &role : roleNames()) {
        if (!bound.contains(role))
            missing.append(role);
    }
    return missing;
}

QString QSocCellBinding::body(const QString &role) const
{
    const auto it = bound.constFind(role);
    if (it == bound.constEnd()) {
        QMap<QString, QString> connect;
        for (const QString &port : rolePorts(role))
            connect.insert(port, port);
        return instance("qsoc_role_unresolved_" + role.mid(5), "u_cell", connect, "    ", false);
    }
    if (!it->via.isEmpty())
        return composedGate(it->via.at(1));
    const Cell &cell = declared.at(it->cell);
    if (cell.type == "sync")
        return syncChain(cell);
    QMap<QString, QString> connect = it->pin;
    if (isGate(cell.type) && !cell.pin.contains("test"))
        connect.insert(cell.pin.value("enable"), "en | test_en");
    return instance(cell.name, "u_cell", withTies(cell, connect), "    ", true);
}

QString QSocCellBinding::detail(const QString &role) const
{
    const auto it = bound.constFind(role);
    if (it == bound.constEnd())
        return "No declared cell implements this role, elaboration fails where it is used.";
    if (!it->via.isEmpty())
        return QString("Composed as %1, two extra inverter delays.").arg(it->via.join(", "));
    const Cell &cell = declared.at(it->cell);
    if (cell.type == "sync")
        return QString("Declared cell %1 of %2 stages as g_cell[i].u_cell, then plain flops.")
            .arg(cell.name)
            .arg(cell.stages);
    return QString("Declared cell %1 as u_cell.").arg(cell.name);
}

QString QSocCellBinding::model(const Cell &cell)
{
    if (cell.type.isEmpty())
        return modelHeader(cell.name) + cell.table.verilog(cell.name);
    QStringList ports;
    for (auto it = cell.ports.constBegin(); it != cell.ports.constEnd(); ++it)
        ports.append(
            QString("    %1 wire %2").arg(it->direction == "in" ? "input " : "output", it.key()));
    QString body;
    if (cell.type == "sync") {
        body = QString(
                   "    reg [%1:0] chain;\n"
                   "    always @(posedge %2 or negedge %3) begin\n"
                   "        if (!%3) chain <= {%4{1'b0}};\n"
                   "        else chain <= (chain << 1) | %5;\n"
                   "    end\n"
                   "    assign %6 = chain[%1];\n")
                   .arg(cell.stages - 1)
                   .arg(cell.pin["clock"], cell.pin["reset"])
                   .arg(cell.stages)
                   .arg(cell.pin["data"], cell.pin["output"]);
    } else {
        const QString clock  = cell.pin["clock"];
        const QString enable = cell.pin.contains("test")
                                   ? cell.pin["enable"] + " | " + cell.pin["test"]
                                   : cell.pin["enable"];
        const bool    pos    = cell.type == "icg_pos";
        body                 = QString(
                                   "    reg enabled;\n"
                                   "    always @* begin\n"
                                   "        if (%1%2) enabled = %3;\n"
                                   "    end\n"
                                   "    assign %4 = %2 %5enabled;\n")
                                   .arg(pos ? "!" : "", clock, enable, cell.pin["output"], pos ? "& " : "| ~");
    }
    return modelHeader(cell.name)
           + QString("module %1 (\n%2\n);\n%3endmodule\n").arg(cell.name, ports.join(",\n"), body);
}

QString QSocCellBinding::report() const
{
    YAML::Emitter out;
    out << YAML::BeginMap << YAML::Key << "target" << YAML::Value
        << (isAsic() ? "asic" : "generic");
    out << YAML::Key << "role" << YAML::Value << YAML::BeginMap;
    for (const QString &role : roleNames()) {
        out << YAML::Key << role.toStdString() << YAML::Value << YAML::BeginMap;
        const auto it = bound.constFind(role);
        if (it == bound.constEnd()) {
            out << YAML::Key << "unresolved" << YAML::Value
                << ("qsoc_role_unresolved_" + role.mid(5)).toStdString();
        } else if (!it->via.isEmpty()) {
            out << YAML::Key << "composed" << YAML::Value << YAML::Flow << YAML::BeginSeq;
            for (const QString &step : it->via)
                out << step.toStdString();
            out << YAML::EndSeq << YAML::Key << "note" << YAML::Value
                << "two extra inverter delays";
        } else {
            const Cell &cell = declared.at(it->cell);
            out << YAML::Key << "cell" << YAML::Value << cell.name.toStdString();
            out << YAML::Key << "instance" << YAML::Value
                << (cell.type == "sync" ? "g_cell[i].u_cell" : "u_cell");
            out << YAML::Key << "pin" << YAML::Value << YAML::Flow << YAML::BeginMap;
            for (auto pin = it->pin.constBegin(); pin != it->pin.constEnd(); ++pin)
                out << YAML::Key << pin.key().toStdString() << YAML::Value
                    << pin.value().toStdString();
            for (auto tie = cell.tie.constBegin(); tie != cell.tie.constEnd(); ++tie)
                out << YAML::Key << tie.key().toStdString() << YAML::Value
                    << (tie.value() ? "1'b1" : "1'b0");
            out << YAML::EndMap;
            if (cell.type == "sync")
                out << YAML::Key << "stages" << YAML::Value << cell.stages;
        }
        out << YAML::EndMap;
    }
    out << YAML::EndMap << YAML::EndMap;
    return "# Cell role binding, generated by QSoC.\n" + QString::fromUtf8(out.c_str()) + "\n";
}
