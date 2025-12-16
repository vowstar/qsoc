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
     * @brief Async reset synchronizer configuration (qsoc_rst_sync)
     */
    struct AsyncConfig
    {
        QString test_enable = "test_en"; /**< Test enable signal name */
        QString clock;                   /**< Clock signal name */
        int     stage = 3;               /**< Number of sync stages */
    };

    /**
     * @brief Sync reset pipeline configuration (qsoc_rst_pipe)
     */
    struct SyncConfig
    {
        QString test_enable = "test_en"; /**< Test enable signal name */
        QString clock;                   /**< Clock signal name */
        int     stage = 4;               /**< Number of pipeline stages */
    };

    /**
     * @brief Counter-based reset configuration (qsoc_rst_count)
     */
    struct CountConfig
    {
        QString test_enable = "test_en"; /**< Test enable signal name */
        QString clock;                   /**< Clock signal name */
        int     cycle = 16;              /**< Counter cycles */
    };

    /**
     * @brief Reset link configuration (source to internal connection)
     */
    struct ResetLink
    {
        QString     source; /**< Source signal name */
        AsyncConfig async;  /**< Async config (qsoc_rst_sync) */
        SyncConfig  sync;   /**< Sync config (qsoc_rst_pipe) */
        CountConfig count;  /**< Count config (qsoc_rst_count) */
    };

    /**
     * @brief Reset target configuration
     */
    struct ResetTarget
    {
        QString          name;   /**< Target signal name */
        QString          active; /**< Output active level ("high" or "low") */
        AsyncConfig      async;  /**< Target-level async config */
        SyncConfig       sync;   /**< Target-level sync config */
        CountConfig      count;  /**< Target-level count config */
        QList<ResetLink> links;  /**< Input links */
    };

    /**
     * @brief Reset reason recording configuration
     */
    struct ResetReasonConfig
    {
        bool        enabled = false; /**< Enable reset reason recording */
        QString     clock;           /**< Always-on clock for recording logic */
        QString     output;          /**< Output bit vector bus name */
        QString     valid;           /**< Valid signal name for output gating */
        QString     clear;           /**< Software clear signal name */
        QString     rootReset;       /**< Root reset signal for async clear */
        QStringList sourceOrder;     /**< Source names in bit order (LSB to MSB) */
        int         vectorWidth = 0; /**< Total bit vector width */
    };

    /**
     * @brief Reset source information
     */
    struct ResetSource
    {
        QString name;   /**< Source signal name */
        QString active; /**< Source active level ("high" or "low") */
    };

    /**
     * @brief Reset controller configuration
     */
    struct ResetControllerConfig
    {
        QString            name       = "rstctrl"; /**< Controller instance name */
        QString            moduleName = "rstctrl"; /**< Module name */
        QString            testEnable;             /**< Test enable signal (optional) */
        QList<ResetSource> sources;                /**< Reset sources */
        QList<ResetTarget> targets;                /**< Reset targets */
        ResetReasonConfig  reason;                 /**< Reset reason recording */
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

    /**
     * @brief Set force overwrite mode for reset_cell.v file
     * @param force true to enable force overwrite, false to preserve existing files
     */
    void setForceOverwrite(bool force);

    /**
     * @brief Generate Typst reset tree diagram from configuration
     * @param config Reset controller configuration
     * @param outputPath Output path for .typ file
     * @return true if generation successful, false otherwise
     */
    bool generateTypstDiagram(const ResetControllerConfig &config, const QString &outputPath);

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
     * @brief Generate reset reason recording logic (Per-source async-set flops)
     * @param config Reset controller configuration
     * @param out Output text stream
     */
    void generateResetReason(const ResetControllerConfig &config, QTextStream &out);

    /**
     * @brief Generate output assignments
     * @param config Reset controller configuration
     * @param out Output text stream
     */
    void generateOutputAssignments(const ResetControllerConfig &config, QTextStream &out);

    /**
     * @brief Generate reset cell template file
     * @param out Output text stream for reset_cell.v
     */
    void generateResetCellFile(QTextStream &out);

    /**
     * @brief Generate or update reset_cell.v file with template cells
     * @param outputDir Output directory path
     * @return true if successful, false otherwise
     */
    bool generateResetCellFile(const QString &outputDir);

    /**
     * @brief Generate single reset component instance
     * @param targetName Target name for instance naming
     * @param linkIndex Link index for instance naming
     * @param async Async config (if component exists)
     * @param sync Sync config (if component exists)
     * @param count Count config (if component exists)
     * @param inv Inverter flag (not used in new architecture)
     * @param inputSignal Input signal name
     * @param outputSignal Output signal name
     * @param out Output text stream
     */
    void generateResetComponentInstance(
        const QString     &targetName,
        int                linkIndex,
        const AsyncConfig *async,
        const SyncConfig  *sync,
        const CountConfig *count,
        bool               inv,
        const QString     &inputSignal,
        const QString     &outputSignal,
        QTextStream       &out);

    /**
     * @brief Get wire name for link connection
     * @param targetName Target signal name
     * @param linkIndex Link index
     * @return Generated wire name
     */
    QString getLinkWireName(const QString &targetName, int linkIndex);

    /**
     * @brief Get instance name for reset component
     * @param targetName Target name
     * @param linkIndex Link index (-1 for target-level)
     * @param componentType Component type ("async", "sync", "count")
     * @return Generated instance name
     */
    QString getComponentInstanceName(
        const QString &targetName, int linkIndex, const QString &componentType);

    /**
     * @brief Get normalized source signal (convert to low-active)
     * @param sourceName Source signal name
     * @param config Reset controller configuration
     * @return Normalized source signal
     */
    QString getNormalizedSource(const QString &sourceName, const ResetControllerConfig &config);

    // Typst generation helper functions
    QString typstHeader() const;
    QString typstLegend() const;
    QString typstSourceTable(const QList<ResetSource> &sources, float x, float &bottomY) const;
    QString typstTarget(
        const ResetTarget         &target,
        const QMap<QString, bool> &sourceIsHighActive,
        float                      x,
        float                      y) const;
    QString escapeTypstId(const QString &str) const;

private:
    QSocGenerateManager *m_parent;                 // Parent manager for accessing utilities
    bool                 m_forceOverwrite = false; // Force overwrite mode for reset_cell.v
};

#endif // QSOCGENERATEPRIMITIVERESET_H
