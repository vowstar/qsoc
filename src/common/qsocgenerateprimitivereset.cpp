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

    // Parse sources
    if (resetNode["sources"] && resetNode["sources"].IsSequence()) {
        for (size_t i = 0; i < resetNode["sources"].size(); ++i) {
            const YAML::Node &srcNode = resetNode["sources"][i];
            if (!srcNode.IsMap() || !srcNode["name"])
                continue;

            ResetSource source;
            source.name    = QString::fromStdString(srcNode["name"].as<std::string>());
            source.active  = srcNode["active"]
                                 ? QString::fromStdString(srcNode["active"].as<std::string>())
                                 : "high";
            source.comment = srcNode["comment"]
                                 ? QString::fromStdString(srcNode["comment"].as<std::string>())
                                 : "";

            config.sources.append(source);
        }
    }

    // Parse targets
    if (resetNode["targets"] && resetNode["targets"].IsSequence()) {
        for (size_t i = 0; i < resetNode["targets"].size(); ++i) {
            const YAML::Node &tgtNode = resetNode["targets"][i];
            if (!tgtNode.IsMap() || !tgtNode["name"])
                continue;

            ResetTarget target;
            target.name    = QString::fromStdString(tgtNode["name"].as<std::string>());
            target.active  = tgtNode["active"]
                                 ? QString::fromStdString(tgtNode["active"].as<std::string>())
                                 : "low";
            target.comment = tgtNode["comment"]
                                 ? QString::fromStdString(tgtNode["comment"].as<std::string>())
                                 : "";

            // Parse sources affecting this target
            if (tgtNode["sources"] && tgtNode["sources"].IsSequence()) {
                for (size_t j = 0; j < tgtNode["sources"].size(); ++j) {
                    target.sources.append(
                        QString::fromStdString(tgtNode["sources"][j].as<std::string>()));
                }
            }

            config.targets.append(target);
        }
    }

    // Parse connections (source-target relationships with modes)
    if (resetNode["connections"] && resetNode["connections"].IsSequence()) {
        for (size_t i = 0; i < resetNode["connections"].size(); ++i) {
            const YAML::Node &connNode = resetNode["connections"][i];
            if (!connNode.IsMap())
                continue;

            ResetConnection connection;
            connection.sourceName = QString::fromStdString(connNode["source"].as<std::string>());
            connection.targetName = QString::fromStdString(connNode["target"].as<std::string>());

            QString modeStr = connNode["mode"]
                                  ? QString::fromStdString(connNode["mode"].as<std::string>())
                                  : "A";

            if (!parseResetMode(modeStr, connection)) {
                qWarning() << "Failed to parse reset mode:" << modeStr;
                continue;
            }

            config.connections.append(connection);
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

    switch (connection.mode) {
    case AsyncComb: {
        out << "A: Async reset Async release combinational logic\n";
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

    case AsyncSync: {
        out << "A(" << connection.syncDepth << "," << connection.clock
            << "): Async reset sync release\n";
        out << "     * (with sync_depth=" << connection.syncDepth << ", clock=" << connection.clock
            << ")\n";
        out << "     */\n";

        out << "    datapath_reset_sync #(\n";
        out << "        .RST_SYNC_LEVEL(" << connection.syncDepth
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

    case AsyncCounter: {
        out << "AC(" << connection.syncDepth << "," << connection.counterWidth << ","
            << connection.timeout << "," << connection.clock << "): Async reset sync release\n";
        out << "     * using one-shot counter (with sync_depth=" << connection.syncDepth
            << ", width=" << connection.counterWidth << ", timeout=" << connection.timeout
            << ", clock=" << connection.clock << ")\n";
        out << "     */\n";

        out << "    datapath_reset_counter #(\n";
        out << "        .RST_SYNC_LEVEL(" << connection.syncDepth
            << "),   /**< Reset synchronization levels */\n";
        out << "        .CNT_WIDTH     (" << connection.counterWidth
            << "),   /**< Counter width */\n";
        out << "        .TIMEOUT       (" << connection.timeout << ")  /**< Timeout value */\n";
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

    default: {
        out << "Unsupported reset mode\n";
        out << "     */\n";
        out << "    // FIXME: Unsupported reset mode for " << connection.sourceName << " -> "
            << connection.targetName << "\n";
        break;
    }
    }
}

bool QSocResetPrimitive::parseResetMode(const QString &modeStr, ResetConnection &connection)
{
    // Default values
    connection.mode         = AsyncComb;
    connection.syncDepth    = 0;
    connection.counterWidth = 0;
    connection.timeout      = 0;
    connection.clock        = "";

    if (modeStr == "A") {
        connection.mode = AsyncComb;
        return true;
    }

    // Parse A(N,clk) format
    QRegularExpression      asyncSyncRegex(R"(^A\((\d+),(\w+)\)$)");
    QRegularExpressionMatch match = asyncSyncRegex.match(modeStr);
    if (match.hasMatch()) {
        connection.mode      = AsyncSync;
        connection.syncDepth = match.captured(1).toInt();
        connection.clock     = match.captured(2);
        return true;
    }

    // Parse AC(N,W,T,clk) format
    QRegularExpression asyncCounterRegex(R"(^AC\((\d+),(\d+),(\d+),(\w+)\)$)");
    match = asyncCounterRegex.match(modeStr);
    if (match.hasMatch()) {
        connection.mode         = AsyncCounter;
        connection.syncDepth    = match.captured(1).toInt();
        connection.counterWidth = match.captured(2).toInt();
        connection.timeout      = match.captured(3).toInt();
        connection.clock        = match.captured(4);
        return true;
    }

    qWarning() << "Unsupported reset mode format:" << modeStr;
    return false;
}

QString QSocResetPrimitive::getConnectionWireName(
    const QString &sourceName, const QString &targetName)
{
    return QString("normalize_%1_to_%2").arg(sourceName).arg(targetName);
}

QString QSocResetPrimitive::getInstanceName(const ResetConnection &connection)
{
    QString baseName;
    switch (connection.mode) {
    case AsyncSync:
        baseName = "u_reset_sync_";
        break;
    case AsyncCounter:
        baseName = "u_reset_counter_";
        break;
    default:
        baseName = "u_reset_logic_";
        break;
    }

    return baseName + getConnectionWireName(connection.sourceName, connection.targetName);
}
