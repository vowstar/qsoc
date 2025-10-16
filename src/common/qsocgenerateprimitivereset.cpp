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

void QSocResetPrimitive::setForceOverwrite(bool force)
{
    m_forceOverwrite = force;
}

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

    // Generate Typst reset diagram (failure does not affect Verilog generation)
    if (m_parent && m_parent->getProjectManager()) {
        QString outputDir = m_parent->getProjectManager()->getOutputPath();
        QString typstPath = outputDir + QStringLiteral("/") + config.moduleName
                            + QStringLiteral(".typ");
        if (!generateTypstDiagram(config, typstPath)) {
            qWarning() << "Failed to generate Typst diagram (non-critical):" << typstPath;
        }
    }

    return true;
}

QSocResetPrimitive::ResetControllerConfig QSocResetPrimitive::parseResetConfig(
    const YAML::Node &resetNode)
{
    ResetControllerConfig config;

    // Basic configuration
    if (!resetNode["name"]) {
        qCritical() << "Error: 'name' field is required in reset configuration";
        qCritical() << "Example: reset: { name: my_reset_ctrl, ... }";
        return config;
    }
    config.name       = QString::fromStdString(resetNode["name"].as<std::string>());
    config.moduleName = config.name; // Use same name for module

    // Test enable is optional - if not set, tie to 1'b0 internally
    if (resetNode["test_enable"]) {
        config.testEnable = QString::fromStdString(resetNode["test_enable"].as<std::string>());
    }

    // Parse sources (source: {name: {polarity: ...}})
    if (resetNode["source"] && resetNode["source"].IsMap()) {
        for (auto it = resetNode["source"].begin(); it != resetNode["source"].end(); ++it) {
            ResetSource source;
            source.name = QString::fromStdString(it->first.as<std::string>());

            if (it->second.IsMap() && it->second["active"]) {
                source.active = QString::fromStdString(it->second["active"].as<std::string>());
            } else {
                qCritical() << "Error: 'active' field is required for source '" << source.name
                            << "'";
                qCritical() << "Please specify active level explicitly: 'high' or 'low'";
                qCritical() << "Example: source: { " << source.name << ": {active: low} }";
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
                if (!asyncNode["clock"]) {
                    qCritical()
                        << "Error: 'clock' field is required for async component in target '"
                        << target.name << "'";
                    return config;
                }
                target.async.clock = QString::fromStdString(asyncNode["clock"].as<std::string>());
                target.async.test_enable = config.testEnable; // Use controller-level test_enable
                target.async.stage       = asyncNode["stage"] ? asyncNode["stage"].as<int>() : 3;
            }

            if (tgtNode["sync"]) {
                const YAML::Node &syncNode = tgtNode["sync"];
                if (!syncNode["clock"]) {
                    qCritical() << "Error: 'clock' field is required for sync component in target '"
                                << target.name << "'";
                    return config;
                }
                target.sync.clock = QString::fromStdString(syncNode["clock"].as<std::string>());
                target.sync.test_enable = config.testEnable; // Use controller-level test_enable
                target.sync.stage       = syncNode["stage"] ? syncNode["stage"].as<int>() : 4;
            }

            if (tgtNode["count"]) {
                const YAML::Node &countNode = tgtNode["count"];
                if (!countNode["clock"]) {
                    qCritical()
                        << "Error: 'clock' field is required for count component in target '"
                        << target.name << "'";
                    return config;
                }
                target.count.clock = QString::fromStdString(countNode["clock"].as<std::string>());
                target.count.test_enable = config.testEnable; // Use controller-level test_enable
                target.count.cycle       = countNode["cycle"] ? countNode["cycle"].as<int>() : 16;
            }

            // Parse links for this target
            if (tgtNode["link"] && tgtNode["link"].IsMap()) {
                for (auto linkIt = tgtNode["link"].begin(); linkIt != tgtNode["link"].end();
                     ++linkIt) {
                    const YAML::Node &linkNode = linkIt->second;

                    ResetLink link;
                    link.source = QString::fromStdString(linkIt->first.as<std::string>());

                    // Handle null/empty links (direct connections)
                    if (!linkNode || linkNode.IsNull()) {
                        // Direct connection - no components
                        target.links.append(link);
                        continue;
                    }

                    if (!linkNode.IsMap())
                        continue;

                    // Parse link-level components
                    if (linkNode["async"]) {
                        const YAML::Node &asyncNode = linkNode["async"];
                        if (!asyncNode["clock"]) {
                            qCritical()
                                << "Error: 'clock' field is required for async component in link '"
                                << link.source << "' of target '" << target.name << "'";
                            return config;
                        }
                        link.async.clock = QString::fromStdString(
                            asyncNode["clock"].as<std::string>());
                        link.async.test_enable
                            = config.testEnable; // Use controller-level test_enable
                        link.async.stage = asyncNode["stage"] ? asyncNode["stage"].as<int>() : 3;
                    }

                    if (linkNode["sync"]) {
                        const YAML::Node &syncNode = linkNode["sync"];
                        if (!syncNode["clock"]) {
                            qCritical()
                                << "Error: 'clock' field is required for sync component in link '"
                                << link.source << "' of target '" << target.name << "'";
                            return config;
                        }
                        link.sync.clock = QString::fromStdString(
                            syncNode["clock"].as<std::string>());
                        link.sync.test_enable = config.testEnable; // Use controller-level test_enable
                        link.sync.stage = syncNode["stage"] ? syncNode["stage"].as<int>() : 4;
                    }

                    if (linkNode["count"]) {
                        const YAML::Node &countNode = linkNode["count"];
                        if (!countNode["clock"]) {
                            qCritical()
                                << "Error: 'clock' field is required for count component in link '"
                                << link.source << "' of target '" << target.name << "'";
                            return config;
                        }
                        link.count.clock = QString::fromStdString(
                            countNode["clock"].as<std::string>());
                        link.count.test_enable
                            = config.testEnable; // Use controller-level test_enable
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

    // Initialize global port tracking at the beginning of the function
    QSet<QString> addedSignals;

    // Collect all unique clock signals
    QStringList clocks;

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

    // Collect all output signals (targets) for "output win" mechanism
    QSet<QString> outputSignals;
    for (const auto &target : config.targets) {
        outputSignals.insert(target.name);
    }

    // Collect all unique source signals, but exclude those that are also outputs
    QStringList sources;
    for (const auto &target : config.targets) {
        for (const auto &link : target.links) {
            // Skip source if it's also an output signal ("output win" mechanism)
            if (!outputSignals.contains(link.source) && !sources.contains(link.source)) {
                sources.append(link.source);
            }
        }
    }

    // Collect port declarations and comments separately for proper comma placement
    QStringList portDecls;
    QStringList portComments;

    // Clock inputs
    for (const auto &clock : clocks) {
        portDecls << QString("    input  wire %1").arg(clock);
        portComments << "    /**< Clock inputs */";
        addedSignals.insert(clock);
    }

    // Source inputs (excluding those that are also outputs)
    for (const auto &source : sources) {
        portDecls << QString("    input  wire %1").arg(source);
        portComments << "    /**< Reset sources */";
        addedSignals.insert(source);
    }

    // Test enable input (if specified)
    if (!config.testEnable.isEmpty()) {
        portDecls << QString("    input  wire %1").arg(config.testEnable);
        portComments << "    /**< Test enable signal */";
        addedSignals.insert(config.testEnable);
    }

    // Reset reason clear signal
    if (config.reason.enabled && !config.reason.clear.isEmpty()) {
        portDecls << QString("    input  wire %1").arg(config.reason.clear);
        portComments << "    /**< Reset reason clear */";
        addedSignals.insert(config.reason.clear);
    }

    // Reset targets (outputs win over inputs)
    for (const auto &target : config.targets) {
        portDecls << QString("    output wire %1").arg(target.name);
        portComments << "    /**< Reset targets */";
        addedSignals.insert(target.name);
    }

    // Reset reason outputs
    if (config.reason.enabled) {
        if (config.reason.vectorWidth > 1) {
            portDecls << QString("    output wire [%1:0] %2")
                             .arg(config.reason.vectorWidth - 1)
                             .arg(config.reason.output);
        } else {
            portDecls << QString("    output wire %1").arg(config.reason.output);
        }
        portComments << "    /**< Reset reason outputs */";
        addedSignals.insert(config.reason.output);

        portDecls << QString("    output wire %1").arg(config.reason.valid);
        portComments << "    /**< Reset reason outputs */";
        addedSignals.insert(config.reason.valid);
    }

    // Output all ports with unified boundary judgment
    for (int i = 0; i < portDecls.size(); ++i) {
        bool    isLast = (i == portDecls.size() - 1);
        QString comma  = isLast ? "" : ",";
        out << portDecls[i] << comma << portComments[i] << "\n";
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
    out << "`timescale 1ns / 1ps\n";

    // qsoc_rst_sync - Asynchronous reset synchronizer
    out << "/**\n";
    out << " * @brief Asynchronous reset synchronizer (active-low)\n";
    out << " * @param STAGE Number of sync stages (>=2 recommended)\n";
    out << " */\n";
    out << "module qsoc_rst_sync\n";
    out << "#(\n";
    out << "    parameter integer STAGE = 3\n";
    out << ")\n";
    out << "(\n";
    out << "    input  wire clk,        /**< Clock input */\n";
    out << "    input  wire rst_in_n,   /**< Reset input (active-low) */\n";
    out << "    input  wire test_enable, /**< Test enable signal */\n";
    out << "    output wire rst_out_n   /**< Reset output (active-low) */\n";
    out << ");\n\n";
    out << "    localparam integer S = (STAGE < 1) ? 1 : STAGE;\n\n";
    out << "    reg  [S-1:0] sync_reg;\n";
    out << "    wire         core_rst_n;\n\n";
    out << "    generate\n";
    out << "        if (S == 1) begin : g_st1\n";
    out << "            always @(posedge clk or negedge rst_in_n) begin\n";
    out << "                if (!rst_in_n) sync_reg <= 1'b0;\n";
    out << "                else           sync_reg <= 1'b1;\n";
    out << "            end\n";
    out << "        end else begin : g_stN\n";
    out << "            always @(posedge clk or negedge rst_in_n) begin\n";
    out << "                if (!rst_in_n) sync_reg <= {S{1'b0}};\n";
    out << "                else           sync_reg <= {sync_reg[S-2:0], 1'b1};\n";
    out << "            end\n";
    out << "        end\n";
    out << "    endgenerate\n\n";
    out << "    assign core_rst_n = sync_reg[S-1];\n";
    out << "    assign rst_out_n  = test_enable ? rst_in_n : core_rst_n;\n\n";
    out << "endmodule\n\n";

    // qsoc_rst_pipe - Synchronous reset pipeline
    out << "/**\n";
    out << " * @brief Synchronous reset pipeline (active-low)\n";
    out << " * @param STAGE Number of pipeline stages (>=1)\n";
    out << " */\n";
    out << "module qsoc_rst_pipe\n";
    out << "#(\n";
    out << "    parameter integer STAGE = 4\n";
    out << ")\n";
    out << "(\n";
    out << "    input  wire clk,        /**< Clock input */\n";
    out << "    input  wire rst_in_n,   /**< Reset input (active-low) */\n";
    out << "    input  wire test_enable, /**< Test enable signal */\n";
    out << "    output wire rst_out_n   /**< Reset output (active-low) */\n";
    out << ");\n\n";
    out << "    localparam integer S = (STAGE < 1) ? 1 : STAGE;\n\n";
    out << "    reg  [S-1:0] pipe_reg;\n";
    out << "    wire         core_rst_n;\n\n";
    out << "    generate\n";
    out << "        if (S == 1) begin : g_st1\n";
    out << "            always @(posedge clk) begin\n";
    out << "                if (!rst_in_n) pipe_reg <= 1'b0;\n";
    out << "                else           pipe_reg <= 1'b1;\n";
    out << "            end\n";
    out << "        end else begin : g_stN\n";
    out << "            always @(posedge clk) begin\n";
    out << "                if (!rst_in_n) pipe_reg <= {S{1'b0}};\n";
    out << "                else           pipe_reg <= {pipe_reg[S-2:0], 1'b1};\n";
    out << "            end\n";
    out << "        end\n";
    out << "    endgenerate\n\n";
    out << "    assign core_rst_n = pipe_reg[S-1];\n";
    out << "    assign rst_out_n  = test_enable ? rst_in_n : core_rst_n;\n\n";
    out << "endmodule\n\n";

    // qsoc_rst_count - Counter-based reset release
    out << "/**\n";
    out << " * @brief Counter-based reset release (active-low)\n";
    out << " * @param CYCLE Number of cycles before release\n";
    out << " */\n";
    out << "module qsoc_rst_count\n";
    out << "#(\n";
    out << "    parameter integer CYCLE = 16\n";
    out << ")\n";
    out << "(\n";
    out << "    input  wire clk,        /**< Clock input */\n";
    out << "    input  wire rst_in_n,   /**< Reset input (active-low) */\n";
    out << "    input  wire test_enable, /**< Test enable signal */\n";
    out << "    output wire rst_out_n   /**< Reset output (active-low) */\n";
    out << ");\n\n";
    out << "    /* ceil(log2(n)) for n>=1 */\n";
    out << "    function integer clog2;\n";
    out << "        input integer n;\n";
    out << "        integer v;\n";
    out << "        begin\n";
    out << "            v = (n < 1) ? 1 : n - 1;\n";
    out << "            clog2 = 0;\n";
    out << "            while (v > 0) begin\n";
    out << "                v = v >> 1;\n";
    out << "                clog2 = clog2 + 1;\n";
    out << "            end\n";
    out << "            if (clog2 == 0) clog2 = 1;\n";
    out << "        end\n";
    out << "    endfunction\n\n";
    out << "    localparam integer C_INT     = (CYCLE < 1) ? 1 : CYCLE;\n";
    out << "    localparam integer CNT_WIDTH = clog2(C_INT);\n";
    out << "    localparam [CNT_WIDTH-1:0] C_M1 = C_INT - 1;\n\n";
    out << "    reg [CNT_WIDTH-1:0] cnt;\n";
    out << "    reg                 core_rst_n;\n\n";
    out << "    always @(posedge clk or negedge rst_in_n) begin\n";
    out << "        if (!rst_in_n) begin\n";
    out << "            cnt        <= {CNT_WIDTH{1'b0}};\n";
    out << "            core_rst_n <= 1'b0;\n";
    out << "        end else if (!core_rst_n) begin\n";
    out << "            if (cnt == C_M1) begin\n";
    out << "                core_rst_n <= 1'b1;             /* Keep exactly CYCLE cycles */\n";
    out << "            end else begin\n";
    out << "                cnt <= cnt + {{(CNT_WIDTH-1){1'b0}}, 1'b1};\n";
    out << "            end\n";
    out << "        end\n";
    out << "    end\n\n";
    out << "    assign rst_out_n = test_enable ? rst_in_n : core_rst_n;\n\n";
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

    generateResetCellFile(out); // Call existing implementation
    file.close();

    /* Format generated reset_cell.v file if verible-verilog-format is available */
    QSocGenerateManager::formatVerilogFile(filePath);

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
        QString testEn = async->test_enable.isEmpty() ? "1'b0" : async->test_enable;
        out << "        .test_enable(" << testEn << "),\n";
        out << "        .rst_out_n(" << outputSignal << ")\n";
        out << "    );\n";
    } else if (sync && !sync->clock.isEmpty()) {
        // Generate qsoc_rst_pipe instance
        out << "    qsoc_rst_pipe #(\n";
        out << "        .STAGE(" << sync->stage << ")\n";
        out << "    ) " << instanceName << " (\n";
        out << "        .clk(" << sync->clock << "),\n";
        out << "        .rst_in_n(" << inputSignal << "),\n";
        QString testEn = sync->test_enable.isEmpty() ? "1'b0" : sync->test_enable;
        out << "        .test_enable(" << testEn << "),\n";
        out << "        .rst_out_n(" << outputSignal << ")\n";
        out << "    );\n";
    } else if (count && !count->clock.isEmpty()) {
        // Generate qsoc_rst_count instance
        out << "    qsoc_rst_count #(\n";
        out << "        .CYCLE(" << count->cycle << ")\n";
        out << "    ) " << instanceName << " (\n";
        out << "        .clk(" << count->clock << "),\n";
        out << "        .rst_in_n(" << inputSignal << "),\n";
        QString testEn = count->test_enable.isEmpty() ? "1'b0" : count->test_enable;
        out << "        .test_enable(" << testEn << "),\n";
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

/* Typst Reset Diagram Generation */

QString QSocResetPrimitive::escapeTypstId(const QString &str) const
{
    QString result = str;
    return result.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_-]+")), QStringLiteral("_"));
}

QString QSocResetPrimitive::typstHeader() const
{
    return QStringLiteral(
        "#import \"@preview/circuiteria:0.2.0\": *\n"
        "#import \"@preview/cetz:0.3.2\": draw\n"
        "#set page(width: auto, height: auto, margin: .5cm)\n"
        "#set text(font: \"Sarasa Mono SC\", size: 10pt)\n"
        "#align(center)[\n"
        "  = Reset tree\n"
        "  #text(size: 8pt, fill: gray)[Generated by QSoC v1.0.2]\n"
        "]\n"
        "#v(0.5cm)\n"
        "#circuit({\n");
}

QString QSocResetPrimitive::typstLegend() const
{
    const float y  = -1.5f;
    const float x  = 0.0f;
    const float sp = 3.5f;

    QString     result;
    QTextStream s(&result);
    s.setRealNumberPrecision(2);
    s.setRealNumberNotation(QTextStream::FixedNotation);

    s << "  // === Legend ===\n";

    // OR - Green
    s << "  element.block(x: " << x << ", y: " << (y + 0.3) << ", w: 1.0, h: 0.8, "
      << "id: \"legend_or\", name: \"OR\", fill: util.colors.green, "
      << "ports: (west: ((id: \"i\"),), east: ((id: \"o\"),)))\n";
    s << "  draw.content((" << (x + 0.5) << ", " << (y - 0.8) << "), [OR])\n";

    // ASYNC - Blue
    s << "  element.block(x: " << (x + sp) << ", y: " << (y + 0.3) << ", w: 1.0, h: 0.8, "
      << "id: \"legend_async\", name: \"ASYNC\", fill: util.colors.blue, "
      << "ports: (west: ((id: \"i\"),), east: ((id: \"o\"),)))\n";
    s << "  draw.content((" << (x + sp + 0.5) << ", " << (y - 0.8) << "), [ASYNC])\n";

    // SYNC - Yellow
    s << "  element.block(x: " << (x + sp * 2) << ", y: " << (y + 0.3) << ", w: 1.0, h: 0.8, "
      << "id: \"legend_sync\", name: \"SYNC\", fill: util.colors.yellow, "
      << "ports: (west: ((id: \"i\"),), east: ((id: \"o\"),)))\n";
    s << "  draw.content((" << (x + sp * 2 + 0.5) << ", " << (y - 0.8) << "), [SYNC])\n";

    // COUNT - Orange
    s << "  element.block(x: " << (x + sp * 3) << ", y: " << (y + 0.3) << ", w: 1.0, h: 0.8, "
      << "id: \"legend_count\", name: \"COUNT\", fill: util.colors.orange, "
      << "ports: (west: ((id: \"i\"),), east: ((id: \"o\"),)))\n";
    s << "  draw.content((" << (x + sp * 3 + 0.5) << ", " << (y - 0.8) << "), [COUNT])\n\n";

    return result;
}

QString QSocResetPrimitive::typstRootStubs(const QList<ResetSource> &sources, float &bottomY) const
{
    if (sources.isEmpty()) {
        bottomY = -5.0f;
        return QString();
    }

    const int   perRow = 4;
    const float y0     = -5.0f;
    const float x0     = 0.0f;
    const float dx     = 4.0f;
    const float dy     = 2.5f;

    QString     result;
    QTextStream s(&result);
    s.setRealNumberPrecision(2);
    s.setRealNumberNotation(QTextStream::FixedNotation);

    s << "  // === Reset sources ===\n";

    int numRows = (sources.size() + perRow - 1) / perRow;
    bottomY     = y0 - (numRows - 1) * dy - 3.5f;

    for (int idx = 0; idx < sources.size(); ++idx) {
        const ResetSource &src = sources[idx];
        int                row = idx / perRow;
        int                col = idx % perRow;
        float              x   = x0 + col * dx;
        float              y   = y0 - row * dy;
        QString            bid = escapeTypstId(QStringLiteral("SRC_") + src.name);

        s << "  element.block(x: " << x << ", y: " << y << ", w: .1, h: .1, "
          << "id: \"" << bid << "\", "
          << "ports: (north: ((id: \"N\"),)))\n";
        s << "  wire.stub(\"" << bid << "-port-N\", \"north\", name: \"" << src.name << "\")\n";
    }

    s << "\n";
    return result;
}

QString QSocResetPrimitive::typstTarget(const ResetTarget &target, float x, float y) const
{
    QString     result;
    QTextStream s(&result);
    s.setRealNumberPrecision(2);
    s.setRealNumberNotation(QTextStream::FixedNotation);

    QString tid   = escapeTypstId(target.name);
    QString title = target.name;

    s << "  // ---- " << title << " ----\n";

    if (target.links.isEmpty()) {
        return result;
    }

    // Analyze if all links share common component configuration
    bool        hasCommonComp = false;
    QString     compType; // "async", "sync", "count"
    AsyncConfig commonAsync;
    SyncConfig  commonSync;
    CountConfig commonCount;

    if (!target.links.isEmpty()) {
        bool    allSame = true;
        QString firstType;
        bool    firstHasComp = false;

        for (int i = 0; i < target.links.size(); ++i) {
            const ResetLink &link = target.links[i];
            QString          currentType;
            bool             currentHasComp = false;

            if (!link.async.clock.isEmpty()) {
                currentType    = QStringLiteral("async");
                currentHasComp = true;
                if (i == 0) {
                    commonAsync = link.async;
                } else if (link.async.clock != commonAsync.clock || link.async.stage != commonAsync.stage) {
                    allSame = false;
                    break;
                }
            } else if (!link.sync.clock.isEmpty()) {
                currentType    = QStringLiteral("sync");
                currentHasComp = true;
                if (i == 0) {
                    commonSync = link.sync;
                } else if (link.sync.clock != commonSync.clock || link.sync.stage != commonSync.stage) {
                    allSame = false;
                    break;
                }
            } else if (!link.count.clock.isEmpty()) {
                currentType    = QStringLiteral("count");
                currentHasComp = true;
                if (i == 0) {
                    commonCount = link.count;
                } else if (link.count.clock != commonCount.clock || link.count.cycle != commonCount.cycle) {
                    allSame = false;
                    break;
                }
            }

            if (i == 0) {
                firstType    = currentType;
                firstHasComp = currentHasComp;
            } else if (currentType != firstType || currentHasComp != firstHasComp) {
                allSame = false;
                break;
            }
        }

        if (allSame && firstHasComp) {
            hasCommonComp = true;
            compType      = firstType;
        }
    }

    // Build source list
    QStringList sources;
    for (const ResetLink &link : target.links)
        sources << link.source;

    int numSources = sources.size();

    float   orHeight = qMax(1.5f, 0.6f * numSources);
    QString prev;

    if (numSources == 1 && !hasCommonComp) {
        // Single source, direct connection
        QString sid = escapeTypstId(tid + QStringLiteral("_SRC"));
        s << "  element.block(x: " << x << ", y: " << (y + 0.6) << ", w: .8, h: .6, "
          << "id: \"" << sid << "\", name: \"\", "
          << "ports: (east: ((id: \"out\"),)))\n";
        s << "  wire.stub(\"" << sid << "-port-out\", \"west\", name: \"" << sources[0] << "\")\n";
        prev = sid + QStringLiteral("-port-out");
    } else {
        // Multiple sources or has component - use OR
        QString orId = escapeTypstId(tid + QStringLiteral("_OR"));
        s << "  element.block(\n";
        s << "    x: " << x << ", y: " << (y + 0.3) << ", w: 1.2, h: " << orHeight << ",\n";
        s << "    id: \"" << orId << "\", name: \"OR\", fill: util.colors.green,\n";
        s << "    ports: (west: (";
        for (int i = 0; i < numSources; ++i) {
            if (i > 0)
                s << ", ";
            s << "(id: \"in" << i << "\")";
        }
        s << ",), east: ((id: \"out\"),))\n";
        s << "  )\n";

        // Input stubs
        for (int i = 0; i < numSources; ++i) {
            s << "  wire.stub(\"" << orId << "-port-in" << i << "\", \"west\", name: \""
              << sources[i] << "\")\n";
        }

        prev = orId + QStringLiteral("-port-out");
    }

    // Add common component if exists
    if (hasCommonComp) {
        float compY = y + orHeight / 2 - 0.6f;

        QString compId, compIn, compOut;
        QString clock;
        QString label1, label2;

        if (compType == QStringLiteral("async")) {
            compId  = escapeTypstId(tid + QStringLiteral("_ASYNC"));
            clock   = commonAsync.clock;
            label1  = clock;
            label2  = QStringLiteral("stage:%1").arg(commonAsync.stage);
            compIn  = compId + QStringLiteral("-port-in");
            compOut = compId + QStringLiteral("-port-out");

            s << "  element.block(\n";
            s << "    x: " << (x + 2.5) << ", y: " << (compY + 0.3) << ", w: 1.5, h: 1.2,\n";
            s << "    id: \"" << compId << "\", name: \"ASYNC\", fill: util.colors.blue,\n";
            s << "    ports: (west: ((id: \"in\"),), east: ((id: \"out\"),))\n";
            s << "  )\n";
        } else if (compType == QStringLiteral("sync")) {
            compId  = escapeTypstId(tid + QStringLiteral("_SYNC"));
            clock   = commonSync.clock;
            label1  = clock;
            label2  = QStringLiteral("stage:%1").arg(commonSync.stage);
            compIn  = compId + QStringLiteral("-port-in");
            compOut = compId + QStringLiteral("-port-out");

            s << "  element.block(\n";
            s << "    x: " << (x + 2.5) << ", y: " << (compY + 0.3) << ", w: 1.5, h: 1.2,\n";
            s << "    id: \"" << compId << "\", name: \"SYNC\", fill: util.colors.yellow,\n";
            s << "    ports: (west: ((id: \"in\"),), east: ((id: \"out\"),))\n";
            s << "  )\n";
        } else if (compType == QStringLiteral("count")) {
            compId  = escapeTypstId(tid + QStringLiteral("_COUNT"));
            clock   = commonCount.clock;
            label1  = clock;
            label2  = QStringLiteral("cycle:%1").arg(commonCount.cycle);
            compIn  = compId + QStringLiteral("-port-in");
            compOut = compId + QStringLiteral("-port-out");

            s << "  element.block(\n";
            s << "    x: " << (x + 2.5) << ", y: " << (compY + 0.3) << ", w: 1.5, h: 1.2,\n";
            s << "    id: \"" << compId << "\", name: \"COUNT\", fill: util.colors.orange,\n";
            s << "    ports: (west: ((id: \"in\"),), east: ((id: \"out\"),))\n";
            s << "  )\n";
        }

        // Add labels
        s << "  draw.content((" << (x + 3.25) << ", " << (compY - 0.3) << "), text(size: 6pt)["
          << label1 << "])\n";
        s << "  draw.content((" << (x + 3.25) << ", " << (compY - 0.7) << "), text(size: 6pt)["
          << label2 << "])\n";

        // Wire from OR/SRC to component
        s << "  wire.wire(\"w_" << tid << "_or_comp\", (\n";
        s << "    \"" << prev << "\", \"" << compIn << "\"\n";
        s << "  ))\n";
        prev = compOut;
    }

    // Final output stub - align with OR center
    QString oid  = escapeTypstId(tid + QStringLiteral("_OUT"));
    float   outY = y + orHeight / 2;
    s << "  element.block(x: " << (x + 5.5) << ", y: " << outY << ", w: .8, h: .6, "
      << "id: \"" << oid << "\", name: \"\", "
      << "ports: (east: ((id: \"E\"),)))\n";
    s << "  wire.wire(\"w_" << tid << "_to_out\", (\n";
    s << "    \"" << prev << "\", \"" << oid << "-port-E\"\n";
    s << "  ))\n";
    s << "  wire.stub(\"" << oid << "-port-E\", \"east\", name: \"" << target.name << "\")\n\n";

    return result;
}

bool QSocResetPrimitive::generateTypstDiagram(
    const ResetControllerConfig &config, const QString &outputPath)
{
    QFile file(outputPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text)) {
        qWarning() << "Failed to open Typst output file:" << outputPath;
        return false;
    }

    QTextStream out(&file);
    out.setEncoding(QStringConverter::Utf8);

    // Generate header
    out << typstHeader();

    // Generate legend
    out << typstLegend();

    // Generate root reset source stubs
    float bottomY = -5.0f;
    out << typstRootStubs(config.sources, bottomY);

    // Generate targets (vertical stacking)
    const float x0 = 0.0f;
    const float y0 = bottomY - 2.5f;
    const float dy = 5.0f; // Vertical spacing

    for (int idx = 0; idx < config.targets.size(); ++idx) {
        const ResetTarget &target = config.targets[idx];
        float              y      = y0 - idx * dy;
        out << typstTarget(target, x0, y);
    }

    // Close circuit
    out << "})\n";

    file.close();
    qInfo() << "Generated Typst reset diagram:" << outputPath;
    return true;
}
