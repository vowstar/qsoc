// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "qsocgenerateprimitivepower.h"
#include "common/qsoccelltext.h"
#include "common/qsocconsole.h"
#include "common/qsocpaths.h"
#include "qsocgeneratemanager.h"
#include "qsocverilogutils.h"
#include <cmath>
#include <QDebug>
#include <QFileInfo>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QSet>

QSocPowerPrimitive::QSocPowerPrimitive(QSocGenerateManager *parent)
    : m_parent(parent)
{}

bool QSocPowerPrimitive::generatePowerController(const YAML::Node &powerNode, QTextStream &out)
{
    if (!powerNode || !powerNode.IsMap()) {
        QSocConsole::warn() << "Invalid power node provided";
        return false;
    }

    // Parse configuration
    PowerControllerConfig config = parsePowerConfig(powerNode);

    if (!config.valid) {
        return false;
    }
    if (config.domains.isEmpty()) {
        QSocConsole::warn() << "Power configuration must have at least one domain";
        return false;
    }

    /* The controller declares rdy_<name> only for its own domains, so a
       dependency on any other name references an undeclared signal. */
    {
        QSet<QString> declaredDomains;
        for (const auto &domain : config.domains) {
            declaredDomains.insert(domain.name);
        }
        bool undefined = false;
        for (const auto &domain : config.domains) {
            for (const auto &dep : domain.depends) {
                if (dep.name.isEmpty()) {
                    QSocConsole::error() << "Power controller" << config.name << "domain"
                                         << domain.name << "has a depend entry without a name";
                    undefined = true;
                } else if (!declaredDomains.contains(dep.name)) {
                    QSocConsole::error()
                        << "Power controller" << config.name << "domain" << domain.name
                        << "depends on undefined domain" << dep.name << "(" + dep.type + ")";
                    undefined = true;
                }
            }
        }
        if (undefined) {
            return false;
        }
    }

    // Generate Verilog code
    generateModuleHeader(config, out);
    generateWireDeclarations(config, out);
    generatePowerLogic(config, out);
    generateOutputAssignments(config, out);

    // Close module
    out << "\nendmodule\n\n";

    // Generate Typst power diagram (failure does not affect Verilog generation)
    if (m_parent && !m_parent->getDiagramDirectory().isEmpty()) {
        const QString outputDir = m_parent->getDiagramDirectory();
        const QString typstName = config.moduleName + QStringLiteral(".typ");
        const auto    artifact  = QSocPaths::resolveArtifactPath(outputDir, typstName);
        if (!artifact.isValid()) {
            QSocConsole::warn() << artifact.error;
        } else if (!generateTypstDiagram(config, artifact.path)) {
            QSocConsole::warn() << "Failed to generate Typst diagram (non-critical):"
                                << artifact.path;
        }
    }

    return true;
}

QSocPowerPrimitive::PowerControllerConfig QSocPowerPrimitive::parsePowerConfig(
    const YAML::Node &powerNode)
{
    /* A malformed shape reaches yaml-cpp as an exception, and an uncaught one
       aborts the process instead of reporting the user's configuration. */
    try {
        return parsePowerConfigUnguarded(powerNode);
    } catch (const YAML::Exception &error) {
        PowerControllerConfig config;
        config.valid = false;
        QSocConsole::error() << "Invalid power configuration:" << error.what();
        return config;
    }
}

QSocPowerPrimitive::PowerControllerConfig QSocPowerPrimitive::parsePowerConfigUnguarded(
    const YAML::Node &powerNode)
{
    PowerControllerConfig config;

    // Parse basic properties
    if (!powerNode["name"]) {
        QSocConsole::error() << "'name' field is required in power configuration";
        QSocConsole::err() << "Example: power: { name: pwr0, ... }" << "\n";
        return config;
    }
    config.name       = QString::fromStdString(powerNode["name"].as<std::string>());
    config.moduleName = config.name; // Use same name for module

    // Host clock and reset (required for FSM)
    if (!powerNode["host_clock"]) {
        QSocConsole::error() << "'host_clock' field is required in power configuration";
        return config;
    }
    config.host_clock = QString::fromStdString(powerNode["host_clock"].as<std::string>());

    if (!powerNode["host_reset"]) {
        QSocConsole::error() << "'host_reset' field is required in power configuration";
        return config;
    }
    config.host_reset = QString::fromStdString(powerNode["host_reset"].as<std::string>());

    // DFT test enable (optional)
    if (powerNode["test_enable"] && powerNode["test_enable"].IsScalar()) {
        config.test_enable = QString::fromStdString(powerNode["test_enable"].as<std::string>());
    }

    // Parse domains
    if (powerNode["domain"] && powerNode["domain"].IsSequence()) {
        for (size_t i = 0; i < powerNode["domain"].size(); ++i) {
            const YAML::Node &domainNode = powerNode["domain"][i];
            if (!domainNode.IsMap())
                continue;

            PowerDomain domain;

            // Domain name (required)
            if (!domainNode["name"]) {
                QSocConsole::error() << "'name' field is required for each domain";
                config.valid = false;
                continue;
            }
            const QString rawName = QString::fromStdString(domainNode["name"].as<std::string>());
            /* Bracket characters in a domain name leak into wire/instance/
               port identifiers and produce illegal Verilog (matches the
               clock/reset bracket-leak fix). Sanitize and warn. */
            domain.name = QSocVerilogUtils::sanitizeBitSelectInName(rawName);
            if (domain.name != rawName) {
                QSocConsole::warn() << "Power domain name" << rawName
                                    << "contains bracket characters; sanitized to" << domain.name;
            }

            // Cache YAML node for type inference
            m_domainYamlCache[domain.name] = domainNode;

            // Parse dependencies (optional, absence = AO, empty array = root)
            if (domainNode["depend"] && domainNode["depend"].IsSequence()) {
                for (size_t j = 0; j < domainNode["depend"].size(); ++j) {
                    const YAML::Node &depNode = domainNode["depend"][j];
                    if (!depNode.IsMap())
                        continue;

                    Dependency dep;
                    if (depNode["name"]) {
                        dep.name = QSocVerilogUtils::sanitizeBitSelectInName(
                            QString::fromStdString(depNode["name"].as<std::string>()));
                    }
                    if (depNode["type"]) {
                        dep.type = QString::fromStdString(depNode["type"].as<std::string>());
                    } else {
                        dep.type = "hard"; // Default to hard dependency
                    }
                    domain.depends.append(dep);
                }
            }

            // Voltage (optional)
            domain.v_mv = domainNode["v_mv"] ? domainNode["v_mv"].as<int>() : 0;

            // Power good signal
            if (domainNode["pgood"]) {
                domain.pgood = QString::fromStdString(domainNode["pgood"].as<std::string>());
            }

            // Timing parameters
            domain.wait_dep   = domainNode["wait_dep"] ? domainNode["wait_dep"].as<int>() : 0;
            domain.settle_on  = domainNode["settle_on"] ? domainNode["settle_on"].as<int>() : 0;
            domain.settle_off = domainNode["settle_off"] ? domainNode["settle_off"].as<int>() : 0;

            // Follow entries for reset synchronization
            if (domainNode["follow"] && domainNode["follow"].IsSequence()) {
                for (size_t j = 0; j < domainNode["follow"].size(); ++j) {
                    const YAML::Node &followNode = domainNode["follow"][j];
                    if (!followNode.IsMap())
                        continue;

                    FollowEntry entry;
                    if (followNode["clock"]) {
                        entry.clock = QString::fromStdString(followNode["clock"].as<std::string>());
                    }
                    if (followNode["reset"]) {
                        entry.reset = QString::fromStdString(followNode["reset"].as<std::string>());
                    }
                    entry.stage = followNode["stage"] ? followNode["stage"].as<int>()
                                                      : 4; // Default to 4 stages
                    if (entry.stage < 1) {
                        QSocConsole::error()
                            << QString("Domain %1 follow stage must be at least 1, got %2")
                                   .arg(domain.name)
                                   .arg(entry.stage);
                        config.valid = false;
                        continue;
                    }

                    if (entry.clock.isEmpty() != entry.reset.isEmpty()) {
                        QSocConsole::error()
                            << "Domain" << domain.name
                            << "follow entry needs both clock and reset; it has only one";
                        config.valid = false;
                        continue;
                    }
                    /* Both empty: no follow pair requested. */
                    if (entry.clock.isEmpty()) {
                        continue;
                    }

                    // FATAL: Check for host signal misuse (creates circular dependency)
                    if (entry.clock == config.host_clock) {
                        QSocConsole::error()
                            << "FATAL: Domain" << domain.name
                            << "follow entry cannot use host_clock" << config.host_clock
                            << "as synchronization clock - this creates circular dependency!";
                        config.valid = false;
                        continue;
                    }
                    if (entry.reset == config.host_reset) {
                        QSocConsole::error()
                            << "FATAL: Domain" << domain.name
                            << "follow entry cannot use host_reset" << config.host_reset
                            << "as reset output - this creates port conflict!";
                        config.valid = false;
                        continue;
                    }
                    domain.follow_entries.append(entry);
                }
            }

            config.domains.append(domain);
        }
    }

    return config;
}

void QSocPowerPrimitive::generateModuleHeader(const PowerControllerConfig &config, QTextStream &out)
{
    out << "/* " << config.moduleName << " - Power Controller\n";
    out << " * Generated by QSoC Power Primitive\n";
    out << " */\n\n";

    out << "module " << config.moduleName << " (\n";

    // Initialize port tracking for "output win" mechanism
    QSet<QString> addedSignals;
    QStringList   portDecls;
    QStringList   portComments;

    // Host clock and reset (inputs)
    portDecls << QString("    input  wire %1").arg(config.host_clock);
    portComments << "/**< Host clock (typically AO) */";
    addedSignals.insert(config.host_clock);

    portDecls << QString("    input  wire %1").arg(config.host_reset);
    portComments << "/**< Host reset (typically AO) */";
    addedSignals.insert(config.host_reset);

    // DFT test enable (optional, check for duplicates)
    if (!config.test_enable.isEmpty() && !addedSignals.contains(config.test_enable)) {
        portDecls << QString("    input  wire %1").arg(config.test_enable);
        portComments << "/**< DFT test enable */";
        addedSignals.insert(config.test_enable);
    }

    // System reset for reset synchronization (check for duplicates)
    if (!addedSignals.contains("rst_sys_n")) {
        portDecls << QString("    input  wire rst_sys_n");
        portComments << "/**< System reset for domain sync */";
        addedSignals.insert("rst_sys_n");
    }

    // Power good inputs (check for duplicates)
    for (const auto &domain : config.domains) {
        if (!domain.pgood.isEmpty() && !addedSignals.contains(domain.pgood)) {
            portDecls << QString("    input  wire %1").arg(domain.pgood);
            portComments << QString("/**< %1 voltage good */").arg(domain.name);
            addedSignals.insert(domain.pgood);
        }
    }

    // Control inputs (enable and fault clear, check for duplicates)
    for (const auto &domain : config.domains) {
        YAML::Node yamlNode = m_domainYamlCache.value(domain.name);
        if (!isAODomain(domain, yamlNode)) {
            QString enableName = QString("en_%1").arg(domain.name);
            QString clearName  = QString("clr_%1").arg(domain.name);

            if (!addedSignals.contains(enableName)) {
                portDecls << QString("    input  wire %1").arg(enableName);
                portComments << QString("/**< Enable %1 */").arg(domain.name);
                addedSignals.insert(enableName);
            }

            if (!addedSignals.contains(clearName)) {
                portDecls << QString("    input  wire %1").arg(clearName);
                portComments << QString("/**< Clear fault for %1 */").arg(domain.name);
                addedSignals.insert(clearName);
            }
        }
    }

    // ICG enable outputs (check for duplicates)
    for (const auto &domain : config.domains) {
        QString icgName = QString("icg_en_%1").arg(domain.name);
        if (!addedSignals.contains(icgName)) {
            portDecls << QString("    output wire %1").arg(icgName);
            portComments << QString("/**< ICG enable for %1 */").arg(domain.name);
            addedSignals.insert(icgName);
        }
    }

    // NOTE: rst_gate_*_n are internal signals, not module ports

    // Domain clock inputs for reset synchronizers (follow entries)
    for (const auto &domain : config.domains) {
        for (const auto &entry : domain.follow_entries) {
            if (!addedSignals.contains(entry.clock)) {
                portDecls << QString("    input  wire %1").arg(entry.clock);
                portComments << QString("/**< Domain clock for %1 reset sync */").arg(domain.name);
                addedSignals.insert(entry.clock);
            }
        }
    }

    // Power switch outputs (check for duplicates)
    for (const auto &domain : config.domains) {
        YAML::Node yamlNode = m_domainYamlCache.value(domain.name);
        if (!isAODomain(domain, yamlNode)) {
            QString switchName = QString("sw_%1").arg(domain.name);
            if (!addedSignals.contains(switchName)) {
                portDecls << QString("    output wire %1").arg(switchName);
                portComments << QString("/**< Switch for %1 */").arg(domain.name);
                addedSignals.insert(switchName);
            }
        }
    }

    // Reset synchronizer outputs (follow entries, OUTPUT WIN over inputs)
    for (const auto &domain : config.domains) {
        for (const auto &entry : domain.follow_entries) {
            if (!addedSignals.contains(entry.reset)) {
                portDecls << QString("    output wire %1").arg(entry.reset);
                portComments << QString("/**< Synchronized reset for %1 */").arg(domain.name);
                addedSignals.insert(entry.reset);
            }
        }
    }

    // Status outputs (check for duplicates)
    for (const auto &domain : config.domains) {
        QString readyName = QString("rdy_%1").arg(domain.name);
        QString faultName = QString("flt_%1").arg(domain.name);

        if (!addedSignals.contains(readyName)) {
            portDecls << QString("    output wire %1").arg(readyName);
            portComments << QString("/**< %1 ready */").arg(domain.name);
            addedSignals.insert(readyName);
        }

        if (!addedSignals.contains(faultName)) {
            portDecls << QString("    output wire %1").arg(faultName);
            portComments << QString("/**< %1 fault */").arg(domain.name);
            addedSignals.insert(faultName);
        }
    }

    // Output all ports with unified boundary judgment
    for (int i = 0; i < portDecls.size(); ++i) {
        bool    isLast = (i == portDecls.size() - 1);
        QString comma  = isLast ? "" : ",";
        out << portDecls[i] << comma << " " << portComments[i] << "\n";
    }

    out << ");\n\n";
}

void QSocPowerPrimitive::generateWireDeclarations(
    const PowerControllerConfig &config, QTextStream &out)
{
    out << "    /* Dependency aggregation: hard (required), soft (optional) */\n";

    for (const auto &domain : config.domains) {
        QString hardSig = getHardDependencySignal(domain);
        QString softSig = getSoftDependencySignal(domain);

        // Build dependency comment: "domain: hard=[noc], soft=[sram]"
        QStringList depParts;
        if (hardSig != "1'b1") {
            QStringList hardNames;
            for (const auto &dep : domain.depends) {
                if (dep.type == "hard") {
                    hardNames << dep.name;
                }
            }
            if (!hardNames.isEmpty()) {
                depParts << QString("hard=[%1]").arg(hardNames.join(", "));
            }
        }
        if (softSig != "1'b1") {
            QStringList softNames;
            for (const auto &dep : domain.depends) {
                if (dep.type == "soft") {
                    softNames << dep.name;
                }
            }
            if (!softNames.isEmpty()) {
                depParts << QString("soft=[%1]").arg(softNames.join(", "));
            }
        }

        if (!depParts.isEmpty()) {
            out << "    // " << domain.name << ": " << depParts.join(", ") << "\n";
        }

        if (hardSig != "1'b1") {
            out << "    wire dep_hard_all_" << domain.name << " = " << hardSig << ";\n";
        }
        if (softSig != "1'b1") {
            out << "    wire dep_soft_all_" << domain.name << " = " << softSig << ";\n";
        }
    }

    out << "\n";
}

void QSocPowerPrimitive::generatePowerLogic(const PowerControllerConfig &config, QTextStream &out)
{
    // Generate internal wire declarations for rst_gate_n signals
    out << "    /* Internal wires for FSM reset gates */\n";
    for (const auto &domain : config.domains) {
        out << "    wire rst_gate_" << domain.name << "_n;\n";
    }
    out << "\n";

    out << "    /* Power FSM instances */\n";

    for (const auto &domain : config.domains) {
        YAML::Node yamlNode = m_domainYamlCache.value(domain.name);
        bool       isAO     = isAODomain(domain, yamlNode);
        bool       isRoot   = isRootDomain(domain, yamlNode);

        out << "    /* " << domain.name << ": ";
        if (isAO) {
            out << "AO domain (no depend key) */\n";
        } else if (isRoot) {
            out << "Root domain (depend: []) */\n";
        } else {
            out << "Normal domain */\n";
        }

        QString hardSig = getHardDependencySignal(domain);
        QString softSig = getSoftDependencySignal(domain);

        out << "    qsoc_power_fsm #(\n";
        out << "        .HAS_SWITCH        (" << (isAO ? "0" : "1") << "),\n";
        out << "        .WAIT_DEP_CYCLES   (" << domain.wait_dep << "),\n";
        out << "        .SETTLE_ON_CYCLES  (" << domain.settle_on << "),\n";
        out << "        .SETTLE_OFF_CYCLES (" << domain.settle_off << ")\n";
        out << "    ) u_pwr_" << domain.name << " (\n";
        out << "        .clk          (" << config.host_clock << "),\n";
        out << "        .rst_n        (" << config.host_reset << "),\n";

        if (!config.test_enable.isEmpty()) {
            out << "        .test_en      (" << config.test_enable << "),\n";
        } else {
            out << "        .test_en      (1'b0),\n";
        }

        if (isAO) {
            out << "        .ctrl_enable  (1'b1), /**< AO always on */\n";
            out << "        .fault_clear  (1'b0),\n";
        } else {
            out << "        .ctrl_enable  (en_" << domain.name << "),\n";
            out << "        .fault_clear  (clr_" << domain.name << "),\n";
        }

        if (hardSig == "1'b1") {
            out << "        .dep_hard_all (1'b1),\n";
        } else {
            out << "        .dep_hard_all (dep_hard_all_" << domain.name << "),\n";
        }

        if (softSig == "1'b1") {
            out << "        .dep_soft_all (1'b1),\n";
        } else {
            out << "        .dep_soft_all (dep_soft_all_" << domain.name << "),\n";
        }

        if (!domain.pgood.isEmpty()) {
            out << "        .pgood        (" << domain.pgood << "),\n";
        } else {
            out << "        .pgood        (1'b1),\n";
        }

        out << "        .clk_enable   (icg_en_" << domain.name << "),\n";
        out << "        .rst_gate_n   (rst_gate_" << domain.name << "_n),\n";

        if (isAO) {
            out << "        .pwr_switch   (), /**< Unused for AO */\n";
        } else {
            out << "        .pwr_switch   (sw_" << domain.name << "),\n";
        }

        out << "        .ready        (rdy_" << domain.name << "),\n";
        out << "        .valid        (), /**< Optional, not exported */\n";
        out << "        .fault        (flt_" << domain.name << ")\n";
        out << "    );\n\n";

        // Generate reset synchronizers for follow entries
        if (!domain.follow_entries.isEmpty()) {
            out << "    /* Reset synchronizers for " << domain.name << " domain */\n";
            for (int i = 0; i < domain.follow_entries.size(); ++i) {
                const auto &entry = domain.follow_entries[i];
                out << "    qsoc_power_rst_sync #(\n";
                out << "        .STAGE (" << entry.stage << ")\n";
                out << "    ) u_rst_sync_" << domain.name << "_" << i << " (\n";
                out << "        .clk_dom     (" << entry.clock << "),\n";
                out << "        .rst_gate_n  (rst_sys_n & rst_gate_" << domain.name << "_n),\n";
                if (!config.test_enable.isEmpty()) {
                    out << "        .test_en     (" << config.test_enable << "),\n";
                } else {
                    out << "        .test_en     (1'b0),\n";
                }
                out << "        .rst_dom_n   (" << entry.reset << ")\n";
                out << "    );\n\n";
            }
        }
    }
}

void QSocPowerPrimitive::generateOutputAssignments(
    const PowerControllerConfig &config, QTextStream &out)
{
    Q_UNUSED(config);
    // No additional assignments needed - all outputs come directly from FSM instances
    out << "    /* All outputs are directly connected from FSM instances */\n";
}

QString QSocPowerPrimitive::generateCellVerilog()
{
    return QSocCellText::power();
}

bool QSocPowerPrimitive::isAODomain(const PowerDomain &domain, const YAML::Node &yamlNode)
{
    Q_UNUSED(domain);
    // AO domain: no "depend" key in YAML
    return !yamlNode["depend"];
}

bool QSocPowerPrimitive::isRootDomain(const PowerDomain &domain, const YAML::Node &yamlNode)
{
    // Root domain: has "depend" key with empty array
    return yamlNode["depend"] && yamlNode["depend"].IsSequence() && domain.depends.isEmpty();
}

QString QSocPowerPrimitive::getHardDependencySignal(const PowerDomain &domain)
{
    QStringList hardDeps;
    for (const auto &dep : domain.depends) {
        if (dep.type == "hard") {
            hardDeps << QString("rdy_%1").arg(dep.name);
        }
    }

    if (hardDeps.isEmpty()) {
        return "1'b1";
    }

    return hardDeps.join(" & ");
}

QString QSocPowerPrimitive::getSoftDependencySignal(const PowerDomain &domain)
{
    QStringList softDeps;
    for (const auto &dep : domain.depends) {
        if (dep.type == "soft") {
            softDeps << QString("rdy_%1").arg(dep.name);
        }
    }

    if (softDeps.isEmpty()) {
        return "1'b1";
    }

    return softDeps.join(" & ");
}

/* Typst Power Diagram Generation */

QString QSocPowerPrimitive::escapeTypstId(const QString &str) const
{
    // Replace non-alphanumeric characters with underscores
    QString result;
    for (const QChar &c : str) {
        if (c.isLetterOrNumber() || c == '_' || c == '-') {
            result += c;
        } else {
            result += '_';
        }
    }
    return result;
}

QString QSocPowerPrimitive::typstHeader() const
{
    return QStringLiteral(
        "#import \"@preview/circuiteria:0.2.1\": *\n"
        "#import \"@preview/cetz:0.3.4\": draw\n"
        "#set page(width: auto, height: auto, margin: .5cm)\n"
        "#set text(font: \"Sarasa Mono SC\", size: 10pt)\n"
        "#align(center)[\n"
        "  = Power tree\n"
        "  #text(size: 8pt, fill: gray)[Generated by QSoC.]\n"
        "]\n"
        "#v(0.5cm)\n"
        "#circuit({\n");
}

QString QSocPowerPrimitive::typstLegend() const
{
    const float y  = -1.5f;
    const float x  = 0.0f;
    const float w  = 1.6f; // Wider blocks to fit text
    const float sp = 4.0f; // Increased spacing for wider blocks
    QTextStream s;
    QString     result;

    s.setString(&result);

    s << "  // === Legend ===\n";

    // AO Domain - Gray
    s << "  element.block(x: " << x << ", y: " << (y + 0.3f) << ", w: " << w << ", h: 0.8, "
      << "id: \"legend_ao\", name: \"AO\", fill: gray, "
      << "ports: (west: ((id: \"i\"),), east: ((id: \"o\"),)))\n";
    s << "  draw.content((" << (x + w / 2) << ", " << (y - 0.8f) << "), [AO])\n";

    // Root Domain - Green
    s << "  element.block(x: " << (x + sp) << ", y: " << (y + 0.3f) << ", w: " << w << ", h: 0.8, "
      << "id: \"legend_root\", name: \"ROOT\", fill: util.colors.green, "
      << "ports: (west: ((id: \"i\"),), east: ((id: \"o\"),)))\n";
    s << "  draw.content((" << (x + sp + w / 2) << ", " << (y - 0.8f) << "), [ROOT])\n";

    // Normal Domain - Blue
    s << "  element.block(x: " << (x + sp * 2) << ", y: " << (y + 0.3f) << ", w: " << w
      << ", h: 0.8, "
      << "id: \"legend_normal\", name: \"NORM\", fill: util.colors.blue, "
      << "ports: (west: ((id: \"i\"),), east: ((id: \"o\"),)))\n";
    s << "  draw.content((" << (x + sp * 2 + w / 2) << ", " << (y - 0.8f) << "), [NORMAL])\n";

    // FSM - Orange
    s << "  element.block(x: " << (x + sp * 3) << ", y: " << (y + 0.3f) << ", w: " << w
      << ", h: 0.8, "
      << "id: \"legend_fsm\", name: \"FSM\", fill: util.colors.orange, "
      << "ports: (west: ((id: \"i\"),), east: ((id: \"o\"),)))\n";
    s << "  draw.content((" << (x + sp * 3 + w / 2) << ", " << (y - 0.8f) << "), [FSM])\n";

    s << "\n";

    return result;
}

QString QSocPowerPrimitive::typstDomain(const PowerDomain &domain, float x, float y) const
{
    QTextStream s;
    QString     result;
    s.setString(&result);

    const QString domainName = domain.name;
    const QString did        = escapeTypstId(domainName);

    const float gx = x;
    const float gy = y;

    // Infer domain type from dependencies
    QString domainType;
    QString domainColor;
    if (domain.depends.isEmpty()) {
        // Check if this was explicitly set as empty list vs missing field
        // For simplicity, treat empty list as AO (always-on)
        domainType  = "ao";
        domainColor = "gray";
    } else {
        // Has dependencies, check if any exist
        bool hasActualDeps = false;
        for (const auto &dep : domain.depends) {
            if (!dep.name.isEmpty()) {
                hasActualDeps = true;
                break;
            }
        }
        if (hasActualDeps) {
            domainType  = "normal";
            domainColor = "util.colors.blue";
        } else {
            // Empty dependency list means root domain
            domainType  = "root";
            domainColor = "util.colors.green";
        }
    }

    // For YAML nodes that explicitly set depend: [] (empty), treat as root
    // This requires checking the original YAML, but for now use simple heuristic:
    // If depends list exists but is empty or all names are empty, it's root
    if (domain.depends.isEmpty()
        || (domain.depends.size() == 1 && domain.depends[0].name.isEmpty())) {
        // Could be AO or root, need more context
        // Default to AO for now
    }

    const QString typeLabel = domainType.toUpper();

    // Build dependency list for display
    QStringList dependsList;
    for (const auto &dep : domain.depends) {
        if (!dep.name.isEmpty()) {
            dependsList << QString("%1(%2)").arg(dep.name, dep.type);
        }
    }

    s << "  // ---- " << domainName << " [" << domainType << "] ----\n";

    // Calculate domain block height based on number of dependencies
    const int   numDeps   = dependsList.size();
    const float domHeight = (numDeps > 0) ? qMax(1.5f, 0.6f * numDeps) : 1.2f;

    // Generate domain block
    s << "  element.block(\n";
    s << "    x: " << (gx + 0.0f) << ", y: " << (gy + 0.3f) << ", w: 1.8, h: " << domHeight
      << ",\n";
    s << "    id: \"" << did << "_DOM\", name: \"" << typeLabel << "\", fill: " << domainColor
      << ",\n";

    if (numDeps > 0) {
        s << "    ports: (west: (";
        for (int i = 0; i < numDeps; ++i) {
            if (i > 0)
                s << ", ";
            s << "(id: \"in" << i << "\")";
        }
        s << ",), east: ((id: \"out\"),))\n";
    } else {
        s << "    ports: (west: ((id: \"in\"),), east: ((id: \"out\"),))\n";
    }
    s << "  )\n";

    // Add domain name and voltage labels
    s << "  draw.content((" << (gx + 0.9f) << ", " << (gy - 0.3f) << "), text(size: 8pt)["
      << domainName << "])\n";
    s << "  draw.content((" << (gx + 0.9f) << ", " << (gy - 0.7f) << "), text(size: 6pt)["
      << domain.v_mv << "mV])\n";

    // Add dependency input stubs
    if (numDeps > 0) {
        for (int i = 0; i < dependsList.size(); ++i) {
            s << "  wire.stub(\"" << did << "_DOM-port-in" << i << "\", \"west\", name: \""
              << dependsList[i] << "\")\n";
        }
    }

    QString prevAnchor = QString("%1_DOM-port-out").arg(did);

    // Align FSM with domain center
    const float fsmY = gy + domHeight / 2.0f - 0.6f;

    // Emit FSM block
    s << "  element.block(\n";
    s << "    x: " << (gx + 3.0f) << ", y: " << (fsmY + 0.3f) << ", w: 1.5, h: 1.2,\n";
    s << "    id: \"" << did << "_FSM\", name: \"FSM\", fill: util.colors.orange,\n";
    s << "    ports: (west: ((id: \"in\"),), east: ((id: \"out\"),))\n";
    s << "  )\n";

    // Add FSM timing parameters
    s << "  draw.content((" << (gx + 3.75f) << ", " << (fsmY - 0.3f)
      << "), text(size: 6pt)[wait:" << domain.wait_dep << "])\n";
    s << "  draw.content((" << (gx + 3.75f) << ", " << (fsmY - 0.6f)
      << "), text(size: 6pt)[on:" << domain.settle_on << "])\n";
    s << "  draw.content((" << (gx + 3.75f) << ", " << (fsmY - 0.9f)
      << "), text(size: 6pt)[off:" << domain.settle_off << "])\n";

    // Wire from domain to FSM
    s << "  wire.wire(\"" << escapeTypstId(QString("w_%1_dom_fsm").arg(did)) << "\", (\n";
    s << "    \"" << prevAnchor << "\", \"" << did << "_FSM-port-in\"\n";
    s << "  ))\n";

    prevAnchor = QString("%1_FSM-port-out").arg(did);

    // Emit reset synchronizers (follow entries)
    const int numSync = domain.follow_entries.size();
    for (int syncIdx = 0; syncIdx < numSync; ++syncIdx) {
        const auto   &followEntry = domain.follow_entries[syncIdx];
        const QString syncId      = QString("%1_SYNC%2").arg(did).arg(syncIdx);

        s << "  element.block(\n";
        s << "    x: " << (gx + 5.5f + syncIdx * 1.8f) << ", y: " << (fsmY + 0.3f)
          << ", w: 1.5, h: 1.2,\n";
        s << "    id: \"" << syncId << "\", name: \"SYNC\", fill: util.colors.yellow,\n";
        s << "    ports: (west: ((id: \"in\"),), east: ((id: \"out\"),))\n";
        s << "  )\n";

        // Add clock, reset, stage labels
        s << "  draw.content((" << (gx + 6.25f + syncIdx * 1.8f) << ", " << (fsmY - 0.3f)
          << "), text(size: 6pt)[" << followEntry.clock << "])\n";
        s << "  draw.content((" << (gx + 6.25f + syncIdx * 1.8f) << ", " << (fsmY - 0.6f)
          << "), text(size: 6pt)[" << followEntry.reset << "])\n";
        s << "  draw.content((" << (gx + 6.25f + syncIdx * 1.8f) << ", " << (fsmY - 0.9f)
          << "), text(size: 6pt)[stage:" << followEntry.stage << "])\n";

        // Wire from previous block to this SYNC
        QString wireId;
        if (syncIdx == 0) {
            wireId = QString("w_%1_fsm_sync%2").arg(did).arg(syncIdx);
        } else {
            wireId = QString("w_%1_sync%2_sync%3").arg(did).arg(syncIdx - 1).arg(syncIdx);
        }

        s << "  wire.wire(\"" << escapeTypstId(wireId) << "\", (\n";
        s << "    \"" << prevAnchor << "\", \"" << syncId << "-port-in\"\n";
        s << "  ))\n";

        prevAnchor = QString("%1-port-out").arg(syncId);
    }

    // Final output - arrow with label
    // Arrow Y should match FSM/SYNC port Y (center of 1.2 height block at fsmY + 0.3)
    const float finalX   = (numSync > 0) ? (gx + 5.5f + numSync * 1.8f + 2.0f) : (gx + 5.5f);
    const float arrowEnd = finalX + 2.0f;
    const float outY     = fsmY + 0.3f + 0.6f; // FSM/SYNC port center Y

    s << "  draw.line(\"" << prevAnchor << "\", (" << arrowEnd << ", " << outY
      << "), mark: (end: \">\", fill: black))\n";
    s << "  draw.content((" << (arrowEnd + 0.3f) << ", " << outY << "), anchor: \"west\", [rdy_"
      << domainName << "])\n";
    s << "\n";

    return result;
}

bool QSocPowerPrimitive::generateTypstDiagram(
    const PowerControllerConfig &config, const QString &outputPath)
{
    const QFileInfo outputInfo(outputPath);
    const auto      artifact
        = QSocPaths::resolveArtifactPath(outputInfo.absolutePath(), outputInfo.fileName());
    if (!artifact.isValid()) {
        QSocConsole::warn() << artifact.error;
        return false;
    }

    QFile file(artifact.path);
    if (!file.open(QIODevice::WriteOnly)) {
        QSocConsole::warn() << "Failed to open file for writing:" << artifact.path;
        return false;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    // Generate header and legend
    out << typstHeader();
    out << typstLegend();

    // Layout parameters - vertical stacking with dynamic spacing
    const float x0          = 0.0f;
    const float extraMargin = 2.0f; // Extra margin between domains

    float currentY = -5.0f; // Start below legend

    // Generate each domain with dynamic spacing
    for (int idx = 0; idx < config.domains.size(); ++idx) {
        const auto &domain = config.domains[idx];

        // Calculate domain height based on dependencies
        int numDeps = 0;
        for (const auto &dep : domain.depends) {
            if (!dep.name.isEmpty()) {
                numDeps++;
            }
        }
        float domHeight = (numDeps > 0) ? qMax(1.5f, 0.6f * numDeps) : 1.2f;

        // Position domain at currentY
        out << typstDomain(domain, x0, currentY);

        // Move to next domain position
        currentY -= domHeight + extraMargin + 2.0f; // Extra padding for labels
    }

    // Close circuit
    out << "})\n";

    file.close();

    QSocConsole::info() << "Generated Typst diagram:" << artifact.path;
    return true;
}
