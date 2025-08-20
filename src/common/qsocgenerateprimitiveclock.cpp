#include "qsocgenerateprimitiveclock.h"
#include "qsocgeneratemanager.h"
#include "qsocverilogutils.h"
#include <cmath>
#include <QDebug>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QSet>

/**
 * Clock generator with signal deduplication support.
 *
 * Features:
 * - Port deduplication: Same-name signals appear only once in module ports
 * - Parameter unification: All qsoc_tc_clk_gate use CLOCK_DURING_RESET parameter
 * - Duplicate target detection: ERROR messages for illegal duplicate outputs
 * - Output-priority deduplication: Output signals take precedence over inputs
 *
 * Implementation uses QSet for efficient duplicate tracking across:
 * ICG signals, MUX signals, divider controls, and reset signals.
 */

QSocClockPrimitive::QSocClockPrimitive(QSocGenerateManager *parent)
    : m_parent(parent)
{}

bool QSocClockPrimitive::generateClockController(const YAML::Node &clockNode, QTextStream &out)
{
    if (!clockNode || !clockNode.IsMap()) {
        qWarning() << "Invalid clock node provided";
        return false;
    }

    // Parse configuration
    ClockControllerConfig config = parseClockConfig(clockNode);

    if (config.inputs.isEmpty() || config.targets.isEmpty()) {
        qWarning() << "Clock configuration must have at least one input and target";
        return false;
    }

    // Generate or update clock_cell.v file
    if (m_parent && m_parent->getProjectManager()) {
        QString outputDir = m_parent->getProjectManager()->getOutputPath();
        if (!generateClockCellFile(outputDir)) {
            qWarning() << "Failed to generate clock_cell.v file";
            return false;
        }
    }

    // Generate Verilog code (without template cells)
    generateModuleHeader(config, out);
    generateWireDeclarations(config, out);
    generateClockLogic(config, out);
    generateOutputAssignments(config, out);

    // Close module
    out << "\nendmodule\n\n";

    return true;
}

QSocClockPrimitive::ClockControllerConfig QSocClockPrimitive::parseClockConfig(
    const YAML::Node &clockNode)
{
    ClockControllerConfig config;

    // Parse basic properties
    if (!clockNode["name"]) {
        qCritical() << "Error: 'name' field is required in clock configuration";
        qCritical() << "Example: clock: { name: my_clk_ctrl, ... }";
        return config;
    }
    config.name       = QString::fromStdString(clockNode["name"].as<std::string>());
    config.moduleName = config.name; // Use same name for module

    if (!clockNode["clock"]) {
        qCritical() << "Error: 'clock' field is required in clock configuration";
        qCritical() << "Example: clock: { clock: clk_sys, ... }";
        return config;
    }
    config.clock = QString::fromStdString(clockNode["clock"].as<std::string>());

    // Optional ref_clock for GF_MUX
    if (clockNode["ref_clock"]) {
        config.ref_clock = QString::fromStdString(clockNode["ref_clock"].as<std::string>());
    }

    // Optional test enable
    if (clockNode["test_en"]) {
        config.test_en = QString::fromStdString(clockNode["test_en"].as<std::string>());
    }

    // Parse clock inputs
    if (clockNode["input"] && clockNode["input"].IsMap()) {
        for (auto it = clockNode["input"].begin(); it != clockNode["input"].end(); ++it) {
            ClockInput input;
            input.name = QString::fromStdString(it->first.as<std::string>());

            if (it->second.IsMap()) {
                if (it->second["freq"]) {
                    input.freq = QString::fromStdString(it->second["freq"].as<std::string>());
                }
                if (it->second["duty"]) {
                    input.duty = QString::fromStdString(it->second["duty"].as<std::string>());
                }
            }
            config.inputs.append(input);
        }
    }

    // Parse clock targets
    if (clockNode["target"] && clockNode["target"].IsMap()) {
        for (auto it = clockNode["target"].begin(); it != clockNode["target"].end(); ++it) {
            ClockTarget target;
            target.name = QString::fromStdString(it->first.as<std::string>());

            if (it->second["freq"]) {
                target.freq = QString::fromStdString(it->second["freq"].as<std::string>());
            }

            // Parse target-level ICG
            if (it->second["icg"] && it->second["icg"].IsMap()) {
                if (it->second["icg"]["enable"]) {
                    target.icg.enable = QString::fromStdString(
                        it->second["icg"]["enable"].as<std::string>());
                }
                target.icg.polarity = QString::fromStdString(
                    it->second["icg"]["polarity"].as<std::string>("high"));
                if (it->second["icg"]["test_enable"]) {
                    target.icg.test_enable = QString::fromStdString(
                        it->second["icg"]["test_enable"].as<std::string>());
                }
                if (it->second["icg"]["reset"]) {
                    target.icg.reset = QString::fromStdString(
                        it->second["icg"]["reset"].as<std::string>());
                }
            }

            // Parse target-level divider
            if (it->second["div"] && it->second["div"].IsMap()) {
                target.div.ratio          = it->second["div"]["ratio"].as<int>(1);
                target.div.width          = it->second["div"]["width"].as<int>(0);
                target.div.default_val    = it->second["div"]["default_val"].as<int>(0);
                target.div.clock_on_reset = it->second["div"]["clock_on_reset"].as<bool>(false);

                if (it->second["div"]["reset"]) {
                    target.div.reset = QString::fromStdString(
                        it->second["div"]["reset"].as<std::string>());
                }
                if (it->second["div"]["enable"]) {
                    target.div.enable = QString::fromStdString(
                        it->second["div"]["enable"].as<std::string>());
                }
                if (it->second["div"]["test_enable"]) {
                    target.div.test_enable = QString::fromStdString(
                        it->second["div"]["test_enable"].as<std::string>());
                }
                if (it->second["div"]["div_signal"]) {
                    target.div.div_signal = QString::fromStdString(
                        it->second["div"]["div_signal"].as<std::string>());
                }
                if (it->second["div"]["div_valid"]) {
                    target.div.div_valid = QString::fromStdString(
                        it->second["div"]["div_valid"].as<std::string>());
                }
                if (it->second["div"]["div_ready"]) {
                    target.div.div_ready = QString::fromStdString(
                        it->second["div"]["div_ready"].as<std::string>());
                }
                if (it->second["div"]["count"]) {
                    target.div.count = QString::fromStdString(
                        it->second["div"]["count"].as<std::string>());
                }
            }

            // Target-level STA guide configuration
            if (it->second["sta_guide"] && it->second["sta_guide"].IsMap()) {
                if (it->second["sta_guide"]["cell"]) {
                    target.sta_guide.cell = QString::fromStdString(
                        it->second["sta_guide"]["cell"].as<std::string>());
                }
                if (it->second["sta_guide"]["in"]) {
                    target.sta_guide.in = QString::fromStdString(
                        it->second["sta_guide"]["in"].as<std::string>());
                }
                if (it->second["sta_guide"]["out"]) {
                    target.sta_guide.out = QString::fromStdString(
                        it->second["sta_guide"]["out"].as<std::string>());
                }
                if (it->second["sta_guide"]["instance"]) {
                    target.sta_guide.instance = QString::fromStdString(
                        it->second["sta_guide"]["instance"].as<std::string>());
                }
            }

            // Parse target-level inverter - key existence only
            target.inv = it->second["inv"] ? true : false;

            // Parse links
            if (it->second["link"] && it->second["link"].IsMap()) {
                for (auto linkIt = it->second["link"].begin(); linkIt != it->second["link"].end();
                     ++linkIt) {
                    ClockLink link;
                    link.source = QString::fromStdString(linkIt->first.as<std::string>());

                    // Link-level inverter flag - key existence only
                    if (linkIt->second.IsMap() && linkIt->second["inv"]) {
                        link.inv = true;
                    } else {
                        link.inv = false;
                    }

                    // Link-level ICG configuration
                    if (linkIt->second.IsMap() && linkIt->second["icg"]
                        && linkIt->second["icg"].IsMap()) {
                        if (linkIt->second["icg"]["enable"]) {
                            link.icg.enable = QString::fromStdString(
                                linkIt->second["icg"]["enable"].as<std::string>());
                        }
                        link.icg.polarity = QString::fromStdString(
                            linkIt->second["icg"]["polarity"].as<std::string>("high"));
                        if (linkIt->second["icg"]["test_enable"]) {
                            link.icg.test_enable = QString::fromStdString(
                                linkIt->second["icg"]["test_enable"].as<std::string>());
                        }
                        if (linkIt->second["icg"]["reset"]) {
                            link.icg.reset = QString::fromStdString(
                                linkIt->second["icg"]["reset"].as<std::string>());
                        }
                    }

                    // Link-level divider configuration
                    if (linkIt->second.IsMap() && linkIt->second["div"]
                        && linkIt->second["div"].IsMap()) {
                        link.div.ratio          = linkIt->second["div"]["ratio"].as<int>(1);
                        link.div.width          = linkIt->second["div"]["width"].as<int>(0);
                        link.div.default_val    = linkIt->second["div"]["default_val"].as<int>(0);
                        link.div.clock_on_reset = linkIt->second["div"]["clock_on_reset"].as<bool>(
                            false);

                        if (linkIt->second["div"]["reset"]) {
                            link.div.reset = QString::fromStdString(
                                linkIt->second["div"]["reset"].as<std::string>());
                        }
                        if (linkIt->second["div"]["enable"]) {
                            link.div.enable = QString::fromStdString(
                                linkIt->second["div"]["enable"].as<std::string>());
                        }
                        if (linkIt->second["div"]["test_enable"]) {
                            link.div.test_enable = QString::fromStdString(
                                linkIt->second["div"]["test_enable"].as<std::string>());
                        }
                        if (linkIt->second["div"]["div_signal"]) {
                            link.div.div_signal = QString::fromStdString(
                                linkIt->second["div"]["div_signal"].as<std::string>());
                        }
                        if (linkIt->second["div"]["div_valid"]) {
                            link.div.div_valid = QString::fromStdString(
                                linkIt->second["div"]["div_valid"].as<std::string>());
                        }
                        if (linkIt->second["div"]["div_ready"]) {
                            link.div.div_ready = QString::fromStdString(
                                linkIt->second["div"]["div_ready"].as<std::string>());
                        }
                        if (linkIt->second["div"]["count"]) {
                            link.div.count = QString::fromStdString(
                                linkIt->second["div"]["count"].as<std::string>());
                        }
                    }

                    // Link-level STA guide configuration
                    if (linkIt->second.IsMap() && linkIt->second["sta_guide"]
                        && linkIt->second["sta_guide"].IsMap()) {
                        if (linkIt->second["sta_guide"]["cell"]) {
                            link.sta_guide.cell = QString::fromStdString(
                                linkIt->second["sta_guide"]["cell"].as<std::string>());
                        }
                        if (linkIt->second["sta_guide"]["in"]) {
                            link.sta_guide.in = QString::fromStdString(
                                linkIt->second["sta_guide"]["in"].as<std::string>());
                        }
                        if (linkIt->second["sta_guide"]["out"]) {
                            link.sta_guide.out = QString::fromStdString(
                                linkIt->second["sta_guide"]["out"].as<std::string>());
                        }
                        if (linkIt->second["sta_guide"]["instance"]) {
                            link.sta_guide.instance = QString::fromStdString(
                                linkIt->second["sta_guide"]["instance"].as<std::string>());
                        }
                    }

                    target.links.append(link);
                }
            }

            // Parse multiplexer configuration (only if ≥2 links) - New format per documentation
            if (target.links.size() >= 2) {
                // Parse target-level MUX signals (new format)
                if (it->second["select"]) {
                    target.select = QString::fromStdString(it->second["select"].as<std::string>());
                }
                if (it->second["reset"]) {
                    target.reset = QString::fromStdString(it->second["reset"].as<std::string>());
                }
                if (it->second["test_enable"]) {
                    target.test_enable = QString::fromStdString(
                        it->second["test_enable"].as<std::string>());
                }
                if (it->second["test_clock"]) {
                    target.test_clock = QString::fromStdString(
                        it->second["test_clock"].as<std::string>());
                }

                // Auto-select mux type based on reset presence
                if (!target.reset.isEmpty()) {
                    target.mux.type = GF_MUX; // Has reset → Glitch-free mux
                } else {
                    target.mux.type = STD_MUX; // No reset → Standard mux
                }

                // Validation: multi-link requires select signal
                if (target.select.isEmpty()) {
                    qCritical() << "Error: 'select' signal is required for multi-link target:"
                                << target.name;
                    qCritical() << "Example: target: { link: {clk1: ~, clk2: ~}, select: sel_sig }";
                    return config;
                }
            }

            config.targets.append(target);
        }
    }

    // Check for duplicate target names (output signals)
    QSet<QString> targetNames;
    for (const auto &target : config.targets) {
        if (targetNames.contains(target.name)) {
            qCritical() << "ERROR: Duplicate output target name:" << target.name;
            qCritical() << "Each target must have a unique output signal name";
        } else {
            targetNames.insert(target.name);
        }
    }

    return config;
}

void QSocClockPrimitive::generateModuleHeader(const ClockControllerConfig &config, QTextStream &out)
{
    out << "\nmodule " << config.moduleName << " (\n";

    QStringList portList;

    // Add default clock if specified
    if (!config.clock.isEmpty()) {
        portList << QString("    input  %1     /**< Default synchronous clock */").arg(config.clock);
    }

    // Add input clocks (skip if already added as default clock)
    for (const auto &input : config.inputs) {
        if (input.name == config.clock) {
            continue; // Skip duplicate default clock
        }
        QString comment = QString("/**< Clock input: %1").arg(input.name);
        if (!input.freq.isEmpty()) {
            comment += QString(" (%1)").arg(input.freq);
        }
        comment += " */";
        portList << QString("    input  %1,    %2").arg(input.name, comment);
    }

    // Add target clocks
    for (const auto &target : config.targets) {
        QString comment = QString("/**< Clock target: %1").arg(target.name);
        if (!target.freq.isEmpty()) {
            comment += QString(" (%1)").arg(target.freq);
        }
        comment += " */";
        portList << QString("    output %1,    %2").arg(target.name, comment);
    }

    // Add dynamic divider interface ports (target-level)
    for (const auto &target : config.targets) {
        if (target.div.ratio > 1) {
            // Add dynamic division ratio input port
            if (!target.div.div_signal.isEmpty()) {
                portList
                    << QString("    input  wire [%1:0] %2,    /**< Dynamic division ratio for %3 */")
                           .arg(target.div.width - 1)
                           .arg(target.div.div_signal, target.name);
            }

            // Add division value valid signal port
            if (!target.div.div_valid.isEmpty()) {
                portList << QString("    input  wire %1,    /**< Division valid signal for %2 */")
                                .arg(target.div.div_valid, target.name);
            }

            // Add division ready output port
            if (!target.div.div_ready.isEmpty()) {
                portList << QString("    output wire %1,    /**< Division ready signal for %2 */")
                                .arg(target.div.div_ready, target.name);
            }

            // Add cycle counter output port
            if (!target.div.count.isEmpty()) {
                portList << QString("    output wire [%1:0] %2,    /**< Cycle counter for %3 */")
                                .arg(target.div.width - 1)
                                .arg(target.div.count, target.name);
            }

            // Add enable signal port
            if (!target.div.enable.isEmpty()) {
                portList << QString("    input  wire %1,    /**< Division enable for %2 */")
                                .arg(target.div.enable, target.name);
            }
        }
    }

    // Add dynamic divider interface ports (link-level)
    for (const auto &target : config.targets) {
        for (const auto &link : target.links) {
            if (link.div.ratio > 1) {
                QString linkName = QString("%1_from_%2").arg(target.name, link.source);

                // Add dynamic division ratio input port
                if (!link.div.div_signal.isEmpty()) {
                    portList << QString(
                                    "    input  wire [%1:0] %2,    /**< Dynamic division ratio for "
                                    "link %3 */")
                                    .arg(link.div.width - 1)
                                    .arg(link.div.div_signal, linkName);
                }

                // Add division value valid signal port
                if (!link.div.div_valid.isEmpty()) {
                    portList
                        << QString(
                               "    input  wire %1,    /**< Division valid signal for link %2 */")
                               .arg(link.div.div_valid, linkName);
                }

                // Add division ready output port
                if (!link.div.div_ready.isEmpty()) {
                    portList
                        << QString(
                               "    output wire %1,    /**< Division ready signal for link %2 */")
                               .arg(link.div.div_ready, linkName);
                }

                // Add cycle counter output port
                if (!link.div.count.isEmpty()) {
                    portList
                        << QString("    output wire [%1:0] %2,    /**< Cycle counter for link %3 */")
                               .arg(link.div.width - 1)
                               .arg(link.div.count, linkName);
                }

                // Add enable signal port
                if (!link.div.enable.isEmpty()) {
                    portList << QString("    input  wire %1,    /**< Division enable for link %2 */")
                                    .arg(link.div.enable, linkName);
                }
            }
        }
    }

    // Add ICG interface ports (target-level)
    QSet<QString> addedSignals;
    for (const auto &target : config.targets) {
        if (!target.icg.enable.isEmpty() && !addedSignals.contains(target.icg.enable)) {
            portList << QString("    input  wire %1,    /**< ICG enable for %2 */")
                            .arg(target.icg.enable, target.name);
            addedSignals.insert(target.icg.enable);
        }
        if (!target.icg.test_enable.isEmpty() && !addedSignals.contains(target.icg.test_enable)) {
            portList << QString("    input  wire %1,    /**< ICG test enable for %2 */")
                            .arg(target.icg.test_enable, target.name);
            addedSignals.insert(target.icg.test_enable);
        }
        if (!target.icg.reset.isEmpty() && !addedSignals.contains(target.icg.reset)) {
            portList << QString("    input  wire %1,    /**< ICG reset for %2 */")
                            .arg(target.icg.reset, target.name);
            addedSignals.insert(target.icg.reset);
        }
    }

    // Add MUX interface ports (target-level)
    for (const auto &target : config.targets) {
        if (target.links.size() >= 2) { // Only for multi-source targets
            if (!target.select.isEmpty() && !addedSignals.contains(target.select)) {
                portList << QString("    input  wire %1,    /**< MUX select for %2 */")
                                .arg(target.select, target.name);
                addedSignals.insert(target.select);
            }
            if (!target.reset.isEmpty() && !addedSignals.contains(target.reset)) {
                portList << QString("    input  wire %1,    /**< MUX reset for %2 */")
                                .arg(target.reset, target.name);
                addedSignals.insert(target.reset);
            }
            if (!target.test_enable.isEmpty() && !addedSignals.contains(target.test_enable)) {
                portList << QString("    input  wire %1,    /**< MUX test enable for %2 */")
                                .arg(target.test_enable, target.name);
                addedSignals.insert(target.test_enable);
            }
            if (!target.test_clock.isEmpty() && !addedSignals.contains(target.test_clock)) {
                portList << QString("    input  wire %1,    /**< MUX test clock for %2 */")
                                .arg(target.test_clock, target.name);
                addedSignals.insert(target.test_clock);
            }
        }
    }

    // Add target-level reset signals for DIV (if not already added via ICG/MUX)
    QStringList addedResets;
    for (const auto &target : config.targets) {
        if (target.div.ratio > 1 && !target.div.reset.isEmpty()) {
            if (!addedResets.contains(target.div.reset)
                && !addedSignals.contains(target.div.reset)) {
                portList << QString("    input  wire %1,    /**< Division reset for %2 */")
                                .arg(target.div.reset, target.name);
                addedResets << target.div.reset;
                addedSignals.insert(target.div.reset);
            }
        }
    }

    // Add link-level reset signals for DIV (if not already added)
    for (const auto &target : config.targets) {
        for (const auto &link : target.links) {
            if (link.div.ratio > 1 && !link.div.reset.isEmpty()) {
                if (!addedResets.contains(link.div.reset)
                    && !addedSignals.contains(link.div.reset)) {
                    QString linkName = QString("%1_from_%2").arg(target.name, link.source);
                    portList << QString("    input  wire %1,    /**< Link division reset for %2 */")
                                    .arg(link.div.reset, linkName);
                    addedResets << link.div.reset;
                    addedSignals.insert(link.div.reset);
                }
            }
        }
    }

    // Add test enable if specified
    if (!config.test_en.isEmpty() && !addedSignals.contains(config.test_en)) {
        portList << QString("    input  %1     /**< Test enable signal */").arg(config.test_en);
    }

    // Join ports and remove last comma
    QString ports = portList.join(",\n");
    if (ports.endsWith(",")) {
        ports = ports.left(ports.length() - 1);
    }

    out << ports << "\n";
    out << ");\n\n";
}

void QSocClockPrimitive::generateWireDeclarations(
    const ClockControllerConfig &config, QTextStream &out)
{
    out << "    /* Wire declarations for clock connections */\n";

    for (const auto &target : config.targets) {
        for (int i = 0; i < target.links.size(); ++i) {
            const auto &link     = target.links[i];
            QString     wireName = getLinkWireName(target.name, link.source, i);
            out << "    wire " << wireName << ";\n";
        }
    }

    out << "\n";
}

void QSocClockPrimitive::generateClockLogic(const ClockControllerConfig &config, QTextStream &out)
{
    out << "    /* Clock logic instances */\n";

    for (const auto &target : config.targets) {
        for (int i = 0; i < target.links.size(); ++i) {
            const auto &link = target.links[i];
            generateClockInstance(link, target.name, i, out);
        }
    }

    out << "\n";
}

void QSocClockPrimitive::generateOutputAssignments(
    const ClockControllerConfig &config, QTextStream &out)
{
    out << "    /* Clock output assignments */\n";

    for (const auto &target : config.targets) {
        QString currentSignal;
        QString instanceName = QString("u_%1_target").arg(target.name);

        // Step 1: Handle mux/single source selection
        if (target.links.size() == 1) {
            // Single source
            QString wireName = getLinkWireName(target.name, target.links[0].source, 0);
            currentSignal    = wireName;

            // Apply legacy inversion if needed (deprecated)
            if (target.links[0].inv) {
                QString invertWire = QString("%1_legacy_inv").arg(target.name);
                out << "    wire " << invertWire << ";\n";
                out << "    assign " << invertWire << " = ~" << wireName << ";\n";
                currentSignal = invertWire;
            }
        } else if (target.links.size() >= 2) {
            // Multiple sources - generate multiplexer first
            QString muxOutput = QString("%1_mux_out").arg(target.name);
            out << "    wire " << muxOutput << ";\n";
            generateMuxInstance(target, config, out, muxOutput);
            currentSignal = muxOutput;
        }

        // Step 2: Apply target-level processing chain
        // Order: currentSignal -> ICG -> DIV -> INV -> target.name

        // Target-level ICG
        if (!target.icg.enable.isEmpty()) {
            QString icgOutput = QString("%1_icg_out").arg(target.name);
            out << "    wire " << icgOutput << ";\n";
            out << "    qsoc_tc_clk_gate #(\n";
            out << "        .CLOCK_DURING_RESET(1'b0)\n";
            out << "    ) " << instanceName << "_icg (\n";
            out << "        .clk(" << currentSignal << "),\n";
            out << "        .en(" << target.icg.enable << "),\n";
            QString testEn = target.icg.test_enable.isEmpty()
                                 ? (config.test_en.isEmpty() ? "1'b0" : config.test_en)
                                 : target.icg.test_enable;
            out << "        .test_en(" << testEn << "),\n";
            out << "        .rst_n(" << (target.icg.reset.isEmpty() ? "1'b1" : target.icg.reset)
                << "),\n";
            out << "        .clk_out(" << icgOutput << ")\n";
            out << "    );\n";
            currentSignal = icgOutput;
        }

        // Target-level DIV
        if (target.div.ratio > 1) {
            // Validate width parameter
            if (target.div.width <= 0) {
                throw std::runtime_error(
                    QString("Clock divider for target '%1' requires explicit width specification")
                        .arg(target.name)
                        .toStdString());
            }

            QString divOutput = QString("%1_div_out").arg(target.name);
            out << "    wire " << divOutput << ";\n";
            out << "    qsoc_clk_div #(\n";
            out << "        .WIDTH(" << target.div.width << "),\n";
            out << "        .DEFAULT_VAL(" << target.div.default_val << "),\n";
            out << "        .CLOCK_DURING_RESET(" << (target.div.clock_on_reset ? "1'b1" : "1'b0")
                << ")\n";
            out << "    ) " << instanceName << "_div (\n";
            out << "        .clk(" << currentSignal << "),\n";
            out << "        .rst_n(" << (target.div.reset.isEmpty() ? "1'b1" : target.div.reset)
                << "),\n";
            out << "        .en(" << (target.div.enable.isEmpty() ? "1'b1" : target.div.enable)
                << "),\n";

            QString testEn = target.div.test_enable.isEmpty()
                                 ? (config.test_en.isEmpty() ? "1'b0" : config.test_en)
                                 : target.div.test_enable;
            out << "        .test_en(" << testEn << "),\n";

            // Dynamic or static division ratio
            if (!target.div.div_signal.isEmpty()) {
                out << "        .div(" << target.div.div_signal << "),\n";
            } else {
                out << "        .div(" << target.div.width << "'d" << target.div.ratio << "),\n";
            }

            out << "        .div_valid("
                << (target.div.div_valid.isEmpty() ? "1'b1" : target.div.div_valid) << "),\n";

            if (!target.div.div_ready.isEmpty()) {
                out << "        .div_ready(" << target.div.div_ready << "),\n";
            } else {
                out << "        .div_ready(),\n";
            }

            out << "        .clk_out(" << divOutput << "),\n";

            if (!target.div.count.isEmpty()) {
                out << "        .count(" << target.div.count << ")\n";
            } else {
                out << "        .count()\n";
            }
            out << "    );\n";
            currentSignal = divOutput;
        }

        // Target-level INV
        if (target.inv) {
            QString invOutput = QString("%1_inv_out").arg(target.name);
            out << "    wire " << invOutput << ";\n";
            out << "    qsoc_tc_clk_inv " << instanceName << "_inv (\n";
            out << "        .clk_in(" << currentSignal << "),\n";
            out << "        .clk_out(" << invOutput << ")\n";
            out << "    );\n";
            currentSignal = invOutput;
        }

        // Target-level STA guide
        if (!target.sta_guide.cell.isEmpty()) {
            QString staOutput = QString("%1_sta_out").arg(target.name);
            out << "    wire " << staOutput << ";\n";
            out << "    " << target.sta_guide.cell << " " << instanceName << "_sta (\n";
            out << "        ." << target.sta_guide.in << "(" << currentSignal << "),\n";
            out << "        ." << target.sta_guide.out << "(" << staOutput << ")\n";
            out << "    );\n";
            currentSignal = staOutput;
        }

        // Final assignment
        out << "    assign " << target.name << " = " << currentSignal << ";\n";
    }

    out << "\n";
}

void QSocClockPrimitive::generateClockInstance(
    const ClockLink &link, const QString &targetName, int linkIndex, QTextStream &out)
{
    QString wireName     = getLinkWireName(targetName, link.source, linkIndex);
    QString instanceName = getInstanceName(targetName, link.source, linkIndex);
    QString inputClk     = link.source;

    out << "    /*\n";
    out << "     * Link processing: " << link.source << " -> " << targetName;

    // Generate chain based on what's specified
    if (!link.icg.enable.isEmpty()) {
        out << " (icg)";
    }
    if (link.div.ratio > 1) {
        out << " (div/" << link.div.ratio << ")";
    }
    if (link.inv) {
        out << " (inv)";
    }
    out << "\n     */\n";

    // Generate processing chain
    bool hasProcessing = !link.icg.enable.isEmpty() || (link.div.ratio > 1) || link.inv;

    if (hasProcessing) {
        // Handle link-level processing: ICG → DIV → INV
        QString currentWire = inputClk;

        // Step 1: Link-level ICG
        if (!link.icg.enable.isEmpty()) {
            QString icgWire = wireName + "_preicg";
            out << "    wire " << icgWire << ";\n";
            out << "    qsoc_tc_clk_gate #(\n";
            out << "        .CLOCK_DURING_RESET(1'b0)\n";
            out << "    ) " << instanceName << "_icg (\n";
            out << "        .clk(" << currentWire << "),\n";
            out << "        .en(" << link.icg.enable << "),\n";
            QString testEn = link.icg.test_enable.isEmpty() ? "1'b0" : link.icg.test_enable;
            out << "        .test_en(" << testEn << "),\n";
            out << "        .rst_n(" << (link.icg.reset.isEmpty() ? "1'b1" : link.icg.reset)
                << "),\n";
            out << "        .clk_out(" << icgWire << ")\n";
            out << "    );\n";
            currentWire = icgWire;
        }

        // Step 2: Link-level divider
        if (link.div.ratio > 1) {
            // Validate width parameter
            if (link.div.width <= 0) {
                throw std::runtime_error(
                    QString("Clock divider for link '%1' requires explicit width specification")
                        .arg(wireName)
                        .toStdString());
            }

            QString divWire = wireName + "_prediv";
            out << "    wire " << divWire << ";\n";
            out << "    qsoc_clk_div #(\n";
            out << "        .WIDTH(" << link.div.width << "),\n";
            out << "        .DEFAULT_VAL(" << link.div.default_val << "),\n";
            out << "        .CLOCK_DURING_RESET(" << (link.div.clock_on_reset ? "1'b1" : "1'b0")
                << ")\n";
            out << "    ) " << instanceName << "_div (\n";
            out << "        .clk(" << currentWire << "),\n";
            out << "        .rst_n(" << (link.div.reset.isEmpty() ? "1'b1" : link.div.reset)
                << "),\n";
            out << "        .en(" << (link.div.enable.isEmpty() ? "1'b1" : link.div.enable)
                << "),\n";

            QString testEn = link.div.test_enable.isEmpty() ? "1'b0" : link.div.test_enable;
            out << "        .test_en(" << testEn << "),\n";

            // Dynamic or static division ratio
            if (!link.div.div_signal.isEmpty()) {
                out << "        .div(" << link.div.div_signal << "),\n";
            } else {
                out << "        .div(" << link.div.width << "'d" << link.div.ratio << "),\n";
            }

            out << "        .div_valid("
                << (link.div.div_valid.isEmpty() ? "1'b1" : link.div.div_valid) << "),\n";

            if (!link.div.div_ready.isEmpty()) {
                out << "        .div_ready(" << link.div.div_ready << "),\n";
            } else {
                out << "        .div_ready(),\n";
            }

            out << "        .clk_out(" << divWire << "),\n";

            if (!link.div.count.isEmpty()) {
                out << "        .count(" << link.div.count << ")\n";
            } else {
                out << "        .count()\n";
            }
            out << "    );\n";
            currentWire = divWire;
        }

        // Step 3: Link-level inverter
        if (link.inv) {
            QString invWire = QString("%1_inv_wire").arg(instanceName);
            out << "    wire " << invWire << ";\n";
            out << "    qsoc_tc_clk_inv " << instanceName << "_inv (\n";
            out << "        .clk_in(" << currentWire << "),\n";
            out << "        .clk_out(" << invWire << ")\n";
            out << "    );\n";
            currentWire = invWire;
        }

        // Step 4: Link-level STA guide
        if (!link.sta_guide.cell.isEmpty()) {
            QString staWire = QString("%1_sta_wire").arg(instanceName);
            out << "    wire " << staWire << ";\n";
            out << "    " << link.sta_guide.cell << " " << instanceName << "_sta (\n";
            out << "        ." << link.sta_guide.in << "(" << currentWire << "),\n";
            out << "        ." << link.sta_guide.out << "(" << staWire << ")\n";
            out << "    );\n";
            currentWire = staWire;
        }

        // Final assignment
        out << "    assign " << wireName << " = " << currentWire << ";\n";

    } else {
        // Simple pass-through case
        out << "    assign " << wireName << " = " << inputClk << ";\n";
    }

    out << "\n";
}

void QSocClockPrimitive::generateMuxInstance(
    const ClockTarget           &target,
    const ClockControllerConfig &config,
    QTextStream                 &out,
    const QString               &outputName)
{
    QString instanceName = QString("u_%1_mux").arg(target.name);
    QString muxOut       = outputName.isEmpty() ? target.name : outputName;

    // Generate intermediate wires for inversion if needed
    QStringList inputWires;
    for (int i = 0; i < target.links.size(); ++i) {
        const auto &link     = target.links[i];
        QString     wireName = getLinkWireName(target.name, link.source, i);

        if (link.inv) {
            QString invertedWire = QString("%1_inv").arg(wireName);
            out << "    wire " << invertedWire << ";\n";
            out << "    assign " << invertedWire << " = ~" << wireName << ";\n";
            inputWires << invertedWire;
        } else {
            inputWires << wireName;
        }
    }

    int numInputs = inputWires.size();
    int selWidth  = 0;
    if (numInputs > 1) {
        selWidth = 1;
        while ((1 << selWidth) < numInputs)
            selWidth++;
    }

    if (target.mux.type == STD_MUX) {
        // Standard mux using qsoc_clk_mux_raw
        out << "    qsoc_clk_mux_raw #(\n";
        out << "        .NUM_INPUTS(" << numInputs << ")\n";
        out << "    ) " << instanceName << " (\n";

        // Connect clock inputs as array
        out << "        .clk_in({";
        for (int i = numInputs - 1; i >= 0; --i) {
            out << inputWires[i];
            if (i > 0)
                out << ", ";
        }
        out << "}),\n";

        // Connect select signal
        out << "        .clk_sel(" << target.select << "),\n";
        out << "        .clk_out(" << muxOut << ")\n";
        out << "    );\n";

    } else if (target.mux.type == GF_MUX) {
        // Glitch-free mux using qsoc_clk_mux_gf
        out << "    qsoc_clk_mux_gf #(\n";
        out << "        .NUM_INPUTS(" << numInputs << "),\n";
        out << "        .NUM_SYNC_STAGES(2),\n";
        out << "        .CLOCK_DURING_RESET(1'b1)\n";
        out << "    ) " << instanceName << " (\n";

        // Connect clock inputs as array
        out << "        .clk_in({";
        for (int i = numInputs - 1; i >= 0; --i) {
            out << inputWires[i];
            if (i > 0)
                out << ", ";
        }
        out << "}),\n";

        // Connect DFT signals
        QString testClk = target.test_clock.isEmpty() ? "1'b0" : target.test_clock;
        QString testEn  = target.test_enable.isEmpty() ? "1'b0" : target.test_enable;
        out << "        .test_clk(" << testClk << "),\n";
        out << "        .test_en(" << testEn << "),\n";

        // Connect reset signal
        QString resetSig = target.reset.isEmpty() ? "1'b1" : target.reset;
        out << "        .async_rst_n(" << resetSig << "),\n";

        // Connect select signal
        out << "        .async_sel(" << target.select << "),\n";
        out << "        .clk_out(" << muxOut << ")\n";
        out << "    );\n";
    }

    out << "\n";
}

QSocClockPrimitive::MuxType QSocClockPrimitive::parseMuxType(const QString &typeStr)
{
    if (typeStr == "STD_MUX")
        return STD_MUX;
    if (typeStr == "GF_MUX")
        return GF_MUX;

    // Validate mux type
    qCritical() << "Error: Unknown mux type:" << typeStr;
    qCritical() << "Valid types: STD_MUX, GF_MUX";
    return STD_MUX; // Still return something for compilation
}

QString QSocClockPrimitive::getLinkWireName(
    const QString &targetName, const QString &sourceName, int linkIndex)
{
    if (linkIndex == 0) {
        return QString("clk_%1_from_%2").arg(targetName, sourceName);
    }
    return QString("clk_%1_from_%2_%3").arg(targetName, sourceName).arg(linkIndex);
}

QString QSocClockPrimitive::getInstanceName(
    const QString &targetName, const QString &sourceName, int linkIndex)
{
    if (linkIndex == 0) {
        return QString("u_%1_%2").arg(targetName, sourceName);
    }
    return QString("u_%1_%2_%3").arg(targetName, sourceName).arg(linkIndex);
}

bool QSocClockPrimitive::generateClockCellFile(const QString &outputDir)
{
    QString filePath = QDir(outputDir).filePath("clock_cell.v");

    QFile file(filePath);

    // Behavior:
    // - If file doesn't exist: create it with header, timescale, and ALL required cells
    // - If file exists but is incomplete: append ONLY missing cells at the end
    // - If file exists and complete: do nothing

    if (!file.exists()) {
        if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            qWarning() << "Cannot open clock_cell.v for writing:" << file.errorString();
            return false;
        }

        QTextStream out(&file);

        // Write file header
        out << "/**\n";
        out << " * @file clock_cell.v\n";
        out << " * @brief Template clock cells for QSoC clock primitives\n";
        out << " *\n";
        out << " * @details This file contains template clock cell modules for clock primitives.\n";
        out << " *          Auto-generated template file. Generated by qsoc.\n";
        out << " * CAUTION: Please replace the templates in this file\n";
        out << " *          with your technology's standard-cell implementations\n";
        out << " *          before using in production.\n";
        out << " */\n\n";

        out << "`timescale 1ns / 1ps\n\n";

        // Generate all required template cells
        const QStringList requiredCells = getRequiredTemplateCells();
        for (const QString &cellName : requiredCells) {
            out << generateTemplateCellDefinition(cellName);
            out << "\n";
        }

        file.close();
        return true;
    }

    // File exists. Determine which cells are missing.
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        qWarning() << "Cannot open clock_cell.v for reading:" << file.errorString();
        return false;
    }

    const QString     content       = file.readAll();
    const QStringList requiredCells = getRequiredTemplateCells();
    file.close();

    QStringList missingCells;
    for (const QString &cellName : requiredCells) {
        if (!content.contains(QString("module %1").arg(cellName))) {
            missingCells << cellName;
        }
    }

    if (missingCells.isEmpty()) {
        // Already complete
        return true;
    }

    // Append missing cells at the end of the file
    if (!file.open(QIODevice::Append | QIODevice::Text)) {
        qWarning() << "Cannot open clock_cell.v for appending:" << file.errorString();
        return false;
    }

    QTextStream outAppend(&file);
    outAppend << "\n"; // Ensure separation
    for (const QString &cellName : missingCells) {
        outAppend << generateTemplateCellDefinition(cellName);
        outAppend << "\n";
    }
    file.close();
    return true;
}

bool QSocClockPrimitive::isClockCellFileComplete(const QString &filePath)
{
    QFile file(filePath);
    if (!file.exists() || !file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        return false;
    }

    QString content = file.readAll();
    file.close();

    // Check if all required cells are present
    QStringList requiredCells = getRequiredTemplateCells();
    for (const QString &cellName : requiredCells) {
        if (!content.contains(QString("module %1").arg(cellName))) {
            return false;
        }
    }

    return true;
}

QStringList QSocClockPrimitive::getRequiredTemplateCells()
{
    return {
        "qsoc_tc_clk_buf",
        "qsoc_tc_clk_gate",
        "qsoc_tc_clk_inv",
        "qsoc_tc_clk_or2",
        "qsoc_tc_clk_mux2",
        "qsoc_tc_clk_xor2",
        "qsoc_clk_div",
        "qsoc_clk_or_tree",
        "qsoc_clk_mux_gf",
        "qsoc_clk_mux_raw"};
}

QString QSocClockPrimitive::generateTemplateCellDefinition(const QString &cellName)
{
    QString     result;
    QTextStream out(&result);

    if (cellName == "qsoc_tc_clk_buf") {
        out << "/**\n";
        out << " * @brief Clock buffer cell module\n";
        out << " *\n";
        out << " * @details Template implementation of clock buffer cell.\n";
        out << " */\n";
        out << "module qsoc_tc_clk_buf (\n";
        out << "    input  wire clk,      /**< Clock input */\n";
        out << "    output wire clk_out   /**< Clock output */\n";
        out << ");\n";
        out << "    /* Template implementation - replace with foundry-specific IP */\n";
        out << "    assign clk_out = clk;\n";
        out << "endmodule\n";

    } else if (cellName == "qsoc_tc_clk_gate") {
        out << "/**\n";
        out << " * @brief Clock gate cell module\n";
        out << " *\n";
        out << " * @details Template implementation of clock gate cell with test and reset "
               "support.\n";
        out << " */\n";
        out << "module qsoc_tc_clk_gate #(\n";
        out << "    parameter CLOCK_DURING_RESET = 1'b0\n";
        out << ")(\n";
        out << "    input  wire clk,        /**< Clock input */\n";
        out << "    input  wire en,         /**< Clock enable */\n";
        out << "    input  wire test_en,    /**< Test enable */\n";
        out << "    input  wire rst_n,      /**< Reset (active low) */\n";
        out << "    output wire clk_out     /**< Clock output */\n";
        out << ");\n";
        out << "    /* Template implementation - replace with foundry-specific IP */\n";
        out << "    wire final_en;\n";
        out << "    // Force enable when test_en or (reset and CLOCK_DURING_RESET parameter)\n";
        out << "    assign final_en = test_en | (!rst_n & CLOCK_DURING_RESET) | (rst_n & en);\n";
        out << "    assign clk_out = clk & final_en;\n";
        out << "endmodule\n";

    } else if (cellName == "qsoc_tc_clk_inv") {
        out << "/**\n";
        out << " * @brief Clock inverter cell module\n";
        out << " *\n";
        out << " * @details Template implementation of clock inverter cell.\n";
        out << " */\n";
        out << "module qsoc_tc_clk_inv (\n";
        out << "    input  wire CLK_IN,   /**< Clock input */\n";
        out << "    output wire CLK_OUT   /**< Clock output */\n";
        out << ");\n";
        out << "    /* Template implementation - replace with foundry-specific IP */\n";
        out << "    assign CLK_OUT = ~CLK_IN;\n";
        out << "endmodule\n";

    } else if (cellName == "qsoc_tc_clk_or2") {
        out << "/**\n";
        out << " * @brief 2-input clock OR gate cell module\n";
        out << " *\n";
        out << " * @details Template implementation of 2-input clock OR gate cell.\n";
        out << " */\n";
        out << "module qsoc_tc_clk_or2 (\n";
        out << "    input  wire CLK_IN0,  /**< Clock input 0 */\n";
        out << "    input  wire CLK_IN1,  /**< Clock input 1 */\n";
        out << "    output wire CLK_OUT   /**< Clock output */\n";
        out << ");\n";
        out << "    /* Template implementation - replace with foundry-specific IP */\n";
        out << "    assign CLK_OUT = CLK_IN0 | CLK_IN1;\n";
        out << "endmodule\n";

    } else if (cellName == "qsoc_tc_clk_mux2") {
        out << "/**\n";
        out << " * @brief 2-to-1 clock multiplexer cell module\n";
        out << " *\n";
        out << " * @details Template implementation of 2-to-1 clock multiplexer.\n";
        out << " */\n";
        out << "module qsoc_tc_clk_mux2 (\n";
        out << "    input  wire CLK_IN0,  /**< Clock input 0 */\n";
        out << "    input  wire CLK_IN1,  /**< Clock input 1 */\n";
        out << "    input  wire CLK_SEL,  /**< Select signal: 0=CLK_IN0, 1=CLK_IN1 */\n";
        out << "    output wire CLK_OUT   /**< Clock output */\n";
        out << ");\n";
        out << "    /* Template implementation - replace with foundry-specific IP */\n";
        out << "    assign CLK_OUT = CLK_SEL ? CLK_IN1 : CLK_IN0;\n";
        out << "endmodule\n";

    } else if (cellName == "qsoc_tc_clk_xor2") {
        out << "/**\n";
        out << " * @brief 2-input clock XOR gate cell module\n";
        out << " *\n";
        out << " * @details Template implementation of 2-input clock XOR gate cell.\n";
        out << " */\n";
        out << "module qsoc_tc_clk_xor2 (\n";
        out << "    input  wire CLK_IN0,  /**< Clock input 0 */\n";
        out << "    input  wire CLK_IN1,  /**< Clock input 1 */\n";
        out << "    output wire CLK_OUT   /**< Clock output */\n";
        out << ");\n";
        out << "    /* Template implementation - replace with foundry-specific IP */\n";
        out << "    assign CLK_OUT = CLK_IN0 ^ CLK_IN1;\n";
        out << "endmodule\n";

    } else if (cellName == "qsoc_clk_div") {
        out << "/**\n";
        out << " * @brief Configurable clock divider cell module\n";
        out << " *\n";
        out << " * @details Professional implementation matching clk_int_div interface with "
               "glitch-free operation.\n";
        out << " *          Supports both odd and even division with 50% duty cycle output.\n";
        out << " */\n";
        out << "module qsoc_clk_div #(\n";
        out << "    parameter integer WIDTH = 4,           /**< Division value width */\n";
        out << "    parameter integer DEFAULT_VAL = 0,     /**< Default divider value after reset "
               "*/\n";
        out << "    parameter CLOCK_DURING_RESET = 1'b0          /**< Enable clock during reset "
               "*/\n";
        out << ")(\n";
        out << "    input  wire                clk,        /**< Clock input */\n";
        out << "    input  wire                rst_n,      /**< Reset (active low) */\n";
        out << "    input  wire                en,         /**< Enable */\n";
        out << "    input  wire                test_en,    /**< Test mode enable */\n";
        out << "    input  wire [WIDTH-1:0]    div,        /**< Division value */\n";
        out << "    input  wire                div_valid,  /**< Division value valid */\n";
        out << "    output reg                 div_ready,  /**< Division ready */\n";
        out << "    output wire                clk_out,    /**< Clock output */\n";
        out << "    output wire [WIDTH-1:0]    count       /**< Cycle counter */\n";
        out << ");\n";
        out << "\n";
        out << "    /* Parameter validation - equivalent to $clog2 check for Verilog 2005 */\n";
        out << "    function integer clog2;\n";
        out << "        input integer value;\n";
        out << "        begin\n";
        out << "            clog2 = 0;\n";
        out << "            while ((1 << clog2) < value) begin\n";
        out << "                clog2 = clog2 + 1;\n";
        out << "            end\n";
        out << "        end\n";
        out << "    endfunction\n";
        out << "    \n";
        out << "    initial begin\n";
        out << "        if (clog2(DEFAULT_VAL + 1) > WIDTH) begin\n";
        out << "            $display(\"ERROR: Default divider value %0d is not representable with "
               "the configured div value width of %0d bits.\", DEFAULT_VAL, WIDTH);\n";
        out << "            $finish;\n";
        out << "        end\n";
        out << "    end\n";
        out << "\n";
        out << "    /* Reset value calculation */\n";
        out << "    localparam [WIDTH-1:0] div_reset_value = (DEFAULT_VAL != 0) ? DEFAULT_VAL : "
               "1;\n";
        out << "    \n";
        out << "    /* State registers */\n";
        out << "    reg [WIDTH-1:0] div_d, div_q;\n";
        out << "    reg toggle_ffs_en;\n";
        out << "    reg t_ff1_d, t_ff1_q;\n";
        out << "    reg t_ff1_en;\n";
        out << "    reg t_ff2_d, t_ff2_q;\n";
        out << "    reg t_ff2_en;\n";
        out << "    reg [WIDTH-1:0] cycle_cntr_d, cycle_cntr_q;\n";
        out << "    reg cycle_counter_en;\n";
        out << "    reg clk_div_bypass_en_d, clk_div_bypass_en_q;\n";
        out << "    reg use_odd_division_d, use_odd_division_q;\n";
        out << "    reg gate_en_d, gate_en_q;\n";
        out << "    reg gate_is_open_q;\n";
        out << "    reg clear_cycle_counter;\n";
        out << "    reg clear_toggle_flops;\n";
        out << "    reg [1:0] clk_gate_state_d, clk_gate_state_q;\n";
        out << "\n";
        out << "    /* FSM state encoding */\n";
        out << "    parameter [1:0] IDLE = 2'b00;\n";
        out << "    parameter [1:0] LOAD_DIV = 2'b01;\n";
        out << "    parameter [1:0] WAIT_END_PERIOD = 2'b10;\n";
        out << "\n";
        out << "    /* Internal signals */\n";
        out << "    wire [WIDTH-1:0] div_i_normalized;\n";
        out << "    wire odd_clk;\n";
        out << "    wire even_clk;\n";
        out << "    wire generated_clock;\n";
        out << "    wire ungated_output_clock;\n";
        out << "    \n";
        out << "    localparam use_odd_division_reset_value = DEFAULT_VAL[0];\n";
        out << "    localparam clk_div_bypass_en_reset_value = (DEFAULT_VAL < 2) ? 1'b1 : 1'b0;\n";
        out << "\n";
        out << "    /* Normalize div input - avoid div=0 issues */\n";
        out << "    assign div_i_normalized = (div != 0) ? div : 1;\n";
        out << "\n";
        out << "    /* Divider Load FSM */\n";
        out << "    always @(*) begin\n";
        out << "        div_d = div_q;\n";
        out << "        div_ready = 1'b0;\n";
        out << "        clk_div_bypass_en_d = clk_div_bypass_en_q;\n";
        out << "        use_odd_division_d = use_odd_division_q;\n";
        out << "        clk_gate_state_d = clk_gate_state_q;\n";
        out << "        cycle_counter_en = 1'b1;\n";
        out << "        clear_cycle_counter = 1'b0;\n";
        out << "        clear_toggle_flops = 1'b0;\n";
        out << "        toggle_ffs_en = 1'b1;\n";
        out << "        gate_en_d = 1'b0;\n";
        out << "\n";
        out << "        case (clk_gate_state_q)\n";
        out << "            IDLE: begin\n";
        out << "                gate_en_d = 1'b1;\n";
        out << "                toggle_ffs_en = 1'b1;\n";
        out << "                if (div_valid) begin\n";
        out << "                    if (div_i_normalized == div_q) begin\n";
        out << "                        div_ready = 1'b1;\n";
        out << "                    end else begin\n";
        out << "                        clk_gate_state_d = LOAD_DIV;\n";
        out << "                        gate_en_d = 1'b0;\n";
        out << "                    end\n";
        out << "                end else if (!en && gate_is_open_q == 1'b0) begin\n";
        out << "                    cycle_counter_en = 1'b0;\n";
        out << "                    toggle_ffs_en = 1'b0;\n";
        out << "                end\n";
        out << "            end\n";
        out << "\n";
        out << "            LOAD_DIV: begin\n";
        out << "                gate_en_d = 1'b0;\n";
        out << "                toggle_ffs_en = 1'b1;\n";
        out << "                if ((gate_is_open_q == 1'b0) || clk_div_bypass_en_q) begin\n";
        out << "                    toggle_ffs_en = 1'b0;\n";
        out << "                    div_d = div_i_normalized;\n";
        out << "                    div_ready = 1'b1;\n";
        out << "                    clear_cycle_counter = 1'b1;\n";
        out << "                    clear_toggle_flops = 1'b1;\n";
        out << "                    use_odd_division_d = div_i_normalized[0];\n";
        out << "                    clk_div_bypass_en_d = (div_i_normalized == 1);\n";
        out << "                    clk_gate_state_d = WAIT_END_PERIOD;\n";
        out << "                end\n";
        out << "            end\n";
        out << "\n";
        out << "            WAIT_END_PERIOD: begin\n";
        out << "                gate_en_d = 1'b0;\n";
        out << "                toggle_ffs_en = 1'b0;\n";
        out << "                if (cycle_cntr_q == div_q - 1) begin\n";
        out << "                    clk_gate_state_d = IDLE;\n";
        out << "                end\n";
        out << "            end\n";
        out << "\n";
        out << "            default: begin\n";
        out << "                clk_gate_state_d = IDLE;\n";
        out << "            end\n";
        out << "        endcase\n";
        out << "    end\n";
        out << "\n";
        out << "    /* State registers */\n";
        out << "    always @(posedge clk, negedge rst_n) begin\n";
        out << "        if (!rst_n) begin\n";
        out << "            use_odd_division_q <= use_odd_division_reset_value;\n";
        out << "            clk_div_bypass_en_q <= clk_div_bypass_en_reset_value;\n";
        out << "            div_q <= div_reset_value;\n";
        out << "            clk_gate_state_q <= IDLE;\n";
        out << "            gate_en_q <= CLOCK_DURING_RESET;\n";
        out << "        end else begin\n";
        out << "            use_odd_division_q <= use_odd_division_d;\n";
        out << "            clk_div_bypass_en_q <= clk_div_bypass_en_d;\n";
        out << "            div_q <= div_d;\n";
        out << "            clk_gate_state_q <= clk_gate_state_d;\n";
        out << "            gate_en_q <= gate_en_d;\n";
        out << "        end\n";
        out << "    end\n";
        out << "\n";
        out << "    /* Cycle Counter */\n";
        out << "    always @(*) begin\n";
        out << "        cycle_cntr_d = cycle_cntr_q;\n";
        out << "        if (clear_cycle_counter) begin\n";
        out << "            cycle_cntr_d = {WIDTH{1'b0}};\n";
        out << "        end else begin\n";
        out << "            if (cycle_counter_en) begin\n";
        out << "                if (clk_div_bypass_en_q || (cycle_cntr_q == div_q-1)) begin\n";
        out << "                    cycle_cntr_d = {WIDTH{1'b0}};\n";
        out << "                end else begin\n";
        out << "                    cycle_cntr_d = cycle_cntr_q + 1;\n";
        out << "                end\n";
        out << "            end\n";
        out << "        end\n";
        out << "    end\n";
        out << "\n";
        out << "    always @(posedge clk, negedge rst_n) begin\n";
        out << "        if (!rst_n) begin\n";
        out << "            cycle_cntr_q <= {WIDTH{1'b0}};\n";
        out << "        end else begin\n";
        out << "            cycle_cntr_q <= cycle_cntr_d;\n";
        out << "        end\n";
        out << "    end\n";
        out << "\n";
        out << "    assign count = cycle_cntr_q;\n";
        out << "\n";
        out << "    /* T-Flip-Flops with intentional blocking assignments */\n";
        out << "    always @(posedge clk, negedge rst_n) begin\n";
        out << "        if (!rst_n) begin\n";
        out << "            t_ff1_q = 1'b0; /* Intentional blocking assignment */\n";
        out << "        end else begin\n";
        out << "            if (t_ff1_en) begin\n";
        out << "                t_ff1_q = t_ff1_d; /* Intentional blocking assignment */\n";
        out << "            end\n";
        out << "        end\n";
        out << "    end\n";
        out << "\n";
        out << "    always @(negedge clk, negedge rst_n) begin\n";
        out << "        if (!rst_n) begin\n";
        out << "            t_ff2_q = 1'b0; /* Intentional blocking assignment */\n";
        out << "        end else begin\n";
        out << "            if (t_ff2_en) begin\n";
        out << "                t_ff2_q = t_ff2_d; /* Intentional blocking assignment */\n";
        out << "            end\n";
        out << "        end\n";
        out << "    end\n";
        out << "\n";
        out << "    always @(*) begin\n";
        out << "        if (clear_toggle_flops) begin\n";
        out << "            t_ff1_d = 1'b0;\n";
        out << "            t_ff2_d = 1'b0;\n";
        out << "        end else begin\n";
        out << "            t_ff1_d = t_ff1_en ? !t_ff1_q : t_ff1_q;\n";
        out << "            t_ff2_d = t_ff2_en ? !t_ff2_q : t_ff2_q;\n";
        out << "        end\n";
        out << "    end\n";
        out << "\n";
        out << "    /* T-FF enable control */\n";
        out << "    always @(*) begin\n";
        out << "        t_ff1_en = 1'b0;\n";
        out << "        t_ff2_en = 1'b0;\n";
        out << "        if (!clk_div_bypass_en_q && toggle_ffs_en) begin\n";
        out << "            if (use_odd_division_q) begin\n";
        out << "                t_ff1_en = (cycle_cntr_q == 0) ? 1'b1 : 1'b0;\n";
        out << "                t_ff2_en = (cycle_cntr_q == (div_q+1)/2) ? 1'b1 : 1'b0;\n";
        out << "            end else begin\n";
        out << "                t_ff1_en = (cycle_cntr_q == 0 || cycle_cntr_q == div_q/2) ? 1'b1 : "
               "1'b0;\n";
        out << "            end\n";
        out << "        end\n";
        out << "    end\n";
        out << "\n";
        out << "    assign even_clk = t_ff1_q;\n";
        out << "\n";
        out << "    /* Clock XOR for odd division logic */\n";
        out << "    qsoc_tc_clk_xor2 i_odd_clk_xor (\n";
        out << "        .CLK_IN0(t_ff1_q),\n";
        out << "        .CLK_IN1(t_ff2_q),\n";
        out << "        .CLK_OUT(odd_clk)\n";
        out << "    );\n";
        out << "\n";
        out << "    /* Clock MUX to select between odd and even division logic */\n";
        out << "    qsoc_tc_clk_mux2 i_clk_mux (\n";
        out << "        .CLK_IN0(even_clk),\n";
        out << "        .CLK_IN1(odd_clk),\n";
        out << "        .CLK_SEL(use_odd_division_q),\n";
        out << "        .CLK_OUT(generated_clock)\n";
        out << "    );\n";
        out << "\n";
        out << "    /* Clock MUX to bypass clock if divide-by-1 */\n";
        out << "    qsoc_tc_clk_mux2 i_clk_bypass_mux (\n";
        out << "        .CLK_IN0(generated_clock),\n";
        out << "        .CLK_IN1(clk),\n";
        out << "        .CLK_SEL(clk_div_bypass_en_q || test_en),\n";
        out << "        .CLK_OUT(ungated_output_clock)\n";
        out << "    );\n";
        out << "\n";
        out << "    /* Clock gate feedback signal */\n";
        out << "    always @(posedge ungated_output_clock, negedge rst_n) begin\n";
        out << "        if (!rst_n) begin\n";
        out << "            gate_is_open_q <= 1'b0;\n";
        out << "        end else begin\n";
        out << "            gate_is_open_q <= gate_en_q & en;\n";
        out << "        end\n";
        out << "    end\n";
        out << "\n";
        out << "    /* Final clock gate for glitch protection */\n";
        out << "    qsoc_tc_clk_gate #(\n";
        out << "        .CLOCK_DURING_RESET(CLOCK_DURING_RESET)\n";
        out << "    ) i_clk_gate (\n";
        out << "        .clk(ungated_output_clock),\n";
        out << "        .en(gate_en_q & en),\n";
        out << "        .test_en(test_en),\n";
        out << "        .rst_n(rst_n),\n";
        out << "        .clk_out(clk_out)\n";
        out << "    );\n";
        out << "\n";
        out << "endmodule\n";

    } else if (cellName == "qsoc_clk_mux_gf") {
        out << "/**\n";
        out << " * @brief Glitch-free clock multiplexer cell module\n";
        out << " *\n";
        out << " * @details Template implementation of glitch-free N-input clock multiplexer\n";
        out << " *          based on ETH Zurich common_cells library design.\n";
        out << " *          Supports multi-input with parametrized sync stages and DFT.\n";
        out << " */\n";
        out << "module qsoc_clk_mux_gf #(\n";
        out << "    parameter integer NUM_INPUTS = 2,        /**< Number of clock inputs */\n";
        out << "    parameter integer NUM_SYNC_STAGES = 2,   /**< Synchronizer stages */\n";
        out << "    parameter CLOCK_DURING_RESET = 1'b1,     /**< Clock during reset */\n";
        out << "    parameter [5:0] WIDTH =                  /**< Helper: select signal width */\n";
        out << "        (NUM_INPUTS <= 2)    ? 6'h01 :\n";
        out << "        (NUM_INPUTS <= 4)    ? 6'h02 :\n";
        out << "        (NUM_INPUTS <= 8)    ? 6'h03 :\n";
        out << "        (NUM_INPUTS <= 16)   ? 6'h04 :\n";
        out << "        (NUM_INPUTS <= 32)   ? 6'h05 :\n";
        out << "        (NUM_INPUTS <= 64)   ? 6'h06 :\n";
        out << "        (NUM_INPUTS <= 128)  ? 6'h07 :\n";
        out << "        (NUM_INPUTS <= 256)  ? 6'h08 :\n";
        out << "        (NUM_INPUTS <= 512)  ? 6'h09 :\n";
        out << "        (NUM_INPUTS <= 1024) ? 6'h0A :\n";
        out << "        (NUM_INPUTS <= 2048) ? 6'h0B :\n";
        out << "        (NUM_INPUTS <= 4096) ? 6'h0C : 6'h20\n";
        out << ") (\n";
        out << "    input  wire [NUM_INPUTS-1:0] clk_in,        /**< Clock inputs */\n";
        out << "    input  wire                  test_clk,      /**< DFT test clock */\n";
        out << "    input  wire                  test_en,       /**< DFT test enable */\n";
        out << "    input  wire                  async_rst_n,   /**< Async reset (active low) */\n";
        out << "    input  wire [WIDTH-1:0]      async_sel,     /**< Async select signal */\n";
        out << "    output reg                   clk_out        /**< Clock output */\n";
        out << ");\n";
        out << "    /* Template implementation - replace with foundry-specific IP */\n";
        out << "    \n";
        out << "    // Note: NUM_INPUTS must be >= 2 for proper operation\n";
        out << "    \n";
        out << "    // Internal signals for glitch-free switching\n";
        out << "    reg [NUM_INPUTS-1:0]        sel_onehot;\n";
        out << "    reg [NUM_INPUTS*2-1:0]   glitch_filter_d, glitch_filter_q;\n";
        out << "    wire [NUM_INPUTS-1:0]        gate_enable_unfiltered;\n";
        out << "    wire [NUM_INPUTS-1:0]        glitch_filter_output;\n";
        out << "    wire [NUM_INPUTS-1:0]        gate_enable_sync;\n";
        out << "    wire [NUM_INPUTS-1:0]        gate_enable;\n";
        out << "    reg [NUM_INPUTS-1:0]        clock_disabled_q;\n";
        out << "    wire [NUM_INPUTS-1:0]        gated_clock;\n";
        out << "    wire                         output_clock;\n";
        out << "    reg [NUM_INPUTS-1:0]        reset_synced;\n";
        out << "    \n";
        out << "    // Onehot decoder\n";
        out << "    always @(*) begin\n";
        out << "        sel_onehot = {NUM_INPUTS{1'b0}};\n";
        out << "        if (async_sel < NUM_INPUTS)\n";
        out << "            sel_onehot[async_sel] = 1'b1;\n";
        out << "    end\n";
        out << "    \n";
        out << "    // Generate logic for each input clock\n";
        out << "    for (genvar i = 0; i < NUM_INPUTS; i++) begin : gen_input_stages\n";
        out << "        // Synchronize reset to each clock domain using dedicated reset "
               "generator\n";
        out << "        // Note: For full compatibility, this should be replaced with a proper "
               "rstgen module\n";
        out << "        // For now, implementing equivalent functionality inline\n";
        out << "        always @(posedge clk_in[i] or negedge async_rst_n) begin\n";
        out << "            if (!async_rst_n) begin\n";
        out << "                reset_synced[i] <= 1'b0;\n";
        out << "            end else begin\n";
        out << "                reset_synced[i] <= 1'b1;\n";
        out << "            end\n";
        out << "        end\n";
        out << "        \n";
        out << "        // Gate enable generation with mutual exclusion\n";
        out << "        always @(*) begin\n";
        out << "            gate_enable_unfiltered[i] = 1'b1;\n";
        out << "            for (int j = 0; j < NUM_INPUTS; j++) begin\n";
        out << "                if (i == j) begin\n";
        out << "                    gate_enable_unfiltered[i] &= sel_onehot[j];\n";
        out << "                end else begin\n";
        out << "                    gate_enable_unfiltered[i] &= clock_disabled_q[j];\n";
        out << "                end\n";
        out << "            end\n";
        out << "        end\n";
        out << "        \n";
        out << "        // Glitch filter (2-stage)\n";
        out << "        assign glitch_filter_d[i*2+0] = gate_enable_unfiltered[i];\n";
        out << "        assign glitch_filter_d[i*2+1] = glitch_filter_q[i*2+0];\n";
        out << "        \n";
        out << "        always @(posedge clk_in[i] or negedge reset_synced[i]) begin\n";
        out << "            if (!reset_synced[i]) begin\n";
        out << "                glitch_filter_q[i*2+1:i*2] <= 2'b00;\n";
        out << "            end else begin\n";
        out << "                glitch_filter_q[i*2+1:i*2] <= glitch_filter_d[i*2+1:i*2];\n";
        out << "            end\n";
        out << "        end\n";
        out << "        \n";
        out << "        assign glitch_filter_output[i] = glitch_filter_q[i*2+1] & \n";
        out << "                                         glitch_filter_q[i*2+0] & \n";
        out << "                                         gate_enable_unfiltered[i];\n";
        out << "        \n";
        out << "        // Synchronizer chain for enable signal (equivalent to sync module)\n";
        out << "        // Note: This implements the same functionality as sync "
               "#(.STAGES(NUM_SYNC_STAGES))\n";
        out << "        if (NUM_SYNC_STAGES >= 1) begin : gen_sync\n";
        out << "            reg [NUM_SYNC_STAGES-1:0] sync_chain;\n";
        out << "            always @(posedge clk_in[i] or negedge reset_synced[i]) begin\n";
        out << "                if (!reset_synced[i]) begin\n";
        out << "                    sync_chain <= {NUM_SYNC_STAGES{1'b0}};\n";
        out << "                end else begin\n";
        out << "                    sync_chain <= {sync_chain[NUM_SYNC_STAGES-2:0], "
               "glitch_filter_output[i]};\n";
        out << "                end\n";
        out << "            end\n";
        out << "            assign gate_enable_sync[i] = sync_chain[NUM_SYNC_STAGES-1];\n";
        out << "        end else begin : gen_no_sync\n";
        out << "            assign gate_enable_sync[i] = glitch_filter_output[i];\n";
        out << "        end\n";
        out << "        \n";
        out << "        // Optional clock during reset bypass\n";
        out << "        if (CLOCK_DURING_RESET) begin : gen_reset_bypass\n";
        out << "            reg bypass_active;\n";
        out << "            always @(posedge clk_in[i] or negedge reset_synced[i]) begin\n";
        out << "                if (!reset_synced[i]) begin\n";
        out << "                    bypass_active <= 1'b1;\n";
        out << "                end else begin\n";
        out << "                    bypass_active <= 1'b0;\n";
        out << "                end\n";
        out << "            end\n";
        out << "            assign gate_enable[i] = bypass_active ? gate_enable_unfiltered[i] : "
               "gate_enable_sync[i];\n";
        out << "        end else begin : gen_no_reset_bypass\n";
        out << "            assign gate_enable[i] = gate_enable_sync[i];\n";
        out << "        end\n";
        out << "        \n";
        out << "        // Clock gating using dedicated clock gate cell\n";
        out << "        qsoc_tc_clk_gate #(\n";
        out << "            .CLOCK_DURING_RESET(CLOCK_DURING_RESET)\n";
        out << "        ) i_clk_gate (\n";
        out << "            .clk(clk_in[i]),\n";
        out << "            .en(gate_enable[i]),\n";
        out << "            .test_en(1'b0),\n";
        out << "            .rst_n(reset_synced[i]),\n";
        out << "            .clk_out(gated_clock[i])\n";
        out << "        );\n";
        out << "        \n";
        out << "        // Feedback for mutual exclusion\n";
        out << "        always @(posedge clk_in[i] or negedge reset_synced[i]) begin\n";
        out << "            if (!reset_synced[i]) begin\n";
        out << "                clock_disabled_q[i] <= 1'b1;\n";
        out << "            end else begin\n";
        out << "                clock_disabled_q[i] <= ~gate_enable[i];\n";
        out << "            end\n";
        out << "        end\n";
        out << "    end\n";
        out << "    \n";
        out << "    // Output OR gate using dedicated clock OR tree\n";
        out << "    qsoc_clk_or_tree #(\n";
        out << "        .INPUT_COUNT(NUM_INPUTS)\n";
        out << "    ) i_clk_or_tree (\n";
        out << "        .clk_in(gated_clock),\n";
        out << "        .clk_out(output_clock)\n";
        out << "    );\n";
        out << "    \n";
        out << "    // DFT mux: select between functional clock and test clock using dedicated "
               "clock mux\n";
        out << "    qsoc_tc_clk_mux2 i_test_clk_mux (\n";
        out << "        .CLK_IN0(output_clock),\n";
        out << "        .CLK_IN1(test_clk),\n";
        out << "        .CLK_SEL(test_en),\n";
        out << "        .CLK_OUT(clk_out)\n";
        out << "    );\n";
        out << "    \n";
        out << "endmodule\n";

    } else if (cellName == "qsoc_clk_mux_raw") {
        out << "/**\n";
        out << " * @brief Standard (non-glitch-free) clock multiplexer cell module\n";
        out << " *\n";
        out << " * @details Template implementation of simple N-input clock multiplexer\n";
        out << " *          using pure combinational logic. No glitch protection.\n";
        out << " */\n";
        out << "module qsoc_clk_mux_raw #(\n";
        out << "    parameter integer NUM_INPUTS = 2,\n";
        out << "    parameter [5:0] WIDTH =                  /**< Helper: select signal width */\n";
        out << "        (NUM_INPUTS <= 2)    ? 6'h01 :\n";
        out << "        (NUM_INPUTS <= 4)    ? 6'h02 :\n";
        out << "        (NUM_INPUTS <= 8)    ? 6'h03 :\n";
        out << "        (NUM_INPUTS <= 16)   ? 6'h04 :\n";
        out << "        (NUM_INPUTS <= 32)   ? 6'h05 :\n";
        out << "        (NUM_INPUTS <= 64)   ? 6'h06 :\n";
        out << "        (NUM_INPUTS <= 128)  ? 6'h07 :\n";
        out << "        (NUM_INPUTS <= 256)  ? 6'h08 :\n";
        out << "        (NUM_INPUTS <= 512)  ? 6'h09 :\n";
        out << "        (NUM_INPUTS <= 1024) ? 6'h0A :\n";
        out << "        (NUM_INPUTS <= 2048) ? 6'h0B :\n";
        out << "        (NUM_INPUTS <= 4096) ? 6'h0C : 6'h20\n";
        out << ") (\n";
        out << "    input  wire [NUM_INPUTS-1:0] clk_in,        /**< Clock inputs */\n";
        out << "    input  wire [WIDTH-1:0]      clk_sel,       /**< Clock select signal */\n";
        out << "    output wire                  clk_out        /**< Clock output */\n";
        out << ");\n";
        out << "    /* Template implementation - replace with foundry-specific IP */\n";
        out << "    \n";
        out << "    /* Generate recursive binary tree multiplexer structure */\n";
        out << "    generate\n";
        out << "        if (NUM_INPUTS < 1) begin : gen_error\n";
        out << "            /* Error condition - invalid parameter */\n";
        out << "            initial begin\n";
        out << "                $display(\"ERROR: qsoc_clk_mux_raw cannot be parametrized "
               "with less than 1 input but was %0d\", NUM_INPUTS);\n";
        out << "                $finish;\n";
        out << "            end\n";
        out << "        end else if (NUM_INPUTS == 1) begin : gen_leaf_single\n";
        out << "            /* Single input - direct connection */\n";
        out << "            assign clk_out = clk_in[0];\n";
        out << "        end else if (NUM_INPUTS == 2) begin : gen_leaf_dual\n";
        out << "            /* Two inputs - single MUX2 cell */\n";
        out << "            qsoc_tc_clk_mux2 i_clkmux2 (\n";
        out << "                .CLK_IN0(clk_in[0]),\n";
        out << "                .CLK_IN1(clk_in[1]),\n";
        out << "                .CLK_SEL(clk_sel[0]),\n";
        out << "                .CLK_OUT(clk_out)\n";
        out << "            );\n";
        out << "        end else begin : gen_recursive\n";
        out << "            /* More than 2 inputs - build recursive tree */\n";
        out << "            wire branch_a;      /**< Output from first branch */\n";
        out << "            wire branch_b;      /**< Output from second branch */\n";
        out << "            \n";
        out << "            /* Use MSB to select between two halves, remaining bits for "
               "sub-selection */\n";
        out << "            wire msb_sel;       /**< MSB selects between upper and lower half */\n";
        out << "            wire [WIDTH-2:0] lower_sel;  /**< Lower bits for sub-mux selection "
               "*/\n";
        out << "            \n";
        out << "            assign msb_sel = clk_sel[WIDTH-1];\n";
        out << "            assign lower_sel = clk_sel[WIDTH-2:0];\n";
        out << "            \n";
        out << "            /* First branch handles lower half of inputs */\n";
        out << "            qsoc_clk_mux_raw #(\n";
        out << "                .NUM_INPUTS(NUM_INPUTS/2)\n";
        out << "            ) i_mux_branch_a (\n";
        out << "                .clk_in(clk_in[0+:NUM_INPUTS/2]),\n";
        out << "                .clk_sel(lower_sel),\n";
        out << "                .clk_out(branch_a)\n";
        out << "            );\n";
        out << "            \n";
        out << "            /* Second branch handles upper half plus any odd input */\n";
        out << "            qsoc_clk_mux_raw #(\n";
        out << "                .NUM_INPUTS(NUM_INPUTS/2 + NUM_INPUTS%2)\n";
        out << "            ) i_mux_branch_b (\n";
        out << "                .clk_in(clk_in[NUM_INPUTS-1:NUM_INPUTS/2]),\n";
        out << "                .clk_sel(lower_sel),\n";
        out << "                .clk_out(branch_b)\n";
        out << "            );\n";
        out << "            \n";
        out << "            /* Combine branches with final MUX2 cell using MSB */\n";
        out << "            qsoc_tc_clk_mux2 i_clkmux2_final (\n";
        out << "                .CLK_IN0(branch_a),\n";
        out << "                .CLK_IN1(branch_b),\n";
        out << "                .CLK_SEL(msb_sel),\n";
        out << "                .CLK_OUT(clk_out)\n";
        out << "            );\n";
        out << "        end\n";
        out << "    endgenerate\n";
        out << "    \n";
        out << "endmodule\n";
        out << "\n";
        out << "\n";

    } else if (cellName == "qsoc_clk_or_tree") {
        out << "/**\n";
        out << " * @brief Clock OR tree cell module\n";
        out << " *\n";
        out << " * @details Generates an N-input clock OR tree using binary tree of "
               "qsoc_tc_clk_or2 instances.\n";
        out << " *          This module recursively builds a balanced tree structure to minimize "
               "propagation delay.\n";
        out << " */\n";
        out << "module qsoc_clk_or_tree #(\n";
        out << "    parameter integer INPUT_COUNT = 4    /**< Number of clock inputs (must be >= "
               "1) */\n";
        out << ")(\n";
        out << "    input  wire [INPUT_COUNT-1:0] clk_in,  /**< Clock inputs */\n";
        out << "    output wire                   clk_out  /**< Clock output */\n";
        out << ");\n";
        out << "    \n";
        out << "    /* Generate recursive binary tree structure */\n";
        out << "    generate\n";
        out << "        if (INPUT_COUNT < 1) begin : gen_error\n";
        out << "            /* Error condition - invalid parameter */\n";
        out << "            initial begin\n";
        out << "                $display(\"ERROR: qsoc_clk_or_tree cannot be parametrized with "
               "less than 1 input but was %0d\", INPUT_COUNT);\n";
        out << "                $finish;\n";
        out << "            end\n";
        out << "        end else if (INPUT_COUNT == 1) begin : gen_leaf_single\n";
        out << "            /* Single input - direct connection */\n";
        out << "            assign clk_out = clk_in[0];\n";
        out << "        end else if (INPUT_COUNT == 2) begin : gen_leaf_dual\n";
        out << "            /* Two inputs - single OR2 cell */\n";
        out << "            qsoc_tc_clk_or2 i_clkor2 (\n";
        out << "                .CLK_IN0(clk_in[0]),\n";
        out << "                .CLK_IN1(clk_in[1]),\n";
        out << "                .CLK_OUT(clk_out)\n";
        out << "            );\n";
        out << "        end else begin : gen_recursive\n";
        out << "            /* More than 2 inputs - build recursive tree */\n";
        out << "            wire branch_a;  /**< Output from first branch */\n";
        out << "            wire branch_b;  /**< Output from second branch */\n";
        out << "            \n";
        out << "            /* First branch handles lower half of inputs */\n";
        out << "            qsoc_clk_or_tree #(\n";
        out << "                .INPUT_COUNT(INPUT_COUNT/2)\n";
        out << "            ) i_or_branch_a (\n";
        out << "                .clk_in(clk_in[0+:INPUT_COUNT/2]),\n";
        out << "                .clk_out(branch_a)\n";
        out << "            );\n";
        out << "            \n";
        out << "            /* Second branch handles upper half plus any odd input */\n";
        out << "            qsoc_clk_or_tree #(\n";
        out << "                .INPUT_COUNT(INPUT_COUNT/2 + INPUT_COUNT%2)\n";
        out << "            ) i_or_branch_b (\n";
        out << "                .clk_in(clk_in[INPUT_COUNT-1:INPUT_COUNT/2]),\n";
        out << "                .clk_out(branch_b)\n";
        out << "            );\n";
        out << "            \n";
        out << "            /* Combine branches with final OR2 cell */\n";
        out << "            qsoc_tc_clk_or2 i_clkor2_final (\n";
        out << "                .CLK_IN0(branch_a),\n";
        out << "                .CLK_IN1(branch_b),\n";
        out << "                .CLK_OUT(clk_out)\n";
        out << "            );\n";
        out << "        end\n";
        out << "    endgenerate\n";
        out << "    \n";
        out << "endmodule\n";
    }

    return result;
}
