#include "qsocgenerateprimitiveseq.h"
#include "qsocgeneratemanager.h"
#include "qsocverilogutils.h"
#include <QDebug>
#include <QRegularExpression>
#include <QRegularExpressionMatch>

QSocSeqPrimitive::QSocSeqPrimitive(QSocGenerateManager *parent)
    : m_parent(parent)
{}

bool QSocSeqPrimitive::generateSeqLogic(const YAML::Node &netlistData, QTextStream &out)
{
    if (!netlistData["seq"] || !netlistData["seq"].IsSequence() || netlistData["seq"].size() == 0) {
        // No seq section or empty - this is valid
        return true;
    }

    /* First pass: collect all outputs that need internal reg declarations */
    QSet<QString> seqRegOutputs;
    for (size_t i = 0; i < netlistData["seq"].size(); ++i) {
        const YAML::Node &seqItem = netlistData["seq"][i];
        if (!seqItem.IsMap() || !seqItem["reg"] || !seqItem["reg"].IsScalar()) {
            continue;
        }
        const QString regName = QString::fromStdString(seqItem["reg"].as<std::string>());
        seqRegOutputs.insert(regName);
    }

    /* Generate internal reg declarations for sequential outputs */
    if (!seqRegOutputs.isEmpty()) {
        out << "\n    /* Internal reg declarations for sequential logic */\n";
        for (const QString &regName : seqRegOutputs) {
            /* Find the port width for this output signal */
            QString regWidth = "";
            if (netlistData["port"] && netlistData["port"].IsMap()) {
                for (const auto &portEntry : netlistData["port"]) {
                    if (portEntry.first.IsScalar()
                        && QString::fromStdString(portEntry.first.as<std::string>()) == regName) {
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
            out << "    reg " << regWidth << regName << "_reg;\n";
        }
        out << "\n    /* Assign internal regs to outputs */\n";
        for (const QString &regName : seqRegOutputs) {
            out << "    assign " << regName << " = " << regName << "_reg;\n";
        }
    }

    out << "\n    /* Sequential logic */\n";

    for (size_t i = 0; i < netlistData["seq"].size(); ++i) {
        const YAML::Node &seqItem = netlistData["seq"][i];

        if (!seqItem.IsMap() || !seqItem["reg"] || !seqItem["clk"] || !seqItem["reg"].IsScalar()
            || !seqItem["clk"].IsScalar()) {
            continue; /* Skip invalid items */
        }

        const QString regName   = QString::fromStdString(seqItem["reg"].as<std::string>());
        const QString regSignal = regName + "_reg";
        const QString clkSignal = QString::fromStdString(seqItem["clk"].as<std::string>());

        /* Get edge type (default to posedge) */
        QString edgeType = "posedge";
        if (seqItem["edge"] && seqItem["edge"].IsScalar()) {
            const QString edge = QString::fromStdString(seqItem["edge"].as<std::string>());
            if (edge == "neg") {
                edgeType = "negedge";
            }
        }

        /* Generate always block */
        out << "    always @(" << edgeType << " " << clkSignal;

        /* Add reset signal to sensitivity list if present */
        if (seqItem["rst"] && seqItem["rst"].IsScalar()) {
            const QString rstSignal = QString::fromStdString(seqItem["rst"].as<std::string>());
            /* Assume asynchronous reset for now */
            out << " or negedge " << rstSignal;
        }

        out << ") begin\n";

        /* Handle reset logic if present */
        if (seqItem["rst"] && seqItem["rst_val"] && seqItem["rst_val"].IsScalar()) {
            const QString rstSignal = QString::fromStdString(seqItem["rst"].as<std::string>());
            const QString rstValue  = QString::fromStdString(seqItem["rst_val"].as<std::string>());

            out << "        if (!" << rstSignal << ") begin\n";
            out << "            " << regSignal << " <= " << rstValue << ";\n";
            out << "        end else begin\n";

            /* Generate main logic with additional indentation */
            generateSeqLogicContent(seqItem, regSignal, out, 3);

            out << "        end\n";
        } else {
            /* Generate main logic without reset */
            generateSeqLogicContent(seqItem, regSignal, out, 2);
        }

        out << "    end\n";

        /* Add blank line between different sequential logic blocks */
        if (i < netlistData["seq"].size() - 1) {
            out << "\n";
        }
    }

    return true;
}

void QSocSeqPrimitive::generateSeqLogicContent(
    const YAML::Node &seqItem, const QString &regName, QTextStream &out, int indentLevel)
{
    QString indent = QSocVerilogUtils::generateIndent(indentLevel);

    /* Check for enable signal */
    if (seqItem["enable"] && seqItem["enable"].IsScalar()) {
        const QString enableSignal = QString::fromStdString(seqItem["enable"].as<std::string>());
        out << indent << "if (" << enableSignal << ") begin\n";
        /* Increase indentation for enabled logic */
        indent = QSocVerilogUtils::generateIndent(indentLevel + 1);
    }

    /* Generate logic based on type */
    if (seqItem["next"] && seqItem["next"].IsScalar()) {
        /* Simple next-state assignment */
        const QString nextValue = QString::fromStdString(seqItem["next"].as<std::string>());
        out << indent << regName << " <= " << nextValue << ";\n";
    } else if (seqItem["if"] && seqItem["if"].IsSequence()) {
        /* Conditional logic using if-else chain */

        /* Set default value if specified */
        if (seqItem["default"] && seqItem["default"].IsScalar()) {
            const QString defaultValue = QString::fromStdString(
                seqItem["default"].as<std::string>());
            out << indent << regName << " <= " << defaultValue << ";\n";
        }

        /* Generate if-else chain */
        bool firstIf = true;
        for (const auto &ifCondition : seqItem["if"]) {
            if (!ifCondition.IsMap() || !ifCondition["cond"] || !ifCondition["then"]
                || !ifCondition["cond"].IsScalar()) {
                continue; /* Skip invalid conditions */
            }

            const QString condition = QString::fromStdString(ifCondition["cond"].as<std::string>());

            if (firstIf) {
                out << indent << "if (" << condition << ") begin\n";
                firstIf = false;
            } else {
                out << indent << "else if (" << condition << ") begin\n";
            }

            /* Handle both scalar and nested 'then' values */
            if (ifCondition["then"].IsScalar()) {
                /* Simple scalar assignment */
                const QString thenValue = QString::fromStdString(
                    ifCondition["then"].as<std::string>());
                out << indent << "    " << regName << " <= " << thenValue << ";\n";
            } else if (ifCondition["then"].IsMap()) {
                /* Nested structure - use helper function */
                QString nestedCode
                    = generateNestedSeqValue(ifCondition["then"], regName, indentLevel + 1);
                out << nestedCode;
            }

            out << indent << "end\n";
        }
    }

    /* Close enable block if present */
    if (seqItem["enable"] && seqItem["enable"].IsScalar()) {
        /* Remove the extra indentation */
        QString outerIndent = QSocVerilogUtils::generateIndent(indentLevel);
        out << outerIndent << "end\n";
    }
}

QString QSocSeqPrimitive::generateNestedSeqValue(
    const YAML::Node &valueNode, const QString &regName, int indentLevel)
{
    QString result;
    QString indent = QSocVerilogUtils::generateIndent(indentLevel);

    if (valueNode.IsScalar()) {
        /* Simple scalar value */
        const QString value = QString::fromStdString(valueNode.as<std::string>());
        result += QString("%1%2 <= %3;\n").arg(indent).arg(regName).arg(value);
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
                result += QString("%1    %2: %3 <= %4;\n")
                              .arg(indent)
                              .arg(caseValue)
                              .arg(regName)
                              .arg(resultValue);
            }
        }

        /* Add default case if specified */
        if (valueNode["default"] && valueNode["default"].IsScalar()) {
            const QString defaultValue = QString::fromStdString(
                valueNode["default"].as<std::string>());
            result
                += QString("%1    default: %2 <= %3;\n").arg(indent).arg(regName).arg(defaultValue);
        }

        result += QString("%1endcase\n").arg(indent);
    } else {
        /* Unsupported nested structure - fallback to comment */
        result += QString("%1/* FIXME: Unsupported nested structure for %2 */\n")
                      .arg(indent)
                      .arg(regName);
    }

    return result;
}
