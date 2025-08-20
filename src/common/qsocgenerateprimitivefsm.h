#ifndef QSOCGENERATEPRIMITIVEFSM_H
#define QSOCGENERATEPRIMITIVEFSM_H

#include <yaml-cpp/yaml.h>
#include <QString>
#include <QStringList>
#include <QTextStream>

// Forward declaration
class QSocGenerateManager;

/**
 * @brief FSM primitive generator for QSoC
 *
 * This class generates finite state machine Verilog code including:
 * - Table-mode FSM with binary/one-hot encoding
 * - Microcode-mode FSM with field-based control
 * - Moore and Mealy output logic
 * - State transition logic with conditions
 */
class QSocFSMPrimitive
{
public:
    /**
     * @brief Constructor
     * @param parent Pointer to parent QSocGenerateManager
     */
    explicit QSocFSMPrimitive(QSocGenerateManager *parent = nullptr);

    /**
     * @brief Generate FSM Verilog code from YAML configuration
     * @param fsmNode YAML node containing FSM specification
     * @param out Output text stream for generated Verilog
     * @return true if generation successful, false otherwise
     */
    bool generateFSMVerilog(const YAML::Node &fsmNode, QTextStream &out);

private:
    /**
     * @brief Generate module header with ports for FSM
     * @param fsmNode The YAML node containing the FSM specification
     * @param out Output text stream
     */
    void generateModuleHeader(const YAML::Node &fsmNode, QTextStream &out);

    /**
     * @brief Generate Table-mode FSM Verilog code
     * @param fsmItem The YAML node containing the FSM specification
     * @param out Output text stream
     */
    void generateTableFSM(const YAML::Node &fsmItem, QTextStream &out);

    /**
     * @brief Generate Microcode-mode FSM Verilog code
     * @param fsmItem The YAML node containing the FSM specification
     * @param out Output text stream
     */
    void generateMicrocodeFSM(const YAML::Node &fsmItem, QTextStream &out);

private:
    QSocGenerateManager *m_parent; // Parent manager for accessing utilities
};

#endif // QSOCGENERATEPRIMITIVEFSM_H
