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

    // Generate template RTL cells first
    generateTemplateCells(out);

    // Generate Verilog code
    generateModuleHeader(config, out);
    generateWireDeclarations(config, out);
    generateClockLogic(config, out);
    generateOutputAssignments(config, out);

    // Close module
    out << "\nendmodule\n\n";
    out << "`endif  /* DEF_" << config.moduleName.toUpper() << " */\n";

    return true;
}

QSocClockPrimitive::ClockControllerConfig QSocClockPrimitive::parseClockConfig(
    const YAML::Node &clockNode)
{
    ClockControllerConfig config;

    // Parse basic properties
    config.name       = QString::fromStdString(clockNode["name"].as<std::string>("clkctrl"));
    config.moduleName = "clkctrl";
    config.clock      = QString::fromStdString(clockNode["clock"].as<std::string>("clk_sys"));
    config.default_ref_clock = QString::fromStdString(
        clockNode["default_ref_clock"].as<std::string>(config.clock.toStdString()));

    if (clockNode["test_enable"]) {
        config.test_enable = QString::fromStdString(clockNode["test_enable"].as<std::string>());
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
                if (it->second["duty_cycle"]) {
                    input.duty_cycle = QString::fromStdString(
                        it->second["duty_cycle"].as<std::string>());
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

            // Parse links
            if (it->second["link"] && it->second["link"].IsMap()) {
                for (auto linkIt = it->second["link"].begin(); linkIt != it->second["link"].end();
                     ++linkIt) {
                    ClockLink link;
                    link.sourceName = QString::fromStdString(linkIt->first.as<std::string>());

                    if (linkIt->second["type"]) {
                        link.type = parseClockType(
                            QString::fromStdString(linkIt->second["type"].as<std::string>()));
                    }

                    link.invert = linkIt->second["invert"].as<bool>(false);

                    // Parse gate configuration
                    if (linkIt->second["gate"] && linkIt->second["gate"].IsMap()) {
                        if (linkIt->second["gate"]["enable"]) {
                            link.gate.enable = QString::fromStdString(
                                linkIt->second["gate"]["enable"].as<std::string>());
                        }
                        link.gate.polarity = QString::fromStdString(
                            linkIt->second["gate"]["polarity"].as<std::string>("high"));
                    }

                    // Parse divider configuration
                    if (linkIt->second["div"] && linkIt->second["div"].IsMap()) {
                        link.div.ratio = linkIt->second["div"]["ratio"].as<int>(2);
                        if (linkIt->second["div"]["reset"]) {
                            link.div.reset = QString::fromStdString(
                                linkIt->second["div"]["reset"].as<std::string>());
                        }
                    }

                    target.links.append(link);
                }
            }

            // Parse multiplexer configuration (only if ≥2 links)
            if (target.links.size() >= 2 && it->second["mux"] && it->second["mux"].IsMap()) {
                if (it->second["mux"]["type"]) {
                    target.mux.type = parseMuxType(
                        QString::fromStdString(it->second["mux"]["type"].as<std::string>()));
                }
                if (it->second["mux"]["select"]) {
                    target.mux.select = QString::fromStdString(
                        it->second["mux"]["select"].as<std::string>());
                }
                if (it->second["mux"]["ref_clock"]) {
                    target.mux.ref_clock = QString::fromStdString(
                        it->second["mux"]["ref_clock"].as<std::string>());
                } else if (target.mux.type == GF_MUX) {
                    // Use default_ref_clock for GF_MUX if not specified
                    target.mux.ref_clock = config.default_ref_clock;
                }
            }

            config.targets.append(target);
        }
    }

    return config;
}

void QSocClockPrimitive::generateTemplateCells(QTextStream &out)
{
    out << "// ============================================================\n";
    out << "// Template RTL cells (behavioural placeholders)\n";
    out << "// Replace with foundry-specific macros in synthesis\n";
    out << "// ============================================================\n\n";

    // Standard clock mux
    out << "`ifndef DEF_CKMUX_CELL\n";
    out << "`define DEF_CKMUX_CELL\n\n";
    out << "module CKMUX_CELL (\n";
    out << "    input  wire CLK0,\n";
    out << "    input  wire CLK1,\n";
    out << "    input  wire SEL,     // 0=CLK0, 1=CLK1\n";
    out << "    output wire CLK_OUT\n";
    out << ");\n";
    out << "    assign CLK_OUT = SEL ? CLK1 : CLK0;\n";
    out << "endmodule\n\n";
    out << "`endif  /* DEF_CKMUX_CELL */\n\n";

    // Glitch-free clock mux
    out << "`ifndef DEF_CKMUX_GF_CELL\n";
    out << "`define DEF_CKMUX_GF_CELL\n\n";
    out << "module CKMUX_GF_CELL (\n";
    out << "    input  wire CLK0,\n";
    out << "    input  wire CLK1,\n";
    out << "    input  wire SEL,       // async\n";
    out << "    input  wire REF_CLK,   // reference clock\n";
    out << "    output wire CLK_OUT\n";
    out << ");\n";
    out << "    reg sel_q;\n";
    out << "    always @(posedge REF_CLK) sel_q <= SEL;   // sync\n";
    out << "    assign CLK_OUT = sel_q ? CLK1 : CLK0;\n";
    out << "endmodule\n\n";
    out << "`endif  /* DEF_CKMUX_GF_CELL */\n\n";

    // Clock gate cell
    out << "`ifndef DEF_CKGATE_CELL\n";
    out << "`define DEF_CKGATE_CELL\n\n";
    out << "module CKGATE_CELL (\n";
    out << "    input  wire CLK_IN,\n";
    out << "    input  wire CLK_EN,\n";
    out << "    output wire CLK_OUT\n";
    out << ");\n";
    out << "    assign CLK_OUT = CLK_IN & CLK_EN;\n";
    out << "endmodule\n\n";
    out << "`endif  /* DEF_CKGATE_CELL */\n\n";

    // ICG-based divider
    out << "`ifndef DEF_CKDIV_ICG\n";
    out << "`define DEF_CKDIV_ICG\n\n";
    out << "module CKDIV_ICG #(\n";
    out << "    parameter integer RATIO = 4\n";
    out << ")(\n";
    out << "    input  wire CLK_IN,\n";
    out << "    input  wire RST_N,\n";
    out << "    output wire CLK_OUT\n";
    out << ");\n";
    out << "    localparam W = $clog2(RATIO);\n";
    out << "    reg [W-1:0] cnt;\n";
    out << "    always @(posedge CLK_IN or negedge RST_N)\n";
    out << "        if (!RST_N) cnt <= 0;\n";
    out << "        else        cnt <= (cnt==RATIO-1) ? 0 : cnt+1;\n";
    out << "    wire pulse_en = (cnt==0);\n";
    out << "    CKGATE_CELL u_icg (.CLK_IN(CLK_IN), .CLK_EN(pulse_en), .CLK_OUT(CLK_OUT));\n";
    out << "endmodule\n\n";
    out << "`endif  /* DEF_CKDIV_ICG */\n\n";

    // D-FF divider
    out << "`ifndef DEF_CKDIV_DFF\n";
    out << "`define DEF_CKDIV_DFF\n\n";
    out << "module CKDIV_DFF #(\n";
    out << "    parameter integer RATIO = 2   // even ≥2\n";
    out << ")(\n";
    out << "    input  wire CLK_IN,\n";
    out << "    input  wire RST_N,\n";
    out << "    output wire CLK_OUT\n";
    out << ");\n";
    out << "    localparam W = $clog2(RATIO);\n";
    out << "    reg [W-1:0] cnt;\n";
    out << "    reg clk_q;\n";
    out << "    always @(posedge CLK_IN or negedge RST_N)\n";
    out << "        if (!RST_N) begin cnt <= 0; clk_q <= 0; end\n";
    out << "        else if (cnt==RATIO/2-1) begin cnt<=0; clk_q<=~clk_q; end\n";
    out << "        else cnt<=cnt+1;\n";
    out << "    assign CLK_OUT = clk_q;\n";
    out << "endmodule\n\n";
    out << "`endif  /* DEF_CKDIV_DFF */\n\n";
}

void QSocClockPrimitive::generateModuleHeader(const ClockControllerConfig &config, QTextStream &out)
{
    out << "`ifndef DEF_" << config.moduleName.toUpper() << "\n";
    out << "`define DEF_" << config.moduleName.toUpper() << "\n\n";

    out << "module " << config.moduleName << " (\n";

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
    if (!config.test_enable.isEmpty()) {
        portList << QString("    input  %1     /**< Test enable signal */").arg(config.test_enable);
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
            QString     wireName = getLinkWireName(target.name, link.sourceName, i);
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
        if (target.links.size() == 1) {
            // Single source - direct assignment
            QString wireName   = getLinkWireName(target.name, target.links[0].sourceName, 0);
            QString outputExpr = wireName;

            // Apply inversion if needed
            if (target.links[0].invert) {
                outputExpr = "~" + outputExpr;
            }

            out << "    assign " << target.name << " = " << outputExpr << ";\n";
        } else if (target.links.size() >= 2) {
            // Multiple sources - generate multiplexer
            generateMuxInstance(target, config, out);
        }
    }

    out << "\n";
}

void QSocClockPrimitive::generateClockInstance(
    const ClockLink &link, const QString &targetName, int linkIndex, QTextStream &out)
{
    QString wireName     = getLinkWireName(targetName, link.sourceName, linkIndex);
    QString instanceName = getInstanceName(targetName, link.sourceName, linkIndex);

    out << "    /*\n";
    out << "     * " << link.sourceName << " -> " << targetName << ": "
        << getClockTypeString(link.type) << "\n";
    out << "     */\n";

    switch (link.type) {
    case PASS_THRU:
        out << "    assign " << wireName << " = " << link.sourceName << ";\n";
        break;

    case GATE_ONLY:
        out << "    CKGATE_CELL " << instanceName << " (\n";
        out << "        .CLK_IN  (" << link.sourceName << "),\n";
        out << "        .CLK_EN  (" << link.gate.enable << "),\n";
        out << "        .CLK_OUT (" << wireName << ")\n";
        out << "    );\n";
        break;

    case DIV_ICG:
        out << "    CKDIV_ICG #(\n";
        out << "        .RATIO(" << link.div.ratio << ")\n";
        out << "    ) " << instanceName << " (\n";
        out << "        .CLK_IN  (" << link.sourceName << "),\n";
        out << "        .RST_N   (" << link.div.reset << "),\n";
        out << "        .CLK_OUT (" << wireName << ")\n";
        out << "    );\n";
        break;

    case DIV_DFF:
        out << "    CKDIV_DFF #(\n";
        out << "        .RATIO(" << link.div.ratio << ")\n";
        out << "    ) " << instanceName << " (\n";
        out << "        .CLK_IN  (" << link.sourceName << "),\n";
        out << "        .RST_N   (" << link.div.reset << "),\n";
        out << "        .CLK_OUT (" << wireName << ")\n";
        out << "    );\n";
        break;

    case GATE_DIV_ICG: {
        QString gateWire = QString("gated_%1_%2").arg(targetName, link.sourceName);
        out << "    wire " << gateWire << ";\n";
        out << "    CKGATE_CELL " << instanceName << "_gate (\n";
        out << "        .CLK_IN  (" << link.sourceName << "),\n";
        out << "        .CLK_EN  (" << link.gate.enable << "),\n";
        out << "        .CLK_OUT (" << gateWire << ")\n";
        out << "    );\n";
        out << "    CKDIV_ICG #(\n";
        out << "        .RATIO(" << link.div.ratio << ")\n";
        out << "    ) " << instanceName << "_div (\n";
        out << "        .CLK_IN  (" << gateWire << "),\n";
        out << "        .RST_N   (" << link.div.reset << "),\n";
        out << "        .CLK_OUT (" << wireName << ")\n";
        out << "    );\n";
        break;
    }

    case GATE_DIV_DFF: {
        QString gateWire = QString("gated_%1_%2").arg(targetName, link.sourceName);
        out << "    wire " << gateWire << ";\n";
        out << "    CKGATE_CELL " << instanceName << "_gate (\n";
        out << "        .CLK_IN  (" << link.sourceName << "),\n";
        out << "        .CLK_EN  (" << link.gate.enable << "),\n";
        out << "        .CLK_OUT (" << gateWire << ")\n";
        out << "    );\n";
        out << "    CKDIV_DFF #(\n";
        out << "        .RATIO(" << link.div.ratio << ")\n";
        out << "    ) " << instanceName << "_div (\n";
        out << "        .CLK_IN  (" << gateWire << "),\n";
        out << "        .RST_N   (" << link.div.reset << "),\n";
        out << "        .CLK_OUT (" << wireName << ")\n";
        out << "    );\n";
        break;
    }
    }

    out << "\n";
}

void QSocClockPrimitive::generateMuxInstance(
    const ClockTarget &target, const ClockControllerConfig &config, QTextStream &out)
{
    QString instanceName = QString("u_%1_mux").arg(target.name);
    QString muxOut       = target.name;

    // Generate intermediate wires for inversion if needed
    QStringList inputWires;
    for (int i = 0; i < target.links.size(); ++i) {
        const auto &link     = target.links[i];
        QString     wireName = getLinkWireName(target.name, link.sourceName, i);

        if (link.invert) {
            QString invertedWire = QString("%1_inv").arg(wireName);
            out << "    wire " << invertedWire << ";\n";
            out << "    assign " << invertedWire << " = ~" << wireName << ";\n";
            inputWires << invertedWire;
        } else {
            inputWires << wireName;
        }
    }

    if (target.mux.type == STD_MUX) {
        // Generate cascaded standard mux for multiple inputs
        if (target.links.size() == 2) {
            out << "    CKMUX_CELL " << instanceName << " (\n";
            out << "        .CLK0    (" << inputWires[0] << "),\n";
            out << "        .CLK1    (" << inputWires[1] << "),\n";
            out << "        .SEL     (" << target.mux.select << "),\n";
            out << "        .CLK_OUT (" << muxOut << ")\n";
            out << "    );\n";
        } else {
            // For more than 2 inputs, generate a behavioral mux
            out << "    reg " << muxOut << "_reg;\n";
            out << "    always @(*) begin\n";
            out << "        case (" << target.mux.select << ")\n";
            for (int i = 0; i < inputWires.size(); ++i) {
                out << "            " << i << ": " << muxOut << "_reg = " << inputWires[i] << ";\n";
            }
            out << "            default: " << muxOut << "_reg = " << inputWires[0] << ";\n";
            out << "        endcase\n";
            out << "    end\n";
            out << "    assign " << muxOut << " = " << muxOut << "_reg;\n";
        }
    } else if (target.mux.type == GF_MUX) {
        // Glitch-free mux (currently only supports 2 inputs)
        if (target.links.size() == 2) {
            out << "    CKMUX_GF_CELL " << instanceName << " (\n";
            out << "        .CLK0    (" << inputWires[0] << "),\n";
            out << "        .CLK1    (" << inputWires[1] << "),\n";
            out << "        .SEL     (" << target.mux.select << "),\n";
            out << "        .REF_CLK (" << target.mux.ref_clock << "),\n";
            out << "        .CLK_OUT (" << muxOut << ")\n";
            out << "    );\n";
        }
    }

    out << "\n";
}

QSocClockPrimitive::ClockType QSocClockPrimitive::parseClockType(const QString &typeStr)
{
    if (typeStr == "PASS_THRU")
        return PASS_THRU;
    if (typeStr == "GATE_ONLY")
        return GATE_ONLY;
    if (typeStr == "DIV_ICG")
        return DIV_ICG;
    if (typeStr == "DIV_DFF")
        return DIV_DFF;
    if (typeStr == "GATE_DIV_ICG")
        return GATE_DIV_ICG;
    if (typeStr == "GATE_DIV_DFF")
        return GATE_DIV_DFF;

    qWarning() << "Unknown clock type:" << typeStr << ", defaulting to PASS_THRU";
    return PASS_THRU;
}

QSocClockPrimitive::MuxType QSocClockPrimitive::parseMuxType(const QString &typeStr)
{
    if (typeStr == "GF_MUX")
        return GF_MUX;
    return STD_MUX; // Default
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

QString QSocClockPrimitive::getClockTypeString(ClockType type)
{
    switch (type) {
    case PASS_THRU:
        return "PASS_THRU: Direct forward";
    case GATE_ONLY:
        return "GATE_ONLY: ICG gate only";
    case DIV_ICG:
        return "DIV_ICG: Narrow-pulse divider (counter + ICG)";
    case DIV_DFF:
        return "DIV_DFF: 50% divider (toggle/D-FF)";
    case GATE_DIV_ICG:
        return "GATE_DIV_ICG: Gate → ICG divider";
    case GATE_DIV_DFF:
        return "GATE_DIV_DFF: Gate → D-FF divider";
    }
    return "UNKNOWN";
}
