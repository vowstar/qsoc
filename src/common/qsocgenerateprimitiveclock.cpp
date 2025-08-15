#include "qsocgenerateprimitiveclock.h"
#include "qsocgeneratemanager.h"
#include "qsocverilogutils.h"
#include <cmath>
#include <QDebug>
#include <QRegularExpression>
#include <QRegularExpressionMatch>

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

    // Parse basic properties (KISS: require explicit values)
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

            // Parse target-level ICG (KISS format)
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

            // Parse target-level divider (KISS format)
            if (it->second["div"] && it->second["div"].IsMap()) {
                target.div.ratio = it->second["div"]["ratio"].as<int>(1);
                if (it->second["div"]["reset"]) {
                    target.div.reset = QString::fromStdString(
                        it->second["div"]["reset"].as<std::string>());
                }
                if (it->second["div"]["test_enable"]) {
                    target.div.test_enable = QString::fromStdString(
                        it->second["div"]["test_enable"].as<std::string>());
                }
            }

            // Parse target-level inverter (KISS format) - key existence only
            target.inv = it->second["inv"] ? true : false;

            // Parse links
            if (it->second["link"] && it->second["link"].IsMap()) {
                for (auto linkIt = it->second["link"].begin(); linkIt != it->second["link"].end();
                     ++linkIt) {
                    ClockLink link;
                    link.source = QString::fromStdString(linkIt->first.as<std::string>());

                    // KISS format: link-level inverter flag - key existence only
                    if (linkIt->second.IsMap() && linkIt->second["inv"]) {
                        link.inv = true;
                    } else {
                        link.inv = false;
                    }

                    // KISS format: link-level ICG configuration
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

                    // KISS format: link-level divider configuration
                    if (linkIt->second.IsMap() && linkIt->second["div"]
                        && linkIt->second["div"].IsMap()) {
                        link.div.ratio = linkIt->second["div"]["ratio"].as<int>(1);
                        if (linkIt->second["div"]["reset"]) {
                            link.div.reset = QString::fromStdString(
                                linkIt->second["div"]["reset"].as<std::string>());
                        }
                        if (linkIt->second["div"]["test_enable"]) {
                            link.div.test_enable = QString::fromStdString(
                                linkIt->second["div"]["test_enable"].as<std::string>());
                        }
                    }

                    target.links.append(link);
                }
            }

            // Parse multiplexer configuration (only if ≥2 links) - KISS format only
            if (target.links.size() >= 2) {
                // KISS format: infer mux from direct properties
                if (it->second["select"]) {
                    target.mux.select = QString::fromStdString(
                        it->second["select"].as<std::string>());
                }
                if (it->second["reset"]) {
                    target.mux.reset = QString::fromStdString(it->second["reset"].as<std::string>());
                }
                if (it->second["test_enable"]) {
                    target.mux.test_enable = QString::fromStdString(
                        it->second["test_enable"].as<std::string>());
                }
                if (it->second["test_clock"]) {
                    target.mux.test_clock = QString::fromStdString(
                        it->second["test_clock"].as<std::string>());
                }

                // Auto-select mux type based on reset presence (KISS principle)
                if (!target.mux.reset.isEmpty()) {
                    target.mux.type = GF_MUX; // Has reset → Glitch-free mux
                } else {
                    target.mux.type = STD_MUX; // No reset → Standard mux
                }

                // Validation: multi-link requires select signal
                if (target.mux.select.isEmpty()) {
                    qCritical() << "Error: 'select' signal is required for multi-link target:"
                                << target.name;
                    qCritical() << "Example: target: { link: {clk1: ~, clk2: ~}, select: sel_sig }";
                    return config;
                }
            }

            config.targets.append(target);
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

    // Add input clocks
    for (const auto &input : config.inputs) {
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

    // Add test enable if specified
    if (!config.test_en.isEmpty()) {
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
            out << "    QSOC_CKGATE_CELL #(\n";
            out << "        .enable_reset(1'b0)\n";
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
            QString divOutput = QString("%1_div_out").arg(target.name);
            out << "    wire " << divOutput << ";\n";
            out << "    QSOC_CLKDIV_CELL #(\n";
            out << "        .width(8),\n";
            out << "        .default_val(" << target.div.ratio << "),\n";
            out << "        .enable_reset(1'b0)\n";
            out << "    ) " << instanceName << "_div (\n";
            out << "        .clk(" << currentSignal << "),\n";
            out << "        .rst_n(" << (target.div.reset.isEmpty() ? "1'b1" : target.div.reset)
                << "),\n";
            out << "        .en(1'b1),\n";
            QString testEn = target.div.test_enable.isEmpty()
                                 ? (config.test_en.isEmpty() ? "1'b0" : config.test_en)
                                 : target.div.test_enable;
            out << "        .test_en(" << testEn << "),\n";
            out << "        .div(8'd" << target.div.ratio << "),\n";
            out << "        .div_valid(1'b1),\n";
            out << "        .div_ready(),\n";
            out << "        .clk_out(" << divOutput << "),\n";
            out << "        .count()\n";
            out << "    );\n";
            currentSignal = divOutput;
        }

        // Target-level INV
        if (target.inv) {
            QString invOutput = QString("%1_inv_out").arg(target.name);
            out << "    wire " << invOutput << ";\n";
            out << "    QSOC_CKINV_CELL " << instanceName << "_inv (\n";
            out << "        .clk_in(" << currentSignal << "),\n";
            out << "        .clk_out(" << invOutput << ")\n";
            out << "    );\n";
            currentSignal = invOutput;
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

    // KISS format: generate chain based on what's specified
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

    // KISS format: generate processing chain
    bool hasProcessing = !link.icg.enable.isEmpty() || (link.div.ratio > 1) || link.inv;

    if (hasProcessing) {
        // Handle link-level processing: ICG → DIV → INV
        QString currentWire = inputClk;

        // Step 1: Link-level ICG
        if (!link.icg.enable.isEmpty()) {
            QString icgWire = wireName + "_preicg";
            out << "    wire " << icgWire << ";\n";
            out << "    QSOC_CKGATE_CELL #(\n";
            out << "        .enable_reset(1'b0)\n";
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
            QString divWire = wireName + "_prediv";
            out << "    wire " << divWire << ";\n";
            out << "    QSOC_CLKDIV_CELL #(\n";
            out << "        .width(8),\n";
            out << "        .default_val(" << link.div.ratio << "),\n";
            out << "        .enable_reset(1'b0)\n";
            out << "    ) " << instanceName << "_div (\n";
            out << "        .clk(" << currentWire << "),\n";
            out << "        .rst_n(" << (link.div.reset.isEmpty() ? "1'b1" : link.div.reset)
                << "),\n";
            out << "        .en(1'b1),\n";
            QString testEn = link.div.test_enable.isEmpty() ? "1'b0" : link.div.test_enable;
            out << "        .test_en(" << testEn << "),\n";
            out << "        .div(8'd" << link.div.ratio << "),\n";
            out << "        .div_valid(1'b1),\n";
            out << "        .div_ready(),\n";
            out << "        .clk_out(" << divWire << "),\n";
            out << "        .count()\n";
            out << "    );\n";
            currentWire = divWire;
        }

        // Step 3: Link-level inverter
        if (link.inv) {
            out << "    QSOC_CKINV_CELL " << instanceName << "_inv (\n";
            out << "        .clk_in(" << currentWire << "),\n";
            out << "        .clk_out(" << wireName << ")\n";
            out << "    );\n";
        } else {
            out << "    assign " << wireName << " = " << currentWire << ";\n";
        }

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
        // Standard mux using QSOC_CLKMUX_STD_CELL
        out << "    QSOC_CLKMUX_STD_CELL #(\n";
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
        out << "        .clk_sel(" << target.mux.select << "),\n";
        out << "        .clk_out(" << muxOut << ")\n";
        out << "    );\n";

    } else if (target.mux.type == GF_MUX) {
        // Glitch-free mux using QSOC_CLKMUX_GF_CELL
        out << "    QSOC_CLKMUX_GF_CELL #(\n";
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
        QString testClk = target.mux.test_clock.isEmpty() ? "1'b0" : target.mux.test_clock;
        QString testEn  = target.mux.test_enable.isEmpty() ? "1'b0" : target.mux.test_enable;
        out << "        .test_clk(" << testClk << "),\n";
        out << "        .test_en(" << testEn << "),\n";

        // Connect reset signal
        QString resetSig = target.mux.reset.isEmpty() ? "1'b1" : target.mux.reset;
        out << "        .async_rst_n(" << resetSig << "),\n";

        // Connect select signal
        out << "        .async_sel(" << target.mux.select << "),\n";
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

    // KISS: Validate mux type
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
        "QSOC_CKGATE_CELL",
        "QSOC_CKINV_CELL",
        "QSOC_CLKOR2_CELL",
        "QSOC_CLKMUX2_CELL",
        "QSOC_CLKXOR2_CELL",
        "QSOC_CLKDIV_CELL",
        "QSOC_CLKMUX_GF_CELL",
        "QSOC_CLKMUX_STD_CELL",
        "QSOC_CLKOR_TREE"};
}

QString QSocClockPrimitive::generateTemplateCellDefinition(const QString &cellName)
{
    QString     result;
    QTextStream out(&result);

    if (cellName == "QSOC_CKGATE_CELL") {
        out << "/**\n";
        out << " * @brief Clock gate cell module\n";
        out << " *\n";
        out << " * @details Template implementation of clock gate cell with test and reset "
               "support.\n";
        out << " */\n";
        out << "module QSOC_CKGATE_CELL #(\n";
        out << "    parameter enable_reset = 1'b0\n";
        out << ")(\n";
        out << "    input  wire clk,        /**< Clock input */\n";
        out << "    input  wire en,         /**< Clock enable */\n";
        out << "    input  wire test_en,    /**< Test enable */\n";
        out << "    input  wire rst_n,      /**< Reset (active low) */\n";
        out << "    output wire clk_out     /**< Clock output */\n";
        out << ");\n";
        out << "    /* Template implementation - replace with foundry-specific IP */\n";
        out << "    wire final_en;\n";
        out << "    // Force enable when test_en or (reset and enable_reset parameter)\n";
        out << "    assign final_en = test_en | (!rst_n & enable_reset) | (rst_n & en);\n";
        out << "    assign clk_out = clk & final_en;\n";
        out << "endmodule\n";

    } else if (cellName == "QSOC_CKINV_CELL") {
        out << "/**\n";
        out << " * @brief Clock inverter cell module\n";
        out << " *\n";
        out << " * @details Template implementation of clock inverter cell.\n";
        out << " */\n";
        out << "module QSOC_CKINV_CELL (\n";
        out << "    input  wire CLK_IN,   /**< Clock input */\n";
        out << "    output wire CLK_OUT   /**< Clock output */\n";
        out << ");\n";
        out << "    /* Template implementation - replace with foundry-specific IP */\n";
        out << "    assign CLK_OUT = ~CLK_IN;\n";
        out << "endmodule\n";

    } else if (cellName == "QSOC_CLKOR2_CELL") {
        out << "/**\n";
        out << " * @brief 2-input clock OR gate cell module\n";
        out << " *\n";
        out << " * @details Template implementation of 2-input clock OR gate cell.\n";
        out << " */\n";
        out << "module QSOC_CLKOR2_CELL (\n";
        out << "    input  wire CLK_IN0,  /**< Clock input 0 */\n";
        out << "    input  wire CLK_IN1,  /**< Clock input 1 */\n";
        out << "    output wire CLK_OUT   /**< Clock output */\n";
        out << ");\n";
        out << "    /* Template implementation - replace with foundry-specific IP */\n";
        out << "    assign CLK_OUT = CLK_IN0 | CLK_IN1;\n";
        out << "endmodule\n";

    } else if (cellName == "QSOC_CLKMUX2_CELL") {
        out << "/**\n";
        out << " * @brief 2-to-1 clock multiplexer cell module\n";
        out << " *\n";
        out << " * @details Template implementation of 2-to-1 clock multiplexer.\n";
        out << " */\n";
        out << "module QSOC_CLKMUX2_CELL (\n";
        out << "    input  wire CLK_IN0,  /**< Clock input 0 */\n";
        out << "    input  wire CLK_IN1,  /**< Clock input 1 */\n";
        out << "    input  wire CLK_SEL,  /**< Select signal: 0=CLK_IN0, 1=CLK_IN1 */\n";
        out << "    output wire CLK_OUT   /**< Clock output */\n";
        out << ");\n";
        out << "    /* Template implementation - replace with foundry-specific IP */\n";
        out << "    assign CLK_OUT = CLK_SEL ? CLK_IN1 : CLK_IN0;\n";
        out << "endmodule\n";

    } else if (cellName == "QSOC_CLKXOR2_CELL") {
        out << "/**\n";
        out << " * @brief 2-input clock XOR gate cell module\n";
        out << " *\n";
        out << " * @details Template implementation of 2-input clock XOR gate cell.\n";
        out << " */\n";
        out << "module QSOC_CLKXOR2_CELL (\n";
        out << "    input  wire CLK_IN0,  /**< Clock input 0 */\n";
        out << "    input  wire CLK_IN1,  /**< Clock input 1 */\n";
        out << "    output wire CLK_OUT   /**< Clock output */\n";
        out << ");\n";
        out << "    /* Template implementation - replace with foundry-specific IP */\n";
        out << "    assign CLK_OUT = CLK_IN0 ^ CLK_IN1;\n";
        out << "endmodule\n";

    } else if (cellName == "QSOC_CLKDIV_CELL") {
        out << "/**\n";
        out << " * @brief Configurable clock divider cell module\n";
        out << " *\n";
        out << " * @details Template implementation matching clk_int_div interface.\n";
        out << " */\n";
        out << "module QSOC_CLKDIV_CELL #(\n";
        out << "    parameter integer width = 4,\n";
        out << "    parameter integer default_val = 0,\n";
        out << "    parameter enable_reset = 1'b0\n";
        out << ")(\n";
        out << "    input  wire                clk,        /**< Clock input */\n";
        out << "    input  wire                rst_n,      /**< Reset (active low) */\n";
        out << "    input  wire                en,         /**< Enable */\n";
        out << "    input  wire                test_en,    /**< Test mode enable */\n";
        out << "    input  wire [width-1:0]    div,        /**< Division value */\n";
        out << "    input  wire                div_valid,  /**< Division value valid */\n";
        out << "    output wire                div_ready,  /**< Division ready */\n";
        out << "    output reg                 clk_out,    /**< Clock output */\n";
        out << "    output wire [width-1:0]    count       /**< Cycle counter */\n";
        out << ");\n";
        out << "    /* Template implementation - replace with foundry-specific IP */\n";
        out << "    reg [width-1:0] cnt_q;\n";
        out << "    reg [width-1:0] div_q;\n";
        out << "    reg clk_q;\n";
        out << "    \n";
        out << "    assign div_ready = 1'b1;\n";
        out << "    assign count = cnt_q;\n";
        out << "    \n";
        out << "    always @(posedge clk, negedge rst_n) begin\n";
        out << "        if (!rst_n) begin\n";
        out << "            cnt_q <= {width{1'b0}};\n";
        out << "            clk_q <= 1'b0;\n";
        out << "            div_q <= default_val;\n";
        out << "        end else if (div_valid) begin\n";
        out << "            div_q <= div;\n";
        out << "        end else if (en) begin\n";
        out << "            if (cnt_q == div_q - 1) begin\n";
        out << "                cnt_q <= {width{1'b0}};\n";
        out << "                clk_q <= ~clk_q;\n";
        out << "            end else begin\n";
        out << "                cnt_q <= cnt_q + 1;\n";
        out << "            end\n";
        out << "        end\n";
        out << "    end\n";
        out << "    \n";
        out << "    assign clk_out = test_en ? clk : (en ? clk_q : 1'b0);\n";
        out << "endmodule\n";

    } else if (cellName == "QSOC_CLKMUX_GF_CELL") {
        out << "/**\n";
        out << " * @brief Glitch-free clock multiplexer cell module\n";
        out << " *\n";
        out << " * @details Template implementation of glitch-free N-input clock multiplexer\n";
        out << " *          based on ETH Zurich common_cells library design.\n";
        out << " *          Supports multi-input with parametrized sync stages and DFT.\n";
        out << " */\n";
        out << "module QSOC_CLKMUX_GF_CELL #(\n";
        out << "    parameter integer NUM_INPUTS = 2,        /**< Number of clock inputs */\n";
        out << "    parameter integer NUM_SYNC_STAGES = 2,   /**< Synchronizer stages */\n";
        out << "    parameter CLOCK_DURING_RESET = 1'b1,      /**< Clock during reset */\n";
        out << "    localparam SelWidth = "
               "(NUM_INPUTS<=2)?1:(NUM_INPUTS<=4)?2:(NUM_INPUTS<=8)?3:(NUM_INPUTS<=16)?4:(NUM_"
               "INPUTS<=32)?5:(NUM_INPUTS<=64)?6:(NUM_INPUTS<=128)?7:(NUM_INPUTS<=256)?8:(NUM_"
               "INPUTS<=512)?9:(NUM_INPUTS<=1024)?10:(NUM_INPUTS<=2048)?11:(NUM_INPUTS<=4096)?12:"
               "32\n";
        out << ") (\n";
        out << "    input  wire [NUM_INPUTS-1:0] clk_in,        /**< Clock inputs */\n";
        out << "    input  wire                  test_clk,      /**< DFT test clock */\n";
        out << "    input  wire                  test_en,       /**< DFT test enable */\n";
        out << "    input  wire                  async_rst_n,   /**< Async reset (active low) "
               "*/\n";
        out << "    input  wire [SelWidth-1:0]   async_sel,     /**< Async select signal */\n";
        out << "    output reg                   clk_out        /**< Clock output */\n";
        out << ");\n";
        out << "    /* Template implementation - replace with foundry-specific IP */\n";
        out << "    \n";
        out << "    // Note: NUM_INPUTS must be >= 2 for proper operation\n";
        out << "    \n";
        out << "    // Internal signals for glitch-free switching\n";
        out << "    reg [NUM_INPUTS-1:0]        sel_onehot;\n";
        out << "    reg [NUM_INPUTS-1:0*2-1:0]   glitch_filter_d, glitch_filter_q;\n";
        out << "    wire [NUM_INPUTS-1:0]        gate_enable_unfiltered;\n";
        out << "    wire [NUM_INPUTS-1:0]        glitch_filter_output;\n";
        out << "    wire [NUM_INPUTS-1:0]        gate_enable_sync;\n";
        out << "    wire [NUM_INPUTS-1:0]        gate_enable;\n";
        out << "    reg [NUM_INPUTS-1:0]        clock_disabled_q;\n";
        out << "    wire [NUM_INPUTS-1:0]        gated_clock;\n";
        out << "    wire                         output_clock;\n";
        out << "    wire [NUM_INPUTS-1:0]        reset_synced;\n";
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
        out << "        // Synchronize reset to each clock domain\n";
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
        out << "        assign glitch_filter_d[i][0] = gate_enable_unfiltered[i];\n";
        out << "        assign glitch_filter_d[i][1] = glitch_filter_q[i][0];\n";
        out << "        \n";
        out << "        always @(posedge clk_in[i] or negedge reset_synced[i]) begin\n";
        out << "            if (!reset_synced[i]) begin\n";
        out << "                glitch_filter_q[i] <= 2'b00;\n";
        out << "            end else begin\n";
        out << "                glitch_filter_q[i] <= glitch_filter_d[i];\n";
        out << "            end\n";
        out << "        end\n";
        out << "        \n";
        out << "        assign glitch_filter_output[i] = glitch_filter_q[i][1] & \n";
        out << "                                         glitch_filter_q[i][0] & \n";
        out << "                                         gate_enable_unfiltered[i];\n";
        out << "        \n";
        out << "        // Synchronizer chain for enable signal\n";
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
        out << "        // Clock gating (simplified behavioral model)\n";
        out << "        reg gate_en_latched;\n";
        out << "        always @(*) begin\n";
        out << "            if (~clk_in[i])\n";
        out << "                gate_en_latched = gate_enable[i];\n";
        out << "        end\n";
        out << "        assign gated_clock[i] = clk_in[i] & gate_en_latched;\n";
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
        out << "    // Output OR gate\n";
        out << "    assign output_clock = |gated_clock;\n";
        out << "    \n";
        out << "    // DFT mux: select between functional clock and test clock\n";
        out << "    assign clk_out = test_en ? test_clk : output_clock;\n";
        out << "    \n";
        out << "endmodule\n";
        out << "\n";
        out << "/**\n";
        out << " * @brief Standard (non-glitch-free) clock multiplexer cell module\n";
        out << " *\n";
        out << " * @details Template implementation of simple N-input clock multiplexer\n";
        out << " *          using pure combinational logic. No glitch protection.\n";
        out << " */\n";
        out << "module QSOC_CLKMUX_STD_CELL #(\n";
        out << "    parameter integer NUM_INPUTS = 2,\n";
        out << "    localparam SelWidth = "
               "(NUM_INPUTS<=2)?1:(NUM_INPUTS<=4)?2:(NUM_INPUTS<=8)?3:(NUM_INPUTS<=16)?4:(NUM_"
               "INPUTS<=32)?5:(NUM_INPUTS<=64)?6:(NUM_INPUTS<=128)?7:(NUM_INPUTS<=256)?8:(NUM_"
               "INPUTS<=512)?9:(NUM_INPUTS<=1024)?10:(NUM_INPUTS<=2048)?11:(NUM_INPUTS<=4096)?12:"
               "32\n";
        out << ") (\n";
        out << "    input  wire [NUM_INPUTS-1:0] clk_in,        /**< Clock inputs */\n";
        out << "    input  wire [SelWidth-1:0]   clk_sel,       /**< Clock select signal */\n";
        out << "    output wire                  clk_out        /**< Clock output */\n";
        out << ");\n";
        out << "    /* Template implementation - replace with foundry-specific IP */\n";
        out << "    \n";
        out << "    // Note: NUM_INPUTS must be >= 2 for proper operation\n";
        out << "    \n";
        out << "    // Simple mux using case statement\n";
        out << "    reg selected_clk;\n";
        out << "    always @(*) begin\n";
        out << "        case (clk_sel)\n";
        out << "            0: selected_clk = clk_in[0];\n";
        out << "            1: selected_clk = (NUM_INPUTS > 1) ? clk_in[1] : clk_in[0];\n";
        out << "            2: selected_clk = (NUM_INPUTS > 2) ? clk_in[2] : clk_in[0];\n";
        out << "            3: selected_clk = (NUM_INPUTS > 3) ? clk_in[3] : clk_in[0];\n";
        out << "            default: selected_clk = clk_in[0];\n";
        out << "        endcase\n";
        out << "    end\n";
        out << "    assign clk_out = selected_clk;\n";
        out << "    \n";
        out << "endmodule\n";

    } else if (cellName == "QSOC_CLKMUX_STD_CELL") {
        // This is already defined inline with QSOC_CLKMUX_GF_CELL above
        // Return empty string to avoid duplication
        return "";

    } else if (cellName == "QSOC_CLKOR_TREE") {
        out << "/**\n";
        out << " * @brief Clock OR tree cell module\n";
        out << " *\n";
        out << " * @details Template implementation of multi-input clock OR tree.\n";
        out << " */\n";
        out << "module QSOC_CLKOR_TREE #(\n";
        out << "    parameter integer INPUT_COUNT = 4    /**< Number of clock inputs */\n";
        out << ")(\n";
        out << "    input  wire [INPUT_COUNT-1:0] clk_in,  /**< Clock inputs */\n";
        out << "    output wire                   clk_out  /**< Clock output */\n";
        out << ");\n";
        out << "    /* Template implementation - replace with foundry-specific IP */\n";
        out << "    assign clk_out = |clk_in;\n";
        out << "endmodule\n";
    }

    return result;
}
