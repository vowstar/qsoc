#include "qsocverilogutils.h"

QString QSocVerilogUtils::cleanTypeForWireDeclaration(const QString &typeStr)
{
    if (typeStr.isEmpty()) {
        return {};
    }

    QString cleaned = typeStr;

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
            if (value <= 1) {
                replacement = QString("1'b%1").arg(value);
            } else if (value <= 15) {
                replacement = QString("4'h%1").arg(value, 0, 16);
            } else if (value <= 255) {
                replacement = QString("8'h%1").arg(value, 0, 16);
            } else if (value <= 65535) {
                replacement = QString("16'h%1").arg(value, 0, 16);
            } else {
                replacement = QString("32'h%1").arg(value, 0, 16);
            }
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

    // Verilog identifier must start with letter or underscore
    if (!identifier[0].isLetter() && identifier[0] != '_') {
        return false;
    }

    // Rest must be letters, digits, underscores, or dollar signs
    for (int i = 1; i < identifier.length(); ++i) {
        QChar c = identifier[i];
        if (!c.isLetterOrNumber() && c != '_' && c != '$') {
            return false;
        }
    }

    // Check against Verilog reserved keywords (basic set)
    static const QStringList reservedWords
        = {"always",   "and",         "assign",    "begin",        "buf",        "bufif0",
           "bufif1",   "case",        "casex",     "casez",        "cmos",       "deassign",
           "default",  "defparam",    "disable",   "edge",         "else",       "end",
           "endcase",  "endfunction", "endmodule", "endprimitive", "endspecify", "endtable",
           "endtask",  "event",       "for",       "force",        "forever",    "fork",
           "function", "highz0",      "highz1",    "if",           "ifnone",     "initial",
           "inout",    "input",       "integer",   "join",         "large",      "medium",
           "module",   "nand",        "negedge",   "nmos",         "nor",        "not",
           "notif0",   "notif1",      "or",        "output",       "parameter",  "pmos",
           "posedge",  "primitive",   "pull0",     "pull1",        "pulldown",   "pullup",
           "rcmos",    "real",        "realtime",  "reg",          "release",    "repeat",
           "rnmos",    "rpmos",       "rtran",     "rtranif0",     "rtranif1",   "scalared",
           "small",    "specify",     "specparam", "strength",     "strong0",    "strong1",
           "supply0",  "supply1",     "table",     "task",         "time",       "tran",
           "tranif0",  "tranif1",     "tri",       "tri0",         "tri1",       "triand",
           "trior",    "trireg",      "vectored",  "wait",         "wand",       "weak0",
           "weak1",    "while",       "wire",      "wor",          "xnor",       "xor"};

    return !reservedWords.contains(identifier.toLower());
}

QString QSocVerilogUtils::escapeVerilogComment(const QString &text)
{
    QString escaped = text;
    // Replace potentially problematic characters in comments
    escaped.replace("*/", "* /"); // Avoid closing block comments accidentally
    escaped.replace("//", "/ /"); // Avoid line comments within comments
    return escaped;
}
