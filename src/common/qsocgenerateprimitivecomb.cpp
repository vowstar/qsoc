#include "qsocgenerateprimitivecomb.h"
#include "qsocconsole.h"
#include "qsocgeneratemanager.h"
#include "qsocverilogutils.h"
#include <QDebug>
#include <QMap>
#include <QRegularExpression>
#include <QRegularExpressionMatch>

#include <vector>

namespace {

struct CombTarget
{
    QString requestedBase;
    QString emittedBase;
    QString slice;
    bool    topInput = false;
    bool    known    = false;
};

bool usesProcessBlock(const YAML::Node &combItem)
{
    return (combItem["if"] && combItem["if"].IsSequence())
           || (combItem["case"] && combItem["case"].IsScalar() && combItem["cases"]
               && combItem["cases"].IsMap());
}

CombTarget resolveCombTarget(
    const YAML::Node                                         &combItem,
    const QMap<QString, QSocGenerateManager::TopPortBinding> &topPortRedirect,
    const QSet<QString>                                      &inputTopPortNames,
    const QSet<QString>                                      &knownTargets)
{
    const QString output = QString::fromStdString(combItem["out"].as<std::string>());
    const auto    parsed = QSocGenerateManager::parseSignalBitSelect(output);
    CombTarget    target;
    target.requestedBase = parsed.first;
    const QSocGenerateManager::TopPortBinding binding
        = topPortRedirect.value(target.requestedBase, {target.requestedBase, QString()});
    target.emittedBase = binding.port;
    target.slice       = parsed.second;
    if (combItem["bits"] && combItem["bits"].IsScalar()) {
        target.slice = QSocVerilogUtils::normalizeBitSelect(
            QString::fromStdString(combItem["bits"].as<std::string>()));
    }
    /* A net bound over a slice denotes just that part of the port, so an
       inner select composes with the binding instead of replacing it. */
    target.slice = QSocGenerateManager::composeBitSelect(
        binding.slice, target.slice, QStringLiteral("comb target ") + target.requestedBase);
    target.topInput = inputTopPortNames.contains(target.emittedBase);
    target.known    = knownTargets.contains(target.requestedBase)
                      || knownTargets.contains(target.emittedBase);
    return target;
}

int highestSelectedBit(const QString &slice)
{
    const QRegularExpression      regex(R"(^\[(\d+)(?::(\d+))?\]$)");
    const QRegularExpressionMatch match = regex.match(slice);
    if (!match.hasMatch()) {
        return -1;
    }
    bool      firstOk = false;
    const int first   = match.captured(1).toInt(&firstOk);
    if (!firstOk || match.captured(2).isEmpty()) {
        return firstOk ? first : -1;
    }
    bool      secondOk = false;
    const int second   = match.captured(2).toInt(&secondOk);
    return secondOk ? qMax(first, second) : first;
}

} // namespace

QSocCombPrimitive::QSocCombPrimitive(QSocGenerateManager *parent)
    : m_parent(parent)
{}

bool QSocCombPrimitive::generateCombLogic(
    const YAML::Node             &netlistData,
    const QMap<QString, QString> &declaredSignalRanges,
    QTextStream                  &out)
{
    if (!netlistData["comb"] || !netlistData["comb"].IsSequence()
        || netlistData["comb"].size() == 0) {
        // No comb section or empty - this is valid
        return true;
    }

    /* Redirect a net to the port that carries it. When a top-level port
       declares `connect: <net>`, the wire-decl pass skips emitting `<net>`
       as a wire; the port itself serves as the wire. A comb item that
       writes to `<net>` would otherwise reference an undefined identifier.
       Also remember top-port direction so we can warn when comb tries to
       drive an input port (illegal Verilog), and collect every name that
       could legally appear on a comb assign LHS so we can warn on typos. */
    const QMap<QString, QSocGenerateManager::TopPortBinding> topPortRedirect
        = QSocGenerateManager::buildTopPortRedirect(netlistData);
    QMap<QString, QString> portDirection;
    QSet<QString>          inputTopPortNames;
    QSet<QString>          knownTargets;
    for (auto redirectIt = topPortRedirect.constBegin(); redirectIt != topPortRedirect.constEnd();
         ++redirectIt) {
        knownTargets.insert(redirectIt.key());
    }
    if (netlistData["port"] && netlistData["port"].IsMap()) {
        for (const auto &portEntry : netlistData["port"]) {
            if (!portEntry.first.IsScalar() || !portEntry.second.IsMap()) {
                continue;
            }
            const QString portName = QString::fromStdString(portEntry.first.as<std::string>());
            knownTargets.insert(portName);
            QString dir;
            if (portEntry.second["direction"] && portEntry.second["direction"].IsScalar()) {
                dir = QString::fromStdString(portEntry.second["direction"].as<std::string>())
                          .toLower();
                portDirection.insert(portName, dir);
                if (dir == "in" || dir == "input") {
                    inputTopPortNames.insert(portName);
                }
            }
            if (portEntry.second["connect"] && portEntry.second["connect"].IsScalar()) {
                const QString netName = QString::fromStdString(
                    portEntry.second["connect"].as<std::string>());
                knownTargets.insert(netName);
                if (dir == "in" || dir == "input") {
                    inputTopPortNames.insert(netName);
                }
            }
        }
    }
    if (netlistData["net"] && netlistData["net"].IsMap()) {
        for (const auto &netEntry : netlistData["net"]) {
            if (netEntry.first.IsScalar()) {
                knownTargets.insert(QString::fromStdString(netEntry.first.as<std::string>()));
            }
        }
    }

    /* First pass: collect process targets and internal reg requirements. */
    std::vector<CombTarget>    combTargets(netlistData["comb"].size());
    QStringList                processOutputBases;
    QSet<QString>              processFullOutputs;
    QMap<QString, QStringList> processOutputSlices;
    QMap<QString, int>         processMaxBits;
    for (size_t i = 0; i < netlistData["comb"].size(); ++i) {
        const YAML::Node &combItem = netlistData["comb"][i];
        if (!combItem.IsMap() || !combItem["out"] || !combItem["out"].IsScalar()) {
            continue;
        }
        CombTarget &target = combTargets[i];
        target = resolveCombTarget(combItem, topPortRedirect, inputTopPortNames, knownTargets);
        if (!usesProcessBlock(combItem)) {
            continue;
        }
        /* A top-level input already has an external source; registering it
           would declare a reg and drive the input from inside the module.
           The emission loop carries the diagnostic. */
        if (target.topInput) {
            continue;
        }
        if (!processOutputBases.contains(target.emittedBase)) {
            processOutputBases.append(target.emittedBase);
        }
        if (target.slice.isEmpty()) {
            processFullOutputs.insert(target.emittedBase);
        } else {
            QStringList &slices = processOutputSlices[target.emittedBase];
            if (!slices.contains(target.slice)) {
                slices.append(target.slice);
            }
            const int maxBit = highestSelectedBit(target.slice);
            if (maxBit > processMaxBits.value(target.emittedBase, -1)) {
                processMaxBits.insert(target.emittedBase, maxBit);
            }
        }
    }

    /* Generate internal reg declarations for always block outputs */
    /* One registry for every target this section assigns, whatever form
       produced it. A whole-base assign and a sliced one overlap just as two
       identical strings do, so conflicts are judged on bit ranges. */
    QMap<QString, QStringList> seenAssignSlices;
    const auto conflictsWithSeen = [&seenAssignSlices](const QString &base, const QString &slice) {
        for (const QString &seen : seenAssignSlices.value(base)) {
            if (seen.isEmpty() || slice.isEmpty()
                || QSocGenerateManager::doBitRangesOverlap(seen, slice)) {
                return true;
            }
        }
        return false;
    };

    if (!processOutputBases.isEmpty()) {
        out << "\n    /* Internal reg declarations for combinational logic */\n";
        for (const QString &baseName : processOutputBases) {
            QString regWidth = declaredSignalRanges.value(baseName);
            if (!regWidth.isEmpty()) {
                regWidth += " ";
            }
            if (!declaredSignalRanges.contains(baseName) && processMaxBits.value(baseName, -1) > 0) {
                regWidth = QString("[%1:0] ").arg(processMaxBits.value(baseName));
            }
            out << "    reg " << regWidth << baseName << "_reg;\n";
        }
        out << "\n    /* Assign internal regs to outputs */\n";
        for (const QString &baseName : processOutputBases) {
            const QStringList slices = processOutputSlices.value(baseName);
            if (processFullOutputs.contains(baseName) || slices.isEmpty()) {
                out << "    assign " << baseName << " = " << baseName << "_reg;\n";
                seenAssignSlices[baseName].append(QString());
                continue;
            }
            for (const QString &slice : slices) {
                out << "    assign " << baseName << slice << " = " << baseName << "_reg" << slice
                    << ";\n";
                seenAssignSlices[baseName].append(slice);
            }
        }
    }

    out << "\n    /* Combinational logic */\n";

    /* Track which (target, bit-slice) we have already emitted an assign for.
       Duplicate `out:` lines silently produced two `assign foo[3:0] = ...;`
       statements, a guaranteed multi-driver conflict. Keep the first. */
    for (size_t i = 0; i < netlistData["comb"].size(); ++i) {
        const YAML::Node &combItem = netlistData["comb"][i];

        if (!combItem.IsMap() || !combItem["out"] || !combItem["out"].IsScalar()) {
            continue; /* Skip invalid items */
        }

        const CombTarget &target        = combTargets[i];
        const bool        hasExpression = combItem["expr"] && combItem["expr"].IsScalar();
        /* Driving a top-level INPUT port from inside the module is illegal
           Verilog whatever form the item takes. Warn and skip the emission
           so the offending driver never lands in the output. */
        if (!hasExpression && target.topInput && usesProcessBlock(combItem)) {
            QSocConsole::warn() << "comb writes to top-level input port" << target.emittedBase
                                << "; cannot drive an input from inside the module - "
                                   "skipping the always block";
            out << "    /* FIXME: comb tried to drive top-level input " << target.emittedBase
                << " - check the source netlist */\n";
            continue;
        }
        if (hasExpression) {
            if (target.topInput) {
                QSocConsole::warn() << "comb writes to top-level input port" << target.emittedBase
                                    << "; cannot drive an input from inside the module - "
                                       "skipping the assign";
                out << "    /* FIXME: comb tried to drive top-level input " << target.emittedBase
                    << " - check the source netlist */\n";
                continue;
            }

            /* Writing to a name that is neither a top port nor a declared
               net can emit an undeclared identifier. Preserve compatibility
               while surfacing the typo. */
            if (!target.known) {
                QSocConsole::warn() << "comb writes to" << target.emittedBase
                                    << "which is not declared as a port or net - "
                                       "the assign will reference an undeclared identifier";
                out << "    /* FIXME: comb target " << target.emittedBase
                    << " is not declared as a port or net */\n";
            }
            /* Generate assign statement */
            const QString expression = QString::fromStdString(combItem["expr"].as<std::string>());
            const QString fullOutputSignal = target.emittedBase + target.slice;

            if (conflictsWithSeen(target.emittedBase, target.slice)) {
                QSocConsole::warn() << "comb has duplicate driver for" << fullOutputSignal
                                    << "; keeping the first - check the source netlist";
                out << "    /* FIXME: duplicate comb driver for " << fullOutputSignal
                    << " skipped - check the source netlist */\n";
                continue;
            }
            seenAssignSlices[target.emittedBase].append(target.slice);

            out << "    assign " << fullOutputSignal << " = " << expression << ";\n";
        } else if (combItem["if"] && combItem["if"].IsSequence()) {
            /* Generate always block with if-else logic */
            const QString regSignal = target.emittedBase + "_reg" + target.slice;
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
            const QString regSignal = target.emittedBase + "_reg" + target.slice;
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
