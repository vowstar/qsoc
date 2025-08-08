#ifndef QSOCGENERATEPRIMITIVESEQ_H
#define QSOCGENERATEPRIMITIVESEQ_H

#include <yaml-cpp/yaml.h>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QTextStream>

// Forward declaration
class QSocGenerateManager;

/**
 * @brief Sequential logic primitive generator for QSoC
 * 
 * This class generates sequential logic Verilog code including:
 * - Clocked always blocks with posedge/negedge support
 * - Asynchronous reset handling
 * - Enable signal support
 * - Conditional logic with if-else chains
 * - Nested case statements within sequential blocks
 * - Internal register declarations for sequential outputs
 */
class QSocSeqPrimitive
{
public:
    /**
     * @brief Constructor
     * @param parent Pointer to parent QSocGenerateManager
     */
    explicit QSocSeqPrimitive(QSocGenerateManager *parent = nullptr);

    /**
     * @brief Generate sequential logic from YAML configuration
     * @param netlistData YAML node containing the full netlist
     * @param out Output text stream for generated Verilog
     * @return true if generation successful, false otherwise
     */
    bool generateSeqLogic(const YAML::Node &netlistData, QTextStream &out);

private:
    /**
     * @brief Generate sequential logic content for a register
     * @param seqItem The YAML node containing the sequential logic specification
     * @param regName The register name
     * @param out Output text stream
     * @param indentLevel The indentation level for proper formatting
     */
    void generateSeqLogicContent(
        const YAML::Node &seqItem, const QString &regName, QTextStream &out, int indentLevel);

    /**
     * @brief Generate nested sequential logic value (for if/case nesting)
     * @param valueNode The YAML node containing the value (scalar or nested structure)
     * @param regName The register name
     * @param indentLevel The indentation level for proper formatting
     * @return Generated Verilog code string
     */
    QString generateNestedSeqValue(
        const YAML::Node &valueNode, const QString &regName, int indentLevel);

private:
    QSocGenerateManager *m_parent; // Parent manager for accessing utilities
};

#endif // QSOCGENERATEPRIMITIVESEQ_H
