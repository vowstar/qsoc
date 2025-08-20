#ifndef QSOCGENERATEPRIMITIVECOMB_H
#define QSOCGENERATEPRIMITIVECOMB_H

#include <yaml-cpp/yaml.h>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTextStream>

// Forward declaration
class QSocGenerateManager;

/**
 * @brief Combinational logic primitive generator for QSoC
 *
 * This class generates combinational logic Verilog code including:
 * - Simple assign statements with expressions
 * - Always blocks with if-else chains
 * - Always blocks with case statements
 * - Nested combinational structures
 * - Internal register declarations for always block outputs
 */
class QSocCombPrimitive
{
public:
    /**
     * @brief Constructor
     * @param parent Pointer to parent QSocGenerateManager
     */
    explicit QSocCombPrimitive(QSocGenerateManager *parent = nullptr);

    /**
     * @brief Generate combinational logic from YAML configuration
     * @param netlistData YAML node containing the full netlist
     * @param out Output text stream for generated Verilog
     * @return true if generation successful, false otherwise
     */
    bool generateCombLogic(const YAML::Node &netlistData, QTextStream &out);

private:
    /**
     * @brief Generate nested combinational logic value (for if/case nesting)
     * @param valueNode YAML node containing the value specification
     * @param outputSignal The output signal name
     * @param indentLevel The indentation level for proper formatting
     * @return Generated Verilog code string
     */
    QString generateNestedCombValue(
        const YAML::Node &valueNode, const QString &outputSignal, int indentLevel);

private:
    QSocGenerateManager *m_parent; // Parent manager for accessing utilities
};

#endif // QSOCGENERATEPRIMITIVECOMB_H
