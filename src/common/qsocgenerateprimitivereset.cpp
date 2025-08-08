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

    if (config.flags.enabled) {
        generateResetFlags(config, out);
    }

    generateOutputAssignments(config, out);

    // Close module
    out << "\nendmodule\n\n";
    out << "`endif  /* DEF_" << config.moduleName.toUpper() << " */\n";

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
                            : "rstctrl";
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

    // Parse reset flags configuration
    config.flags.enabled = false;
    if (resetNode["record_reset_flags"] && resetNode["record_reset_flags"].as<bool>()) {
        config.flags.enabled   = true;
        config.flags.porSignal = resetNode["por_signal"]
                                     ? QString::fromStdString(
                                           resetNode["por_signal"].as<std::string>())
                                     : "por_rst_n";
        config.flags.flagBus   = resetNode["flag_bus"]
                                     ? QString::fromStdString(resetNode["flag_bus"].as<std::string>())
                                     : "reset_flags";
        config.flags.aonClock  = resetNode["aon_clock"]
                                     ? QString::fromStdString(
                                          resetNode["aon_clock"].as<std::string>())
                                     : "";
    }

    return config;
}

void QSocResetPrimitive::generateModuleHeader(const ResetControllerConfig &config, QTextStream &out)
{
    out << "\n`ifndef DEF_" << config.moduleName.toUpper() << "\n";
    out << "`define DEF_" << config.moduleName.toUpper() << "\n\n";

    out << "module " << config.moduleName << " (\n";

    // Clock input
    out << "    /* Reset clock */\n";
    out << "    input  " << config.clock << ",                   /**< System clock input */\n";

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

    // Reset flags output (if enabled)
    if (config.flags.enabled) {
        int flagWidth = config.sources.size();
        int widthBits = static_cast<int>(std::ceil(std::log2(flagWidth)));
        if (widthBits == 0)
            widthBits = 1;

        out << "    /* Reset flags */\n";
        out << "    output [" << (flagWidth - 1) << ":0] " << config.flags.flagBus
            << ",    /**< Reset source flags */\n";
    }

    // Test enable
    out << "    /* DFT */\n";
    out << "    input  " << config.testEnable << "                    /**< Test enable signal */\n";
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
        generateResetInstance(connection, out);
    }
}

void QSocResetPrimitive::generateResetFlags(const ResetControllerConfig &config, QTextStream &out)
{
    if (!config.flags.enabled || config.sources.isEmpty()) {
        return;
    }

    out << "    /* Reset flags recording logic */\n";
    out << "    genvar rst_flag_i;\n";
    out << "    generate\n";
    out << "        for (rst_flag_i = 0; rst_flag_i < " << config.sources.size()
        << "; rst_flag_i++) begin : g_reset_flags\n";

    // Generate per-source async preset flags
    out << "            always_ff @(";
    if (!config.flags.aonClock.isEmpty()) {
        out << "posedge " << config.flags.aonClock << " or ";
    }
    out << "negedge " << config.flags.porSignal << ")\n";
    out << "                case (rst_flag_i)\n";

    for (int i = 0; i < config.sources.size(); ++i) {
        const auto &source = config.sources[i];
        out << "                    " << i << ": begin\n";
        out << "                        if (!" << config.flags.porSignal << ")\n";
        out << "                            " << config.flags.flagBus << "[" << i
            << "] <= 1'b0;  // POR clear\n";

        if (source.active == "high") {
            out << "                        else if (" << source.name << ")\n";
        } else {
            out << "                        else if (!" << source.name << ")\n";
        }
        out << "                            " << config.flags.flagBus << "[" << i
            << "] <= 1'b1;  // Capture event\n";
        out << "                    end\n";
    }

    out << "                endcase\n";
    out << "        end\n";
    out << "    endgenerate\n\n";
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

void QSocResetPrimitive::generateResetInstance(const ResetConnection &connection, QTextStream &out)
{
    QString wireName     = getConnectionWireName(connection.sourceName, connection.targetName);
    QString instanceName = getInstanceName(connection);

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

        out << "    datapath_reset_sync #(\n";
        out << "        .RST_SYNC_LEVEL(" << connection.sync_depth
            << ")  /**< Delay cycles for reset */\n";
        out << "    ) " << instanceName << " (\n";
        out << "        .clk         (" << connection.clock
            << "),                                  /**< Clock signal */\n";

        if (connection.sourceName.endsWith("_n")) {
            out << "        .rst_n_a     (" << connection.sourceName
                << "),                             /**< Active low reset input */\n";
        } else {
            out << "        .rst_n_a     (~" << connection.sourceName
                << "),                             /**< Active low reset input */\n";
        }

        out << "        .reset_bypass(test_en),                                  /**< Bypass reset "
               "when DFT scan */\n";
        out << "        .rst_n_sync  (" << wireName << ")  /**< Active low reset output */\n";
        out << "    );\n";
        break;
    }

    case ASYNC_CNT: {
        out << "ASYNC_CNT: Async reset sync release using one-shot counter\n";
        out << "     * (Legacy AC(" << connection.sync_depth << "," << connection.counter_width
            << "," << connection.timeout_cycles << "," << connection.clock << "))\n";
        out << "     * with sync_depth=" << connection.sync_depth
            << ", width=" << connection.counter_width << ", timeout=" << connection.timeout_cycles
            << ", clock=" << connection.clock << ")\n";
        out << "     */\n";

        out << "    datapath_reset_counter #(\n";
        out << "        .RST_SYNC_LEVEL(" << connection.sync_depth
            << "),   /**< Reset synchronization levels */\n";
        out << "        .CNT_WIDTH     (" << connection.counter_width
            << "),   /**< Counter width */\n";
        out << "        .TIMEOUT       (" << connection.timeout_cycles
            << ")  /**< Timeout value */\n";
        out << "    ) " << instanceName << " (\n";
        out << "        .clk          (" << connection.clock
            << "),                              /**< Clock signal */\n";
        out << "        .rst_n_a      (" << connection.sourceName
            << "),                            /**< Active low reset input */\n";
        out << "        .reset_bypass (test_en),                              /**< Bypass reset "
               "when DFT scan */\n";
        out << "        .rst_n_timeout(" << wireName << ")  /**< Active low reset output */\n";
        out << "    );\n";
        break;
    }

    case SYNC_ONLY: {
        out << "SYNC_ONLY: Sync reset sync release\n";
        out << "     * (Legacy S(" << connection.sync_depth << "," << connection.clock << ")\n";
        out << "     * with sync_depth=" << connection.sync_depth << ", clock=" << connection.clock
            << ")\n";
        out << "     */\n";

        out << "    datapath_reset_sync #(\n";
        out << "        .RST_SYNC_LEVEL(" << connection.sync_depth
            << ")  /**< Delay cycles for reset */\n";
        out << "    ) " << instanceName << " (\n";
        out << "        .clk         (" << connection.clock
            << "),                                  /**< Clock signal */\n";
        out << "        .rst_n_a     (" << connection.sourceName
            << "),                             /**< Sync reset input */\n";
        out << "        .reset_bypass(test_en),                                  /**< Bypass reset "
               "when DFT scan */\n";
        out << "        .rst_n_sync  (" << wireName << ")  /**< Sync reset output */\n";
        out << "    );\n";
        break;
    }

    case ASYNC_PIPE: {
        out << "ASYNC_PIPE: Async reset sync release with pipeline\n";
        out << "     * (Legacy AS(" << connection.sync_depth << "," << connection.pipe_depth << ","
            << connection.clock << ")\n";
        out << "     * with sync_depth=" << connection.sync_depth
            << ", pipe_depth=" << connection.pipe_depth << ", clock=" << connection.clock << ")\n";
        out << "     */\n";

        // For now, implement as two-stage: first async_sync, then pipeline
        out << "    // TODO: Implement ASYNC_PIPE as two-stage reset with pipeline\n";
        out << "    datapath_reset_sync #(\n";
        out << "        .RST_SYNC_LEVEL(" << connection.sync_depth
            << ")  /**< Initial sync depth */\n";
        out << "    ) " << instanceName << " (\n";
        out << "        .clk         (" << connection.clock
            << "),                                  /**< Clock signal */\n";

        if (connection.sourceName.endsWith("_n")) {
            out << "        .rst_n_a     (" << connection.sourceName
                << "),                             /**< Active low reset input */\n";
        } else {
            out << "        .rst_n_a     (~" << connection.sourceName
                << "),                             /**< Active low reset input */\n";
        }

        out << "        .reset_bypass(test_en),                                  /**< Bypass reset "
               "when DFT scan */\n";
        out << "        .rst_n_sync  (" << wireName
            << ")  /**< Pipeline reset output (simplified) */\n";
        out << "    );\n";
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
    } else if (typeStr == "ASYNC_PIPE") {
        return ASYNC_PIPE;
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
    return QString("normalize_%1_to_%2").arg(sourceName).arg(targetName);
}

QString QSocResetPrimitive::getInstanceName(const ResetConnection &connection)
{
    QString baseName;
    switch (connection.type) {
    case ASYNC_SYNC:
        baseName = "u_reset_sync_";
        break;
    case ASYNC_CNT:
        baseName = "u_reset_counter_";
        break;
    case SYNC_ONLY:
        baseName = "u_reset_sync_only_";
        break;
    case ASYNC_PIPE:
        baseName = "u_reset_pipe_";
        break;
    default:
        baseName = "u_reset_logic_";
        break;
    }

    return baseName + getConnectionWireName(connection.sourceName, connection.targetName);
}
