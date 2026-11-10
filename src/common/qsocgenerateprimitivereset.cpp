#include "qsocgenerateprimitivereset.h"
#include "common/qsoccelltext.h"
#include "common/qsocconsole.h"
#include "common/qsocprimitivereport.h"
#include "qsocgeneratemanager.h"
#include "qsocverilogutils.h"
#include <vector>

namespace {

/* Read a stage or cycle count; values below one are rejected, not clamped. */
bool readCount(
    const YAML::Node &node, const char *key, int fallback, const QString &where, int *value)
{
    *value = node[key] ? node[key].as<int>() : fallback;
    if (*value >= 1)
        return true;
    QSocConsole::error() << QString("%1 %2 must be at least 1, got %3").arg(where, key).arg(*value);
    return false;
}

} // namespace

QSocResetPrimitive::QSocResetPrimitive(QSocGenerateManager *parent)
    : m_parent(parent)
{}

bool QSocResetPrimitive::generateResetController(const YAML::Node &resetNode, QTextStream &out)
{
    if (!resetNode || !resetNode.IsMap()) {
        QSocConsole::warn() << "Invalid reset node provided";
        return false;
    }

    // Parse configuration
    ResetControllerConfig config = parseResetConfig(resetNode);

    if (!config.valid) {
        return false;
    }
    if (config.targets.isEmpty()) {
        QSocConsole::warn() << "Reset configuration must have at least one target";
        return false;
    }
    if (config.sources.isEmpty()) {
        /* Pre-fix the controller still emitted, falling back to
           `assign <target> = 1'b1;` for every target - the system would
           never actually reset. Refuse to generate so the user notices. */
        QSocConsole::warn() << "Reset controller" << config.name
                            << "has no `source:` declared; refusing to emit a controller "
                               "that ties every target inactive";
        return false;
    }

    /* By design, a link source that is neither a declared source nor
       another target gets auto-promoted to a fresh input port on the
       controller (so the parent can wire software-controlled signals
       like `rst_sw_dcmi_n`). Do NOT warn about undeclared sources here;
       the typo case surfaces downstream as an unwired controller pin. */

    out << generateControllerVerilog(config);

    if (m_parent && !m_parent->getDiagramDirectory().isEmpty()) {
        QSocPrimitiveReport::stage(
            m_parent->getDiagramDirectory(), config.moduleName, QSocPrimitiveReport::reset(config));
    }

    return true;
}

QString QSocResetPrimitive::generateControllerVerilog(const ResetControllerConfig &config)
{
    if (!config.valid || config.sources.isEmpty() || config.targets.isEmpty()) {
        return {};
    }
    QString     verilog;
    QTextStream out(&verilog);
    generateModuleHeader(config, out);
    generateWireDeclarations(config, out);
    generateResetLogic(config, out);

    if (config.reason.enabled) {
        generateResetReason(config, out);
    }

    generateOutputAssignments(config, out);

    out << "\nendmodule\n\n";

    out.flush();
    return verilog;
}

QSocResetPrimitive::ResetControllerConfig QSocResetPrimitive::parseResetConfig(
    const YAML::Node &resetNode)
{
    /* A malformed shape reaches yaml-cpp as an exception, and an uncaught one
       aborts the process instead of reporting the user's configuration. */
    try {
        return parseResetConfigUnguarded(resetNode);
    } catch (const YAML::Exception &error) {
        ResetControllerConfig config;
        config.valid = false;
        QSocConsole::error() << "Invalid reset configuration:" << error.what();
        return config;
    }
}

QSocResetPrimitive::ResetControllerConfig QSocResetPrimitive::parseResetConfigUnguarded(
    const YAML::Node &resetNode)
{
    ResetControllerConfig config;

    // Basic configuration
    if (!resetNode["name"]) {
        QSocConsole::error() << "'name' field is required in reset configuration";
        QSocConsole::err() << "Example: reset: { name: my_reset_ctrl, ... }" << "\n";
        config.valid = false;
        return config;
    }
    config.name       = QString::fromStdString(resetNode["name"].as<std::string>());
    config.moduleName = config.name; // Use same name for module

    // Test enable is optional - if not set, tie to 1'b0 internally
    if (resetNode["test_enable"]) {
        config.testEnable = QString::fromStdString(resetNode["test_enable"].as<std::string>());
        /* The name becomes a module port. A constant would emit
           `input wire 1'b0`, and a tied 1'b1 would bypass reset for good. */
        if (!config.testEnable.isEmpty()
            && !QSocVerilogUtils::isValidVerilogIdentifier(config.testEnable)) {
            QSocConsole::error() << "Reset controller" << config.name << "test_enable"
                                 << config.testEnable << "must be a valid Verilog identifier";
            config.valid = false;
        }
    }

    // Parse sources (source: {name: {polarity: ...}})
    if (resetNode["source"] && resetNode["source"].IsMap()) {
        for (auto it = resetNode["source"].begin(); it != resetNode["source"].end(); ++it) {
            ResetSource   source;
            const QString rawName = QString::fromStdString(it->first.as<std::string>());
            /* Bracket characters in a source name leak into wire/instance/
               port identifiers and produce illegal Verilog. Sanitize and
               warn so the user knows to use plain identifier names. */
            source.name = QSocVerilogUtils::sanitizeBitSelectInName(rawName);
            if (source.name != rawName) {
                QSocConsole::warn() << "Reset source name" << rawName
                                    << "contains bracket characters; sanitized to" << source.name;
            }

            if (it->second.IsMap() && it->second["active"]) {
                source.active = QString::fromStdString(it->second["active"].as<std::string>());
            } else {
                QSocConsole::error()
                    << "'active' field is required for source '" << source.name << "'";
                QSocConsole::err()
                    << "Please specify active level explicitly: 'high' or 'low'" << "\n";
                QSocConsole::error() << "Example: source: { " << source.name << ": {active: low} }";
                config.valid = false;
                return config;
            }

            config.sources.append(source);
        }
    }

    // Parse targets with component-based configuration
    if (resetNode["target"] && resetNode["target"].IsMap()) {
        for (auto tgtIt = resetNode["target"].begin(); tgtIt != resetNode["target"].end(); ++tgtIt) {
            const YAML::Node tgtNode = tgtIt->second;
            if (!tgtNode.IsMap())
                continue;

            ResetTarget   target;
            const QString rawName = QString::fromStdString(tgtIt->first.as<std::string>());
            target.name           = QSocVerilogUtils::sanitizeBitSelectInName(rawName);
            if (target.name != rawName) {
                QSocConsole::warn() << "Reset target name" << rawName
                                    << "contains bracket characters; sanitized to" << target.name;
            }

            // Parse target active level
            if (tgtNode["active"]) {
                target.active = QString::fromStdString(tgtNode["active"].as<std::string>());
            } else {
                QSocConsole::error()
                    << "'active' field is required for target '" << target.name << "'";
                config.valid = false;
                return config;
            }

            // Parse target-level components
            if (tgtNode["async"]) {
                const YAML::Node &asyncNode = tgtNode["async"];
                if (!asyncNode["clock"]) {
                    QSocConsole::error()
                        << "'clock' field is required for async component in target '"
                        << target.name << "'";
                    config.valid = false;
                    return config;
                }
                target.async.clock = QString::fromStdString(asyncNode["clock"].as<std::string>());
                target.async.test_enable = config.testEnable; // Use controller-level test_enable
                if (!readCount(
                        asyncNode,
                        "stage",
                        3,
                        QString("Reset target '%1' async").arg(target.name),
                        &target.async.stage)) {
                    config.valid = false;
                    return config;
                }
            }

            if (tgtNode["sync"]) {
                const YAML::Node &syncNode = tgtNode["sync"];
                if (!syncNode["clock"]) {
                    QSocConsole::error()
                        << "'clock' field is required for sync component in target '" << target.name
                        << "'";
                    config.valid = false;
                    return config;
                }
                target.sync.clock = QString::fromStdString(syncNode["clock"].as<std::string>());
                target.sync.test_enable = config.testEnable; // Use controller-level test_enable
                if (!readCount(
                        syncNode,
                        "stage",
                        4,
                        QString("Reset target '%1' sync").arg(target.name),
                        &target.sync.stage)) {
                    config.valid = false;
                    return config;
                }
            }

            if (tgtNode["count"]) {
                const YAML::Node &countNode = tgtNode["count"];
                if (!countNode["clock"]) {
                    QSocConsole::error()
                        << "'clock' field is required for count component in target '"
                        << target.name << "'";
                    config.valid = false;
                    return config;
                }
                target.count.clock = QString::fromStdString(countNode["clock"].as<std::string>());
                target.count.test_enable = config.testEnable; // Use controller-level test_enable
                if (!readCount(
                        countNode,
                        "cycle",
                        16,
                        QString("Reset target '%1' count").arg(target.name),
                        &target.count.cycle)) {
                    config.valid = false;
                    return config;
                }
            }

            // Parse links for this target
            // Note: Use a copy of the link map to avoid yaml-cpp iterator corruption
            // when map contains null values
            YAML::Node linkMap = tgtNode["link"];
            if (linkMap && linkMap.IsMap()) {
                // First collect all keys to avoid iterator issues
                std::vector<std::string> linkKeys;
                for (auto it = linkMap.begin(); it != linkMap.end(); ++it) {
                    if (it->first.IsDefined()) {
                        linkKeys.push_back(it->first.as<std::string>());
                    }
                }

                // Now iterate using collected keys
                for (const auto &key : linkKeys) {
                    YAML::Node linkNode = linkMap[key];

                    ResetLink     link;
                    const QString rawSource = QString::fromStdString(key);
                    link.source             = QSocVerilogUtils::sanitizeBitSelectInName(rawSource);
                    if (link.source != rawSource) {
                        QSocConsole::warn()
                            << "Reset link source name" << rawSource
                            << "contains bracket characters; sanitized to" << link.source;
                    }

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
                            QSocConsole::error()
                                << "'clock' field is required for async component in link '"
                                << link.source << "' of target '" << target.name << "'";
                            config.valid = false;
                            return config;
                        }
                        link.async.clock = QString::fromStdString(
                            asyncNode["clock"].as<std::string>());
                        link.async.test_enable
                            = config.testEnable; // Use controller-level test_enable
                        if (!readCount(
                                asyncNode,
                                "stage",
                                3,
                                QString("Reset link '%1' of target '%2' async")
                                    .arg(link.source, target.name),
                                &link.async.stage)) {
                            config.valid = false;
                            return config;
                        }
                    }

                    if (linkNode["sync"]) {
                        const YAML::Node &syncNode = linkNode["sync"];
                        if (!syncNode["clock"]) {
                            QSocConsole::error()
                                << "'clock' field is required for sync component in link '"
                                << link.source << "' of target '" << target.name << "'";
                            config.valid = false;
                            return config;
                        }
                        link.sync.clock = QString::fromStdString(
                            syncNode["clock"].as<std::string>());
                        link.sync.test_enable = config.testEnable; // Use controller-level test_enable
                        if (!readCount(
                                syncNode,
                                "stage",
                                4,
                                QString("Reset link '%1' of target '%2' sync")
                                    .arg(link.source, target.name),
                                &link.sync.stage)) {
                            config.valid = false;
                            return config;
                        }
                    }

                    if (linkNode["count"]) {
                        const YAML::Node &countNode = linkNode["count"];
                        if (!countNode["clock"]) {
                            QSocConsole::error()
                                << "'clock' field is required for count component in link '"
                                << link.source << "' of target '" << target.name << "'";
                            config.valid = false;
                            return config;
                        }
                        link.count.clock = QString::fromStdString(
                            countNode["clock"].as<std::string>());
                        link.count.test_enable
                            = config.testEnable; // Use controller-level test_enable
                        if (!readCount(
                                countNode,
                                "cycle",
                                16,
                                QString("Reset link '%1' of target '%2' count")
                                    .arg(link.source, target.name),
                                &link.count.cycle)) {
                            config.valid = false;
                            return config;
                        }
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
                QSocConsole::error() << "Specified root_reset '" << config.reason.rootReset
                                     << "' not found in source list.";
                QSocConsole::err() << "Available sources:" << "\n";
                for (const auto &source : config.sources) {
                    QSocConsole::error()
                        << "  - " << source.name << " (active: " << source.active << ")";
                }
                config.valid = false;
                return config;
            }
        } else {
            QSocConsole::error() << "'root_reset' field is required in reason configuration.";
            QSocConsole::error()
                << "Please specify which source signal should be used as the root reset.";
            QSocConsole::err() << "Example: reason: { root_reset: por_rst_n, ... }" << "\n";
            config.valid = false;
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

    /* A target without a link was silently tied to its inactive level and
       never reset. Refuse while the author is present. */
    for (const auto &target : config.targets) {
        if (target.links.isEmpty()) {
            QSocConsole::error() << "Reset target" << target.name << "requires at least one link";
            config.valid = false;
        }
    }

    return config;
}

QList<QSocResetPrimitive::ResetPort> QSocResetPrimitive::describePorts(
    const ResetControllerConfig &config)
{
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

    // The reason recorder reads every declared source, linked or not
    if (config.reason.enabled) {
        for (const auto &source : config.sources) {
            if (!outputSignals.contains(source.name) && !sources.contains(source.name)) {
                sources.append(source.name);
            }
        }
    }

    QList<ResetPort> ports;

    // Clock inputs
    for (const auto &clock : clocks) {
        ports.append({clock, true, 1, "    /**< Clock inputs */"});
    }

    // Source inputs (excluding those that are also outputs)
    for (const auto &source : sources) {
        ports.append({source, true, 1, "    /**< Reset sources */"});
    }

    // Test enable input (if specified)
    if (!config.testEnable.isEmpty()) {
        ports.append({config.testEnable, true, 1, "    /**< Test enable signal */"});
    }

    // Reset reason clear signal
    if (config.reason.enabled && !config.reason.clear.isEmpty()) {
        ports.append({config.reason.clear, true, 1, "    /**< Reset reason clear */"});
    }

    // Reset targets (outputs win over inputs)
    for (const auto &target : config.targets) {
        ports.append({target.name, false, 1, "    /**< Reset targets */"});
    }

    // Reset reason outputs
    if (config.reason.enabled) {
        ports.append(
            {config.reason.output,
             false,
             config.reason.vectorWidth > 1 ? config.reason.vectorWidth : 1,
             "    /**< Reset reason outputs */"});
        ports.append({config.reason.valid, false, 1, "    /**< Reset reason outputs */"});
    }
    return ports;
}

void QSocResetPrimitive::generateModuleHeader(const ResetControllerConfig &config, QTextStream &out)
{
    out << "\nmodule " << config.moduleName << " (\n";
    const auto ports = describePorts(config);
    for (qsizetype i = 0; i < ports.size(); ++i) {
        const auto &port = ports[i];
        out << (port.isInput ? "    input  wire " : "    output wire ");
        if (port.width > 1) {
            out << "[" << port.width - 1 << ":0] ";
        }
        out << port.name << (i + 1 == ports.size() ? "" : ",") << port.comment << "\n";
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
        if (hasTargetProcessing) {
            out << "    wire " << target.name << "_processed;\n";
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

            const QString normalizedSource = getNormalizedSource(link.source, config);

            if (hasAsync || hasSync || hasCount) {
                generateResetComponentInstance(
                    target.name,
                    linkIdx,
                    hasAsync ? &link.async : nullptr,
                    hasSync ? &link.sync : nullptr,
                    hasCount ? &link.count : nullptr,
                    false, // no inv in new architecture
                    normalizedSource,
                    outputWire,
                    out);
            } else {
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

    const QString rootSync = "root_sync_n";
    out << "    /* Root reset released on the second rising edge of " << config.reason.clock
        << " */\n";
    out << "    wire " << rootSync << ";\n";
    out << "    qsoc_sync #(\n";
    out << "        .STAGES(2),\n";
    out << "        .RESET_VALUE(1'b0)\n";
    out << "    ) u_root_sync (\n";
    out << "        .clk  (" << config.reason.clock << "),\n";
    out << "        .rst_n(" << config.reason.rootReset << "),\n";
    out << "        .d    (1'b1),\n";
    out << "        .q    (" << rootSync << ")\n";
    out << "    );\n\n";

    // Generate SW clear synchronizer and pulse generator
    if (!config.reason.clear.isEmpty()) {
        out << "    /* Synchronize software clear and generate pulse */\n";
        out << "    wire swc_d2;\n";
        out << "    qsoc_sync #(\n";
        out << "        .STAGES(2),\n";
        out << "        .RESET_VALUE(1'b0)\n";
        out << "    ) u_swc_sync (\n";
        out << "        .clk  (" << config.reason.clock << "),\n";
        out << "        .rst_n(" << rootSync << "),\n";
        out << "        .d    (" << config.reason.clear << "),\n";
        out << "        .q    (swc_d2)\n";
        out << "    );\n";
        out << "    reg swc_d3;\n";
        out << "    always @(posedge " << config.reason.clock << " or negedge " << rootSync
            << ") begin\n";
        out << "        if (!" << rootSync << ") swc_d3 <= 1'b0;\n";
        out << "        else swc_d3 <= swc_d2;\n";
        out << "    end\n";
        out << "    wire sw_clear_pulse = swc_d2 & ~swc_d3;  // Rising-edge pulse\n\n";
    }

    // Generate fixed 2-cycle clear controller (no configurable parameters)
    out << "    /* Fixed 2-cycle clear controller and valid signal generation */\n";
    out << "    reg        init_done;   /* Set after first post-POR action */\n";
    out << "    reg [1:0]  clr_sr;      /* Fixed 2-cycle clear shift register */\n";
    out << "    reg        valid_q;     /* " << config.reason.valid << " register */\n\n";

    out << "    wire clr_en = |clr_sr;  /* Clear enable (active during 2-cycle window) */\n\n";

    out << "    always @(posedge " << config.reason.clock << " or negedge " << rootSync
        << ") begin\n";
    out << "        if (!" << rootSync << ") begin\n";
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
    out << "            /* Event release synchronized, so the set never releases between\n";
    out << "             * clock edges or races the clear window */\n";
    out << "            wire set_n;\n";
    out << "            qsoc_sync #(\n";
    out << "                .STAGES(2),\n";
    out << "                .RESET_VALUE(1'b0)\n";
    out << "            ) u_event_sync (\n";
    out << "                .clk  (" << config.reason.clock << "),\n";
    out << "                .rst_n(src_event_n[reason_idx]),\n";
    out << "                .d    (1'b1),\n";
    out << "                .q    (set_n)\n";
    out << "            );\n";
    out << "            always @(posedge " << config.reason.clock << " or negedge set_n) begin\n";
    out << "                if (!set_n) begin\n";
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

QString QSocResetPrimitive::generateCellVerilog()
{
    return QSocCellText::reset();
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
