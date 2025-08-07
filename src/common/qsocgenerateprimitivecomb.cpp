#include "qsocgenerateprimitivecomb.h"
#include "qsocgeneratemanager.h"
#include "qsocverilogutils.h"
#include <QDebug>
#include <QRegularExpression>
#include <QRegularExpressionMatch>

QSocCombPrimitive::QSocCombPrimitive(QSocGenerateManager *parent)
    : m_parent(parent)
{}

bool QSocCombPrimitive::generateCombLogic(const YAML::Node &netlistData, QTextStream &out)
{
    if (!netlistData["comb"] || !netlistData["comb"].IsSequence()
        || netlistData["comb"].size() == 0) {
        // No comb section or empty - this is valid
        return true;
    }

    /* First pass: collect all outputs that need internal reg declarations */
    QSet<QString> alwaysBlockOutputs;
    for (size_t i = 0; i < netlistData["comb"].size(); ++i) {
        const YAML::Node &combItem = netlistData["comb"][i];
        if (!combItem.IsMap() || !combItem["out"] || !combItem["out"].IsScalar()) {
            continue;
        }
        const QString outputSignal = QString::fromStdString(combItem["out"].as<std::string>());

        /* Check if this output needs an always block */
        if ((combItem["if"] && combItem["if"].IsSequence())
            || (combItem["case"] && combItem["case"].IsScalar() && combItem["cases"]
                && combItem["cases"].IsMap())) {
            alwaysBlockOutputs.insert(outputSignal);
        }
    }

    /* Generate internal reg declarations for always block outputs */
    if (!alwaysBlockOutputs.isEmpty()) {
        out << "\n    /* Internal reg declarations for combinational logic */\n";
        for (const QString &outputSignal : alwaysBlockOutputs) {
            /* Find the port width for this output signal */
            QString regWidth = "";
            if (netlistData["port"] && netlistData["port"].IsMap()) {
                for (const auto &portEntry : netlistData["port"]) {
                    if (portEntry.first.IsScalar()
                        && QString::fromStdString(portEntry.first.as<std::string>())
                               == outputSignal) {
                        if (portEntry.second.IsMap() && portEntry.second["type"]
                            && portEntry.second["type"].IsScalar()) {
                            QString portType = QString::fromStdString(
                                portEntry.second["type"].as<std::string>());
                            if (portType != "logic" && portType != "wire") {
                                /* Extract width from type like "logic[7:0]" */
                                QRegularExpression widthRegex(R"(\[\s*(\d+)\s*:\s*(\d+)\s*\])");
                                QRegularExpressionMatch match = widthRegex.match(portType);
                                if (match.hasMatch()) {
                                    int msb  = match.captured(1).toInt();
                                    int lsb  = match.captured(2).toInt();
                                    regWidth = QString("[%1:%2] ").arg(msb).arg(lsb);
                                }
                            }
                            break;
                        }
                    }
                }
            }
            out << "    reg " << regWidth << outputSignal << "_reg;\n";
        }
        out << "\n    /* Assign internal regs to outputs */\n";
        for (const QString &outputSignal : alwaysBlockOutputs) {
            out << "    assign " << outputSignal << " = " << outputSignal << "_reg;\n";
        }
    }

    out << "\n    /* Combinational logic */\n";

    for (size_t i = 0; i < netlistData["comb"].size(); ++i) {
        const YAML::Node &combItem = netlistData["comb"][i];

        if (!combItem.IsMap() || !combItem["out"] || !combItem["out"].IsScalar()) {
            continue; /* Skip invalid items */
        }

        const QString outputSignal = QString::fromStdString(combItem["out"].as<std::string>());

        if (combItem["expr"] && combItem["expr"].IsScalar()) {
            /* Generate assign statement */
            const QString expression = QString::fromStdString(combItem["expr"].as<std::string>());

            /* Check if bits attribute exists for bit selection */
            QString fullOutputSignal = outputSignal;
            if (combItem["bits"] && combItem["bits"].IsScalar()) {
                const QString bitSelection = QString::fromStdString(
                    combItem["bits"].as<std::string>());
                fullOutputSignal = outputSignal + bitSelection;
            }

            out << "    assign " << fullOutputSignal << " = " << expression << ";\n";
        } else if (combItem["if"] && combItem["if"].IsSequence()) {
            /* Generate always block with if-else logic */
            const QString regSignal = outputSignal + "_reg";
            out << "    always @(*) begin\n";

            /* Set default value if specified */
            if (combItem["default"] && combItem["default"].IsScalar()) {
                const QString defaultValue = QString::fromStdString(
                    combItem["default"].as<std::string>());
                out << "        " << regSignal << " = " << defaultValue << ";\n";
            }

            /* Generate if-else chain */
            bool firstIf = true;
            for (const auto &ifCondition : combItem["if"]) {
                if (!ifCondition.IsMap() || !ifCondition["cond"] || !ifCondition["then"]) {
                    continue; /* Skip invalid conditions */
                }

                const QString condition = QString::fromStdString(
                    ifCondition["cond"].as<std::string>());

                if (firstIf) {
                    out << "        if (" << condition << ") begin\n";
                    firstIf = false;
                } else {
                    out << "        else if (" << condition << ") begin\n";
                }

                /* Generate nested value (could be simple or nested case) */
                QString nestedCode = generateNestedCombValue(ifCondition["then"], regSignal, 3);
                out << nestedCode;
                out << "        end\n";
            }

            out << "    end\n";
        } else if (
            combItem["case"] && combItem["case"].IsScalar() && combItem["cases"]
            && combItem["cases"].IsMap()) {
            /* Generate always block with case statement */
            const QString regSignal = outputSignal + "_reg";
            out << "    always @(*) begin\n";

            /* Set default value if specified */
            if (combItem["default"] && combItem["default"].IsScalar()) {
                const QString defaultValue = QString::fromStdString(
                    combItem["default"].as<std::string>());
                out << "        " << regSignal << " = " << defaultValue << ";\n";
            }

            const QString caseExpression = QString::fromStdString(
                combItem["case"].as<std::string>());
            out << "        case (" << caseExpression << ")\n";

            /* Generate case entries */
            for (const auto &caseEntry : combItem["cases"]) {
                if (!caseEntry.first.IsScalar() || !caseEntry.second.IsScalar()) {
                    continue; /* Skip invalid entries */
                }

                const QString caseValue = QString::fromStdString(caseEntry.first.as<std::string>());
                const QString resultValue = QString::fromStdString(
                    caseEntry.second.as<std::string>());
                out << "            " << caseValue << ": " << regSignal << " = " << resultValue
                    << ";\n";
            }

            /* Add default case if specified */
            if (combItem["default"] && combItem["default"].IsScalar()) {
                const QString defaultValue = QString::fromStdString(
                    combItem["default"].as<std::string>());
                out << "            default: " << regSignal << " = " << defaultValue << ";\n";
            }

            out << "        endcase\n";
            out << "    end\n";
        }

        /* Add blank line between different combinational logic blocks */
        if (i < netlistData["comb"].size() - 1) {
            out << "\n";
        }
    }

    return true;
}

QString QSocCombPrimitive::generateNestedCombValue(
    const YAML::Node &valueNode, const QString &outputSignal, int indentLevel)
{
    QString result;
    QString indent = QSocVerilogUtils::generateIndent(indentLevel);

    if (valueNode.IsScalar()) {
        /* Simple scalar value */
        const QString value = QString::fromStdString(valueNode.as<std::string>());
        result += QString("%1%2 = %3;\n").arg(indent).arg(outputSignal).arg(value);
    } else if (valueNode.IsMap() && valueNode["case"]) {
        /* Nested case statement */
        const QString caseExpression = QString::fromStdString(valueNode["case"].as<std::string>());
        result += QString("%1case (%2)\n").arg(indent).arg(caseExpression);

        /* Generate case entries */
        if (valueNode["cases"] && valueNode["cases"].IsMap()) {
            for (const auto &caseEntry : valueNode["cases"]) {
                if (!caseEntry.first.IsScalar() || !caseEntry.second.IsScalar()) {
                    continue; /* Skip invalid entries */
                }

                const QString caseValue = QString::fromStdString(caseEntry.first.as<std::string>());
                const QString resultValue = QString::fromStdString(
                    caseEntry.second.as<std::string>());
                result += QString("%1    %2: %3 = %4;\n")
                              .arg(indent)
                              .arg(caseValue)
                              .arg(outputSignal)
                              .arg(resultValue);
            }
        }

        /* Add default case if specified */
        if (valueNode["default"] && valueNode["default"].IsScalar()) {
            const QString defaultValue = QString::fromStdString(
                valueNode["default"].as<std::string>());
            result += QString("%1    default: %2 = %3;\n")
                          .arg(indent)
                          .arg(outputSignal)
                          .arg(defaultValue);
        }

        result += QString("%1endcase\n").arg(indent);
    } else {
        /* Unsupported nested structure - fallback to comment */
        result += QString("%1/* FIXME: Unsupported nested structure for %2 */\n")
                      .arg(indent)
                      .arg(outputSignal);
    }

    return result;
}
