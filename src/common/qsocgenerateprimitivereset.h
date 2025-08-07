#ifndef QSOCGENERATEPRIMITIVERESET_H
#define QSOCGENERATEPRIMITIVERESET_H

#include <yaml-cpp/yaml.h>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QTextStream>

// Forward declaration
class QSocGenerateManager;

/**
 * @brief Reset primitive generator for QSoC
 * 
 * This class generates reset control logic including:
 * - Reset matrix functionality (source to target mapping)  
 * - Per-source async bit-flag recording
 * - Various reset modes (async, sync, counter-based)
 * - Test enable bypass support
 */
class QSocResetPrimitive
{
public:
    /**
     * @brief Reset mode enumeration
     */
    enum ResetMode {
        AsyncComb,     // A: Async reset + Async release (combinational)
        AsyncSync,     // A(N,clk): Async reset + Sync release
        AsyncCounter,  // AC(N,W,T,clk): Async reset + Counter timeout
        AsyncThenSync, // AS(N1,N2,clk): Async reset + Sync release + Sync reset
        Sync           // S(N,clk): Sync reset + Sync release
    };

    /**
     * @brief Reset source configuration
     */
    struct ResetSource
    {
        QString name;    // Source signal name
        QString active;  // "high" or "low" polarity
        QString comment; // Optional comment
    };

    /**
     * @brief Reset target configuration  
     */
    struct ResetTarget
    {
        QString     name;    // Target signal name
        QString     active;  // "high" or "low" polarity (usually "low" for _n)
        QStringList sources; // List of source names affecting this target
        QString     comment; // Optional comment
    };

    /**
     * @brief Reset connection configuration
     */
    struct ResetConnection
    {
        QString   sourceName;   // Source signal name
        QString   targetName;   // Target signal name
        ResetMode mode;         // Reset mode
        int       syncDepth;    // Synchronization depth
        int       counterWidth; // Counter width (for AC mode)
        int       timeout;      // Timeout value (for AC mode)
        QString   clock;        // Clock signal name
    };

    /**
     * @brief Reset flags configuration
     */
    struct ResetFlagsConfig
    {
        bool    enabled;   // Enable reset flags recording
        QString porSignal; // Power-on reset signal name
        QString flagBus;   // Output flag bus name
        QString aonClock;  // Always-on clock (optional)
    };

    /**
     * @brief Reset controller configuration
     */
    struct ResetControllerConfig
    {
        QString                name;        // Controller instance name
        QString                moduleName;  // Module name (default: "rstctrl")
        QString                clock;       // Main clock signal
        QString                testEnable;  // Test enable signal (default: "test_en")
        QList<ResetSource>     sources;     // Reset sources
        QList<ResetTarget>     targets;     // Reset targets
        QList<ResetConnection> connections; // Source-target connections
        ResetFlagsConfig       flags;       // Reset flags configuration
    };

public:
    /**
     * @brief Constructor
     * @param parent Pointer to parent QSocGenerateManager
     */
    explicit QSocResetPrimitive(QSocGenerateManager *parent = nullptr);

    /**
     * @brief Generate reset controller from YAML configuration
     * @param resetNode YAML node containing reset configuration
     * @param out Output text stream for generated Verilog
     * @return true if generation successful, false otherwise
     */
    bool generateResetController(const YAML::Node &resetNode, QTextStream &out);

    /**
     * @brief Parse reset configuration from YAML
     * @param resetNode YAML node containing reset configuration
     * @return Parsed configuration structure
     */
    ResetControllerConfig parseResetConfig(const YAML::Node &resetNode);

private:
    /**
     * @brief Generate module header and ports
     * @param config Reset controller configuration
     * @param out Output text stream
     */
    void generateModuleHeader(const ResetControllerConfig &config, QTextStream &out);

    /**
     * @brief Generate internal wire declarations
     * @param config Reset controller configuration  
     * @param out Output text stream
     */
    void generateWireDeclarations(const ResetControllerConfig &config, QTextStream &out);

    /**
     * @brief Generate reset logic instances
     * @param config Reset controller configuration
     * @param out Output text stream
     */
    void generateResetLogic(const ResetControllerConfig &config, QTextStream &out);

    /**
     * @brief Generate reset flags recording logic
     * @param config Reset controller configuration
     * @param out Output text stream
     */
    void generateResetFlags(const ResetControllerConfig &config, QTextStream &out);

    /**
     * @brief Generate output assignments  
     * @param config Reset controller configuration
     * @param out Output text stream
     */
    void generateOutputAssignments(const ResetControllerConfig &config, QTextStream &out);

    /**
     * @brief Generate single reset connection instance
     * @param connection Reset connection configuration
     * @param out Output text stream
     */
    void generateResetInstance(const ResetConnection &connection, QTextStream &out);

    /**
     * @brief Parse reset mode from string
     * @param modeStr Mode string (e.g., "A", "A(4,clk)", "AC(2,8,255,clk)")
     * @param connection Output connection structure
     * @return true if parse successful
     */
    bool parseResetMode(const QString &modeStr, ResetConnection &connection);

    /**
     * @brief Get wire name for source-target connection
     * @param sourceName Source signal name  
     * @param targetName Target signal name
     * @return Generated wire name
     */
    QString getConnectionWireName(const QString &sourceName, const QString &targetName);

    /**
     * @brief Get instance name for reset logic
     * @param connection Reset connection
     * @return Generated instance name
     */
    QString getInstanceName(const ResetConnection &connection);

private:
    QSocGenerateManager *m_parent; // Parent manager for accessing utilities
};

#endif // QSOCGENERATEPRIMITIVERESET_H
