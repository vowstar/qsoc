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
        QString enable;      // Gate enable signal
        QString polarity;    // "high" or "low" (default: "high")
        QString test_enable; // Test enable signal (optional)
        QString reset;       // Reset signal name (active-low default)
    };

    /**
     * @brief Clock divider configuration  
     */
    struct ClockDivider
    {
        // Core parameters (required for proper operation)
        int ratio = 1; // Division ratio (default 1 = no division)
        int width = 0; // Divider width in bits (0 = error, must be specified)

        // Optional configuration parameters
        int  default_val    = 0;     // Reset default value (default 0)
        bool clock_on_reset = false; // Enable clock output during reset (default false)

        // Control signals (empty string = use safe defaults)
        QString reset;       // Reset signal name (empty = 1'b1)
        QString enable;      // Enable signal name (empty = 1'b1)
        QString test_enable; // Test enable signal (empty = use global or 1'b0)

        // Dynamic configuration interface (empty = not connected)
        QString div_signal; // Dynamic division ratio input (empty = use static ratio)
        QString div_valid;  // Division value valid signal (empty = 1'b1)
        QString div_ready;  // Division ready output signal (empty = unconnected)
        QString count;      // Cycle counter output (empty = unconnected)
    };

    /**
     * @brief Clock STA guide configuration
     */
    struct ClockSTAGuide
    {
        QString cell;     // Foundry cell name (e.g., TSMC_CKBUF)
        QString in;       // Input port name (e.g., I)
        QString out;      // Output port name (e.g., Z)
        QString instance; // Instance name (e.g., u_cpu_clk_sta_guide)
    };

    /**
     * @brief Clock multiplexer configuration - DEPRECATED
     * Use target-level signals (select, reset, test_enable, test_clock) instead
     */
    struct ClockMux
    {
        MuxType type = STD_MUX; // Auto-selected based on reset presence, kept for compatibility
        QString select;         // DEPRECATED: use target.select
        QString reset;          // DEPRECATED: use target.reset
        QString test_enable;    // DEPRECATED: use target.test_enable
        QString test_clock;     // DEPRECATED: use target.test_clock
    };

    /**
     * @brief Clock link configuration (source to target connection)
     */
    struct ClockLink
    {
        QString       source;      // Source clock name
        ClockGate     icg;         // ICG configuration
        ClockDivider  div;         // Divider configuration
        bool          inv = false; // Inverter flag
        ClockSTAGuide sta_guide;   // STA guide buffer configuration
    };

    /**
     * @brief Clock target configuration
     */
    struct ClockTarget
    {
        QString          name;        // Target clock signal name
        QString          freq;        // Target frequency for SDC generation
        QList<ClockLink> links;       // List of source connections
        ClockMux         mux;         // Multiplexer configuration (if ≥2 links) - DEPRECATED
        ClockGate        icg;         // Target-level ICG
        ClockDivider     div;         // Target-level divider
        bool             inv = false; // Target-level inverter
        ClockSTAGuide    sta_guide;   // Target-level STA guide buffer
        QString          comment;     // Optional comment

        // Target-level MUX signals (new format per documentation)
        QString select;      // MUX select signal (required for ≥2 links)
        QString reset;       // Reset signal for GF_MUX auto-selection (optional)
        QString test_enable; // DFT test enable signal (GF_MUX only, optional)
        QString test_clock;  // DFT test clock signal (GF_MUX only, optional)
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
     * @param outputName Output wire name (optional, uses target.name if empty)
     */
    void generateMuxInstance(
        const ClockTarget           &target,
        const ClockControllerConfig &config,
        QTextStream                 &out,
        const QString               &outputName = "");

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

private:
    QSocGenerateManager *m_parent; // Parent manager for accessing utilities
};

#endif // QSOCGENERATEPRIMITIVECLOCK_H
