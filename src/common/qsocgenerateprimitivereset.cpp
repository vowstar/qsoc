#include "qsocgenerateprimitivereset.h"
#include "qsocgeneratemanager.h"
#include "qsocverilogutils.h"
#include <cmath>
#include <QDebug>
#include <QRegularExpression>
#include <QRegularExpressionMatch>

QSocResetPrimitive::QSocResetPrimitive(QSocGenerateManager *parent)
    : m_parent(parent)
{}

bool QSocResetPrimitive::generateResetController(const YAML::Node &resetNode, QTextStream &out)
{
    if (!resetNode || !resetNode.IsMap()) {
        qWarning() << "Invalid reset node provided";
        return false;
    }

    // Parse configuration
    ResetControllerConfig config = parseResetConfig(resetNode);

    if (config.targets.isEmpty()) {
        qWarning() << "Reset configuration must have at least one target";
        return false;
    }

    // Generate or update reset_cell.v file
    if (m_parent && m_parent->getProjectManager()) {
        QString outputDir = m_parent->getProjectManager()->getOutputPath();
        if (!generateResetCellFile(outputDir)) {
            qWarning() << "Failed to generate reset_cell.v file";
            return false;
        }
    }

    // Generate Verilog code
    generateModuleHeader(config, out);
    generateWireDeclarations(config, out);
    generateResetLogic(config, out);

    if (config.reason.enabled) {
        generateResetReason(config, out);
    }

    generateOutputAssignments(config, out);

    // Close module
    out << "\nendmodule\n\n";

    return true;
}

QSocResetPrimitive::ResetControllerConfig QSocResetPrimitive::parseResetConfig(
    const YAML::Node &resetNode)
{
    ResetControllerConfig config;

    // Basic configuration
    config.name = resetNode["name"] ? QString::fromStdString(resetNode["name"].as<std::string>())
                                    : "reset_ctrl";
    config.moduleName = resetNode["module_name"]
                            ? QString::fromStdString(resetNode["module_name"].as<std::string>())
                            : config.name;
    config.clock = resetNode["clock"] ? QString::fromStdString(resetNode["clock"].as<std::string>())
                                      : "clk_sys";
    config.testEnable = resetNode["test_enable"]
                            ? QString::fromStdString(resetNode["test_enable"].as<std::string>())
                            : "test_en";

    // Parse sources (source: {name: {polarity: ...}})
    if (resetNode["source"] && resetNode["source"].IsMap()) {
        for (auto it = resetNode["source"].begin(); it != resetNode["source"].end(); ++it) {
            ResetSource source;
            source.name = QString::fromStdString(it->first.as<std::string>());

            if (it->second.IsMap() && it->second["active"]) {
                source.active = QString::fromStdString(it->second["active"].as<std::string>());
            } else if (it->second.IsScalar()) {
                source.active = QString::fromStdString(it->second.as<std::string>());
            } else {
                qCritical() << "Error: 'active' field is required for source '" << source.name
                            << "'";
                qCritical() << "Please specify active level explicitly: 'high' or 'low'";
                qCritical() << "Example: source: { " << source.name << ": low }";
                return config;
            }

            config.sources.append(source);
        }
    }

    // Parse targets with component-based configuration
    if (resetNode["target"] && resetNode["target"].IsMap()) {
        for (auto tgtIt = resetNode["target"].begin(); tgtIt != resetNode["target"].end(); ++tgtIt) {
            const YAML::Node &tgtNode = tgtIt->second;
            if (!tgtNode.IsMap())
                continue;

            ResetTarget target;
            target.name = QString::fromStdString(tgtIt->first.as<std::string>());

            // Parse target active level
            if (tgtNode["active"]) {
                target.active = QString::fromStdString(tgtNode["active"].as<std::string>());
            } else {
                qCritical() << "Error: 'active' field is required for target '" << target.name
                            << "'";
                return config;
            }

            // Parse target-level components
            if (tgtNode["async"]) {
                const YAML::Node &asyncNode = tgtNode["async"];
                target.async.clock          = asyncNode["clock"] ? QString::fromStdString(
                                                              asyncNode["clock"].as<std::string>())
                                                                 : config.clock;
                target.async.test_enable    = asyncNode["test_enable"]
                                                  ? QString::fromStdString(
                                                     asyncNode["test_enable"].as<std::string>())
                                                  : config.testEnable;
                target.async.stage          = asyncNode["stage"] ? asyncNode["stage"].as<int>() : 3;
            }

            if (tgtNode["sync"]) {
                const YAML::Node &syncNode = tgtNode["sync"];
                target.sync.clock          = syncNode["clock"]
                                                 ? QString::fromStdString(syncNode["clock"].as<std::string>())
                                                 : config.clock;
                target.sync.test_enable    = syncNode["test_enable"]
                                                 ? QString::fromStdString(
                                                    syncNode["test_enable"].as<std::string>())
                                                 : config.testEnable;
                target.sync.stage          = syncNode["stage"] ? syncNode["stage"].as<int>() : 4;
            }

            if (tgtNode["count"]) {
                const YAML::Node &countNode = tgtNode["count"];
                target.count.clock          = countNode["clock"] ? QString::fromStdString(
                                                              countNode["clock"].as<std::string>())
                                                                 : config.clock;
                target.count.test_enable    = countNode["test_enable"]
                                                  ? QString::fromStdString(
                                                     countNode["test_enable"].as<std::string>())
                                                  : config.testEnable;
                target.count.cycle = countNode["cycle"] ? countNode["cycle"].as<int>() : 16;
            }

            // Parse links for this target
            if (tgtNode["link"] && tgtNode["link"].IsMap()) {
                for (auto linkIt = tgtNode["link"].begin(); linkIt != tgtNode["link"].end();
                     ++linkIt) {
                    const YAML::Node &linkNode = linkIt->second;
                    if (!linkNode.IsMap())
                        continue;

                    ResetLink link;
                    link.source = QString::fromStdString(linkIt->first.as<std::string>());

                    // Parse link-level components
                    if (linkNode["async"]) {
                        const YAML::Node &asyncNode = linkNode["async"];
                        link.async.clock            = asyncNode["clock"]
                                                          ? QString::fromStdString(
                                                     asyncNode["clock"].as<std::string>())
                                                          : config.clock;
                        link.async.test_enable
                            = asyncNode["test_enable"]
                                  ? QString::fromStdString(
                                        asyncNode["test_enable"].as<std::string>())
                                  : config.testEnable;
                        link.async.stage = asyncNode["stage"] ? asyncNode["stage"].as<int>() : 3;
                    }

                    if (linkNode["sync"]) {
                        const YAML::Node &syncNode = linkNode["sync"];
                        link.sync.clock            = syncNode["clock"]
                                                         ? QString::fromStdString(
                                                    syncNode["clock"].as<std::string>())
                                                         : config.clock;
                        link.sync.test_enable      = syncNode["test_enable"]
                                                         ? QString::fromStdString(
                                                          syncNode["test_enable"].as<std::string>())
                                                         : config.testEnable;
                        link.sync.stage = syncNode["stage"] ? syncNode["stage"].as<int>() : 4;
                    }

                    if (linkNode["count"]) {
                        const YAML::Node &countNode = linkNode["count"];
                        link.count.clock            = countNode["clock"]
                                                          ? QString::fromStdString(
                                                     countNode["clock"].as<std::string>())
                                                          : config.clock;
                        link.count.test_enable
                            = countNode["test_enable"]
                                  ? QString::fromStdString(
                                        countNode["test_enable"].as<std::string>())
                                  : config.testEnable;
                        link.count.cycle = countNode["cycle"] ? countNode["cycle"].as<int>() : 16;
                    }

                    target.links.append(link);
                }
            }

            config.targets.append(target);
        }
    }

    // Parse reset reason recording configuration (simplified)
    config.reason.enabled = false;
    if (resetNode["reason"] && resetNode["reason"].IsMap()) {
        const YAML::Node &reasonNode = resetNode["reason"];
        config.reason.enabled        = true; // Having reason node means enabled

        // Always-on clock for recording logic
        config.reason.clock = reasonNode["clock"]
                                  ? QString::fromStdString(reasonNode["clock"].as<std::string>())
                                  : "clk_32k";

        // Output bus name
        config.reason.output = reasonNode["output"]
                                   ? QString::fromStdString(reasonNode["output"].as<std::string>())
                                   : "reason";

        // Valid signal name (support simplified field name)
        config.reason.valid = reasonNode["valid"]
                                  ? QString::fromStdString(reasonNode["valid"].as<std::string>())
                              : reasonNode["valid_signal"]
                                  ? QString::fromStdString(
                                        reasonNode["valid_signal"].as<std::string>())
                                  : "reason_valid";

        // Software clear signal
        config.reason.clear = reasonNode["clear"]
                                  ? QString::fromStdString(reasonNode["clear"].as<std::string>())
                                  : "reason_clear";

        // Explicit root reset signal specification (KISS: no auto-detection!)
        if (reasonNode["root_reset"]) {
            config.reason.rootReset = QString::fromStdString(
                reasonNode["root_reset"].as<std::string>());

            // Validate that root_reset exists in source list
            bool rootResetFound = false;
            for (const auto &source : config.sources) {
                if (source.name == config.reason.rootReset) {
                    rootResetFound = true;
                    break;
                }
            }

            if (!rootResetFound) {
                qCritical() << "Error: Specified root_reset '" << config.reason.rootReset
                            << "' not found in source list.";
                qCritical() << "Available sources:";
                for (const auto &source : config.sources) {
                    qCritical() << "  - " << source.name << " (active: " << source.active << ")";
                }
                return config;
            }
        } else {
            qCritical() << "Error: 'root_reset' field is required in reason configuration.";
            qCritical() << "Please specify which source signal should be used as the root reset.";
            qCritical() << "Example: reason: { root_reset: por_rst_n, ... }";
            return config; // Return with error
        }

        // Build source order (exclude root_reset, use source declaration order)
        config.reason.sourceOrder.clear();
        for (const auto &source : config.sources) {
            if (source.name != config.reason.rootReset) {
                config.reason.sourceOrder.append(source.name);
            }
        }

        // Calculate bit vector width
        config.reason.vectorWidth = config.reason.sourceOrder.size();
        if (config.reason.vectorWidth == 0)
            config.reason.vectorWidth = 1; // Minimum 1 bit
    }

    return config;
}

void QSocResetPrimitive::generateModuleHeader(const ResetControllerConfig &config, QTextStream &out)
{
    out << "\nmodule " << config.moduleName << " (\n";

    // Collect all unique clock signals
    QStringList clocks;
    clocks.append(config.clock);

    for (const auto &target : config.targets) {
        for (const auto &link : target.links) {
            if (!link.async.clock.isEmpty() && !clocks.contains(link.async.clock))
                clocks.append(link.async.clock);
            if (!link.sync.clock.isEmpty() && !clocks.contains(link.sync.clock))
                clocks.append(link.sync.clock);
            if (!link.count.clock.isEmpty() && !clocks.contains(link.count.clock))
                clocks.append(link.count.clock);
        }
        if (!target.async.clock.isEmpty() && !clocks.contains(target.async.clock))
            clocks.append(target.async.clock);
        if (!target.sync.clock.isEmpty() && !clocks.contains(target.sync.clock))
            clocks.append(target.sync.clock);
        if (!target.count.clock.isEmpty() && !clocks.contains(target.count.clock))
            clocks.append(target.count.clock);
    }

    // Add reason clock if enabled
    if (config.reason.enabled && !config.reason.clock.isEmpty()
        && !clocks.contains(config.reason.clock)) {
        clocks.append(config.reason.clock);
    }

    // Collect all unique source signals
    QStringList sources;
    for (const auto &target : config.targets) {
        for (const auto &link : target.links) {
            if (!sources.contains(link.source))
                sources.append(link.source);
        }
    }

    // Collect all unique test_enable signals
    QStringList testEnables;
    testEnables.append(config.testEnable);
    for (const auto &target : config.targets) {
        for (const auto &link : target.links) {
            if (!link.async.test_enable.isEmpty() && !testEnables.contains(link.async.test_enable))
                testEnables.append(link.async.test_enable);
            if (!link.sync.test_enable.isEmpty() && !testEnables.contains(link.sync.test_enable))
                testEnables.append(link.sync.test_enable);
            if (!link.count.test_enable.isEmpty() && !testEnables.contains(link.count.test_enable))
                testEnables.append(link.count.test_enable);
        }
        if (!target.async.test_enable.isEmpty() && !testEnables.contains(target.async.test_enable))
            testEnables.append(target.async.test_enable);
        if (!target.sync.test_enable.isEmpty() && !testEnables.contains(target.sync.test_enable))
            testEnables.append(target.sync.test_enable);
        if (!target.count.test_enable.isEmpty() && !testEnables.contains(target.count.test_enable))
            testEnables.append(target.count.test_enable);
    }

    // Clock inputs
    out << "    /* Clock inputs */\n";
    for (const auto &clock : clocks) {
        out << "    input  wire " << clock << ",\n";
    }

    // Source inputs
    out << "    /* Reset sources */\n";
    for (const auto &source : sources) {
        out << "    input  wire " << source << ",\n";
    }

    // Test enable inputs
    out << "    /* Test enable signals */\n";
    for (const auto &testEn : testEnables) {
        out << "    input  wire " << testEn << ",\n";
    }

    // Reset reason clear signal
    if (config.reason.enabled && !config.reason.clear.isEmpty()) {
        out << "    /* Reset reason clear */\n";
        out << "    input  wire " << config.reason.clear << ",\n";
    }

    // Reset targets
    out << "    /* Reset targets */\n";
    for (int i = 0; i < config.targets.size(); ++i) {
        const auto &target = config.targets[i];
        out << "    output wire " << target.name;
        if (i < config.targets.size() - 1 || config.reason.enabled) {
            out << ",";
        }
        out << "\n";
    }

    // Reset reason outputs
    if (config.reason.enabled) {
        out << "    /* Reset reason outputs */\n";
        if (config.reason.vectorWidth > 1) {
            out << "    output wire [" << (config.reason.vectorWidth - 1) << ":0] "
                << config.reason.output << ",\n";
        } else {
            out << "    output wire " << config.reason.output << ",\n";
        }
        out << "    output wire " << config.reason.valid << "\n";
    }

    out << ");\n\n";
}

void QSocResetPrimitive::generateWireDeclarations(
    const ResetControllerConfig &config, QTextStream &out)
{
    out << "    /* Wire declarations */\n";

    // Generate wires for each link and target processing stage
    for (int targetIdx = 0; targetIdx < config.targets.size(); ++targetIdx) {
        const auto &target = config.targets[targetIdx];

        // Link-level wires
        for (int linkIdx = 0; linkIdx < target.links.size(); ++linkIdx) {
            QString wireName = getLinkWireName(target.name, linkIdx);
            out << "    wire " << wireName << ";\n";
        }

        // Target-level intermediate wire (if target has processing)
        bool hasTargetProcessing = !target.async.clock.isEmpty() || !target.sync.clock.isEmpty()
                                   || !target.count.clock.isEmpty();
        if (hasTargetProcessing && target.links.size() > 0) {
            out << "    wire " << target.name << "_internal;\n";
        }
    }

    out << "\n";
}

void QSocResetPrimitive::generateResetLogic(const ResetControllerConfig &config, QTextStream &out)
{
    out << "    /* Reset logic instances */\n";

    for (int targetIdx = 0; targetIdx < config.targets.size(); ++targetIdx) {
        const auto &target = config.targets[targetIdx];

        out << "    /* Target: " << target.name << " */\n";

        // Generate link-level processing
        for (int linkIdx = 0; linkIdx < target.links.size(); ++linkIdx) {
            const auto &link       = target.links[linkIdx];
            QString     outputWire = getLinkWireName(target.name, linkIdx);

            // Determine if we need component processing for this link
            bool hasAsync = !link.async.clock.isEmpty();
            bool hasSync  = !link.sync.clock.isEmpty();
            bool hasCount = !link.count.clock.isEmpty();

            if (hasAsync || hasSync || hasCount) {
                generateResetComponentInstance(
                    target.name,
                    linkIdx,
                    hasAsync ? &link.async : nullptr,
                    hasSync ? &link.sync : nullptr,
                    hasCount ? &link.count : nullptr,
                    false, // no inv in new architecture
                    link.source,
                    outputWire,
                    out);
            } else {
                // Direct connection - apply source polarity normalization
                QString normalizedSource = getNormalizedSource(link.source, config);
                out << "    assign " << outputWire << " = " << normalizedSource << ";\n";
            }
        }

        out << "\n";
    }
}

void QSocResetPrimitive::generateResetReason(const ResetControllerConfig &config, QTextStream &out)
{
    if (!config.reason.enabled || config.reason.sourceOrder.isEmpty()) {
        return;
    }

    out << "    /* Reset reason recording logic (Sync-clear async-capture sticky flags) */\n";
    out << "    // New architecture: async-set + sync-clear only, avoids S+R registers\n";
    out << "    // 2-cycle clear window after POR release or SW clear pulse\n";
    out << "    // Outputs gated by valid signal for proper initialization\n\n";

    // Generate event normalization (convert all to LOW-active _n signals)
    out << "    /* Event normalization: convert all sources to LOW-active format */\n";
    for (int i = 0; i < config.reason.sourceOrder.size(); ++i) {
        const QString &sourceName = config.reason.sourceOrder[i];
        QString        eventName  = QString("%1_event_n").arg(sourceName);

        // Find source active level
        QString sourceActive = "low"; // Default
        for (const auto &source : config.sources) {
            if (source.name == sourceName) {
                sourceActive = source.active;
                break;
            }
        }

        out << "    wire " << eventName << " = ";
        if (sourceActive == "high") {
            out << "~" << sourceName << ";  /* HIGH-active -> LOW-active */\n";
        } else {
            out << sourceName << ";   /* Already LOW-active */\n";
        }
    }
    out << "\n";

    // Generate SW clear synchronizer and pulse generator
    if (!config.reason.clear.isEmpty()) {
        out << "    /* Synchronize software clear and generate pulse */\n";
        out << "    reg swc_d1, swc_d2, swc_d3;\n";
        out << "    always @(posedge " << config.reason.clock << " or negedge "
            << config.reason.rootReset << ") begin\n";
        out << "        if (!" << config.reason.rootReset << ") begin\n";
        out << "            swc_d1 <= 1'b0;\n";
        out << "            swc_d2 <= 1'b0;\n";
        out << "            swc_d3 <= 1'b0;\n";
        out << "        end else begin\n";
        out << "            swc_d1 <= " << config.reason.clear << ";\n";
        out << "            swc_d2 <= swc_d1;\n";
        out << "            swc_d3 <= swc_d2;\n";
        out << "        end\n";
        out << "    end\n";
        out << "    wire sw_clear_pulse = swc_d2 & ~swc_d3;  // Rising-edge pulse\n\n";
    }

    // Generate fixed 2-cycle clear controller (no configurable parameters)
    out << "    /* Fixed 2-cycle clear controller and valid signal generation */\n";
    out << "    /* Design rationale: 2-cycle clear ensures clean removal of async events */\n";
    out << "    reg        init_done;   /* Set after first post-POR action */\n";
    out << "    reg [1:0]  clr_sr;      /* Fixed 2-cycle clear shift register */\n";
    out << "    reg        valid_q;     /* " << config.reason.valid << " register */\n\n";

    out << "    wire clr_en = |clr_sr;  /* Clear enable (active during 2-cycle window) */\n\n";

    out << "    always @(posedge " << config.reason.clock << " or negedge "
        << config.reason.rootReset << ") begin\n";
    out << "        if (!" << config.reason.rootReset << ") begin\n";
    out << "            init_done <= 1'b0;\n";
    out << "            clr_sr    <= 2'b00;\n";
    out << "            valid_q   <= 1'b0;\n";
    out << "        end else begin\n";
    out << "            /* Start fixed 2-cycle clear after POR release */\n";
    out << "            if (!init_done) begin\n";
    out << "                init_done <= 1'b1;\n";
    out << "                clr_sr    <= 2'b11;  /* Fixed: exactly 2 cycles */\n";
    out << "                valid_q   <= 1'b0;\n";

    if (!config.reason.clear.isEmpty()) {
        out << "            /* SW clear retriggers fixed 2-cycle clear */\n";
        out << "            end else if (sw_clear_pulse) begin\n";
        out << "                clr_sr  <= 2'b11;  /* Fixed: exactly 2 cycles */\n";
        out << "                valid_q <= 1'b0;\n";
    }

    out << "            /* Shift down the 2-cycle clear window */\n";
    out << "            end else if (clr_en) begin\n";
    out << "                clr_sr <= {1'b0, clr_sr[1]};\n";
    out << "            /* Set valid after fixed 2-cycle clear completes */\n";
    out << "            end else begin\n";
    out << "                valid_q <= 1'b1;\n";
    out << "            end\n";
    out << "        end\n";
    out << "    end\n\n";

    // Generate sticky flags with pure async-set + sync-clear using generate statement
    out << "    /* Sticky flags: async-set on event, sync-clear during clear window */\n";
    out << "    reg [" << (config.reason.vectorWidth - 1) << ":0] flags;\n\n";

    // Create event vector for generate block
    out << "    /* Event vector for generate block */\n";
    out << "    wire [" << (config.reason.vectorWidth - 1) << ":0] src_event_n = {\n";
    for (int i = config.reason.sourceOrder.size() - 1; i >= 0; --i) {
        const QString &sourceName = config.reason.sourceOrder[i];
        QString        eventName  = QString("%1_event_n").arg(sourceName);
        out << "        " << eventName;
        if (i > 0)
            out << ",";
        out << "\n";
    }
    out << "    };\n\n";

    // Use generate statement for all flags
    out << "    /* Reset reason flags generation using generate for loop */\n";
    out << "    genvar reason_idx;\n";
    out << "    generate\n";
    out << "        for (reason_idx = 0; reason_idx < " << config.reason.vectorWidth
        << "; reason_idx = reason_idx + 1) begin : gen_reason\n";
    out << "            always @(posedge " << config.reason.clock
        << " or negedge src_event_n[reason_idx]) begin\n";
    out << "                if (!src_event_n[reason_idx]) begin\n";
    out << "                    flags[reason_idx] <= 1'b1;      /* Async set on event assert (low) "
           "*/\n";
    out << "                end else if (clr_en) begin\n";
    out << "                    flags[reason_idx] <= 1'b0;      /* Sync clear during clear window "
           "*/\n";
    out << "                end\n";
    out << "            end\n";
    out << "        end\n";
    out << "    endgenerate\n\n";

    // Generate gated outputs
    out << "    /* Output gating: zeros until valid */\n";
    out << "    assign " << config.reason.valid << " = valid_q;\n";
    out << "    assign " << config.reason.output << " = " << config.reason.valid
        << " ? flags : " << config.reason.vectorWidth << "'b0;\n\n";
}

void QSocResetPrimitive::generateOutputAssignments(
    const ResetControllerConfig &config, QTextStream &out)
{
    out << "    /* Target output assignments */\n";

    for (const auto &target : config.targets) {
        QString inputSignal;

        if (target.links.size() == 0) {
            // No links - assign constant based on active level
            inputSignal = (target.active == "low") ? "1'b1" : "1'b0";
        } else if (target.links.size() == 1) {
            // Single link
            inputSignal = getLinkWireName(target.name, 0);
        } else {
            // Multiple links - AND them together (assuming active-low reset processing)
            out << "    wire " << target.name << "_combined = ";
            for (int i = 0; i < target.links.size(); ++i) {
                if (i > 0)
                    out << " & ";
                out << getLinkWireName(target.name, i);
            }
            out << ";\n";
            inputSignal = target.name + "_combined";
        }

        // Check if target has processing
        bool hasAsync = !target.async.clock.isEmpty();
        bool hasSync  = !target.sync.clock.isEmpty();
        bool hasCount = !target.count.clock.isEmpty();

        if (hasAsync || hasSync || hasCount) {
            // Target-level processing
            generateResetComponentInstance(
                target.name,
                -1, // -1 indicates target-level
                hasAsync ? &target.async : nullptr,
                hasSync ? &target.sync : nullptr,
                hasCount ? &target.count : nullptr,
                false, // no inv
                inputSignal,
                target.name + "_processed",
                out);

            // Apply active level conversion for final output
            out << "    assign " << target.name << " = ";
            if (target.active == "low") {
                out << target.name << "_processed"; // Keep low-active
            } else {
                out << "~" << target.name << "_processed"; // Convert to high-active
            }
            out << ";\n";
        } else {
            // Direct assignment with active level conversion
            out << "    assign " << target.name << " = ";
            if (target.active == "low") {
                out << inputSignal; // Keep low-active
            } else {
                out << "~" << inputSignal; // Convert to high-active
            }
            out << ";\n";
        }
    }

    out << "\n";
}

void QSocResetPrimitive::generateResetCellFile(QTextStream &out)
{
    out << "`timescale 1ns / 1ps\n\n";

    // qsoc_rst_sync - Asynchronous reset synchronizer
    out << "/**\n";
    out << " * @brief Asynchronous reset synchronizer (active-low)\n";
    out << " * @param STAGE Number of sync stages (>=2 recommended)\n";
    out << " */\n";
    out << "module qsoc_rst_sync\n";
    out << "#(\n";
    out << "  parameter [31:0] STAGE = 32'h3\n";
    out << ")\n";
    out << "(\n";
    out << "  input  wire clk,\n";
    out << "  input  wire rst_in_n,\n";
    out << "  input  wire test_enable,\n";
    out << "  output wire rst_out_n\n";
    out << ");\n\n";
    out << "  reg  [STAGE-1:0] sync_reg;\n";
    out << "  wire             core_rst_n;\n\n";
    out << "  always @(posedge clk or negedge rst_in_n) begin\n";
    out << "    if (!rst_in_n) begin\n";
    out << "      sync_reg <= {STAGE{1'b0}};\n";
    out << "    end else begin\n";
    out << "      sync_reg <= {sync_reg[STAGE-2:0], 1'b1};\n";
    out << "    end\n";
    out << "  end\n\n";
    out << "  assign core_rst_n = sync_reg[STAGE-1];\n";
    out << "  assign rst_out_n  = test_enable ? rst_in_n : core_rst_n;\n\n";
    out << "endmodule\n\n";

    // qsoc_rst_pipe - Synchronous reset pipeline
    out << "/**\n";
    out << " * @brief Synchronous reset pipeline (active-low)\n";
    out << " * @param STAGE Number of pipeline stages (>=1)\n";
    out << " */\n";
    out << "module qsoc_rst_pipe\n";
    out << "#(\n";
    out << "  parameter [31:0] STAGE = 32'h4\n";
    out << ")\n";
    out << "(\n";
    out << "  input  wire clk,\n";
    out << "  input  wire rst_in_n,\n";
    out << "  input  wire test_enable,\n";
    out << "  output wire rst_out_n\n";
    out << ");\n\n";
    out << "  reg  [STAGE-1:0] pipe_reg;\n";
    out << "  wire             core_rst_n;\n\n";
    out << "  always @(posedge clk) begin\n";
    out << "    if (!rst_in_n) begin\n";
    out << "      pipe_reg <= {STAGE{1'b0}};\n";
    out << "    end else begin\n";
    out << "      pipe_reg <= {pipe_reg[STAGE-2:0], 1'b1};\n";
    out << "    end\n";
    out << "  end\n\n";
    out << "  assign core_rst_n = pipe_reg[STAGE-1];\n";
    out << "  assign rst_out_n  = test_enable ? rst_in_n : core_rst_n;\n\n";
    out << "endmodule\n\n";

    // qsoc_rst_count - Counter-based reset release
    out << "/**\n";
    out << " * @brief Counter-based reset release (active-low)\n";
    out << " * @param CYCLE Number of cycles before release\n";
    out << " */\n";
    out << "module qsoc_rst_count\n";
    out << "#(\n";
    out << "  parameter [31:0] CYCLE = 32'h10\n";
    out << ")\n";
    out << "(\n";
    out << "  input  wire clk,\n";
    out << "  input  wire rst_in_n,\n";
    out << "  input  wire test_enable,\n";
    out << "  output wire rst_out_n\n";
    out << ");\n\n";
    out << "  localparam [5:0] CNT_WIDTH =\n";
    out << "    (CYCLE <= 32'h2)         ? 6'h01 :\n";
    out << "    (CYCLE <= 32'h4)         ? 6'h02 :\n";
    out << "    (CYCLE <= 32'h8)         ? 6'h03 :\n";
    out << "    (CYCLE <= 32'h10)        ? 6'h04 :\n";
    out << "    (CYCLE <= 32'h20)        ? 6'h05 :\n";
    out << "    (CYCLE <= 32'h40)        ? 6'h06 :\n";
    out << "    (CYCLE <= 32'h80)        ? 6'h07 :\n";
    out << "    (CYCLE <= 32'h100)       ? 6'h08 :\n";
    out << "    (CYCLE <= 32'h200)       ? 6'h09 :\n";
    out << "    (CYCLE <= 32'h400)       ? 6'h0A :\n";
    out << "    (CYCLE <= 32'h800)       ? 6'h0B :\n";
    out << "    (CYCLE <= 32'h1000)      ? 6'h0C :\n";
    out << "    (CYCLE <= 32'h2000)      ? 6'h0D :\n";
    out << "    (CYCLE <= 32'h4000)      ? 6'h0E :\n";
    out << "    (CYCLE <= 32'h8000)      ? 6'h0F :\n";
    out << "    (CYCLE <= 32'h10000)     ? 6'h10 :\n";
    out << "    (CYCLE <= 32'h20000)     ? 6'h11 :\n";
    out << "    (CYCLE <= 32'h40000)     ? 6'h12 :\n";
    out << "    (CYCLE <= 32'h80000)     ? 6'h13 :\n";
    out << "    (CYCLE <= 32'h100000)    ? 6'h14 :\n";
    out << "    (CYCLE <= 32'h200000)    ? 6'h15 :\n";
    out << "    (CYCLE <= 32'h400000)    ? 6'h16 :\n";
    out << "    (CYCLE <= 32'h800000)    ? 6'h17 :\n";
    out << "    (CYCLE <= 32'h1000000)   ? 6'h18 :\n";
    out << "    (CYCLE <= 32'h2000000)   ? 6'h19 :\n";
    out << "    (CYCLE <= 32'h4000000)   ? 6'h1A :\n";
    out << "    (CYCLE <= 32'h8000000)   ? 6'h1B :\n";
    out << "    (CYCLE <= 32'h10000000)  ? 6'h1C :\n";
    out << "    (CYCLE <= 32'h20000000)  ? 6'h1D :\n";
    out << "    (CYCLE <= 32'h40000000)  ? 6'h1E :\n";
    out << "    (CYCLE <= 32'h80000000)  ? 6'h1F : 6'h20;\n\n";
    out << "  reg [CNT_WIDTH-1:0] cnt;\n";
    out << "  reg                 core_rst_n;\n\n";
    out << "  always @(posedge clk or negedge rst_in_n) begin\n";
    out << "    if (!rst_in_n) begin\n";
    out << "      cnt        <= {CNT_WIDTH{1'b0}};\n";
    out << "      core_rst_n <= 1'b0;\n";
    out << "    end else if (cnt < CYCLE[CNT_WIDTH-1:0]) begin\n";
    out << "      cnt        <= cnt + {{(CNT_WIDTH-1){1'b0}}, 1'b1};\n";
    out << "      core_rst_n <= 1'b0;\n";
    out << "    end else begin\n";
    out << "      core_rst_n <= 1'b1;\n";
    out << "    end\n";
    out << "  end\n\n";
    out << "  assign rst_out_n = test_enable ? rst_in_n : core_rst_n;\n\n";
    out << "endmodule\n\n";
}

bool QSocResetPrimitive::generateResetCellFile(const QString &outputDir)
{
    QString filePath = QDir(outputDir).filePath("reset_cell.v");
    QFile   file(filePath);

    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "Cannot open reset_cell.v for writing:" << file.errorString();
        return false;
    }

    QTextStream out(&file);

    // Write file header
    out << "/**\n";
    out << " * @file reset_cell.v\n";
    out << " * @brief Template reset cells for QSoC reset primitives\n";
    out << " *\n";
    out << " * @details This file contains template reset cell modules for reset primitives.\n";
    out << " *          Auto-generated template file. Generated by qsoc.\n";
    out << " * CAUTION: Please replace the templates in this file\n";
    out << " *          with your technology's standard-cell implementations\n";
    out << " *          before using in production.\n";
    out << " */\n\n";

    generateResetCellFile(out); // Call existing implementation
    return true;
}

void QSocResetPrimitive::generateResetComponentInstance(
    const QString     &targetName,
    int                linkIndex,
    const AsyncConfig *async,
    const SyncConfig  *sync,
    const CountConfig *count,
    bool               inv,
    const QString     &inputSignal,
    const QString     &outputSignal,
    QTextStream       &out)
{
    Q_UNUSED(inv); // No inv in new architecture

    QString instanceName = getComponentInstanceName(
        targetName,
        linkIndex,
        async  ? "async"
        : sync ? "sync"
               : "count");

    if (async && !async->clock.isEmpty()) {
        // Generate qsoc_rst_sync instance
        out << "    qsoc_rst_sync #(\n";
        out << "        .STAGE(" << async->stage << ")\n";
        out << "    ) " << instanceName << " (\n";
        out << "        .clk(" << async->clock << "),\n";
        out << "        .rst_in_n(" << inputSignal << "),\n";
        out << "        .test_enable(" << async->test_enable << "),\n";
        out << "        .rst_out_n(" << outputSignal << ")\n";
        out << "    );\n";
    } else if (sync && !sync->clock.isEmpty()) {
        // Generate qsoc_rst_pipe instance
        out << "    qsoc_rst_pipe #(\n";
        out << "        .STAGE(" << sync->stage << ")\n";
        out << "    ) " << instanceName << " (\n";
        out << "        .clk(" << sync->clock << "),\n";
        out << "        .rst_in_n(" << inputSignal << "),\n";
        out << "        .test_enable(" << sync->test_enable << "),\n";
        out << "        .rst_out_n(" << outputSignal << ")\n";
        out << "    );\n";
    } else if (count && !count->clock.isEmpty()) {
        // Generate qsoc_rst_count instance
        out << "    qsoc_rst_count #(\n";
        out << "        .CYCLE(" << count->cycle << ")\n";
        out << "    ) " << instanceName << " (\n";
        out << "        .clk(" << count->clock << "),\n";
        out << "        .rst_in_n(" << inputSignal << "),\n";
        out << "        .test_enable(" << count->test_enable << "),\n";
        out << "        .rst_out_n(" << outputSignal << ")\n";
        out << "    );\n";
    }
}

QString QSocResetPrimitive::getNormalizedSource(
    const QString &sourceName, const ResetControllerConfig &config)
{
    // Find source active level and normalize to low-active
    for (const auto &source : config.sources) {
        if (source.name == sourceName) {
            if (source.active == "high") {
                return "~" + sourceName; // Convert high-active to low-active
            } else {
                return sourceName; // Already low-active
            }
        }
    }

    // Default to low-active if not found
    return sourceName;
}

QString QSocResetPrimitive::getLinkWireName(const QString &targetName, int linkIndex)
{
    // Remove _n suffix for clean naming
    QString cleanTarget = targetName;
    if (cleanTarget.endsWith("_n")) {
        cleanTarget = cleanTarget.left(cleanTarget.length() - 2);
    }

    return QString("%1_link%2_n").arg(cleanTarget).arg(linkIndex);
}

QString QSocResetPrimitive::getComponentInstanceName(
    const QString &targetName, int linkIndex, const QString &componentType)
{
    // Remove _n suffix for clean naming
    QString cleanTarget = targetName;
    if (cleanTarget.endsWith("_n")) {
        cleanTarget = cleanTarget.left(cleanTarget.length() - 2);
    }

    if (linkIndex >= 0) {
        return QString("i_%1_link%2_%3").arg(cleanTarget).arg(linkIndex).arg(componentType);
    } else {
        return QString("i_%1_target_%2").arg(cleanTarget).arg(componentType);
    }
}
