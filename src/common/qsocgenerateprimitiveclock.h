#ifndef QSOCGENERATEPRIMITIVECLOCK_H
#define QSOCGENERATEPRIMITIVECLOCK_H

#include <yaml-cpp/yaml.h>
#include <QDir>
#include <QFile>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QTextStream>

// Forward declaration
class QSocGenerateManager;

/**
 * @brief Clock primitive generator for QSoC
 * 
 * This class generates clock control logic including:
 * - Clock input and target definitions with frequency specifications
 * - Six clock types (PASS_THRU, GATE_ONLY, DIV_ICG, DIV_DFF, GATE_DIV_ICG, GATE_DIV_DFF)
 * - Clock multiplexers (STD_MUX, GF_MUX) for multi-source selection
 * - Clock inversion, gating, and division functionality
 * - Test enable bypass support
 */
class QSocClockPrimitive
{
public:
    /**
     * @brief Clock type enumeration (6 clock types)
     */
    enum ClockType {
        PASS_THRU,    // Direct forward
        GATE_ONLY,    // ICG gate only
        DIV_ICG,      // Narrow-pulse divider (counter + ICG)
        DIV_DFF,      // 50% divider (toggle/D-FF)
        GATE_DIV_ICG, // Gate → ICG divider
        GATE_DIV_DFF  // Gate → D-FF divider
    };

    /**
     * @brief Clock multiplexer type enumeration
     */
    enum MuxType {
        STD_MUX, // Pure combinational mux, no clock domain crossing
        GF_MUX   // Two-stage glitch-free mux, requires ref_clock
    };

    /**
     * @brief Clock input configuration
     */
    struct ClockInput
    {
        QString name;    // Input clock signal name
        QString freq;    // Frequency with unit (e.g., "24MHz", "800MHz")
        QString duty;    // Optional duty cycle (e.g., "50%")
        QString comment; // Optional comment
    };

    /**
     * @brief Clock gate configuration
     */
    struct ClockGate
    {
        QString enable;   // Gate enable signal
        QString polarity; // "high" or "low" (default: "high")
    };

    /**
     * @brief Clock divider configuration  
     */
    struct ClockDivider
    {
        int     ratio; // Division ratio (≥2)
        QString reset; // Reset signal name
    };

    /**
     * @brief Clock multiplexer configuration
     */
    struct ClockMux
    {
        MuxType type;      // STD_MUX or GF_MUX
        QString select;    // Mux select signal
        QString ref_clock; // Reference clock (GF_MUX only, optional)
    };

    /**
     * @brief Clock link configuration (source to target connection)
     */
    struct ClockLink
    {
        QString      source; // Source clock name
        ClockType    type;   // Clock operation type
        bool         invert; // Optional clock inversion
        ClockGate    gate;   // Gate configuration (if type contains "GATE")
        ClockDivider div;    // Divider configuration (if type contains "DIV")
    };

    /**
     * @brief Clock target configuration
     */
    struct ClockTarget
    {
        QString          name;    // Target clock signal name
        QString          freq;    // Target frequency for SDC generation
        QList<ClockLink> links;   // List of source connections
        ClockMux         mux;     // Multiplexer configuration (if ≥2 links)
        QString          comment; // Optional comment
    };

    /**
     * @brief Clock controller configuration
     */
    struct ClockControllerConfig
    {
        QString            name;       // Controller instance name
        QString            moduleName; // Module name
        QString            clock;      // Default synchronous clock
        QString            ref_clock;  // Reference clock for GF_MUX
        QString            test_en;    // Test enable signal (optional)
        QList<ClockInput>  inputs;     // Clock inputs
        QList<ClockTarget> targets;    // Clock targets
    };

public:
    /**
     * @brief Constructor
     * @param parent Pointer to parent QSocGenerateManager
     */
    explicit QSocClockPrimitive(QSocGenerateManager *parent = nullptr);

    /**
     * @brief Generate clock controller from YAML configuration
     * @param clockNode YAML node containing clock configuration
     * @param out Output text stream for generated Verilog
     * @return true if generation successful, false otherwise
     */
    bool generateClockController(const YAML::Node &clockNode, QTextStream &out);

    /**
     * @brief Parse clock configuration from YAML
     * @param clockNode YAML node containing clock configuration
     * @return Parsed configuration structure
     */
    ClockControllerConfig parseClockConfig(const YAML::Node &clockNode);

private:
    /**
     * @brief Generate or update clock_cell.v file with template cells
     * @param outputDir Output directory path
     * @return true if successful, false otherwise
     */
    bool generateClockCellFile(const QString &outputDir);

    /**
     * @brief Check if clock_cell.v file exists and is complete
     * @param filePath Path to clock_cell.v file
     * @return true if file exists and contains all required cells
     */
    bool isClockCellFileComplete(const QString &filePath);

    /**
     * @brief Get all required template cell names with QSOC_ prefix
     * @return List of cell names
     */
    QStringList getRequiredTemplateCells();

    /**
     * @brief Generate single template cell definition
     * @param cellName Cell name (with QSOC_ prefix)
     * @return Cell definition string
     */
    QString generateTemplateCellDefinition(const QString &cellName);

    /**
     * @brief Generate module header and ports
     * @param config Clock controller configuration
     * @param out Output text stream
     */
    void generateModuleHeader(const ClockControllerConfig &config, QTextStream &out);

    /**
     * @brief Generate internal wire declarations
     * @param config Clock controller configuration  
     * @param out Output text stream
     */
    void generateWireDeclarations(const ClockControllerConfig &config, QTextStream &out);

    /**
     * @brief Generate clock logic instances
     * @param config Clock controller configuration
     * @param out Output text stream
     */
    void generateClockLogic(const ClockControllerConfig &config, QTextStream &out);

    /**
     * @brief Generate output assignments and multiplexers  
     * @param config Clock controller configuration
     * @param out Output text stream
     */
    void generateOutputAssignments(const ClockControllerConfig &config, QTextStream &out);

    /**
     * @brief Generate single clock link instance
     * @param link Clock link configuration
     * @param targetName Target clock name (for instance naming)
     * @param linkIndex Link index for multi-link targets
     * @param out Output text stream
     */
    void generateClockInstance(
        const ClockLink &link, const QString &targetName, int linkIndex, QTextStream &out);

    /**
     * @brief Generate clock multiplexer instance
     * @param target Clock target with multiplexer
     * @param config Controller configuration (for ref_clock)
     * @param out Output text stream
     */
    void generateMuxInstance(
        const ClockTarget &target, const ClockControllerConfig &config, QTextStream &out);

    /**
     * @brief Parse clock type from string
     * @param typeStr Type string (e.g., "PASS_THRU", "DIV_ICG", "GATE_DIV_DFF")
     * @return Clock type enumeration value
     */
    ClockType parseClockType(const QString &typeStr);

    /**
     * @brief Parse multiplexer type from string
     * @param typeStr Type string ("STD_MUX" or "GF_MUX")
     * @return Mux type enumeration value
     */
    MuxType parseMuxType(const QString &typeStr);

    /**
     * @brief Get wire name for clock link
     * @param targetName Target clock name
     * @param sourceName Source clock name  
     * @param linkIndex Link index for multi-link targets
     * @return Generated wire name
     */
    QString getLinkWireName(const QString &targetName, const QString &sourceName, int linkIndex);

    /**
     * @brief Get instance name for clock logic
     * @param targetName Target clock name
     * @param sourceName Source clock name
     * @param linkIndex Link index
     * @return Generated instance name
     */
    QString getInstanceName(const QString &targetName, const QString &sourceName, int linkIndex);

    /**
     * @brief Get clock type string for comments
     * @param type Clock type enumeration
     * @return Human-readable type string
     */
    QString getClockTypeString(ClockType type);

private:
    QSocGenerateManager *m_parent; // Parent manager for accessing utilities
};

#endif // QSOCGENERATEPRIMITIVECLOCK_H
