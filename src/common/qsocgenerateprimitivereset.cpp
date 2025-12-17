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
    const float w  = 1.6f; // Wider blocks to fit text
    const float sp = 4.0f; // Increased spacing for wider blocks

    QString     result;
    QTextStream s(&result);
    s.setRealNumberPrecision(2);
    s.setRealNumberNotation(QTextStream::FixedNotation);

    s << "  // === Legend ===\n";

    // AND - Green (active-low reset signals use AND logic)
    s << "  element.block(x: " << x << ", y: " << (y + 0.3) << ", w: " << w << ", h: 0.8, "
      << "id: \"legend_and\", name: \"AND\", fill: util.colors.green, "
      << "ports: (west: ((id: \"i\"),), east: ((id: \"o\"),)))\n";
    s << "  draw.content((" << (x + w / 2) << ", " << (y - 0.8) << "), [AND])\n";

    // ASYNC - Blue
    s << "  element.block(x: " << (x + sp) << ", y: " << (y + 0.3) << ", w: " << w << ", h: 0.8, "
      << "id: \"legend_async\", name: \"ASYNC\", fill: util.colors.blue, "
      << "ports: (west: ((id: \"i\"),), east: ((id: \"o\"),)))\n";
    s << "  draw.content((" << (x + sp + w / 2) << ", " << (y - 0.8) << "), [ASYNC])\n";

    // SYNC - Yellow
    s << "  element.block(x: " << (x + sp * 2) << ", y: " << (y + 0.3) << ", w: " << w
      << ", h: 0.8, "
      << "id: \"legend_sync\", name: \"SYNC\", fill: util.colors.yellow, "
      << "ports: (west: ((id: \"i\"),), east: ((id: \"o\"),)))\n";
    s << "  draw.content((" << (x + sp * 2 + w / 2) << ", " << (y - 0.8) << "), [SYNC])\n";

    // COUNT - Orange
    s << "  element.block(x: " << (x + sp * 3) << ", y: " << (y + 0.3) << ", w: " << w
      << ", h: 0.8, "
      << "id: \"legend_count\", name: \"COUNT\", fill: util.colors.orange, "
      << "ports: (west: ((id: \"i\"),), east: ((id: \"o\"),)))\n";
    s << "  draw.content((" << (x + sp * 3 + w / 2) << ", " << (y - 0.8) << "), [COUNT])\n\n";

    return result;
}

QString QSocResetPrimitive::typstSourceTable(
    const QList<ResetSource> &sources, float x, float &bottomY) const
{
    if (sources.isEmpty()) {
        bottomY = -5.0f;
        return QString();
    }

    QString     result;
    QTextStream s(&result);
    s.setRealNumberPrecision(2);
    s.setRealNumberNotation(QTextStream::FixedNotation);

    // Use Typst table for clean two-column layout
    // End the circuit block temporarily to insert table
    s << "})\n\n";

    s << "#v(0.3cm)\n";
    s << "#align(center)[\n";
    s << "  #text(weight: \"bold\", size: 10pt)[Reset Sources]\n";
    s << "]\n";
    s << "#v(0.2cm)\n";

    // Two-column table with source name and active level
    s << "#align(center)[\n";
    s << "#table(\n";
    s << "  columns: (auto, auto, auto, auto),\n";
    s << "  align: (left, center, left, center),\n";
    s << "  stroke: 0.5pt + gray,\n";
    s << "  inset: 5pt,\n";
    s << "  fill: (col, row) => if row == 0 { rgb(\"#e0e0e0\") },\n";
    s << "  [*Source*], [*Active*], [*Source*], [*Active*],\n";

    // Fill table rows - two sources per row
    int numSources = sources.size();
    for (int i = 0; i < numSources; i += 2) {
        const ResetSource &src1    = sources[i];
        QString            active1 = (src1.active == QStringLiteral("high")) ? "H" : "L";
        QString srcColor1          = (src1.active == QStringLiteral("high")) ? QStringLiteral("red")
                                                                             : QStringLiteral("blue");

        s << "  [#text(fill: " << srcColor1 << ")[" << src1.name << "]], ";
        s << "[#text(fill: " << srcColor1 << ")[" << active1 << "]], ";

        if (i + 1 < numSources) {
            const ResetSource &src2    = sources[i + 1];
            QString            active2 = (src2.active == QStringLiteral("high")) ? "H" : "L";
            QString srcColor2 = (src2.active == QStringLiteral("high")) ? QStringLiteral("red")
                                                                        : QStringLiteral("blue");
            s << "[#text(fill: " << srcColor2 << ")[" << src2.name << "]], ";
            s << "[#text(fill: " << srcColor2 << ")[" << active2 << "]],\n";
        } else {
            s << "[], [],\n"; // Empty cells for odd number of sources
        }
    }

    s << ")\n";
    s << "]\n\n";

    // Resume circuit block for targets
    s << "#v(0.3cm)\n";
    s << "#circuit({\n";

    // Calculate bottomY for target positioning
    int numRows = (numSources + 1) / 2; // Two sources per row
    bottomY     = -3.0f - numRows * 0.8f;

    return result;
}

QString QSocResetPrimitive::typstTarget(
    const ResetTarget &target, const QMap<QString, bool> &sourceIsHighActive, float x, float y) const
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

    int numSources = target.links.size();

    // Analyze link-level components (per-link ARSR, before AND gate)
    QVector<bool>    linkHasComp(numSources, false);
    QVector<QString> linkCompType(numSources);
    bool             anyLinkHasComp = false;

    for (int i = 0; i < numSources; ++i) {
        const ResetLink &link = target.links[i];
        if (!link.async.clock.isEmpty()) {
            linkHasComp[i]  = true;
            linkCompType[i] = QStringLiteral("async");
            anyLinkHasComp  = true;
        } else if (!link.sync.clock.isEmpty()) {
            linkHasComp[i]  = true;
            linkCompType[i] = QStringLiteral("sync");
            anyLinkHasComp  = true;
        } else if (!link.count.clock.isEmpty()) {
            linkHasComp[i]  = true;
            linkCompType[i] = QStringLiteral("count");
            anyLinkHasComp  = true;
        }
    }

    // Check which sources are high-active (need inversion bubble at AND input)
    QVector<bool> linkNeedsInvert(numSources, false);
    for (int i = 0; i < numSources; ++i) {
        const QString &srcName = target.links[i].source;
        if (sourceIsHighActive.contains(srcName) && sourceIsHighActive[srcName]) {
            linkNeedsInvert[i] = true;
        }
    }

    // Analyze target-level component (Post-AND ARSR, after AND gate)
    bool    hasTargetComp = false;
    QString targetCompType;
    if (!target.async.clock.isEmpty()) {
        hasTargetComp  = true;
        targetCompType = QStringLiteral("async");
    } else if (!target.sync.clock.isEmpty()) {
        hasTargetComp  = true;
        targetCompType = QStringLiteral("sync");
    } else if (!target.count.clock.isEmpty()) {
        hasTargetComp  = true;
        targetCompType = QStringLiteral("count");
    }

    // Layout calculation - NEW MODEL
    // Key insight from user: align BLOCK center to AND port, text hangs below
    // The NEXT port must clear the text from above
    //
    // Element sizes:
    const float blockHeight     = 1.0f; // ASYNC/SYNC/COUNT block height
    const float blockTopPad     = 0.3f; // Padding above block
    const float textHang        = 1.2f; // Text hangs below block (increased for better clearance)
    const float stubHeight      = 0.5f; // Visual height for stub label
    const float compGap         = 1.5f; // Larger gap for compound rows (with ASYNC/SYNC/COUNT)
    const float stubGap         = 0.8f; // Gap for simple stub rows
    const float targetCompH     = 1.2f; // Target component height
    const float targetTextBelow = 0.8f; // Space for text below target component

    // Calculate Y positions for each link port using bottom-up accumulation
    // Each element needs: its own height + clearance from element above
    QVector<float> linkPortY(numSources);
    QVector<float> slotHeight(numSources);

    // First pass: calculate slot heights (each slot clears text from above if needed)
    for (int i = 0; i < numSources; ++i) {
        float gap = linkHasComp[i] ? compGap : stubGap;
        if (linkHasComp[i]) {
            // Compound: block + top padding
            // If previous was compound, need extra space to clear its hanging text
            float extraTop = (i > 0 && linkHasComp[i - 1]) ? textHang : 0.0f;
            slotHeight[i]  = blockTopPad + blockHeight + extraTop + gap;
        } else {
            // Simple: stub + clearance from above
            float extraTop = (i > 0 && linkHasComp[i - 1]) ? textHang : 0.0f;
            slotHeight[i]  = stubHeight + extraTop + gap;
        }
    }
    // Last slot's hanging text (if compound) needs space at bottom
    float bottomExtra = linkHasComp[numSources - 1] ? textHang : 0.0f;
    float bottomGap   = linkHasComp[numSources - 1] ? compGap : stubGap;

    // Calculate total AND height
    float andHeight = bottomExtra + bottomGap;
    for (int i = 0; i < numSources; ++i) {
        andHeight += slotHeight[i];
    }
    andHeight = qMax(1.5f, andHeight);

    // y parameter is the CENTER of allocated space
    float andCenterY = y;
    float andBottomY = y - andHeight / 2;
    float andTopY    = y + andHeight / 2;

    // Second pass: position ports from top down
    // Port Y = block/stub center
    float currentY = andTopY;
    for (int i = 0; i < numSources; ++i) {
        float gap = linkHasComp[i] ? compGap : stubGap;
        currentY -= gap; // Gap at top of slot

        // Clear text from previous compound row
        if (i > 0 && linkHasComp[i - 1]) {
            currentY -= textHang;
        }

        if (linkHasComp[i]) {
            currentY -= blockTopPad;     // Top padding
            currentY -= blockHeight / 2; // To block center
            linkPortY[i] = currentY;
            currentY -= blockHeight
                        / 2; // To block bottom (text hangs below, handled by next iteration)
        } else {
            currentY -= stubHeight / 2; // To stub center
            linkPortY[i] = currentY;
            currentY -= stubHeight / 2; // To stub bottom
        }
    }

    // X positions
    float linkCompX   = x;                               // Link components start position
    float andX        = anyLinkHasComp ? (x + 2.5f) : x; // AND gate X position
    float targetCompX = andX + 2.0f;                     // Target component X position
    float outX        = hasTargetComp ? (targetCompX + 2.5f) : (andX + 2.5f); // Output X position

    // Store AND gate input connection points
    QVector<QString> andInputPorts(numSources);

    // Step 1: Draw link-level components (before AND gate)
    // Link component Y-center aligns with AND port Y
    for (int i = 0; i < numSources; ++i) {
        const ResetLink &link    = target.links[i];
        float            portY   = linkPortY[i];            // Y center for this link
        float            compY   = portY - blockHeight / 2; // Block bottom-left Y
        QString          srcName = link.source;

        if (linkHasComp[i]) {
            QString compId, clock, label2;
            QString fillColor;

            if (linkCompType[i] == QStringLiteral("async")) {
                compId = escapeTypstId(
                    tid + QStringLiteral("_L") + QString::number(i) + QStringLiteral("_ASYNC"));
                clock     = link.async.clock;
                label2    = QStringLiteral("stage:%1").arg(link.async.stage);
                fillColor = QStringLiteral("util.colors.blue");
            } else if (linkCompType[i] == QStringLiteral("sync")) {
                compId = escapeTypstId(
                    tid + QStringLiteral("_L") + QString::number(i) + QStringLiteral("_SYNC"));
                clock     = link.sync.clock;
                label2    = QStringLiteral("stage:%1").arg(link.sync.stage);
                fillColor = QStringLiteral("util.colors.yellow");
            } else if (linkCompType[i] == QStringLiteral("count")) {
                compId = escapeTypstId(
                    tid + QStringLiteral("_L") + QString::number(i) + QStringLiteral("_COUNT"));
                clock     = link.count.clock;
                label2    = QStringLiteral("cycle:%1").arg(link.count.cycle);
                fillColor = QStringLiteral("util.colors.orange");
            }

            // Draw component block (Y is bottom-left corner in circuiteria)
            s << "  element.block(\n";
            s << "    x: " << linkCompX << ", y: " << compY << ", w: 1.5, h: " << blockHeight
              << ",\n";
            s << "    id: \"" << compId << "\", name: \"" << linkCompType[i].toUpper()
              << "\", fill: " << fillColor << ",\n";
            s << "    ports: (west: ((id: \"in\"),), east: ((id: \"out\"),))\n";
            s << "  )\n";

            // Draw labels below component
            s << "  draw.content((" << (linkCompX + 0.75f) << ", " << (compY - 0.25f)
              << "), text(size: 5pt)[" << clock << "])\n";
            s << "  draw.content((" << (linkCompX + 0.75f) << ", " << (compY - 0.55f)
              << "), text(size: 5pt)[" << label2 << "])\n";

            // Draw input stub to component
            s << "  wire.stub(\"" << compId << "-port-in\", \"west\", name: \"" << srcName
              << "\")\n";

            // Store output port as AND input
            andInputPorts[i] = compId + QStringLiteral("-port-out");
        } else {
            // No link component - will connect directly to AND gate with stub
            andInputPorts[i] = QString(); // Mark as direct connection
        }
    }

    // Step 2: Draw AND gate (or direct connection for single source without components)
    // Note: It's AND gate because all reset signals are active-low (assert all to reset)
    QString andOutputPort;

    if (numSources == 1 && !anyLinkHasComp && !hasTargetComp) {
        QString sid = escapeTypstId(tid + QStringLiteral("_SRC"));
        // Right-pointing triangle (42° tip angle), sized to match output arrow
        float triWidth = 0.38f;
        float triHalfH = 0.16f;
        float triBaseX = andX;
        float triTipX  = triBaseX + triWidth;
        float triY     = andCenterY;
        s << "  draw.line((" << triBaseX << ", " << (triY + triHalfH) << "), (" << triTipX << ", "
          << triY << "), (" << triBaseX << ", " << (triY - triHalfH)
          << "), close: true, fill: black, stroke: none)\n";
        s << "  draw.content((" << (triBaseX - 0.1f) << ", " << triY
          << "), anchor: \"east\", text(size: 8pt)[" << target.links[0].source << "])\n";
        // Tiny invisible anchor: position so east port aligns with triangle tip
        float anchorS = 0.01f;
        s << "  element.block(x: " << (triTipX - anchorS) << ", y: " << (triY - anchorS / 2)
          << ", w: " << anchorS << ", h: " << anchorS << ", id: \"" << sid
          << "\", name: \"\", stroke: none, fill: none, ports: (east: ((id: \"out\"),)))\n";
        andOutputPort = sid + QStringLiteral("-port-out");
    } else {
        // Use AND gate - height accommodates all link components
        // AND gate bottom at andBottomY, so it's centered at andCenterY
        QString andId = escapeTypstId(tid + QStringLiteral("_AND"));
        s << "  element.block(\n";
        s << "    x: " << andX << ", y: " << andBottomY << ", w: 1.2, h: " << andHeight << ",\n";
        s << "    id: \"" << andId << "\", name: \"AND\", fill: util.colors.green,\n";

        // Define ports with explicit positions to align with link components
        s << "    ports: (west: (";
        for (int i = 0; i < numSources; ++i) {
            if (i > 0)
                s << ", ";
            // Calculate port position as ratio within AND gate height (from bottom)
            float portRatio = (linkPortY[i] - andBottomY) / andHeight;
            s << "(id: \"in" << i << "\", pos: " << portRatio << ")";
        }
        s << ",), east: ((id: \"out\"),))\n";
        s << "  )\n";

        // Connect inputs to AND gate (with inversion bubble for high-active sources)
        for (int i = 0; i < numSources; ++i) {
            QString andInPort = andId + QStringLiteral("-port-in") + QString::number(i);
            float   portY     = linkPortY[i];

            // Draw inversion bubble at AND input for high-active sources
            if (linkNeedsInvert[i]) {
                float bubbleX = andX - 0.15f; // Just before AND gate west edge
                s << "  draw.circle((" << bubbleX << ", " << portY << "), radius: 0.1, "
                  << "stroke: black, fill: white)\n";
            }

            if (andInputPorts[i].isEmpty()) {
                // Direct connection - draw stub
                s << "  wire.stub(\"" << andInPort << "\", \"west\", name: \""
                  << target.links[i].source << "\")\n";
            } else {
                // Connect from link component output to AND input
                // If bubble exists, wire ends at bubble, otherwise at AND port
                if (linkNeedsInvert[i]) {
                    float bubbleX = andX - 0.15f;
                    s << "  draw.line(\"" << andInputPorts[i] << "\", (" << (bubbleX - 0.1f) << ", "
                      << portY << "))\n";
                } else {
                    s << "  wire.wire(\"w_" << tid << "_l" << i << "_to_and\", (\n";
                    s << "    \"" << andInputPorts[i] << "\", \"" << andInPort << "\"\n";
                    s << "  ))\n";
                }
            }
        }

        andOutputPort = andId + QStringLiteral("-port-out");
    }

    // Step 3: Draw target-level component (Post-AND ARSR, after AND gate)
    // Target component Y-center aligns with output port Y (which is andCenterY)
    QString finalOutputPort = andOutputPort;

    if (hasTargetComp) {
        float   compY = andCenterY - targetCompH / 2; // Center-aligned with output
        QString compId, clock, label2;
        QString fillColor;

        if (targetCompType == QStringLiteral("async")) {
            compId    = escapeTypstId(tid + QStringLiteral("_ASYNC"));
            clock     = target.async.clock;
            label2    = QStringLiteral("stage:%1").arg(target.async.stage);
            fillColor = QStringLiteral("util.colors.blue");
        } else if (targetCompType == QStringLiteral("sync")) {
            compId    = escapeTypstId(tid + QStringLiteral("_SYNC"));
            clock     = target.sync.clock;
            label2    = QStringLiteral("stage:%1").arg(target.sync.stage);
            fillColor = QStringLiteral("util.colors.yellow");
        } else if (targetCompType == QStringLiteral("count")) {
            compId    = escapeTypstId(tid + QStringLiteral("_COUNT"));
            clock     = target.count.clock;
            label2    = QStringLiteral("cycle:%1").arg(target.count.cycle);
            fillColor = QStringLiteral("util.colors.orange");
        }

        // Draw component block
        s << "  element.block(\n";
        s << "    x: " << targetCompX << ", y: " << compY << ", w: 1.5, h: " << targetCompH
          << ",\n";
        s << "    id: \"" << compId << "\", name: \"" << targetCompType.toUpper()
          << "\", fill: " << fillColor << ",\n";
        s << "    ports: (west: ((id: \"in\"),), east: ((id: \"out\"),))\n";
        s << "  )\n";

        // Draw labels below component
        s << "  draw.content((" << (targetCompX + 0.75f) << ", " << (compY - 0.3f)
          << "), text(size: 6pt)[" << clock << "])\n";
        s << "  draw.content((" << (targetCompX + 0.75f) << ", " << (compY - 0.7f)
          << "), text(size: 6pt)[" << label2 << "])\n";

        // Wire from AND to target component
        s << "  wire.wire(\"w_" << tid << "_and_to_comp\", (\n";
        s << "    \"" << andOutputPort << "\", \"" << compId << "-port-in\"\n";
        s << "  ))\n";

        finalOutputPort = compId + QStringLiteral("-port-out");
    }

    // Step 4: Draw output port as arrow with label
    // Output port Y-center aligns with POST ASYNC Y-center (both at andCenterY)
    float arrowStartX = outX - 0.3f;
    float arrowEndX   = outX + 0.3f;

    // Draw arrow line from component to arrow tip
    s << "  draw.line(";
    if (hasTargetComp) {
        s << "\"" << finalOutputPort << "\", ";
    } else {
        s << "\"" << andOutputPort << "\", ";
    }
    s << "(" << arrowEndX << ", " << andCenterY << "), mark: (end: \">\", fill: black))\n";

    // Draw label to the right of arrow
    s << "  draw.content((" << (arrowEndX + 0.3f) << ", " << andCenterY << "), "
      << "anchor: \"west\", [" << target.name << "])\n\n";

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

    // Build sourceIsHighActive map for polarity indication
    QMap<QString, bool> sourceIsHighActive;
    for (const ResetSource &src : config.sources) {
        sourceIsHighActive[src.name] = (src.active == QStringLiteral("high"));
    }

    // Generate header
    out << typstHeader();

    // Generate legend
    out << typstLegend();

    // Generate reset source table (two-column layout)
    float bottomY = -5.0f;
    out << typstSourceTable(config.sources, 0.0f, bottomY);

    // Generate targets (vertical stacking with dynamic spacing)
    // Use same height calculation as typstTarget to determine spacing
    const float x0          = 0.0f;
    const float blockHeight = 1.0f;
    const float blockTopPad = 0.3f;
    const float textHang    = 1.2f; // Match typstTarget
    const float stubHeight  = 0.5f;
    const float compGap     = 1.5f; // Match typstTarget
    const float stubGap     = 0.8f; // Match typstTarget
    const float extraMargin = 2.0f; // Extra margin between targets

    float currentY = bottomY - 3.0f;

    for (int idx = 0; idx < config.targets.size(); ++idx) {
        const ResetTarget &target = config.targets[idx];

        // Calculate this target's AND height using same logic as typstTarget
        int  numLinks       = target.links.size();
        bool hasTargetAsync = !target.async.clock.isEmpty() || !target.sync.clock.isEmpty()
                              || !target.count.clock.isEmpty();

        // Analyze which links have components
        QVector<bool> linkHasComp(numLinks, false);
        for (int i = 0; i < numLinks; ++i) {
            const ResetLink &link = target.links[i];
            if (!link.async.clock.isEmpty() || !link.sync.clock.isEmpty()
                || !link.count.clock.isEmpty()) {
                linkHasComp[i] = true;
            }
        }

        // Calculate slot heights with appropriate gaps
        float firstGap     = (numLinks > 0 && linkHasComp[0]) ? compGap : stubGap;
        float targetHeight = firstGap; // Top margin
        for (int i = 0; i < numLinks; ++i) {
            float gap      = linkHasComp[i] ? compGap : stubGap;
            float extraTop = (i > 0 && linkHasComp[i - 1]) ? textHang : 0.0f;
            if (linkHasComp[i]) {
                targetHeight += blockTopPad + blockHeight + extraTop + gap;
            } else {
                targetHeight += stubHeight + extraTop + gap;
            }
        }
        // Bottom extra for last compound
        if (numLinks > 0 && linkHasComp[numLinks - 1]) {
            targetHeight += textHang;
        }
        targetHeight = qMax(1.5f, targetHeight);

        // Position target center, then move down for next target
        float targetCenterY = currentY - targetHeight / 2;
        out << typstTarget(target, sourceIsHighActive, x0, targetCenterY);

        // Move to next target position
        currentY = targetCenterY - targetHeight / 2 - extraMargin;
    }

    // Close circuit
    out << "})\n";

    file.close();
    qInfo() << "Generated Typst reset diagram:" << outputPath;
    return true;
}
