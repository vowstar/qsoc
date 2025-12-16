// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCGENERATEPRIMITIVEPOWER_H
#define QSOCGENERATEPRIMITIVEPOWER_H

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
 * @brief Power primitive generator for QSoC
 *
 * This class generates power control logic including:
 * - Power domain management (AO, root, and normal domains)
 * - Hard and soft dependency handling
 * - 8-state FSM power sequencing (switch -> pgood -> clock enable -> reset release)
 * - Clock-before-reset timing compliance with SYNC_CYCLES parameter
 * - Fault detection and recovery with auto-heal
 * - qsoc_rst_pipe reset synchronizer generation
 * - Clock and reset follow signals for domain coordination
 */
class QSocPowerPrimitive
{
public:
    /**
     * @brief Power dependency configuration
     */
    struct Dependency
    {
        QString name; /**< Dependency domain name */
        QString type; /**< Dependency type: "hard" or "soft" */
    };

    /**
     * @brief Follow entry for reset synchronization
     */
    struct FollowEntry
    {
        QString clock;     /**< Domain working clock (typically post-ICG) */
        QString reset;     /**< Output reset signal name */
        int     stage = 4; /**< Reset synchronizer stages */
    };

    /**
     * @brief Power domain configuration
     */
    struct PowerDomain
    {
        QString            name;           /**< Domain name */
        QList<Dependency>  depends;        /**< Dependency list (empty = AO, explicit [] = root) */
        int                v_mv;           /**< Voltage in millivolts */
        QString            pgood;          /**< Power good signal name */
        int                wait_dep;       /**< Dependency wait cycles */
        int                settle_on;      /**< Power-on settle cycles */
        int                settle_off;     /**< Power-off settle cycles */
        QList<FollowEntry> follow_entries; /**< Reset synchronization entries */
    };

    /**
     * @brief Power controller configuration
     */
    struct PowerControllerConfig
    {
        QString            name;        /**< Controller instance name */
        QString            moduleName;  /**< Module name */
        QString            host_clock;  /**< Host clock (typically AO clock) */
        QString            host_reset;  /**< Host reset (typically AO reset) */
        QString            test_enable; /**< DFT test enable signal (optional) */
        QList<PowerDomain> domains;     /**< Power domains */
    };

public:
    /**
     * @brief Constructor
     * @param parent Pointer to parent QSocGenerateManager
     */
    explicit QSocPowerPrimitive(QSocGenerateManager *parent = nullptr);

    /**
     * @brief Generate power controller from YAML configuration
     * @param powerNode YAML node containing power configuration
     * @param out Output text stream for generated Verilog
     * @return true if generation successful, false otherwise
     */
    bool generatePowerController(const YAML::Node &powerNode, QTextStream &out);

    /**
     * @brief Parse power configuration from YAML
     * @param powerNode YAML node containing power configuration
     * @return Parsed configuration structure
     */
    PowerControllerConfig parsePowerConfig(const YAML::Node &powerNode);

    /**
     * @brief Set force overwrite mode for power_cell.v file
     * @param force true to enable force overwrite, false to preserve existing files
     */
    void setForceOverwrite(bool force);

    /**
     * @brief Generate Typst power tree diagram from configuration
     * @param config Power controller configuration
     * @param outputPath Output path for .typ file
     * @return true if generation successful, false otherwise
     */
    bool generateTypstDiagram(const PowerControllerConfig &config, const QString &outputPath);

private:
    /**
     * @brief Generate module header and ports
     * @param config Power controller configuration
     * @param out Output text stream
     */
    void generateModuleHeader(const PowerControllerConfig &config, QTextStream &out);

    /**
     * @brief Generate internal wire declarations
     * @param config Power controller configuration
     * @param out Output text stream
     */
    void generateWireDeclarations(const PowerControllerConfig &config, QTextStream &out);

    /**
     * @brief Generate power FSM logic instances
     * @param config Power controller configuration
     * @param out Output text stream
     */
    void generatePowerLogic(const PowerControllerConfig &config, QTextStream &out);

    /**
     * @brief Generate output assignments
     * @param config Power controller configuration
     * @param out Output text stream
     */
    void generateOutputAssignments(const PowerControllerConfig &config, QTextStream &out);

    /**
     * @brief Generate or update power_cell.v file with template cells
     * @param outputDir Output directory path
     * @return true if successful, false otherwise
     */
    bool generatePowerCellFile(const QString &outputDir);

    /**
     * @brief Check if power_cell.v file exists and is complete
     * @param filePath Path to power_cell.v file
     * @return true if file exists and contains required module
     */
    bool isPowerCellFileComplete(const QString &filePath);

    /**
     * @brief Generate qsoc_power_fsm module definition with 8-state FSM
     * @return Module definition string
     */
    QString generatePowerFSMModule();

    /**
     * @brief Generate qsoc_rst_pipe reset synchronizer module
     * @return Module definition string
     */
    QString generateResetPipeModule();

    /**
     * @brief Determine if domain is AO (always-on)
     * @param domain Power domain configuration
     * @param yamlNode Original YAML node for this domain
     * @return true if domain is AO type
     */
    bool isAODomain(const PowerDomain &domain, const YAML::Node &yamlNode);

    /**
     * @brief Determine if domain is root (controllable root)
     * @param domain Power domain configuration
     * @param yamlNode Original YAML node for this domain
     * @return true if domain is root type
     */
    bool isRootDomain(const PowerDomain &domain, const YAML::Node &yamlNode);

    /**
     * @brief Get aggregated hard dependency signal expression
     * @param domain Power domain configuration
     * @return Signal expression (e.g., "rdy_ao & rdy_vmem" or "1'b1")
     */
    QString getHardDependencySignal(const PowerDomain &domain);

    /**
     * @brief Get aggregated soft dependency signal expression
     * @param domain Power domain configuration
     * @return Signal expression (e.g., "rdy_mem" or "1'b1")
     */
    QString getSoftDependencySignal(const PowerDomain &domain);

    // Typst generation helper functions
    QString typstHeader() const;
    QString typstLegend() const;
    QString typstDomain(const PowerDomain &domain, float x, float y) const;
    QString escapeTypstId(const QString &str) const;

private:
    QSocGenerateManager      *m_parent;                 // Parent manager for accessing utilities
    bool                      m_forceOverwrite = false; // Force overwrite mode for power_cell.v
    QMap<QString, YAML::Node> m_domainYamlCache; // Cache YAML nodes for domain type inference
};

#endif // QSOCGENERATEPRIMITIVEPOWER_H
