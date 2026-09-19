#include "qsocverilogutils.h"
#include "qsocconsole.h"
#include "qstaticstringweaver.h"

#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

#include <QSet>

QString QSocVerilogUtils::cleanTypeForWireDeclaration(const QString &typeStr)
{
    if (typeStr.isEmpty()) {
        return {};
    }

    QString cleaned = typeStr;

    /* Strip NUL and other control characters first. Pre-fix a YAML
       `type: "logic\x00 [7:0]"` would leak NUL bytes into the generated
       Verilog, breaking diff/grep tools and many synth flows. */
    cleaned.remove(QRegularExpression("[\\x00-\\x08\\x0B\\x0C\\x0E-\\x1F\\x7F]"));

    /* Remove leading whitespace + keyword + keyword trailing whitespace */
    static const QRegularExpression regularExpression(R"(\s*[A-Za-z_]+\s*(?=\[|\s*$))");
    /* Explanation:
     *   \s*           optional leading whitespace
     *   [A-Za-z_]+    keyword (only letters and underscores)
     *   \s*           whitespace after keyword
     *   (?=\[|\s*$)   only match when followed by '[' or whitespace until end of line
     */
    cleaned.replace(regularExpression, "");

    /* Clean up any remaining whitespace */
    cleaned = cleaned.trimmed();

    return cleaned;
}

QPair<QString, QString> QSocVerilogUtils::parseSignalBitSelect(const QString &signalName)
{
    const QRegularExpression      bitSelectRegex(R"(^([^[]+)(\[\s*\d+\s*(?::\s*\d+)?\s*\])?\s*$)");
    const QRegularExpressionMatch match = bitSelectRegex.match(signalName);

    if (match.hasMatch()) {
        QString baseName  = match.captured(1).trimmed();
        QString bitSelect = match.captured(2);
        return qMakePair(baseName, bitSelect);
    }

    return qMakePair(signalName, QString());
}

QString QSocVerilogUtils::formatConditionForVerilog(const QString &condition)
{
    QString formatted = condition;

    /* Replace simple numeric values with proper Verilog format */
    QRegularExpression              simpleNumRegex("\\b(\\d+)\\b");
    QRegularExpressionMatchIterator it = simpleNumRegex.globalMatch(formatted);
    QStringList                     matches;
    while (it.hasNext()) {
        QRegularExpressionMatch match = it.next();
        matches.append(match.captured(1));
    }

    /* Replace from right to left to preserve positions */
    for (int i = matches.size() - 1; i >= 0; i--) {
        QString num = matches[i];
        QString replacement;
        if (num == "0") {
            replacement = "1'b0";
        } else if (num == "1") {
            replacement = "1'b1";
        } else {
            /* For multi-bit numbers, try to determine width from context */
            int value = num.toInt();
            int width = 1;
            while ((1 << width) <= value) {
                width++;
            }
            replacement = QString("%1'd%2").arg(width).arg(num);
        }
        formatted.replace(
            QRegularExpression(QString("\\b%1\\b").arg(QRegularExpression::escape(num))),
            replacement);
    }

    return formatted;
}

QString QSocVerilogUtils::generateIndent(int level)
{
    return QString("    ").repeated(level); // 4 spaces per indent level
}

bool QSocVerilogUtils::isValidVerilogIdentifier(const QString &identifier)
{
    if (identifier.isEmpty()) {
        return false;
    }

    const auto isAsciiLetter = [](QChar character) {
        const ushort value = character.unicode();
        return (value >= 'A' && value <= 'Z') || (value >= 'a' && value <= 'z');
    };
    const auto isAsciiDigit = [](QChar character) {
        const ushort value = character.unicode();
        return value >= '0' && value <= '9';
    };

    if (!isAsciiLetter(identifier[0]) && identifier[0] != '_') {
        return false;
    }

    for (int i = 1; i < identifier.length(); ++i) {
        const QChar character = identifier[i];
        if (!isAsciiLetter(character) && !isAsciiDigit(character) && character != '_'
            && character != '$') {
            return false;
        }
    }

    static const QSet<QString> reservedWords = {
        "always",
        "and",
        "assign",
        "automatic",
        "begin",
        "buf",
        "bufif0",
        "bufif1",
        "case",
        "casex",
        "casez",
        "cell",
        "cmos",
        "config",
        "deassign",
        "default",
        "defparam",
        "design",
        "disable",
        "edge",
        "else",
        "end",
        "endcase",
        "endconfig",
        "endfunction",
        "endgenerate",
        "endmodule",
        "endprimitive",
        "endspecify",
        "endtable",
        "endtask",
        "event",
        "for",
        "force",
        "forever",
        "fork",
        "function",
        "generate",
        "genvar",
        "highz0",
        "highz1",
        "if",
        "ifnone",
        "incdir",
        "include",
        "initial",
        "inout",
        "input",
        "instance",
        "integer",
        "join",
        "large",
        "liblist",
        "library",
        "localparam",
        "macromodule",
        "medium",
        "module",
        "nand",
        "negedge",
        "nmos",
        "nor",
        "noshowcancelled",
        "not",
        "notif0",
        "notif1",
        "or",
        "output",
        "parameter",
        "pmos",
        "posedge",
        "primitive",
        "pull0",
        "pull1",
        "pulldown",
        "pullup",
        "pulsestyle_ondetect",
        "pulsestyle_onevent",
        "rcmos",
        "real",
        "realtime",
        "reg",
        "release",
        "repeat",
        "rnmos",
        "rpmos",
        "rtran",
        "rtranif0",
        "rtranif1",
        "scalared",
        "showcancelled",
        "signed",
        "small",
        "specify",
        "specparam",
        "strong0",
        "strong1",
        "supply0",
        "supply1",
        "table",
        "task",
        "time",
        "tran",
        "tranif0",
        "tranif1",
        "tri",
        "tri0",
        "tri1",
        "triand",
        "trior",
        "trireg",
        "unsigned",
        "use",
        "vectored",
        "wait",
        "wand",
        "weak0",
        "weak1",
        "while",
        "wire",
        "wor",
        "xnor",
        "xor",
    };

    return !reservedWords.contains(identifier);
}

QString QSocVerilogUtils::escapeVerilogComment(const QString &text)
{
    QString escaped = text;
    // Replace potentially problematic characters in comments
    escaped.replace("*/", "* /"); // Avoid closing block comments accidentally
    escaped.replace("//", "/ /"); // Avoid line comments within comments
    return escaped;
}

QString QSocVerilogUtils::normalizeBitSelect(const QString &bitSelect)
{
    if (bitSelect.isEmpty()) {
        return bitSelect;
    }
    static const QRegularExpression rangeRegex(R"(^\s*\[\s*(\d+)\s*:\s*(\d+)\s*\]\s*$)");
    const QRegularExpressionMatch   rangeMatch = rangeRegex.match(bitSelect);
    if (rangeMatch.hasMatch()) {
        bool      leftOk   = false;
        bool      rightOk  = false;
        const int leftIdx  = rangeMatch.captured(1).toInt(&leftOk);
        const int rightIdx = rangeMatch.captured(2).toInt(&rightOk);
        if (leftOk && rightOk) {
            const int hi = leftIdx >= rightIdx ? leftIdx : rightIdx;
            const int lo = leftIdx >= rightIdx ? rightIdx : leftIdx;
            return QString("[%1:%2]").arg(hi).arg(lo);
        }
    }
    static const QRegularExpression singleRegex(R"(^\s*\[\s*(\d+)\s*\]\s*$)");
    const QRegularExpressionMatch   singleMatch = singleRegex.match(bitSelect);
    if (singleMatch.hasMatch()) {
        bool      ok  = false;
        const int idx = singleMatch.captured(1).toInt(&ok);
        if (ok) {
            return QString("[%1]").arg(idx);
        }
    }
    /* Anything that begins with '[' but does not match the canonical forms
       (e.g. "[]", "[abc]", "[3:]") would otherwise leak verbatim into the
       Verilog and the synth tool would reject it. Drop the malformed
       bracket and let the rest of the pipeline treat the connection as
       full-width. */
    if (bitSelect.trimmed().startsWith('[')) {
        return {};
    }
    return bitSelect;
}

QString QSocVerilogUtils::sanitizeBitSelectInName(const QString &name)
{
    if (name.isEmpty()) {
        return name;
    }
    QString sanitized = name;
    sanitized.replace('[', '_');
    sanitized.replace(':', '_');
    /* Drop the closing bracket entirely so `clk_out[3]` becomes `clk_out_3`
       rather than `clk_out_3_`. */
    sanitized.remove(']');
    return sanitized;
}

bool QSocVerilogUtils::formatFile(const QString &candidatePath)
{
    const QString formatterPath = QStandardPaths::findExecutable("verible-verilog-format");
    if (formatterPath.isEmpty()) {
        QSocConsole::error() << "Verilog formatter not found.";
        return false;
    }

    QSocConsole::info() << "Formatting Verilog file...";

    QProcess formatter;
    /* clang-format off */
    const QString argsStr = QStaticStringWeaver::stripCommonLeadingWhitespace(R"(
        --inplace
        --column_limit 119
        --indentation_spaces 4
        --line_break_penalty 4
        --wrap_spaces 4
        --port_declarations_alignment align
        --port_declarations_indentation indent
        --formal_parameters_alignment align
        --formal_parameters_indentation indent
        --assignment_statement_alignment align
        --enum_assignment_statement_alignment align
        --class_member_variable_alignment align
        --module_net_variable_alignment align
        --named_parameter_alignment align
        --named_parameter_indentation indent
        --named_port_alignment align
        --named_port_indentation indent
        --struct_union_members_alignment align
    )");
    /* clang-format on */

    QStringList args = argsStr.split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
    args << candidatePath;

    formatter.start(formatterPath, args);
    if (!formatter.waitForStarted()) {
        QSocConsole::error() << "failed to start Verilog formatter:" << formatter.errorString();
        return false;
    }
    if (!formatter.waitForFinished()) {
        formatter.kill();
        formatter.waitForFinished();
        QSocConsole::error() << "Verilog formatter timed out.";
        return false;
    }
    if (formatter.exitStatus() != QProcess::NormalExit) {
        QSocConsole::error() << "Verilog formatter terminated abnormally:"
                             << formatter.errorString();
        return false;
    }
    if (formatter.exitCode() != 0) {
        const QString standardError = QString::fromUtf8(formatter.readAllStandardError()).trimmed();
        QSocConsole::error() << "Verilog formatter failed with exit code" << formatter.exitCode()
                             << (standardError.isEmpty() ? QString() : ": " + standardError);
        return false;
    }

    const QFileInfo candidateInfo(candidatePath);
    if (!candidateInfo.exists() || !candidateInfo.isFile() || candidateInfo.isSymbolicLink()) {
        QSocConsole::error() << "Verilog formatter did not leave a regular candidate file";
        return false;
    }
    return true;
}
