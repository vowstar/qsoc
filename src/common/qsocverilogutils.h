#ifndef QSOCVERILOGUTILS_H
#define QSOCVERILOGUTILS_H

#include <QPair>
#include <QRegularExpression>
#include <QRegularExpressionMatch>
#include <QRegularExpressionMatchIterator>
#include <QString>

/**
 * @brief Utility class for common Verilog code generation functions
 * 
 * This class provides static helper methods used across different primitive generators
 * to maintain consistency in Verilog code generation.
 */
class QSocVerilogUtils
{
public:
    /**
     * @brief Clean type string for wire declaration
     * @param typeStr Original type string (e.g., "logic [7:0]")
     * @return Cleaned type string with keywords removed (e.g., "[7:0]")
     */
    static QString cleanTypeForWireDeclaration(const QString &typeStr);

    /**
     * @brief Parse signal name to extract base name and bit selection
     * @param signalName Full signal name (e.g., "data[7:0]")
     * @return QPair<baseName, bitSelect> (e.g., {"data", "[7:0]"})
     */
    static QPair<QString, QString> parseSignalBitSelect(const QString &signalName);

    /**
     * @brief Format condition expression for Verilog output
     * @param condition Original condition string
     * @return Formatted Verilog condition with proper literals
     */
    static QString formatConditionForVerilog(const QString &condition);

    /**
     * @brief Generate indentation string
     * @param level Indentation level (each level = 4 spaces)
     * @return Indentation string
     */
    static QString generateIndent(int level);

    /**
     * @brief Check if a string is a valid Verilog identifier
     * @param identifier String to check
     * @return true if valid Verilog identifier
     */
    static bool isValidVerilogIdentifier(const QString &identifier);

    /**
     * @brief Escape string for Verilog comments
     * @param text Text to escape
     * @return Escaped text safe for Verilog comments
     */
    static QString escapeVerilogComment(const QString &text);
};

#endif // QSOCVERILOGUTILS_H
