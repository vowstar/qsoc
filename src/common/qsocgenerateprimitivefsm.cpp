#include "qsocgenerateprimitivefsm.h"
#include "qsocgeneratemanager.h"
#include "qsocverilogutils.h"
#include <QDebug>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QSet>

QSocFSMPrimitive::QSocFSMPrimitive(QSocGenerateManager *parent)
    : m_parent(parent)
{}

bool QSocFSMPrimitive::generateFSMVerilog(const YAML::Node &fsmNode, QTextStream &out)
{
    if (!fsmNode || !fsmNode.IsMap()) {
        qWarning() << "Invalid FSM node provided";
        return false;
    }

    const QString fsmName   = QString::fromStdString(fsmNode["name"].as<std::string>());
    const QString clkSignal = QString::fromStdString(fsmNode["clk"].as<std::string>());
    const QString rstSignal = QString::fromStdString(fsmNode["rst"].as<std::string>());
    const QString rstState  = QString::fromStdString(fsmNode["rst_state"].as<std::string>());

    /* Get encoding type (default to binary) */
    QString encoding = "bin";
    if (fsmNode["encoding"] && fsmNode["encoding"].IsScalar()) {
        encoding = QString::fromStdString(fsmNode["encoding"].as<std::string>());
    }

    /* Check if this is microcode mode */
    bool isMicrocodeMode = fsmNode["fields"] && fsmNode["fields"].IsMap();

    /* Generate module wrapper */
    generateModuleHeader(fsmNode, out);

    /* Generate FSM logic inside module */
    if (isMicrocodeMode) {
        generateMicrocodeFSM(fsmNode, out);
    } else {
        generateTableFSM(fsmNode, out);
    }

    /* Close module */
    out << "\nendmodule\n\n";

    return true;
}

void QSocFSMPrimitive::generateModuleHeader(const YAML::Node &fsmNode, QTextStream &out)
{
    const QString fsmName   = QString::fromStdString(fsmNode["name"].as<std::string>());
    const QString clkSignal = QString::fromStdString(fsmNode["clk"].as<std::string>());
    const QString rstSignal = QString::fromStdString(fsmNode["rst"].as<std::string>());

    /* Start module declaration */
    out << "module " << fsmName << " (\n";

    /* Always include clock and reset */
    out << "    /* FSM clock and reset */\n";
    out << "    input  " << clkSignal << ",                   /**< FSM clock input */\n";
    out << "    input  " << rstSignal << ",                   /**< FSM reset input */\n";

    /* Check if this is microcode mode to determine additional ports */
    bool isMicrocodeMode = fsmNode["fields"] && fsmNode["fields"].IsMap();

    if (isMicrocodeMode) {
        /* Microcode FSM ports */

        /* Branch condition input for microcode FSM */
        QMap<QString, QPair<int, int>> fields;
        bool                           hasBranch = false;
        if (fsmNode["fields"] && fsmNode["fields"].IsMap()) {
            for (const auto &fieldEntry : fsmNode["fields"]) {
                if (fieldEntry.first.IsScalar()) {
                    const QString fieldName = QString::fromStdString(
                        fieldEntry.first.as<std::string>());
                    if (fieldName == "branch") {
                        hasBranch = true;
                        break;
                    }
                }
            }
        }

        if (hasBranch) {
            out << "    /* Branch condition */\n";
            out << "    input  cond,                        /**< Branch condition signal */\n";
        }

        /* ROM mode dependent ports */
        QString romMode = "parameter";
        if (fsmNode["rom_mode"] && fsmNode["rom_mode"].IsScalar()) {
            romMode = QString::fromStdString(fsmNode["rom_mode"].as<std::string>());
        }

        if (romMode == "port") {
            out << "    /* ROM write interface */\n";
            out << "    input  " << fsmName << "_rom_we,           /**< ROM write enable */\n";
            out << "    input  [" << fsmName.toUpper() << "_AW-1:0] " << fsmName
                << "_rom_addr,     /**< ROM write address */\n";
            out << "    input  [" << fsmName.toUpper() << "_DW:0] " << fsmName
                << "_rom_wdata,    /**< ROM write data */\n";
        }

        /* Control outputs from fields (excluding branch and next) */
        if (fsmNode["fields"] && fsmNode["fields"].IsMap()) {
            out << "    /* Control outputs */\n";
            for (const auto &fieldEntry : fsmNode["fields"]) {
                if (!fieldEntry.first.IsScalar() || !fieldEntry.second.IsSequence()
                    || fieldEntry.second.size() != 2) {
                    continue;
                }

                const QString fieldName = QString::fromStdString(fieldEntry.first.as<std::string>());
                const int loBit = fieldEntry.second[0].as<int>();
                const int hiBit = fieldEntry.second[1].as<int>();

                if (fieldName != "branch" && fieldName != "next") {
                    QString outputPortName = fieldName;
                    if (fieldName == "ctrl") {
                        outputPortName = "ctrl_bus"; /* Map ctrl field to ctrl_bus port */
                    }

                    if (loBit == hiBit) {
                        out << "    output " << outputPortName << ",                      /**< "
                            << fieldName << " field output */\n";
                    } else {
                        out << "    output [" << hiBit << ":" << loBit << "] " << outputPortName
                            << ",        /**< " << fieldName << " field output */\n";
                    }
                }
            }
        }
    } else {
        /* Table FSM ports */

        /* Collect input signals from transition conditions */
        QSet<QString> inputSignals;
        if (fsmNode["trans"] && fsmNode["trans"].IsMap()) {
            for (const auto &transEntry : fsmNode["trans"]) {
                if (transEntry.second.IsSequence()) {
                    for (const auto &transition : transEntry.second) {
                        if (transition.IsMap() && transition["cond"]) {
                            const QString condition = QString::fromStdString(
                                transition["cond"].as<std::string>());
                            // Extract signal names from conditions (simple parsing)
                            QStringList tokens = condition.split(
                                QRegularExpression(
                                    "[\\s\\(\\)\\&\\|\\=\\!\\<\\>\\+\\-\\*\\/\\%\\^\\~]+"),
                                Qt::SkipEmptyParts);
                            for (const QString &token : tokens) {
                                if (!token.isEmpty() && token != "1" && token != "0"
                                    && !token.startsWith("'") && !token.contains("'d")) {
                                    inputSignals.insert(token);
                                }
                            }
                        }
                    }
                }
            }
        }

        /* Add input signals from Mealy conditions */
        if (fsmNode["mealy"] && fsmNode["mealy"].IsSequence()) {
            for (const auto &mealyEntry : fsmNode["mealy"]) {
                if (mealyEntry.IsMap() && mealyEntry["cond"]) {
                    const QString condition = QString::fromStdString(
                        mealyEntry["cond"].as<std::string>());
                    QStringList tokens = condition.split(
                        QRegularExpression("[\\s\\(\\)\\&\\|\\=\\!\\<\\>\\+\\-\\*\\/\\%\\^\\~]+"),
                        Qt::SkipEmptyParts);
                    for (const QString &token : tokens) {
                        if (!token.isEmpty() && token != "1" && token != "0"
                            && !token.startsWith("'") && !token.contains("'d")
                            && !token.contains("cur_state") && !token.contains("_CUR_STATE")) {
                            inputSignals.insert(token);
                        }
                    }
                }
            }
        }

        /* Remove clock and reset as they're already declared */
        inputSignals.remove(clkSignal);
        inputSignals.remove(rstSignal);

        if (!inputSignals.isEmpty()) {
            out << "    /* Input signals */\n";
            for (const QString &signal : inputSignals) {
                // Check if it's a bus signal based on common patterns
                if (signal.contains("cnt") || signal.contains("data") || signal.contains("addr")) {
                    out << "    input  [7:0] " << signal
                        << ",               /**< Input signal */\n";
                } else {
                    out << "    input  " << signal << ",                    /**< Input signal */\n";
                }
            }
        }

        /* Collect output signals from Moore and Mealy outputs */
        QSet<QString> outputSignals;
        if (fsmNode["moore"] && fsmNode["moore"].IsMap()) {
            for (const auto &mooreEntry : fsmNode["moore"]) {
                if (mooreEntry.second.IsMap()) {
                    for (const auto &output : mooreEntry.second) {
                        if (output.first.IsScalar()) {
                            outputSignals.insert(
                                QString::fromStdString(output.first.as<std::string>()));
                        }
                    }
                }
            }
        }

        if (fsmNode["mealy"] && fsmNode["mealy"].IsSequence()) {
            for (const auto &mealyEntry : fsmNode["mealy"]) {
                if (mealyEntry.IsMap() && mealyEntry["sig"]) {
                    outputSignals.insert(
                        QString::fromStdString(mealyEntry["sig"].as<std::string>()));
                }
            }
        }

        if (!outputSignals.isEmpty()) {
            out << "    /* Output signals */\n";
            QStringList sortedOutputs = outputSignals.values();
            sortedOutputs.sort();
            for (int i = 0; i < sortedOutputs.size(); ++i) {
                const QString &signal        = sortedOutputs[i];
                QString        trailingComma = (i < sortedOutputs.size() - 1) ? "," : "";
                out << "    output " << signal << trailingComma
                    << "                    /**< Output signal */\n";
            }
        }
    }

    out << ");\n\n";
}

void QSocFSMPrimitive::generateTableFSM(const YAML::Node &fsmItem, QTextStream &out)
{
    const QString fsmName      = QString::fromStdString(fsmItem["name"].as<std::string>());
    const QString fsmNameUpper = fsmName.toUpper();
    const QString fsmNameLower = fsmName.toLower();
    const QString clkSignal    = QString::fromStdString(fsmItem["clk"].as<std::string>());
    const QString rstSignal    = QString::fromStdString(fsmItem["rst"].as<std::string>());
    const QString rstState     = QString::fromStdString(fsmItem["rst_state"].as<std::string>());

    /* Get encoding type (default to binary) */
    QString encoding = "bin";
    if (fsmItem["encoding"] && fsmItem["encoding"].IsScalar()) {
        encoding = QString::fromStdString(fsmItem["encoding"].as<std::string>());
    }

    /* Collect all states from trans section */
    QStringList allStates;
    if (fsmItem["trans"] && fsmItem["trans"].IsMap()) {
        for (const auto &transEntry : fsmItem["trans"]) {
            if (transEntry.first.IsScalar()) {
                const QString stateName = QString::fromStdString(transEntry.first.as<std::string>());
                if (!allStates.contains(stateName)) {
                    allStates.append(stateName);
                }
            }
        }
    }

    /* Also collect states from moore section */
    if (fsmItem["moore"] && fsmItem["moore"].IsMap()) {
        for (const auto &mooreEntry : fsmItem["moore"]) {
            if (mooreEntry.first.IsScalar()) {
                const QString stateName = QString::fromStdString(mooreEntry.first.as<std::string>());
                if (!allStates.contains(stateName)) {
                    allStates.append(stateName);
                }
            }
        }
    }

    /* Generate state typedef */
    out << "\n    /* " << fsmName << " : Table FSM generated by YAML-DSL */\n";

    /* Calculate state width */
    int stateWidth = 1;
    int numStates  = allStates.size();
    if (encoding == "onehot") {
        stateWidth = numStates;
    } else {
        while ((1 << stateWidth) < numStates) {
            stateWidth++;
        }
    }

    /* Skip typedef enum for Verilog 2005 compatibility - will use localparam instead */

    /* Generate state registers */
    out << "    /* " << fsmName << " state registers */\n";
    out << "    reg [" << (stateWidth - 1) << ":0] " << fsmNameLower << "_cur_state, "
        << fsmNameLower << "_nxt_state;\n\n";

    /* Generate state parameter definitions for Verilog 2005 compatibility */
    for (int i = 0; i < allStates.size(); ++i) {
        QString stateValue;
        if (encoding == "onehot") {
            stateValue = QString("%1'd%2").arg(stateWidth).arg(1 << i);
        } else if (encoding == "gray") {
            int grayValue = i ^ (i >> 1);
            stateValue    = QString("%1'd%2").arg(stateWidth).arg(grayValue);
        } else {
            stateValue = QString("%1'd%2").arg(stateWidth).arg(i);
        }
        out << "    localparam " << fsmNameUpper << "_" << allStates[i] << " = " << stateValue
            << ";\n";
    }
    out << "\n";

    /* Generate next state logic */
    out << "    /* " << fsmName << " next-state logic */\n";
    out << "    always @(*) begin\n";
    out << "        " << fsmNameLower << "_nxt_state = " << fsmNameLower << "_cur_state;\n";
    out << "        case (" << fsmNameLower << "_cur_state)\n";

    /* Generate transitions */
    if (fsmItem["trans"] && fsmItem["trans"].IsMap()) {
        for (const auto &transEntry : fsmItem["trans"]) {
            if (!transEntry.first.IsScalar() || !transEntry.second.IsSequence()) {
                continue;
            }

            const QString stateName = QString::fromStdString(transEntry.first.as<std::string>());
            out << "            " << fsmNameUpper << "_" << stateName << ":";

            /* Check if multiple transitions exist for this state */
            bool hasMultipleTransitions = transEntry.second.size() > 1;
            if (hasMultipleTransitions) {
                out << "\n                begin\n";
            }

            /* Process each transition condition */
            for (const auto &transition : transEntry.second) {
                if (!transition.IsMap() || !transition["cond"] || !transition["next"]) {
                    continue;
                }

                const QString condition = QString::fromStdString(
                    transition["cond"].as<std::string>());
                const QString nextState = QString::fromStdString(
                    transition["next"].as<std::string>());

                QString indent = hasMultipleTransitions ? "                    "
                                                        : "\n                ";
                if (condition == "1") {
                    out << indent << "if (1'b1) " << fsmNameLower << "_nxt_state = " << fsmNameUpper
                        << "_" << nextState << ";\n";
                } else {
                    QString formattedCondition = QSocVerilogUtils::formatConditionForVerilog(
                        condition);
                    out << indent << "if (" << formattedCondition << ") " << fsmNameLower
                        << "_nxt_state = " << fsmNameUpper << "_" << nextState << ";\n";
                }
            }

            if (hasMultipleTransitions) {
                out << "                end\n";
            }
        }
    }

    out << "            default: " << fsmNameLower << "_nxt_state = " << fsmNameLower
        << "_cur_state;\n";
    out << "        endcase\n";
    out << "    end\n\n";

    /* Generate state register */
    out << "    /* " << fsmName << " state register w/ async reset */\n";
    out << "    always @(posedge " << clkSignal << " or negedge " << rstSignal << ")\n";
    out << "        if (!" << rstSignal << ") " << fsmNameLower << "_cur_state <= " << fsmNameUpper
        << "_" << rstState << ";\n";
    out << "        else        " << fsmNameLower << "_cur_state <= " << fsmNameLower
        << "_nxt_state;\n\n";

    /* Generate Moore outputs */
    if (fsmItem["moore"] && fsmItem["moore"].IsMap()) {
        out << "    /* " << fsmName << " Moore outputs */\n";

        /* Collect all unique output signals */
        QSet<QString> allOutputs;
        for (const auto &mooreEntry : fsmItem["moore"]) {
            if (mooreEntry.second.IsMap()) {
                for (const auto &output : mooreEntry.second) {
                    if (output.first.IsScalar()) {
                        allOutputs.insert(QString::fromStdString(output.first.as<std::string>()));
                    }
                }
            }
        }

        /* Generate internal reg signals for Moore outputs (Verilog 2005 compatibility) */
        for (const QString &output : allOutputs) {
            out << "    reg " << fsmNameLower << "_" << output << "_reg;\n";
        }
        out << "\n";

        /* Generate assigns from internal regs to output ports */
        for (const QString &output : allOutputs) {
            out << "    assign " << output << " = " << fsmNameLower << "_" << output << "_reg;\n";
        }
        out << "\n";

        /* Generate always block with default values */
        out << "    always @(*) begin\n";
        for (const QString &output : allOutputs) {
            out << "        " << fsmNameLower << "_" << output << "_reg = 1'b0;\n";
        }

        out << "        case (" << fsmNameLower << "_cur_state)\n";
        for (const auto &mooreEntry : fsmItem["moore"]) {
            if (!mooreEntry.first.IsScalar() || !mooreEntry.second.IsMap()) {
                continue;
            }

            const QString stateName = QString::fromStdString(mooreEntry.first.as<std::string>());
            out << "            " << fsmNameUpper << "_" << stateName << ":\n";
            out << "                begin\n";

            for (const auto &output : mooreEntry.second) {
                if (output.first.IsScalar() && output.second.IsScalar()) {
                    const QString outputName = QString::fromStdString(
                        output.first.as<std::string>());
                    const QString outputValue = QString::fromStdString(
                        output.second.as<std::string>());
                    QString formattedValue = QSocVerilogUtils::formatConditionForVerilog(
                        outputValue);
                    out << "                    " << fsmNameLower << "_" << outputName
                        << "_reg = " << formattedValue << ";\n";
                }
            }

            out << "                end\n";
        }
        out << "            default: begin\n";
        for (const QString &output : allOutputs) {
            out << "                " << fsmNameLower << "_" << output << "_reg = 1'b0;\n";
        }
        out << "            end\n";
        out << "        endcase\n";
        out << "    end\n\n";
    }

    /* Generate Mealy outputs */
    if (fsmItem["mealy"] && fsmItem["mealy"].IsSequence()) {
        out << "    /* " << fsmName << " Mealy outputs */\n";
        for (const auto &mealyEntry : fsmItem["mealy"]) {
            if (!mealyEntry.IsMap() || !mealyEntry["cond"] || !mealyEntry["sig"]
                || !mealyEntry["val"]) {
                continue;
            }

            const QString condition = QString::fromStdString(mealyEntry["cond"].as<std::string>());
            const QString signal    = QString::fromStdString(mealyEntry["sig"].as<std::string>());
            const QString value     = QString::fromStdString(mealyEntry["val"].as<std::string>());

            /* Replace cur_state with prefixed version in condition */
            QString processedCondition = condition;
            /* Only replace bare 'cur_state' if it doesn't already have the FSM prefix */
            QString prefixedPattern = fsmNameLower + "_cur_state";
            if (!processedCondition.contains(prefixedPattern)) {
                processedCondition.replace("cur_state", prefixedPattern);
            }

            /* Format the condition for proper Verilog syntax */
            QString formattedCondition = QSocVerilogUtils::formatConditionForVerilog(
                processedCondition);
            QString formattedValue = QSocVerilogUtils::formatConditionForVerilog(value);

            out << "    assign " << signal << " = (" << formattedCondition << ") ? "
                << formattedValue << " : 1'b0;\n";
        }
        out << "\n";
    }
}

void QSocFSMPrimitive::generateMicrocodeFSM(const YAML::Node &fsmItem, QTextStream &out)
{
    const QString fsmName      = QString::fromStdString(fsmItem["name"].as<std::string>());
    const QString fsmNameUpper = fsmName.toUpper();
    const QString fsmNameLower = fsmName.toLower();
    const QString clkSignal    = QString::fromStdString(fsmItem["clk"].as<std::string>());
    const QString rstSignal    = QString::fromStdString(fsmItem["rst"].as<std::string>());
    const QString rstState     = QString::fromStdString(fsmItem["rst_state"].as<std::string>());

    /* Get ROM mode (default to parameter) */
    QString romMode = "parameter";
    if (fsmItem["rom_mode"] && fsmItem["rom_mode"].IsScalar()) {
        romMode = QString::fromStdString(fsmItem["rom_mode"].as<std::string>());
    }

    /* Parse fields */
    QMap<QString, QPair<int, int>> fields;
    int                            maxBit = -1;

    if (fsmItem["fields"] && fsmItem["fields"].IsMap()) {
        for (const auto &fieldEntry : fsmItem["fields"]) {
            if (!fieldEntry.first.IsScalar() || !fieldEntry.second.IsSequence()
                || fieldEntry.second.size() != 2) {
                continue;
            }

            const QString fieldName = QString::fromStdString(fieldEntry.first.as<std::string>());
            const int     loBit     = fieldEntry.second[0].as<int>();
            const int     hiBit     = fieldEntry.second[1].as<int>();
            fields[fieldName]       = qMakePair(loBit, hiBit);

            if (hiBit > maxBit)
                maxBit = hiBit;
        }
    }

    /* Calculate inferred data width from fields */
    const int inferredDataWidth = maxBit + 1;

    /* Allow user to specify data width, but use inferred if user's is smaller */
    int userDataWidth = inferredDataWidth;
    if (fsmItem["data_width"] && fsmItem["data_width"].IsScalar()) {
        int userSpecified = fsmItem["data_width"].as<int>();
        userDataWidth     = qMax(userSpecified, inferredDataWidth);
    }

    const int dataWidth = userDataWidth;

    out << "\n    /* " << fsmName << " : microcode FSM with ";
    if (romMode == "port") {
        out << "programmable ROM */\n";
    } else {
        out << "constant ROM */\n";
    }

    /* Calculate address width based on actual usage */
    int inferredRomDepth = 32; /* Default for port mode */
    if (romMode == "port" && fsmItem["rom_depth"] && fsmItem["rom_depth"].IsScalar()) {
        inferredRomDepth = fsmItem["rom_depth"].as<int>();
    } else if (fsmItem["rom"] && fsmItem["rom"].IsMap()) {
        /* For parameter mode, calculate depth based on max address and next field values */
        int maxAddress = 0;
        for (const auto &romEntry : fsmItem["rom"]) {
            if (romEntry.first.IsScalar()) {
                int address = romEntry.first.as<int>();
                if (address > maxAddress) {
                    maxAddress = address;
                }
                /* Also check next field values if they exist */
                if (romEntry.second.IsMap() && fields.contains("next")) {
                    if (romEntry.second["next"]) {
                        int nextValue = romEntry.second["next"].as<int>();
                        if (nextValue > maxAddress) {
                            maxAddress = nextValue;
                        }
                    }
                }
            }
        }
        inferredRomDepth = maxAddress + 1; /* Exact depth needed */
    }

    /* Allow user to specify ROM depth, but use inferred if user's is smaller */
    int userRomDepth = inferredRomDepth;
    if (fsmItem["rom_depth"] && fsmItem["rom_depth"].IsScalar()) {
        int userSpecified = fsmItem["rom_depth"].as<int>();
        userRomDepth      = qMax(userSpecified, inferredRomDepth);
    }

    const int romDepth = userRomDepth;

    /* Calculate address width from ROM depth */
    int inferredAddressWidth = 1;
    while ((1 << inferredAddressWidth) < romDepth) {
        inferredAddressWidth++;
    }

    /* Allow user to specify address width, but use inferred if user's is smaller */
    int userAddressWidth = inferredAddressWidth;
    if (fsmItem["addr_width"] && fsmItem["addr_width"].IsScalar()) {
        int userSpecified = fsmItem["addr_width"].as<int>();
        userAddressWidth  = qMax(userSpecified, inferredAddressWidth);
    }

    const int addressWidth = userAddressWidth;

    /* Check if user wants parameters instead of localparams for external configuration */
    bool useParameters = false;
    if (fsmItem["use_parameters"] && fsmItem["use_parameters"].IsScalar()) {
        useParameters = fsmItem["use_parameters"].as<bool>();
    }

    QString paramType = useParameters ? "parameter" : "localparam";
    out << "    " << paramType << " " << fsmNameUpper << "_AW = " << addressWidth << ";\n";
    out << "    " << paramType << " " << fsmNameUpper << "_DW = " << (dataWidth - 1) << ";\n\n";

    /* Generate program counter */
    out << "    /* " << fsmName << " program counter */\n";
    out << "    reg [" << fsmNameUpper << "_AW-1:0] " << fsmNameLower << "_pc, " << fsmNameLower
        << "_nxt_pc;\n\n";

    /* Generate ROM array */
    out << "    /* " << fsmName << " ROM array */\n";
    out << "    reg [" << fsmNameUpper << "_DW:0] " << fsmNameLower << "_rom [0:(1<<"
        << fsmNameUpper << "_AW)-1];\n\n";

    if (romMode == "parameter") {
        /* Generate ROM initialization for parameter mode */
        if (fsmItem["rom_init_file"] && fsmItem["rom_init_file"].IsScalar()) {
            /* Use $readmemh for file initialization */
            const QString initFile = QString::fromStdString(
                fsmItem["rom_init_file"].as<std::string>());
            out << "    /* " << fsmName << " ROM initialization from file */\n";
            out << "    initial begin\n";
            out << "        $readmemh(\"" << initFile << "\", " << fsmNameLower << "_rom);\n";
            out << "    end\n\n";
        } else if (fsmItem["rom"] && fsmItem["rom"].IsMap()) {
            /* Generate reset-time ROM initialization */
            out << "    /* " << fsmName << " reset-time ROM initialization */\n";
            out << "    always @(posedge " << clkSignal << " or negedge " << rstSignal
                << ") begin\n";
            out << "        if (!" << rstSignal << ") begin\n";

            for (const auto &romEntry : fsmItem["rom"]) {
                if (!romEntry.first.IsScalar() || !romEntry.second.IsMap()) {
                    continue;
                }

                const int address = romEntry.first.as<int>();

                /* Build the ROM word from fields - sort by high bit position for correct ordering */
                QMap<int, QString> romPartsMap; /* key = high bit position, value = field part */
                for (auto it = fields.begin(); it != fields.end(); ++it) {
                    const QString         &fieldName = it.key();
                    const QPair<int, int> &bitRange  = it.value();

                    QString fieldPart;
                    if (romEntry.second[fieldName.toStdString()]) {
                        const QString fieldValue = QString::fromStdString(
                            romEntry.second[fieldName.toStdString()].as<std::string>());
                        const int fieldWidth = bitRange.second - bitRange.first + 1;
                        /* Handle hexadecimal values properly */
                        if (fieldValue.startsWith("0x") || fieldValue.startsWith("0X")) {
                            QString hexValue = fieldValue.mid(2); /* Remove 0x prefix */
                            fieldPart        = QString("%1'h%2").arg(fieldWidth).arg(hexValue);
                        } else {
                            fieldPart = QString("%1'd%2").arg(fieldWidth).arg(fieldValue);
                        }
                    } else {
                        const int fieldWidth = bitRange.second - bitRange.first + 1;
                        fieldPart            = QString("%1'd0").arg(fieldWidth);
                    }
                    romPartsMap[bitRange.second] = fieldPart;
                }

                /* Convert map to list in descending bit order */
                QStringList romParts;
                for (auto it = romPartsMap.end(); it != romPartsMap.begin();) {
                    --it;
                    romParts.append(it.value());
                }

                /* Calculate total bits used by all defined fields */
                int totalFieldBits = 0;
                for (auto fieldIt = fields.begin(); fieldIt != fields.end(); ++fieldIt) {
                    const QPair<int, int> &bitRange   = fieldIt.value();
                    int                    fieldWidth = bitRange.second - bitRange.first + 1;
                    totalFieldBits += fieldWidth;
                }

                /* Add padding if dataWidth is larger than total field bits */
                if (totalFieldBits < dataWidth) {
                    int paddingBits = dataWidth - totalFieldBits;
                    romParts.prepend(QString("%1'd0").arg(paddingBits));
                }

                out << "            " << fsmNameLower << "_rom[" << address << "] <= {"
                    << romParts.join(", ") << "};\n";
            }

            out << "        end\n";
            out << "    end\n\n";
        }
    } else {
        /* Generate write port for programmable ROM */
        out << "    /* " << fsmName << " write port */\n";
        out << "    always @(posedge " << clkSignal << ")\n";
        out << "        if (" << fsmNameLower << "_rom_we) " << fsmNameLower << "_rom["
            << fsmNameLower << "_rom_addr] <= " << fsmNameLower << "_rom_wdata[" << fsmNameUpper
            << "_DW:0];\n\n";
    }

    /* Generate branch decode logic */
    if (fields.contains("branch") && fields.contains("next")) {
        out << "    /* " << fsmName << " branch decode */\n";
        out << "    always @(*) begin\n";
        out << "        " << fsmNameLower << "_nxt_pc = " << fsmNameLower << "_pc + 1'b1;\n";

        const QPair<int, int> &branchRange = fields["branch"];
        const QPair<int, int> &nextRange   = fields["next"];

        out << "        case (" << fsmNameLower << "_rom[" << fsmNameLower << "_pc]["
            << branchRange.second << ":" << branchRange.first << "])\n";
        out << "            2'd0: " << fsmNameLower << "_nxt_pc = " << fsmNameLower
            << "_pc + 1'b1;\n";
        out << "            2'd1: if (cond)  " << fsmNameLower << "_nxt_pc = " << fsmNameLower
            << "_rom[" << fsmNameLower << "_pc][" << nextRange.second << ":" << nextRange.first
            << "][" << fsmNameUpper << "_AW-1:0];\n";
        out << "            2'd2: if (!cond) " << fsmNameLower << "_nxt_pc = " << fsmNameLower
            << "_rom[" << fsmNameLower << "_pc][" << nextRange.second << ":" << nextRange.first
            << "][" << fsmNameUpper << "_AW-1:0];\n";
        out << "            2'd3: " << fsmNameLower << "_nxt_pc = " << fsmNameLower << "_rom["
            << fsmNameLower << "_pc][" << nextRange.second << ":" << nextRange.first << "]["
            << fsmNameUpper << "_AW-1:0];\n";
        out << "            default: " << fsmNameLower << "_nxt_pc = " << fsmNameLower
            << "_pc + 1'b1;\n";
        out << "        endcase\n";
        out << "    end\n\n";
    }

    /* Generate PC register */
    out << "    /* " << fsmName << " pc register */\n";
    out << "    always @(posedge " << clkSignal << " or negedge " << rstSignal << ")\n";
    out << "        if (!" << rstSignal << ") " << fsmNameLower << "_pc <= " << addressWidth << "'d"
        << rstState << ";\n";
    out << "        else        " << fsmNameLower << "_pc <= " << fsmNameLower << "_nxt_pc;\n\n";

    /* Generate control outputs */
    out << "    /* " << fsmName << " control outputs */\n";
    for (auto it = fields.begin(); it != fields.end(); ++it) {
        const QString         &fieldName = it.key();
        const QPair<int, int> &bitRange  = it.value();

        if (fieldName != "branch" && fieldName != "next") {
            /* Map field names to actual output port names */
            QString outputPortName = fieldName;
            if (fieldName == "ctrl") {
                outputPortName = "ctrl_bus"; /* Map ctrl field to ctrl_bus port */
            }

            if (bitRange.first == bitRange.second) {
                out << "    assign " << outputPortName << " = " << fsmNameLower << "_rom["
                    << fsmNameLower << "_pc][" << bitRange.first << "];\n";
            } else {
                out << "    assign " << outputPortName << " = " << fsmNameLower << "_rom["
                    << fsmNameLower << "_pc][" << bitRange.second << ":" << bitRange.first
                    << "];\n";
            }
        }
    }
    out << "\n";
}
