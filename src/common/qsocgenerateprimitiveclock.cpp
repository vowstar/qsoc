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

void QSocClockPrimitive::setForceOverwrite(bool force)
{
    m_forceOverwrite = force;
}

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

    // Test enable is optional - if not set, tie to 1'b0 internally
    if (clockNode["test_enable"]) {
        config.testEnable = QString::fromStdString(clockNode["test_enable"].as<std::string>());
    }

    // Optional ref_clock for GF_MUX
    if (clockNode["ref_clock"]) {
        config.ref_clock = QString::fromStdString(clockNode["ref_clock"].as<std::string>());
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
                target.icg.test_enable = config.testEnable; // Use controller-level test_enable
                if (it->second["icg"]["reset"]) {
                    target.icg.reset = QString::fromStdString(
                        it->second["icg"]["reset"].as<std::string>());
                }
            }

            // Parse target-level divider
            if (it->second["div"] && it->second["div"].IsMap()) {
                // Clean field names only
                target.div.default_value  = it->second["div"]["default"].as<int>(1);
                target.div.clock_on_reset = it->second["div"]["clock_on_reset"].as<bool>(false);

                // Check if dynamic mode (has value signal)
                bool hasDynamicControl = it->second["div"]["value"]
                                         && !it->second["div"]["value"].as<std::string>().empty();

                if (hasDynamicControl) {
                    // Dynamic mode: width is required
                    target.div.width = it->second["div"]["width"].as<int>(0);
                    if (target.div.width <= 0) {
                        qWarning() << "ERROR: Dynamic divider for target"
                                   << QString::fromStdString(it->first.as<std::string>())
                                   << "requires explicit width specification";
                        target.div.width = 8; // Default fallback
                    }
                    // Verify default value fits in specified width
                    int maxValue = (1 << target.div.width) - 1;
                    if (target.div.default_value > maxValue) {
                        qWarning()
                            << "ERROR: Default value" << target.div.default_value << "for target"
                            << QString::fromStdString(it->first.as<std::string>())
                            << "exceeds maximum value" << maxValue << "for width"
                            << target.div.width << "bits";
                    }
                } else {
                    // Static mode: calculate width from default value
                    target.div.width = static_cast<int>(
                        std::ceil(std::log2(std::max(target.div.default_value + 1, 2))));
                    // Override if explicitly specified (for manual control)
                    if (it->second["div"]["width"]) {
                        target.div.width = it->second["div"]["width"].as<int>(target.div.width);
                    }
                }

                if (it->second["div"]["reset"]) {
                    target.div.reset = QString::fromStdString(
                        it->second["div"]["reset"].as<std::string>());
                }
                if (it->second["div"]["enable"]) {
                    target.div.enable = QString::fromStdString(
                        it->second["div"]["enable"].as<std::string>());
                }
                target.div.test_enable = config.testEnable; // Use controller-level test_enable

                // Clean field names - no legacy support
                if (it->second["div"]["value"]) {
                    target.div.value = QString::fromStdString(
                        it->second["div"]["value"].as<std::string>());
                }
                if (it->second["div"]["valid"]) {
                    target.div.valid = QString::fromStdString(
                        it->second["div"]["valid"].as<std::string>());
                }
                if (it->second["div"]["ready"]) {
                    target.div.ready = QString::fromStdString(
                        it->second["div"]["ready"].as<std::string>());
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
                        link.icg.test_enable = config.testEnable; // Use controller-level test_enable
                        if (linkIt->second["icg"]["reset"]) {
                            link.icg.reset = QString::fromStdString(
                                linkIt->second["icg"]["reset"].as<std::string>());
                        }
                    }

                    // Link-level divider configuration - design only
                    if (linkIt->second.IsMap() && linkIt->second["div"]
                        && linkIt->second["div"].IsMap()) {
                        // Clean field names only
                        link.div.default_value = linkIt->second["div"]["default"].as<int>(1);

                        // Check if dynamic mode (has value signal)
                        bool hasDynamicControl
                            = linkIt->second["div"]["value"]
                              && !linkIt->second["div"]["value"].as<std::string>().empty();

                        if (hasDynamicControl) {
                            // Dynamic mode: width is required
                            link.div.width = linkIt->second["div"]["width"].as<int>(0);
                            if (link.div.width <= 0) {
                                qWarning()
                                    << "ERROR: Dynamic divider for link"
                                    << QString::fromStdString(it->first.as<std::string>()) << "->"
                                    << QString::fromStdString(linkIt->first.as<std::string>())
                                    << "requires explicit width specification";
                                link.div.width = 8; // Default fallback
                            }
                            // Verify default value fits in specified width
                            int maxValue = (1 << link.div.width) - 1;
                            if (link.div.default_value > maxValue) {
                                qWarning()
                                    << "ERROR: Default value" << link.div.default_value
                                    << "for link"
                                    << QString::fromStdString(it->first.as<std::string>()) << "->"
                                    << QString::fromStdString(linkIt->first.as<std::string>())
                                    << "exceeds maximum value" << maxValue << "for width"
                                    << link.div.width << "bits";
                            }
                        } else {
                            // Static mode: calculate width from default value
                            link.div.width = static_cast<int>(
                                std::ceil(std::log2(std::max(link.div.default_value + 1, 2))));
                            // Override if explicitly specified (for manual control)
                            if (linkIt->second["div"]["width"]) {
                                link.div.width = linkIt->second["div"]["width"].as<int>(
                                    link.div.width);
                            }
                        }
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
                        link.div.test_enable = config.testEnable; // Use controller-level test_enable

                        // Clean field names - no legacy support
                        if (linkIt->second["div"]["value"]) {
                            link.div.value = QString::fromStdString(
                                linkIt->second["div"]["value"].as<std::string>());
                        }
                        if (linkIt->second["div"]["valid"]) {
                            link.div.valid = QString::fromStdString(
                                linkIt->second["div"]["valid"].as<std::string>());
                        }
                        if (linkIt->second["div"]["ready"]) {
                            link.div.ready = QString::fromStdString(
                                linkIt->second["div"]["ready"].as<std::string>());
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
                target.test_enable = config.testEnable; // Use controller-level test_enable
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

    QStringList portDecls;
    QStringList portComments;

    // Initialize global port tracking for unified "output win" mechanism
    QSet<QString> addedSignals;

    // Collect input clock names for "output win" mechanism
    QSet<QString> inputClocks;
    for (const auto &input : config.inputs) {
        inputClocks.insert(input.name);
    }

    // Add input clocks
    for (const auto &input : config.inputs) {
        QString comment = QString("/**< Clock input: %1").arg(input.name);
        if (!input.freq.isEmpty()) {
            comment += QString(" (%1)").arg(input.freq);
        }
        comment += " */";
        portDecls << QString("    input  wire %1").arg(input.name);
        portComments << comment;
        addedSignals.insert(input.name); // Track input clocks in global set
    }

    // Add target clocks
    for (const auto &target : config.targets) {
        QString comment = QString("/**< Clock target: %1").arg(target.name);
        if (!target.freq.isEmpty()) {
            comment += QString(" (%1)").arg(target.freq);
        }
        comment += " */";
        portDecls << QString("    output wire %1").arg(target.name);
        portComments << comment;
    }

    // Add dynamic divider interface ports (target-level)
    QSet<QString> divSignalNames;
    for (const auto &target : config.targets) {
        if (target.div.default_value > 1 || !target.div.value.isEmpty()) {
            // Add dynamic division value input port
            if (!target.div.value.isEmpty()) {
                if (divSignalNames.contains(target.div.value)) {
                    throw std::runtime_error(QString("Duplicate divider value signal name: %1")
                                                 .arg(target.div.value)
                                                 .toStdString());
                }
                divSignalNames.insert(target.div.value);

                portDecls << QString("    input  wire [%1:0] %2")
                                 .arg(target.div.width - 1)
                                 .arg(target.div.value);
                portComments << QString("/**< Dynamic division value for %1 */").arg(target.name);
            }

            // Add division value valid signal port
            if (!target.div.valid.isEmpty()) {
                if (divSignalNames.contains(target.div.valid)) {
                    throw std::runtime_error(QString("Duplicate divider valid signal name: %1")
                                                 .arg(target.div.valid)
                                                 .toStdString());
                }
                divSignalNames.insert(target.div.valid);

                portDecls << QString("    input  wire %1").arg(target.div.valid);
                portComments << QString("/**< Division valid signal for %1 */").arg(target.name);
            }

            // Add division ready output port
            if (!target.div.ready.isEmpty()) {
                if (divSignalNames.contains(target.div.ready)) {
                    throw std::runtime_error(QString("Duplicate divider ready signal name: %1")
                                                 .arg(target.div.ready)
                                                 .toStdString());
                }
                divSignalNames.insert(target.div.ready);

                portDecls << QString("    output wire %1").arg(target.div.ready);
                portComments << QString("/**< Division ready signal for %1 */").arg(target.name);
            }

            // Add cycle counter output port
            if (!target.div.count.isEmpty()) {
                if (divSignalNames.contains(target.div.count)) {
                    throw std::runtime_error(QString("Duplicate divider count signal name: %1")
                                                 .arg(target.div.count)
                                                 .toStdString());
                }
                divSignalNames.insert(target.div.count);

                portDecls << QString("    output wire [%1:0] %2")
                                 .arg(target.div.width - 1)
                                 .arg(target.div.count);
                portComments << QString("/**< Cycle counter for %1 */").arg(target.name);
            }

            // Add enable signal port
            if (!target.div.enable.isEmpty()) {
                portDecls << QString("    input  wire %1").arg(target.div.enable);
                portComments << QString("/**< Division enable for %1 */").arg(target.name);
            }
        }
    }

    // Add dynamic divider interface ports (link-level)
    for (const auto &target : config.targets) {
        for (const auto &link : target.links) {
            if (link.div.default_value > 1 || !link.div.value.isEmpty()) {
                QString linkName = QString("%1_from_%2").arg(target.name, link.source);

                // Add dynamic division value input port
                if (!link.div.value.isEmpty()) {
                    if (divSignalNames.contains(link.div.value)) {
                        throw std::runtime_error(QString("Duplicate divider value signal name: %1")
                                                     .arg(link.div.value)
                                                     .toStdString());
                    }
                    divSignalNames.insert(link.div.value);

                    portDecls << QString("    input  wire [%1:0] %2")
                                     .arg(link.div.width - 1)
                                     .arg(link.div.value);
                    portComments << QString("/**< Dynamic division value for link %1 */")
                                        .arg(linkName);
                }

                // Add division value valid signal port
                if (!link.div.valid.isEmpty()) {
                    if (divSignalNames.contains(link.div.valid)) {
                        throw std::runtime_error(QString("Duplicate divider valid signal name: %1")
                                                     .arg(link.div.valid)
                                                     .toStdString());
                    }
                    divSignalNames.insert(link.div.valid);

                    portDecls << QString("    input  wire %1").arg(link.div.valid);
                    portComments << QString("/**< Division valid signal for link %1 */")
                                        .arg(linkName);
                }

                // Add division ready output port
                if (!link.div.ready.isEmpty()) {
                    if (divSignalNames.contains(link.div.ready)) {
                        throw std::runtime_error(QString("Duplicate divider ready signal name: %1")
                                                     .arg(link.div.ready)
                                                     .toStdString());
                    }
                    divSignalNames.insert(link.div.ready);

                    portDecls << QString("    output wire %1").arg(link.div.ready);
                    portComments << QString("/**< Division ready signal for link %1 */")
                                        .arg(linkName);
                }

                // Add cycle counter output port
                if (!link.div.count.isEmpty()) {
                    if (divSignalNames.contains(link.div.count)) {
                        throw std::runtime_error(QString("Duplicate divider count signal name: %1")
                                                     .arg(link.div.count)
                                                     .toStdString());
                    }
                    divSignalNames.insert(link.div.count);

                    portDecls << QString("    output wire [%1:0] %2")
                                     .arg(link.div.width - 1)
                                     .arg(link.div.count);
                    portComments << QString("/**< Cycle counter for link %1 */").arg(linkName);
                }

                // Add enable signal port
                if (!link.div.enable.isEmpty()) {
                    portDecls << QString("    input  wire %1").arg(link.div.enable);
                    portComments << QString("/**< Division enable for link %1 */").arg(linkName);
                }
            }
        }
    }

    // Add test enable signal (if specified)
    if (!config.testEnable.isEmpty() && !addedSignals.contains(config.testEnable)) {
        portDecls << QString("    input  wire %1").arg(config.testEnable);
        portComments << QString("/**< Test enable signal */");
        addedSignals.insert(config.testEnable);
    }

    // Add ICG interface ports (target-level)
    for (const auto &target : config.targets) {
        if (!target.icg.enable.isEmpty() && !addedSignals.contains(target.icg.enable)) {
            portDecls << QString("    input  wire %1").arg(target.icg.enable);
            portComments << QString("/**< ICG enable for %1 */").arg(target.name);
            addedSignals.insert(target.icg.enable);
        }
        if (!target.icg.reset.isEmpty() && !addedSignals.contains(target.icg.reset)) {
            portDecls << QString("    input  wire %1").arg(target.icg.reset);
            portComments << QString("/**< ICG reset for %1 */").arg(target.name);
            addedSignals.insert(target.icg.reset);
        }
    }

    // Add MUX interface ports (target-level)
    for (const auto &target : config.targets) {
        if (target.links.size() >= 2) { // Only for multi-source targets
            if (!target.select.isEmpty() && !addedSignals.contains(target.select)) {
                // Calculate select signal width based on number of inputs
                int numInputs   = target.links.size();
                int selectWidth = 1;
                while ((1 << selectWidth) < numInputs) {
                    selectWidth++;
                }

                QString selectDecl;
                if (selectWidth > 1) {
                    selectDecl = QString("[%1:0] %2").arg(selectWidth - 1).arg(target.select);
                } else {
                    selectDecl = target.select;
                }

                portDecls << QString("    input  wire %1").arg(selectDecl);
                portComments << QString("/**< MUX select for %1 */").arg(target.name);
                addedSignals.insert(target.select);
            }
            if (!target.reset.isEmpty() && !addedSignals.contains(target.reset)) {
                portDecls << QString("    input  wire %1").arg(target.reset);
                portComments << QString("/**< MUX reset for %1 */").arg(target.name);
                addedSignals.insert(target.reset);
            }
            // Test enable is already added at controller level
            if (!target.test_clock.isEmpty() && !addedSignals.contains(target.test_clock)) {
                // Implement "output win" mechanism: if test_clock matches an input clock, skip it
                if (inputClocks.contains(target.test_clock)) {
                    // Port already exists as input clock, just mark as processed
                    addedSignals.insert(
                        target.test_clock); // Mark as processed to avoid future conflicts
                } else {
                    portDecls << QString("    input  wire %1").arg(target.test_clock);
                    portComments << QString("/**< MUX test clock for %1 */").arg(target.name);
                    addedSignals.insert(target.test_clock);
                }
            }
        }
    }

    // Add target-level reset signals for DIV (if not already added via ICG/MUX)
    QStringList addedResets;
    for (const auto &target : config.targets) {
        if ((target.div.default_value > 1 || !target.div.value.isEmpty())
            && !target.div.reset.isEmpty()) {
            if (!addedResets.contains(target.div.reset)
                && !addedSignals.contains(target.div.reset)) {
                portDecls << QString("    input  wire %1").arg(target.div.reset);
                portComments << QString("/**< Division reset for %1 */").arg(target.name);
                addedResets << target.div.reset;
                addedSignals.insert(target.div.reset);
            }
        }
    }

    // Add link-level reset signals for DIV (if not already added)
    for (const auto &target : config.targets) {
        for (const auto &link : target.links) {
            if ((link.div.default_value > 1 || !link.div.value.isEmpty())
                && !link.div.reset.isEmpty()) {
                if (!addedResets.contains(link.div.reset)
                    && !addedSignals.contains(link.div.reset)) {
                    QString linkName = QString("%1_from_%2").arg(target.name, link.source);
                    portDecls << QString("    input  wire %1").arg(link.div.reset);
                    portComments << QString("/**< Link division reset for %1 */").arg(linkName);
                    addedResets << link.div.reset;
                    addedSignals.insert(link.div.reset);
                }
            }
        }
    }

    // Test enable is handled at controller level, no need for fallback

    // Output all ports with unified boundary judgment
    for (int i = 0; i < portDecls.size(); ++i) {
        bool    isLast = (i == portDecls.size() - 1);
        QString comma  = isLast ? "" : ",";
        out << portDecls[i] << comma << "    " << portComments[i] << "\n";
    }

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
            out << "        .CLOCK_DURING_RESET(1'b0),\n";
            out << "        .POLARITY(" << (target.icg.polarity == "high" ? "1'b1" : "1'b0")
                << ")\n";
            out << "    ) " << instanceName << "_icg (\n";
            out << "        .clk(" << currentSignal << "),\n";
            out << "        .en(" << target.icg.enable << "),\n";
            QString testEn = target.icg.test_enable.isEmpty() ? "1'b0" : target.icg.test_enable;
            out << "        .test_en(" << testEn << "),\n";
            out << "        .rst_n(" << (target.icg.reset.isEmpty() ? "1'b1" : target.icg.reset)
                << "),\n";
            out << "        .clk_out(" << icgOutput << ")\n";
            out << "    );\n";
            currentSignal = icgOutput;
        }

        // Target-level DIV
        if (target.div.default_value > 1 || !target.div.value.isEmpty()) {
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
            out << "        .DEFAULT_VAL(" << target.div.default_value << "),\n";
            out << "        .CLOCK_DURING_RESET(" << (target.div.clock_on_reset ? "1'b1" : "1'b0")
                << ")\n";
            out << "    ) " << instanceName << "_div (\n";
            out << "        .clk(" << currentSignal << "),\n";
            out << "        .rst_n(" << (target.div.reset.isEmpty() ? "1'b1" : target.div.reset)
                << "),\n";
            out << "        .en(" << (target.div.enable.isEmpty() ? "1'b1" : target.div.enable)
                << "),\n";

            QString testEn = target.div.test_enable.isEmpty() ? "1'b0" : target.div.test_enable;
            out << "        .test_en(" << testEn << "),\n";

            // Dynamic or static division value -  design
            if (!target.div.value.isEmpty()) {
                // Dynamic mode: connect to value signal
                out << "        .div(" << target.div.value << "),\n";
            } else {
                // Static mode: tie to default constant
                out << "        .div(" << target.div.width << "'d" << target.div.default_value
                    << "),\n";
            }

            out << "        .div_valid(" << (target.div.valid.isEmpty() ? "1'b1" : target.div.valid)
                << "),\n";

            if (!target.div.ready.isEmpty()) {
                out << "        .div_ready(" << target.div.ready << "),\n";
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
            QString staInstanceName = target.sta_guide.instance.isEmpty()
                                          ? instanceName + "_sta"
                                          : target.sta_guide.instance;
            out << "    " << target.sta_guide.cell << " " << staInstanceName << " (\n";
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
    if (link.div.default_value > 1 || !link.div.value.isEmpty()) {
        out << " (div/" << link.div.default_value << ")";
    }
    if (link.inv) {
        out << " (inv)";
    }
    out << "\n     */\n";

    // Generate processing chain
    bool hasProcessing = !link.icg.enable.isEmpty()
                         || (link.div.default_value > 1 || !link.div.value.isEmpty()) || link.inv;

    if (hasProcessing) {
        // Handle link-level processing: ICG → DIV → INV
        QString currentWire = inputClk;

        // Step 1: Link-level ICG
        if (!link.icg.enable.isEmpty()) {
            QString icgWire = wireName + "_preicg";
            out << "    wire " << icgWire << ";\n";
            out << "    qsoc_tc_clk_gate #(\n";
            out << "        .CLOCK_DURING_RESET(1'b0),\n";
            out << "        .POLARITY(" << (link.icg.polarity == "high" ? "1'b1" : "1'b0") << ")\n";
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
        if (link.div.default_value > 1 || !link.div.value.isEmpty()) {
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
            out << "        .DEFAULT_VAL(" << link.div.default_value << "),\n";
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

            // Dynamic or static division value -  design
            if (!link.div.value.isEmpty()) {
                // Dynamic mode: connect to value signal
                out << "        .div(" << link.div.value << "),\n";
            } else {
                // Static mode: tie to default constant
                out << "        .div(" << link.div.width << "'d" << link.div.default_value
                    << "),\n";
            }

            out << "        .div_valid(" << (link.div.valid.isEmpty() ? "1'b1" : link.div.valid)
                << "),\n";

            if (!link.div.ready.isEmpty()) {
                out << "        .div_ready(" << link.div.ready << "),\n";
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
    // Source names are unique, no need for linkIndex suffix
    Q_UNUSED(linkIndex);
    return QString("clk_%1_from_%2").arg(targetName, sourceName);
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
    // - If force mode enabled: always overwrite existing file

    if (!file.exists() || m_forceOverwrite) {
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

        /* Format generated clock_cell.v file if verible-verilog-format is available */
        QSocGenerateManager::formatVerilogFile(filePath);

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

    /* Format generated clock_cell.v file if verible-verilog-format is available */
    QSocGenerateManager::formatVerilogFile(filePath);

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
        "qsoc_tc_clk_gate_pos",
        "qsoc_tc_clk_gate_neg",
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
        out << " * @brief Wrapper: polarity select + test/reset bypass via MUX2\n";
        out << " *\n";
        out << " * @details POLARITY=1 -> LATCH-AND; POLARITY=0 -> LATCH-OR\n";
        out << " *          bypass_sel = test_en | (~rst_n & CLOCK_DURING_RESET)\n";
        out << " */\n";
        out << "module qsoc_tc_clk_gate #(\n";
        out << "    parameter CLOCK_DURING_RESET = 1'b0,\n";
        out << "    parameter POLARITY = 1'b1\n";
        out << ")(\n";
        out << "    input  wire clk,        /**< Clock input */\n";
        out << "    input  wire en,         /**< Clock enable */\n";
        out << "    input  wire test_en,    /**< Test enable */\n";
        out << "    input  wire rst_n,      /**< Reset (active low) */\n";
        out << "    output wire clk_out     /**< Clock output */\n";
        out << ");\n";
        out << "    wire gated_clk;\n";
        out << "    \n";
        out << "    /* Select ICG primitive by polarity */\n";
        out << "    generate\n";
        out << "        if (POLARITY == 1'b1) begin : g_pos\n";
        out << "            qsoc_tc_clk_gate_pos u_pos (\n";
        out << "                .clk    (clk),\n";
        out << "                .en     (en),\n";
        out << "                .test_en(test_en),\n";
        out << "                .clk_out(gated_clk)\n";
        out << "            );\n";
        out << "        end else begin : g_neg\n";
        out << "            qsoc_tc_clk_gate_neg u_neg (\n";
        out << "                .clk    (clk),\n";
        out << "                .en     (en),\n";
        out << "                .test_en(test_en),\n";
        out << "                .clk_out(gated_clk)\n";
        out << "            );\n";
        out << "        end\n";
        out << "    endgenerate\n";
        out << "    \n";
        out << "    /* Bypass: immediate pass-through in test mode or during reset */\n";
        out << "    wire bypass_sel = test_en | (~rst_n & CLOCK_DURING_RESET);\n";
        out << "    \n";
        out << "    qsoc_tc_clk_mux2 i_clk_bypass_mux (\n";
        out << "        .CLK_IN0(gated_clk),\n";
        out << "        .CLK_IN1(clk),\n";
        out << "        .CLK_SEL(bypass_sel),\n";
        out << "        .CLK_OUT(clk_out)\n";
        out << "    );\n";
        out << "endmodule\n";

    } else if (cellName == "qsoc_tc_clk_gate_pos") {
        out << "/**\n";
        out << " * @brief LATCH-AND ICG: Positive-edge style (pre-controlled)\n";
        out << " *\n";
        out << " * @details IQ updates when clk==0: IQ = (test_en | en); Q = IQ & clk\n";
        out << " */\n";
        out << "module qsoc_tc_clk_gate_pos (\n";
        out << "    input  wire clk,        /**< Clock input */\n";
        out << "    input  wire en,         /**< Clock enable */\n";
        out << "    input  wire test_en,    /**< Test enable */\n";
        out << "    output wire clk_out     /**< Clock output */\n";
        out << ");\n";
        out << "    reg iq;\n";
        out << "`ifndef SYNTHESIS\n";
        out << "    initial iq = 1'b0;  /* sim-only init to block X fanout */\n";
        out << "`endif\n";
        out << "    /* Level-sensitive latch with explicit sensitivity list to avoid delta "
               "oscillation */\n";
        out << "    always @(clk or en or test_en) begin\n";
        out << "        if (!clk) iq <= (test_en | en);  /* Use non-blocking for time-sequence "
               "consistency */\n";
        out << "    end\n";
        out << "    assign clk_out = iq & clk;\n";
        out << "endmodule\n";

    } else if (cellName == "qsoc_tc_clk_gate_neg") {
        out << "/**\n";
        out << " * @brief LATCH-OR ICG: Negative-edge style (pre-controlled)\n";
        out << " *\n";
        out << " * @details IQ updates when clk==1: IQ = ~(test_en | en); Q = IQ | clk\n";
        out << " */\n";
        out << "module qsoc_tc_clk_gate_neg (\n";
        out << "    input  wire clk,        /**< Clock input */\n";
        out << "    input  wire en,         /**< Clock enable */\n";
        out << "    input  wire test_en,    /**< Test enable */\n";
        out << "    output wire clk_out     /**< Clock output */\n";
        out << ");\n";
        out << "    reg iq;\n";
        out << "`ifndef SYNTHESIS\n";
        out << "    initial iq = 1'b0;  /* sim-only init to block X fanout */\n";
        out << "`endif\n";
        out << "    /* Level-sensitive latch with explicit sensitivity list to avoid delta "
               "oscillation */\n";
        out << "    always @(clk or en or test_en) begin\n";
        out << "        if (clk) iq <= ~(test_en | en);  /* Use non-blocking for time-sequence "
               "consistency */\n";
        out << "    end\n";
        out << "    assign clk_out = iq | clk;\n";
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
        out << "    localparam [WIDTH-1:0] div_reset_value =\n";
        out << "        (DEFAULT_VAL != 0) ? DEFAULT_VAL : {{(WIDTH-1){1'b0}}, 1'b1};\n";
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
        out << "    assign div_i_normalized = (div != {WIDTH{1'b0}}) ? div : {{(WIDTH-1){1'b0}}, "
               "1'b1};\n";
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
        out << "                    clk_div_bypass_en_d = (div_i_normalized == {{(WIDTH-1){1'b0}}, "
               "1'b1});\n";
        out << "                    clk_gate_state_d = WAIT_END_PERIOD;\n";
        out << "                end\n";
        out << "            end\n";
        out << "\n";
        out << "            WAIT_END_PERIOD: begin\n";
        out << "                gate_en_d = 1'b0;\n";
        out << "                toggle_ffs_en = 1'b0;\n";
        out << "                if (cycle_cntr_q == (div_q - 1'b1)) begin\n";
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
        out << "    always @(posedge clk or negedge rst_n) begin\n";
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
        out << "                if (clk_div_bypass_en_q || (cycle_cntr_q == (div_q - 1'b1))) "
               "begin\n";
        out << "                    cycle_cntr_d = {WIDTH{1'b0}};\n";
        out << "                end else begin\n";
        out << "                    cycle_cntr_d = cycle_cntr_q + 1'b1;\n";
        out << "                end\n";
        out << "            end\n";
        out << "        end\n";
        out << "    end\n";
        out << "\n";
        out << "    always @(posedge clk or negedge rst_n) begin\n";
        out << "        if (!rst_n) begin\n";
        out << "            cycle_cntr_q <= {WIDTH{1'b0}};\n";
        out << "        end else begin\n";
        out << "            cycle_cntr_q <= cycle_cntr_d;\n";
        out << "        end\n";
        out << "    end\n";
        out << "\n";
        out << "    assign count = cycle_cntr_q;\n";
        out << "\n";
        out << "    /* Precompute (div_q + 1)/2 in WIDTH+1 domain, then truncate to WIDTH.\n";
        out << "     * Do this via named wires to avoid part-select on expressions (slang "
               "limitation). */\n";
        out << "    wire [WIDTH:0]   div_ext            = {1'b0, div_q};\n";
        out << "    wire [WIDTH:0]   div_plus1_ext      = div_ext + {{WIDTH{1'b0}}, 1'b1};\n";
        out << "    wire [WIDTH:0]   div_plus1_ext_half = div_plus1_ext >> 1;\n";
        out << "    wire [WIDTH-1:0] div_plus1_half     = div_plus1_ext_half[WIDTH-1:0];\n";
        out << "\n";
        out << "    /* T-Flip-Flops with non-blocking assignments for synthesis */\n";
        out << "    always @(posedge clk or negedge rst_n) begin\n";
        out << "        if (!rst_n) begin\n";
        out << "            t_ff1_q <= 1'b0;\n";
        out << "        end else if (t_ff1_en) begin\n";
        out << "            t_ff1_q <= t_ff1_d;\n";
        out << "        end\n";
        out << "    end\n";
        out << "\n";
        out << "    always @(negedge clk or negedge rst_n) begin\n";
        out << "        if (!rst_n) begin\n";
        out << "            t_ff2_q <= 1'b0;\n";
        out << "        end else if (t_ff2_en) begin\n";
        out << "            t_ff2_q <= t_ff2_d;\n";
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
        out << "                t_ff1_en = (cycle_cntr_q == {WIDTH{1'b0}}) ? 1'b1 : 1'b0;\n";
        out << "                t_ff2_en = (cycle_cntr_q == div_plus1_half) ? 1'b1 : 1'b0;\n";
        out << "            end else begin\n";
        out << "                t_ff1_en = ((cycle_cntr_q == {WIDTH{1'b0}}) || (cycle_cntr_q == "
               "(div_q >> 1))) ? 1'b1 : "
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
        out << "    always @(posedge ungated_output_clock or negedge rst_n) begin\n";
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
        out << "    output wire                  clk_out        /**< Clock output */\n";
        out << ");\n";
        out << "    /* Template implementation - replace with foundry-specific IP */\n";
        out << "    \n";
        out << "    // Note: NUM_INPUTS must be >= 2 for proper operation\n";
        out << "    \n";
        out << "    /* Integer alias to avoid signed/unsigned compare warnings */\n";
        out << "    localparam integer NUM_INPUTS_I = (NUM_INPUTS < 1) ? 1 : NUM_INPUTS;\n";
        out << "    \n";
        out << "    /* Vector-form upper bound for async_sel (same width as async_sel) */\n";
        out << "    localparam [WIDTH-1:0] NUM_INPUTS_M1 = NUM_INPUTS_I - 1;\n";
        out << "    \n";
        out << "    /* Safe sync stages constant to avoid negative slice */\n";
        out << "    localparam integer SYNC_S = (NUM_SYNC_STAGES < 1) ? 1 : NUM_SYNC_STAGES;\n";
        out << "    \n";
        out << "    // Internal signals for glitch-free switching\n";
        out << "    reg [NUM_INPUTS-1:0]        sel_onehot;\n";
        out << "    wire [NUM_INPUTS*2-1:0]   glitch_filter_d;\n";
        out << "    reg [NUM_INPUTS*2-1:0]   glitch_filter_q;\n";
        out << "    reg [NUM_INPUTS-1:0]         gate_enable_unfiltered;\n";
        out << "    wire [NUM_INPUTS-1:0]        glitch_filter_output;\n";
        out << "    wire [NUM_INPUTS-1:0]        gate_enable_sync;\n";
        out << "    wire [NUM_INPUTS-1:0]        gate_enable;\n";
        out << "    reg [NUM_INPUTS-1:0]        clock_disabled_q;\n";
        out << "    wire [NUM_INPUTS-1:0]        gated_clock;\n";
        out << "    wire                         output_clock;\n";
        out << "    reg [NUM_INPUTS-1:0]        reset_synced;\n";
        out << "    \n";
        out << "    /* Onehot decoder */\n";
        out << "    always @(*) begin\n";
        out << "        sel_onehot = {NUM_INPUTS{1'b0}};\n";
        out << "        /* compare vector vs vector to avoid sign-compare warning */\n";
        out << "        if (async_sel <= NUM_INPUTS_M1)\n";
        out << "            sel_onehot[async_sel] = 1'b1;\n";
        out << "    end\n";
        out << "    \n";
        out << "    // Generate logic for each input clock\n";
        out << "    genvar i;\n";
        out << "    generate\n";
        out << "    for (i = 0; i < NUM_INPUTS_I; i = i + 1) begin : gen_input_stages\n";
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
        out << "        integer j;\n";
        out << "        always @(*) begin\n";
        out << "            gate_enable_unfiltered[i] = 1'b1;\n";
        out << "            for (j = 0; j < NUM_INPUTS_I; j = j + 1) begin\n";
        out << "                if (i == j) begin\n";
        out << "                    gate_enable_unfiltered[i] = gate_enable_unfiltered[i] & "
               "sel_onehot[j];\n";
        out << "                end else begin\n";
        out << "                    gate_enable_unfiltered[i] = gate_enable_unfiltered[i] & "
               "clock_disabled_q[j];\n";
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
        out << "        assign glitch_filter_output[i] = glitch_filter_q[i*2+1] &\n";
        out << "                                         glitch_filter_q[i*2+0] &\n";
        out << "                                         gate_enable_unfiltered[i];\n";
        out << "        \n";
        out << "        // Synchronizer chain for enable signal (equivalent to sync module)\n";
        out << "        // Note: This implements the same functionality as sync "
               "#(.STAGES(NUM_SYNC_STAGES))\n";
        out << "        /* Synchronizer chain for enable signal. Width-safe for SYNC_S. */\n";
        out << "        /* Compile-time split to avoid nested generate and SYNC_S-2 when SYNC_S==1 "
               "*/\n";
        out << "        reg  [SYNC_S-1:0] sync_chain;\n";
        out << "        \n";
        out << "        if (SYNC_S == 1) begin : sync_single\n";
        out << "            always @(posedge clk_in[i] or negedge reset_synced[i]) begin\n";
        out << "                if (!reset_synced[i]) begin\n";
        out << "                    sync_chain <= {SYNC_S{1'b0}};\n";
        out << "                end else begin\n";
        out << "                    // Replicate the single-bit input across the 1-wide vector\n";
        out << "                    sync_chain <= {SYNC_S{glitch_filter_output[i]}};\n";
        out << "                end\n";
        out << "            end\n";
        out << "        end else begin : sync_multi\n";
        out << "            always @(posedge clk_in[i] or negedge reset_synced[i]) begin\n";
        out << "                if (!reset_synced[i]) begin\n";
        out << "                    sync_chain <= {SYNC_S{1'b0}};\n";
        out << "                end else begin\n";
        out << "                    sync_chain <= {sync_chain[SYNC_S-2:0], "
               "glitch_filter_output[i]};\n";
        out << "                end\n";
        out << "            end\n";
        out << "        end\n";
        out << "        assign gate_enable_sync[i] = sync_chain[SYNC_S-1];\n";
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
        out << "    endgenerate\n";
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
