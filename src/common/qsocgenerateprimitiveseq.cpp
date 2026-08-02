#include "qsocgenerateprimitiveseq.h"
#include "qsocconsole.h"
#include "qsocgeneratemanager.h"
#include "qsocverilogutils.h"
#include <QDebug>
#include <QRegularExpression>
#include <QRegularExpressionMatch>

#include <vector>

namespace {

QString numericPortWidth(const YAML::Node &netlistData, const QString &baseName)
{
    if (!netlistData["port"] || !netlistData["port"].IsMap()) {
        return {};
    }
    for (const auto &portEntry : netlistData["port"]) {
        if (!portEntry.first.IsScalar()
            || QString::fromStdString(portEntry.first.as<std::string>()) != baseName) {
            continue;
        }
        if (!portEntry.second.IsMap() || !portEntry.second["type"]
            || !portEntry.second["type"].IsScalar()) {
            return {};
        }
        const QString portType = QString::fromStdString(portEntry.second["type"].as<std::string>());
        if (portType == "logic" || portType == "wire") {
            return {};
        }
        const QRegularExpression      widthRegex(R"(\[\s*(\d+)\s*:\s*(\d+)\s*\])");
        const QRegularExpressionMatch match = widthRegex.match(portType);
        if (!match.hasMatch()) {
            return {};
        }
        const int msb = match.captured(1).toInt();
        const int lsb = match.captured(2).toInt();
        return QString("[%1:%2]").arg(msb).arg(lsb);
    }
    return {};
}

} // namespace

QSocSeqPrimitive::QSocSeqPrimitive(QSocGenerateManager *parent)
    : m_parent(parent)
{}

bool QSocSeqPrimitive::generateSeqLogic(const YAML::Node &netlistData, QTextStream &out)
{
    return generateSeqLogicImpl(netlistData, nullptr, out);
}

bool QSocSeqPrimitive::generateSeqLogicWithRanges(
    const YAML::Node             &netlistData,
    const QMap<QString, QString> &declaredSignalRanges,
    QTextStream                  &out)
{
    return generateSeqLogicImpl(netlistData, &declaredSignalRanges, out);
}

bool QSocSeqPrimitive::generateSeqLogicImpl(
    const YAML::Node                   &netlistData,
    const QMap<QString, QString> *const declaredSignalRanges,
    QTextStream                        &out)
{
    if (!netlistData["seq"] || !netlistData["seq"].IsSequence() || netlistData["seq"].size() == 0) {
        // No seq section or empty - this is valid
        return true;
    }

    /* A seq target may be spelled as a net that a top-level port carries.
       Without this redirect the reg is sized from a name that owns no
       declaration, so it degrades to a scalar and its assign references an
       identifier the module never declares. */
    const QMap<QString, QSocGenerateManager::TopPortBinding> topPortRedirect
        = QSocGenerateManager::buildTopPortRedirect(netlistData);
    const auto resolveBase = [&topPortRedirect](const QString &baseName) -> QString {
        return topPortRedirect.value(baseName, {baseName, QString()}).port;
    };
    /* A net bound over a slice carries only that part of the port. Without the
       slice the assign would drive the whole port and silently capture the
       bits the netlist never bound. */
    const auto resolveSlice =
        [&topPortRedirect](const QString &baseName, const QString &ownSlice) -> QString {
        const QString boundSlice = topPortRedirect.value(baseName, {baseName, QString()}).slice;
        return QSocGenerateManager::composeBitSelect(
            boundSlice, ownSlice, QStringLiteral("seq target ") + baseName);
    };

    /* A top-level input already has an external source; a seq reg on it
       would drive the input from inside the module. */
    QSet<QString> inputTopPortNames;
    if (netlistData["port"] && netlistData["port"].IsMap()) {
        for (const auto &portEntry : netlistData["port"]) {
            if (!portEntry.first.IsScalar() || !portEntry.second.IsMap()) {
                continue;
            }
            if (!portEntry.second["direction"] || !portEntry.second["direction"].IsScalar()) {
                continue;
            }
            const QString dir
                = QString::fromStdString(portEntry.second["direction"].as<std::string>()).toLower();
            if (dir == "in" || dir == "input") {
                inputTopPortNames.insert(QString::fromStdString(portEntry.first.as<std::string>()));
            }
        }
    }

    std::vector<bool>          seqOwnsDriverRange(netlistData["seq"].size(), false);
    QMap<QString, QStringList> seqDriverRanges;
    for (size_t i = 0; i < netlistData["seq"].size(); ++i) {
        const YAML::Node &seqItem = netlistData["seq"][i];
        if (!QSocGenerateManager::seqItemCanEmitDriver(seqItem)) {
            continue;
        }
        const auto parsed = QSocGenerateManager::parseSignalBitSelect(
            QString::fromStdString(seqItem["reg"].as<std::string>()));
        const QString regBase = resolveBase(parsed.first);
        if (inputTopPortNames.contains(regBase)) {
            continue;
        }
        seqOwnsDriverRange[i] = QSocGenerateManager::claimDriverRange(
            seqDriverRanges, regBase, resolveSlice(parsed.first, parsed.second));
    }

    /* First pass: collect base names that need internal reg declarations.
       A `reg: counter[3]` form historically formed `counter[3]_reg`, an
       illegal Verilog identifier. Strip the bit-select for the reg name
       and emit a full-width reg per base; the bit-select stays on the
       always-block target so each item still writes the right bit. */
    QStringList   seqRegBases;
    QSet<QString> seenSeqRegBases;
    for (size_t i = 0; i < netlistData["seq"].size(); ++i) {
        const YAML::Node &seqItem = netlistData["seq"][i];
        if (!seqItem.IsMap() || !seqItem["reg"] || !seqItem["reg"].IsScalar()) {
            continue;
        }
        if (!seqOwnsDriverRange[i]) {
            continue;
        }
        const QString regName = QString::fromStdString(seqItem["reg"].as<std::string>());
        const auto    parsed  = QSocGenerateManager::parseSignalBitSelect(regName);
        const QString regBase = resolveBase(parsed.first);
        if (inputTopPortNames.contains(regBase)) {
            continue;
        }
        if (!seenSeqRegBases.contains(regBase)) {
            seenSeqRegBases.insert(regBase);
            seqRegBases.append(regBase);
        }
    }

    /* For each base, also remember the highest bit index any seq item
       writes via `reg: base[bits]`. When there is no top-level port to
       borrow a width from, the reg must still be wide enough to carry
       every bit-selected target. Also remember the union of bit-selects
       so the assigns can target the same slices instead of blanket-
       assigning the whole port (the blanket form silently overrides any
       comb that drives a different slice of the same port). */
    QMap<QString, int>           seqRegMaxBit;
    QMap<QString, QSet<QString>> seqRegBitSelects;
    QMap<QString, bool>          seqRegHasFullWrite;
    for (size_t i = 0; i < netlistData["seq"].size(); ++i) {
        const YAML::Node &seqItem = netlistData["seq"][i];
        if (!seqItem.IsMap() || !seqItem["reg"] || !seqItem["reg"].IsScalar()) {
            continue;
        }
        if (!seqOwnsDriverRange[i]) {
            continue;
        }
        const QString regName = QString::fromStdString(seqItem["reg"].as<std::string>());
        const auto    parsed  = QSocGenerateManager::parseSignalBitSelect(regName);
        const QString regBase = resolveBase(parsed.first);
        if (inputTopPortNames.contains(regBase)) {
            continue;
        }
        const QString bitSelect = resolveSlice(parsed.first, parsed.second);
        if (bitSelect.isEmpty()) {
            seqRegHasFullWrite.insert(regBase, true);
            continue;
        }
        seqRegBitSelects[regBase].insert(bitSelect);
        const QRegularExpression      widthRegex(R"(\[\s*(\d+)\s*(?::\s*(\d+))?\s*\])");
        const QRegularExpressionMatch match = widthRegex.match(bitSelect);
        if (!match.hasMatch()) {
            continue;
        }
        bool      ok  = false;
        const int idx = match.captured(1).toInt(&ok);
        if (ok && idx > seqRegMaxBit.value(regBase, -1)) {
            seqRegMaxBit.insert(regBase, idx);
        }
    }

    /* Warn when a seq reg targets a name that is neither a top-level
       port nor a declared net. Pre-fix the assign emitted
       `assign ghost_reg = ghost_reg_reg;` against an undeclared
       identifier. */
    {
        QSet<QString> seqKnownTargets;
        if (netlistData["port"] && netlistData["port"].IsMap()) {
            for (const auto &portEntry : netlistData["port"]) {
                if (portEntry.first.IsScalar()) {
                    seqKnownTargets.insert(
                        QString::fromStdString(portEntry.first.as<std::string>()));
                }
            }
        }
        if (netlistData["net"] && netlistData["net"].IsMap()) {
            for (const auto &netEntry : netlistData["net"]) {
                if (netEntry.first.IsScalar()) {
                    seqKnownTargets.insert(QString::fromStdString(netEntry.first.as<std::string>()));
                }
            }
        }
        for (const QString &baseName : seqRegBases) {
            if (!seqKnownTargets.contains(baseName)) {
                QSocConsole::warn() << "seq reg" << baseName
                                    << "is not declared as a port or net - the assign will "
                                       "reference an undeclared identifier";
            }
        }
    }

    /* Generate internal reg declarations for sequential outputs */
    if (!seqRegBases.isEmpty()) {
        out << "\n    /* Internal reg declarations for sequential logic */\n";
        for (const QString &baseName : seqRegBases) {
            QString regWidth;
            if (declaredSignalRanges != nullptr) {
                regWidth                = declaredSignalRanges->value(baseName);
                const QString portWidth = numericPortWidth(netlistData, baseName);
                if (regWidth.count('[') == 1 && !portWidth.isEmpty()) {
                    regWidth = portWidth;
                }
                if (!regWidth.isEmpty()) {
                    regWidth += " ";
                }
            } else {
                regWidth = numericPortWidth(netlistData, baseName);
                if (!regWidth.isEmpty()) {
                    regWidth += " ";
                }
            }
            /* Without a packed range, size the reg from the highest bit-select
               we saw, otherwise it would be scalar and any [N] write would
               be a part-select on a non-vector reg. */
            const bool declared = declaredSignalRanges != nullptr
                                  && declaredSignalRanges->contains(baseName);
            if (!declared && regWidth.isEmpty() && seqRegMaxBit.contains(baseName)
                && seqRegMaxBit.value(baseName) > 0) {
                regWidth = QString("[%1:0] ").arg(seqRegMaxBit.value(baseName));
            }
            out << "    reg " << regWidth << baseName << "_reg;\n";
        }
        /* Build the set of (base, slice) targets the comb section already
           drives. Two `assign foo = ...;` lines on the same target are a
           Verilog multi-driver conflict, and pre-fix this seq vs comb
           split silently emitted both. */
        QMap<QString, QStringList> combSlicesByBase;
        if (netlistData["comb"] && netlistData["comb"].IsSequence()) {
            for (size_t combIdx = 0; combIdx < netlistData["comb"].size(); ++combIdx) {
                const YAML::Node &combItem = netlistData["comb"][combIdx];
                if (!combItem.IsMap() || !combItem["out"] || !combItem["out"].IsScalar()) {
                    continue;
                }
                const QString outSig  = QString::fromStdString(combItem["out"].as<std::string>());
                const auto    parsed  = QSocGenerateManager::parseSignalBitSelect(outSig);
                QString       slice   = parsed.second;
                const QString outBase = resolveBase(parsed.first);
                if (combItem["bits"] && combItem["bits"].IsScalar()) {
                    slice = QSocVerilogUtils::normalizeBitSelect(
                        QString::fromStdString(combItem["bits"].as<std::string>()));
                }
                combSlicesByBase[outBase].append(resolveSlice(parsed.first, slice));
            }
        }
        /* A whole-base comb assign overlaps every seq slice of the base, so
           the conflict is judged on bit ranges, not string equality. */
        const auto combDrives = [&combSlicesByBase](const QString &base, const QString &slice) {
            for (const QString &combSlice : combSlicesByBase.value(base)) {
                if (combSlice.isEmpty() || slice.isEmpty()
                    || QSocGenerateManager::doBitRangesOverlap(combSlice, slice)) {
                    return true;
                }
            }
            return false;
        };

        out << "\n    /* Assign internal regs to outputs */\n";
        for (const QString &baseName : seqRegBases) {
            const bool           hasFullWrite = seqRegHasFullWrite.value(baseName, false);
            const QSet<QString> &slices       = seqRegBitSelects[baseName];
            if (hasFullWrite || slices.isEmpty()) {
                if (combDrives(baseName, QString())) {
                    QSocConsole::warn() << "seq and comb both drive" << baseName
                                        << "- multi-driver conflict in synth";
                    out << "    /* FIXME: comb also drives " << baseName
                        << " - multi-driver conflict */\n";
                }
                out << "    assign " << baseName << " = " << baseName << "_reg;\n";
            } else {
                /* Per-slice assigns. A blanket `assign base = base_reg;` would
                   over-drive any comb that targets a different slice of the
                   same port. */
                QStringList sortedSlices = slices.values();
                sortedSlices.sort();
                for (const QString &slice : sortedSlices) {
                    if (combDrives(baseName, slice)) {
                        QSocConsole::warn() << "seq and comb both drive" << baseName << slice
                                            << "- multi-driver conflict in synth";
                        out << "    /* FIXME: comb also drives " << baseName << slice
                            << " - multi-driver conflict */\n";
                    }
                    out << "    assign " << baseName << slice << " = " << baseName << "_reg"
                        << slice << ";\n";
                }
            }
        }
    }

    out << "\n    /* Sequential logic */\n";

    /* Build a set of known signal names so we can warn when seq references
       a clk/rst that is not declared anywhere in the netlist. The synth
       tool would catch this, but the qsoc warning points the user back to
       the source line. */
    QSet<QString> knownSignals;
    if (netlistData["port"] && netlistData["port"].IsMap()) {
        for (const auto &portEntry : netlistData["port"]) {
            if (portEntry.first.IsScalar()) {
                knownSignals.insert(QString::fromStdString(portEntry.first.as<std::string>()));
            }
        }
    }
    if (netlistData["net"] && netlistData["net"].IsMap()) {
        for (const auto &netEntry : netlistData["net"]) {
            if (netEntry.first.IsScalar()) {
                knownSignals.insert(QString::fromStdString(netEntry.first.as<std::string>()));
            }
        }
    }

    for (size_t i = 0; i < netlistData["seq"].size(); ++i) {
        const YAML::Node &seqItem = netlistData["seq"][i];

        if (!QSocGenerateManager::seqItemCanEmitDriver(seqItem)) {
            continue; /* Skip invalid items */
        }

        const QString regName   = QString::fromStdString(seqItem["reg"].as<std::string>());
        const auto    parsedReg = QSocGenerateManager::parseSignalBitSelect(regName);
        const QString regBase   = resolveBase(parsedReg.first);
        if (inputTopPortNames.contains(regBase)) {
            QSocConsole::warn() << "seq writes to top-level input port" << regBase
                                << "; cannot drive an input from inside the module - "
                                   "skipping the always block";
            out << "    /* FIXME: seq tried to drive top-level input " << regBase
                << " - check the source netlist */\n";
            continue;
        }
        const QString regBitSlice = resolveSlice(parsedReg.first, parsedReg.second);
        if (!seqOwnsDriverRange[i]) {
            const QString fullRegName = regBase + regBitSlice;
            QSocConsole::warn() << "seq has overlapping process driver for" << fullRegName
                                << "; keeping the first - check the source netlist";
            out << "    /* FIXME: overlapping seq process driver for " << fullRegName
                << " skipped - check the source netlist */\n";
            continue;
        }
        const QString regSignal = regBase + "_reg" + regBitSlice;
        const QString clkSignal = QString::fromStdString(seqItem["clk"].as<std::string>());
        if (!knownSignals.contains(clkSignal)) {
            QSocConsole::warn() << "seq for" << regName << "uses clk" << clkSignal
                                << "which is not declared as a port or net";
        }
        if (seqItem["rst"] && seqItem["rst"].IsScalar()) {
            const QString rstSignal = QString::fromStdString(seqItem["rst"].as<std::string>());
            if (!knownSignals.contains(rstSignal)) {
                QSocConsole::warn() << "seq for" << regName << "uses rst" << rstSignal
                                    << "which is not declared as a port or net";
            }
        }

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
