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

    if (config.sources.isEmpty() || config.targets.isEmpty()) {
        qWarning() << "Reset configuration must have at least one source and target";
        return false;
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

    // Parse sources (new format: source: {name: {polarity: ...}})
    if (resetNode["source"] && resetNode["source"].IsMap()) {
        for (auto it = resetNode["source"].begin(); it != resetNode["source"].end(); ++it) {
            ResetSource source;
            source.name = QString::fromStdString(it->first.as<std::string>());

            if (it->second.IsMap() && it->second["polarity"]) {
                QString polarity = QString::fromStdString(it->second["polarity"].as<std::string>());
                source.active    = (polarity == "low") ? "low" : "high";
            } else if (it->second.IsScalar()) {
                QString polarity = QString::fromStdString(it->second.as<std::string>());
                source.active    = (polarity == "low") ? "low" : "high";
            } else {
                source.active = "high"; // default
            }

            source.comment = it->second.IsMap() && it->second["comment"]
                                 ? QString::fromStdString(it->second["comment"].as<std::string>())
                                 : "";

            config.sources.append(source);
        }
    }

    // Parse targets and their links (new format)
    if (resetNode["target"] && resetNode["target"].IsMap()) {
        for (auto tgtIt = resetNode["target"].begin(); tgtIt != resetNode["target"].end(); ++tgtIt) {
            const YAML::Node &tgtNode = tgtIt->second;
            if (!tgtNode.IsMap())
                continue;

            ResetTarget target;
            target.name = QString::fromStdString(tgtIt->first.as<std::string>());

            // Parse target polarity
            if (tgtNode["polarity"]) {
                QString polarity = QString::fromStdString(tgtNode["polarity"].as<std::string>());
                target.active    = (polarity == "low") ? "low" : "high";
            } else {
                target.active = "low"; // default for targets
            }

            target.comment = tgtNode["comment"]
                                 ? QString::fromStdString(tgtNode["comment"].as<std::string>())
                                 : "";

            // Parse links for this target
            if (tgtNode["link"] && tgtNode["link"].IsMap()) {
                for (auto linkIt = tgtNode["link"].begin(); linkIt != tgtNode["link"].end();
                     ++linkIt) {
                    const YAML::Node &linkNode = linkIt->second;
                    if (!linkNode.IsMap() || !linkNode["type"])
                        continue;

                    ResetConnection connection;
                    connection.sourceName = QString::fromStdString(linkIt->first.as<std::string>());
                    connection.targetName = target.name;
                    connection.clock      = config.clock; // Use controller clock

                    // Parse type
                    QString typeStr = QString::fromStdString(linkNode["type"].as<std::string>());
                    connection.type = parseResetType(typeStr);

                    // Parse parameters based on type
                    connection.sync_depth     = linkNode["sync_depth"]
                                                    ? linkNode["sync_depth"].as<int>()
                                                    : 0;
                    connection.counter_width  = linkNode["counter_width"]
                                                    ? linkNode["counter_width"].as<int>()
                                                    : 0;
                    connection.timeout_cycles = linkNode["timeout_cycles"]
                                                    ? linkNode["timeout_cycles"].as<int>()
                                                    : 0;
                    connection.pipe_depth     = linkNode["pipe_depth"]
                                                    ? linkNode["pipe_depth"].as<int>()
                                                    : 0;

                    config.connections.append(connection);
                    target.sources.append(connection.sourceName); // Track source for target
                }
            }

            config.targets.append(target);
        }
    }

    // Parse reset reason recording configuration (simplified)
    config.reason.enabled = false;
    if (resetNode["reason"] && resetNode["reason"].IsMap()) {
        const YAML::Node &reasonNode = resetNode["reason"];
        if (reasonNode["enable"] && reasonNode["enable"].as<bool>()) {
            config.reason.enabled = true;

            // Required: always-on clock
            config.reason.aonClock = reasonNode["register_clock"]
                                         ? QString::fromStdString(
                                               reasonNode["register_clock"].as<std::string>())
                                         : "clk_32k";

            // Output bus name (support legacy naming with automatic mapping)
            if (reasonNode["output_bus"]) {
                QString outputBus = QString::fromStdString(
                    reasonNode["output_bus"].as<std::string>());
                if (outputBus == "reset_reason_bits") {
                    config.reason.outputBus = "reason"; // Auto-map legacy naming
                } else {
                    config.reason.outputBus = outputBus;
                }
            } else {
                config.reason.outputBus = "reason"; // New unified default
            }

            // Valid signal name
            config.reason.validSignal = reasonNode["valid_signal"]
                                            ? QString::fromStdString(
                                                  reasonNode["valid_signal"].as<std::string>())
                                            : "reason_valid";

            // Software clear signal
            config.reason.clearSignal = reasonNode["clear_signal"]
                                            ? QString::fromStdString(
                                                  reasonNode["clear_signal"].as<std::string>())
                                            : "reason_clear";

            // Auto-detect POR signal (first active-low source or default)
            config.reason.porSignal = "por_rst_n"; // Default
            for (const auto &source : config.sources) {
                if (source.name.contains("por", Qt::CaseInsensitive) && source.active == "low") {
                    config.reason.porSignal = source.name;
                    break;
                }
            }

            // Build source order (exclude POR,按source声明顺序)
            config.reason.sourceOrder.clear();
            for (const auto &source : config.sources) {
                if (source.name != config.reason.porSignal) {
                    config.reason.sourceOrder.append(source.name);
                }
            }

            // Calculate bit vector width
            config.reason.vectorWidth = config.reason.sourceOrder.size();
            if (config.reason.vectorWidth == 0)
                config.reason.vectorWidth = 1; // Minimum 1 bit
        }
    }

    return config;
}

void QSocResetPrimitive::generateModuleHeader(const ResetControllerConfig &config, QTextStream &out)
{
    out << "\nmodule " << config.moduleName << " (\n";

    // Clock inputs
    out << "    /* Reset clocks */\n";
    out << "    input        " << config.clock << ",            /**< System clock input */\n";

    // Add reset reason clock if enabled
    if (config.reason.enabled && !config.reason.aonClock.isEmpty()
        && config.reason.aonClock != config.clock) {
        out << "    input        " << config.reason.aonClock
            << ",            /**< Always-on clock for reason recording */\n";
    }

    // Reset sources
    out << "    /* Reset source */\n";
    for (const auto &source : config.sources) {
        QString comment = source.comment.isEmpty() ? QString("Reset source: %1").arg(source.name)
                                                   : source.comment;
        out << "    input  " << source.name << ",                 /**< " << comment;
        if (source.active == "low") {
            out << " (active low)";
        }
        out << " */\n";
    }

    // Reset targets
    out << "    /* Reset target */\n";
    for (int i = 0; i < config.targets.size(); ++i) {
        const auto &target = config.targets[i];
        QString comment    = target.comment.isEmpty() ? QString("Reset target: %1").arg(target.name)
                                                      : target.comment;
        out << "    output " << target.name << ",             /**< " << comment;
        if (target.active == "low") {
            out << " (active low)";
        }
        out << " */\n";
    }

    // Reset reason output (if enabled)
    if (config.reason.enabled) {
        out << "    /* Reset reason recording */\n";
        if (config.reason.vectorWidth > 1) {
            out << "    output [" << (config.reason.vectorWidth - 1) << ":0] "
                << config.reason.outputBus
                << ",  /**< Reset reason bit vector (per-source sticky flags) */\n";
        } else {
            out << "    output " << config.reason.outputBus
                << ",                    /**< Reset reason bit vector (single sticky flag) */\n";
        }

        // Add valid signal output
        out << "    output " << config.reason.validSignal
            << ",          /**< Reset reason valid flag (indicates reason output is meaningful) "
               "*/\n";
    }

    // Control signals
    out << "    /* Control signals */\n";
    out << "    input        " << config.testEnable << ",            /**< Test enable signal */\n";

    // Add reset reason clear signal if enabled and specified
    if (config.reason.enabled && !config.reason.clearSignal.isEmpty()) {
        out << "    input        " << config.reason.clearSignal
            << "        /**< Software clear signal for reset reason */\n";
    }

    out << ");\n\n";
}

void QSocResetPrimitive::generateWireDeclarations(
    const ResetControllerConfig &config, QTextStream &out)
{
    // Generate connection wires
    out << "    /* Wires for reset connections */\n";
    for (const auto &connection : config.connections) {
        QString wireName = getConnectionWireName(connection.sourceName, connection.targetName);
        out << "    wire " << wireName << ";\n";
    }
    out << "\n";
}

void QSocResetPrimitive::generateResetLogic(const ResetControllerConfig &config, QTextStream &out)
{
    out << "    /* Reset logic instances */\n";

    for (const auto &connection : config.connections) {
        generateResetInstance(connection, config, out);
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

    // Generate event normalization (convert all to LOW-active)
    out << "    /* Event normalization: convert all sources to LOW-active format */\n";
    for (int i = 0; i < config.reason.sourceOrder.size(); ++i) {
        const QString &sourceName = config.reason.sourceOrder[i];
        QString        eventName  = QString("%1_event_n").arg(sourceName);

        // Find source polarity
        QString sourceActive = "low"; // Default
        for (const auto &source : config.sources) {
            if (source.name == sourceName) {
                sourceActive = source.active;
                break;
            }
        }

        out << "    wire " << eventName << " = ";
        if (sourceActive == "high") {
            out << "~" << sourceName << ";  // Convert HIGH-active to LOW-active\n";
        } else {
            out << sourceName << ";   // Already LOW-active\n";
        }
    }
    out << "\n";

    // Generate SW clear synchronizer and pulse generator
    if (!config.reason.clearSignal.isEmpty()) {
        out << "    /* Synchronize software clear and generate pulse */\n";
        out << "    reg swc_d1, swc_d2, swc_d3;\n";
        out << "    always @(posedge " << config.reason.aonClock << " or negedge "
            << config.reason.porSignal << ") begin\n";
        out << "        if (!" << config.reason.porSignal << ") begin\n";
        out << "            swc_d1 <= 1'b0;\n";
        out << "            swc_d2 <= 1'b0;\n";
        out << "            swc_d3 <= 1'b0;\n";
        out << "        end else begin\n";
        out << "            swc_d1 <= " << config.reason.clearSignal << ";\n";
        out << "            swc_d2 <= swc_d1;\n";
        out << "            swc_d3 <= swc_d2;\n";
        out << "        end\n";
        out << "    end\n";
        out << "    wire sw_clear_pulse = swc_d2 & ~swc_d3;  // Rising-edge pulse\n\n";
    }

    // Generate 2-cycle clear controller
    out << "    /* 2-cycle clear controller and valid signal generation */\n";
    out << "    reg        init_done;   // Set after first post-POR action\n";
    out << "    reg [1:0]  clr_sr;      // 2-cycle clear shift register\n";
    out << "    reg        valid_q;     // " << config.reason.validSignal << " register\n\n";

    out << "    wire clr_en = |clr_sr;  // Clear enable (any bit in shift register)\n\n";

    out << "    always @(posedge " << config.reason.aonClock << " or negedge "
        << config.reason.porSignal << ") begin\n";
    out << "        if (!" << config.reason.porSignal << ") begin\n";
    out << "            init_done <= 1'b0;\n";
    out << "            clr_sr    <= 2'b00;\n";
    out << "            valid_q   <= 1'b0;\n";
    out << "        end else begin\n";
    out << "            // Start clear after POR release (first clock)\n";
    out << "            if (!init_done) begin\n";
    out << "                init_done <= 1'b1;\n";
    out << "                clr_sr    <= 2'b11;  // Fixed two-cycle clear window\n";
    out << "                valid_q   <= 1'b0;\n";

    if (!config.reason.clearSignal.isEmpty()) {
        out << "            // SW clear retriggers the same two-cycle clear\n";
        out << "            end else if (sw_clear_pulse) begin\n";
        out << "                clr_sr  <= 2'b11;\n";
        out << "                valid_q <= 1'b0;\n";
    }

    out << "            // Shift down the clear window\n";
    out << "            end else if (clr_en) begin\n";
    out << "                clr_sr <= {1'b0, clr_sr[1]};\n";
    out << "            // Declare valid once clear window is over\n";
    out << "            end else begin\n";
    out << "                valid_q <= 1'b1;\n";
    out << "            end\n";
    out << "        end\n";
    out << "    end\n\n";

    // Generate sticky flags with pure async-set + sync-clear
    out << "    /* Sticky flags: async-set on event, sync-clear during clear window */\n";
    out << "    reg [" << (config.reason.vectorWidth - 1) << ":0] flags;\n\n";

    // Generate per-flag logic
    for (int i = 0; i < config.reason.sourceOrder.size(); ++i) {
        const QString &sourceName = config.reason.sourceOrder[i];
        QString        eventName  = QString("%1_event_n").arg(sourceName);

        out << "    // Bit[" << i << "]: " << sourceName << " sticky flag\n";
        out << "    always @(posedge " << config.reason.aonClock << " or negedge " << eventName
            << ") begin\n";
        out << "        if (!" << eventName << ") begin\n";
        out << "            flags[" << i << "] <= 1'b1;      // Async set on event assert\n";
        out << "        end else if (clr_en) begin\n";
        out << "            flags[" << i << "] <= 1'b0;      // Sync clear during clear window\n";
        out << "        end else begin\n";
        out << "            flags[" << i << "] <= flags[" << i << "];  // Hold state\n";
        out << "        end\n";
        out << "    end\n\n";
    }

    // Generate gated outputs
    out << "    /* Output gating: zeros until valid */\n";
    out << "    assign " << config.reason.validSignal << " = valid_q;\n";
    out << "    assign " << config.reason.outputBus << " = " << config.reason.validSignal
        << " ? flags : " << config.reason.vectorWidth << "'b0;\n\n";
}

void QSocResetPrimitive::generateOutputAssignments(
    const ResetControllerConfig &config, QTextStream &out)
{
    out << "    /* Reset output assignments */\n";

    for (const auto &target : config.targets) {
        out << "    /* Reset target: " << target.name;
        if (target.active == "low") {
            out << " (active low)";
        }
        out << " */\n";

        out << "    assign " << target.name << " = ";

        if (target.active == "low") {
            // Active low - AND all normalized signals
            out << "& {\n";

            QStringList wireNames;
            for (const auto &connection : config.connections) {
                if (connection.targetName == target.name) {
                    wireNames.append(
                        getConnectionWireName(connection.sourceName, connection.targetName));
                }
            }

            for (int i = 0; i < wireNames.size(); ++i) {
                out << "        " << wireNames[i];
                if (i < wireNames.size() - 1) {
                    out << ",";
                }
                out << "\n";
            }
            out << "    };\n";
        } else {
            // Active high - OR all normalized signals
            out << "| {\n";

            QStringList wireNames;
            for (const auto &connection : config.connections) {
                if (connection.targetName == target.name) {
                    wireNames.append(
                        getConnectionWireName(connection.sourceName, connection.targetName));
                }
            }

            for (int i = 0; i < wireNames.size(); ++i) {
                out << "        " << wireNames[i];
                if (i < wireNames.size() - 1) {
                    out << ",";
                }
                out << "\n";
            }
            out << "    };\n";
        }
    }
}

void QSocResetPrimitive::generateResetInstance(
    const ResetConnection &connection, const ResetControllerConfig &config, QTextStream &out)
{
    QString wireName     = getConnectionWireName(connection.sourceName, connection.targetName);
    QString instanceName = getInstanceName(connection, config);

    out << "    /*\n";
    out << "     * " << connection.sourceName << " -> " << connection.targetName << ": ";

    switch (connection.type) {
    case ASYNC_COMB: {
        out << "ASYNC_COMB: Async reset Async release combinational logic\n";
        out << "     * (Legacy A: combinational logic)\n";
        out << "     */\n";

        // Determine if source needs inversion
        QString sourceSignal = connection.sourceName;
        // Assume sources ending with _n are active low, others are active high
        if (connection.sourceName.endsWith("_n")) {
            sourceSignal = connection.sourceName; // Keep as-is for active low
        } else {
            sourceSignal
                = "~" + connection.sourceName; // Invert for active high to get active low output
        }

        out << "    assign " << wireName << " = " << sourceSignal << ";\n";
        break;
    }

    case ASYNC_SYNC: {
        out << "ASYNC_SYNC: Async reset sync release\n";
        out << "     * (Legacy A(" << connection.sync_depth << "," << connection.clock << ")\n";
        out << "     * with sync_depth=" << connection.sync_depth << ", clock=" << connection.clock
            << ")\n";
        out << "     */\n";

        // Generate classic N-stage synchronizer
        out << "    reg [" << (connection.sync_depth - 1) << ":0] " << instanceName << "_ff;\n";
        out << "    always @(posedge " << connection.clock;
        if (connection.sourceName.endsWith("_n")) {
            out << " or negedge " << connection.sourceName << ") begin\n";
            out << "        if (!" << connection.sourceName << ")\n";
        } else {
            out << " or posedge " << connection.sourceName << ") begin\n";
            out << "        if (" << connection.sourceName << ")\n";
        }
        out << "            " << instanceName << "_ff <= " << connection.sync_depth << "'b0;\n";
        out << "        else\n";
        out << "            " << instanceName << "_ff <= {" << instanceName << "_ff["
            << (connection.sync_depth - 2) << ":0], 1'b1};\n";
        out << "    end\n";
        out << "    assign " << wireName << " = test_en ? ";
        if (connection.sourceName.endsWith("_n")) {
            out << connection.sourceName;
        } else {
            out << "~" << connection.sourceName;
        }
        out << " : " << instanceName << "_ff[" << (connection.sync_depth - 1) << "];\n";
        break;
    }

    case ASYNC_CNT: {
        out << "ASYNC_CNT: Async reset counter timeout release\n";
        out << "     * (Legacy AC(" << connection.sync_depth << "," << connection.counter_width
            << "," << connection.timeout_cycles << "," << connection.clock << "))\n";
        out << "     * with sync_depth=" << connection.sync_depth
            << ", width=" << connection.counter_width << ", timeout=" << connection.timeout_cycles
            << ", clock=" << connection.clock << ")\n";
        out << "     */\n";

        // Generate DFF-based counter implementation
        out << "    reg [" << (connection.counter_width - 1) << ":0] " << instanceName
            << "_counter;\n";
        out << "    reg " << instanceName << "_counting;\n";
        out << "    always @(posedge " << connection.clock;
        if (connection.sourceName.endsWith("_n")) {
            out << " or negedge " << connection.sourceName << ") begin\n";
            out << "        if (!" << connection.sourceName << ") begin\n";
        } else {
            out << " or posedge " << connection.sourceName << ") begin\n";
            out << "        if (" << connection.sourceName << ") begin\n";
        }
        out << "            " << instanceName << "_counter <= " << connection.counter_width
            << "'b0;\n";
        out << "            " << instanceName << "_counting <= 1'b1;\n";
        out << "        end else if (" << instanceName << "_counting && " << instanceName
            << "_counter < " << connection.timeout_cycles << ") begin\n";
        out << "            " << instanceName << "_counter <= " << instanceName
            << "_counter + 1'b1;\n";
        out << "        end else if (" << instanceName
            << "_counter >= " << connection.timeout_cycles << ") begin\n";
        out << "            " << instanceName << "_counting <= 1'b0;\n";
        out << "        end\n";
        out << "    end\n";
        out << "    assign " << wireName << " = test_en ? ";
        if (connection.sourceName.endsWith("_n")) {
            out << connection.sourceName;
        } else {
            out << "~" << connection.sourceName;
        }
        out << " : (" << instanceName << "_counting ? 1'b0 : 1'b1);\n";
        break;
    }

    case SYNC_ONLY: {
        out << "SYNC_ONLY: Sync reset sync release\n";
        out << "     * (Legacy S(" << connection.sync_depth << "," << connection.clock << ")\n";
        out << "     * with sync_depth=" << connection.sync_depth << ", clock=" << connection.clock
            << ")\n";
        out << "     */\n";

        // Generate synchronous reset/release synchronizer
        out << "    reg [" << (connection.sync_depth - 1) << ":0] " << instanceName << "_ff;\n";
        out << "    always @(posedge " << connection.clock << ") begin\n";
        if (connection.sourceName.endsWith("_n")) {
            out << "        if (!" << connection.sourceName << ")\n";
        } else {
            out << "        if (" << connection.sourceName << ")\n";
        }
        out << "            " << instanceName << "_ff <= " << connection.sync_depth << "'b0;\n";
        out << "        else\n";
        out << "            " << instanceName << "_ff <= {" << instanceName << "_ff["
            << (connection.sync_depth - 2) << ":0], 1'b1};\n";
        out << "    end\n";
        out << "    assign " << wireName << " = test_en ? ";
        if (connection.sourceName.endsWith("_n")) {
            out << connection.sourceName;
        } else {
            out << "~" << connection.sourceName;
        }
        out << " : " << instanceName << "_ff[" << (connection.sync_depth - 1) << "];\n";
        break;
    }

    case ASYNC_SYNCNT: {
        out << "ASYNC_SYNCNT: Async reset with sync-then-count release\n";
        out << "     * (Legacy AS(" << connection.sync_depth << "," << connection.counter_width
            << "," << connection.timeout_cycles << "," << connection.clock << ")\n";
        out << "     * Two-stage: sync_depth=" << connection.sync_depth
            << ", count_width=" << connection.counter_width
            << ", timeout=" << connection.timeout_cycles << ", clock=" << connection.clock << ")\n";
        out << "     */\n";

        // Generate intermediate wire names
        QString syncWireName = wireName + "_stage1";

        // Stage 1: Sync release synchronizer
        out << "    /* Stage 1: Sync release */\n";
        out << "    reg [" << (connection.sync_depth - 1) << ":0] " << instanceName
            << "_sync_ff;\n";
        out << "    always @(posedge " << connection.clock;
        if (connection.sourceName.endsWith("_n")) {
            out << " or negedge " << connection.sourceName << ") begin\n";
            out << "        if (!" << connection.sourceName << ")\n";
        } else {
            out << " or posedge " << connection.sourceName << ") begin\n";
            out << "        if (" << connection.sourceName << ")\n";
        }
        out << "            " << instanceName << "_sync_ff <= " << connection.sync_depth
            << "'b0;\n";
        out << "        else\n";
        out << "            " << instanceName << "_sync_ff <= {" << instanceName << "_sync_ff["
            << (connection.sync_depth - 2) << ":0], 1'b1};\n";
        out << "    end\n";
        out << "    wire " << syncWireName << " = " << instanceName << "_sync_ff["
            << (connection.sync_depth - 1) << "];\n\n";

        // Stage 2: Counter timeout
        out << "    /* Stage 2: Counter timeout */\n";
        out << "    reg [" << (connection.counter_width - 1) << ":0] " << instanceName
            << "_counter;\n";
        out << "    reg " << instanceName << "_counting;\n";
        out << "    always @(posedge " << connection.clock << " or negedge " << syncWireName
            << ") begin\n";
        out << "        if (!" << syncWireName << ") begin\n";
        out << "            " << instanceName << "_counter <= " << connection.counter_width
            << "'b0;\n";
        out << "            " << instanceName << "_counting <= 1'b1;\n";
        out << "        end else if (" << instanceName << "_counting && " << instanceName
            << "_counter < " << connection.timeout_cycles << ") begin\n";
        out << "            " << instanceName << "_counter <= " << instanceName
            << "_counter + 1'b1;\n";
        out << "        end else if (" << instanceName
            << "_counter >= " << connection.timeout_cycles << ") begin\n";
        out << "            " << instanceName << "_counting <= 1'b0;\n";
        out << "        end\n";
        out << "    end\n";

        // Output assignment with test bypass
        out << "    assign " << wireName << " = test_en ? ";
        if (connection.sourceName.endsWith("_n")) {
            out << connection.sourceName;
        } else {
            out << "~" << connection.sourceName;
        }
        out << " : (" << instanceName << "_counting ? 1'b0 : 1'b1);\n";
        break;
    }

    default: {
        out << "Unsupported reset type\n";
        out << "     */\n";
        out << "    // FIXME: Unsupported reset type for " << connection.sourceName << " -> "
            << connection.targetName << "\n";
        break;
    }
    }
}

QSocResetPrimitive::ResetType QSocResetPrimitive::parseResetType(const QString &typeStr)
{
    if (typeStr == "ASYNC_COMB") {
        return ASYNC_COMB;
    } else if (typeStr == "ASYNC_SYNC") {
        return ASYNC_SYNC;
    } else if (typeStr == "ASYNC_CNT") {
        return ASYNC_CNT;
    } else if (typeStr == "ASYNC_SYNCNT") {
        return ASYNC_SYNCNT;
    } else if (typeStr == "SYNC_ONLY") {
        return SYNC_ONLY;
    } else {
        qWarning() << "Unsupported reset type:" << typeStr << ", defaulting to ASYNC_COMB";
        return ASYNC_COMB;
    }
}

QString QSocResetPrimitive::getConnectionWireName(
    const QString &sourceName, const QString &targetName)
{
    // Use a shorter, more descriptive wire name
    return QString("%1_%2_sync").arg(sourceName).arg(targetName);
}

QString QSocResetPrimitive::getInstanceName(
    const ResetConnection &connection, const ResetControllerConfig &config)
{
    QString typePrefix;
    switch (connection.type) {
    case ASYNC_SYNC:
        typePrefix = "sync";
        break;
    case ASYNC_CNT:
        typePrefix = "cnt";
        break;
    case SYNC_ONLY:
        typePrefix = "sync_only";
        break;
    case ASYNC_SYNCNT:
        typePrefix = "syncnt";
        break;
    default:
        typePrefix = "logic";
        break;
    }

    // Simplified format: {type}_{source}_{target}
    return QString("%1_%2_%3").arg(typePrefix).arg(connection.sourceName).arg(connection.targetName);
}
