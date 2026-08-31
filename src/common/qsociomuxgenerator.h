// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCIOMUXGENERATOR_H
#define QSOCIOMUXGENERATOR_H

#include "common/qsocmmiogenerator.h"

#include <optional>
#include <QList>
#include <QMap>
#include <QString>
#include <QStringList>
#include <QtGlobal>

#include <yaml-cpp/yaml.h>

struct QSocModuleDefinition;

enum class QSocIomuxRole { InputValue, InputEnable, OutputValue, OutputEnable };

struct QSocIomuxEndpointPlan
{
    QString                link;
    std::optional<quint32> bit;
    bool                   invert = false;
    std::optional<quint8>  constant;

    bool operator==(const QSocIomuxEndpointPlan &) const = default;
};

struct QSocIomuxRoutePlan
{
    quint32               pin  = 0;
    quint32               slot = 0;
    QString               function;
    QString               signal;
    QSocIomuxEndpointPlan inputValue;
    QSocIomuxEndpointPlan inputEnable;
    QSocIomuxEndpointPlan outputValue;
    QSocIomuxEndpointPlan outputEnable;
    QString               pullMode;     /**< A mode name from the pad cell table, or empty */
    QString               pullStrength; /**< A strength label under that mode, or empty */
    QString               driveLevel;   /**< A label from the drive table, or empty */

    bool operator==(const QSocIomuxRoutePlan &) const = default;
};

/**
 * @brief One row of a pad control table.
 *
 * The values are transcribed from the device databook, one entry per pin named
 * in the owning group, so the source never spells a net name into a pad pin.
 * A missing row means the cell cannot reach that state.
 */
struct QSocPadTableRow
{
    QString        label; /**< Strength label, empty when the group has one row */
    QList<QString> value; /**< One of "0", "1" or "x" per port */

    bool operator==(const QSocPadTableRow &) const = default;
};

/**
 * @brief The pull control of a pad cell.
 */
struct QSocPadPullPlan
{
    QList<QString> port; /**< Cell ports that carry the pull control */
    /**
     * @brief Mode name to its rows, one row per strength label.
     *
     * `none`, `up`, `down` and `keeper` carry meaning to the generator. Any
     * other name is a mode the cell documents and a route asks for by name,
     * which is how a pad that gives both pull pins together a defined
     * behaviour stays expressible.
     */
    QMap<QString, QList<QSocPadTableRow>> mode;
    bool isDriver = false; /**< The pull is the driver, not a resistor */

    bool has(const QString &name) const { return mode.contains(name); }

    bool operator==(const QSocPadPullPlan &) const = default;
};

/**
 * @brief The drive strength control of a pad cell.
 */
struct QSocPadDrivePlan
{
    QList<QString>         port;
    QList<QSocPadTableRow> level;

    bool operator==(const QSocPadDrivePlan &) const = default;
};

/**
 * @brief One property over the pad cell pins, written in SystemVerilog.
 *
 * The body is never parsed. Identifiers that name cell ports are rewritten to
 * the per-pin nets, every other identifier must be a SystemVerilog keyword or
 * a system function, and the property is emitted once per pin.
 */
struct QSocPadConstraint
{
    QString name;
    QString body;
    bool    temporal  = false; /**< `property` rather than `expr`, needs a clock */
    bool    assume    = false; /**< environment constraint rather than a claim */
    bool    kindGiven = false;

    bool operator==(const QSocPadConstraint &) const = default;
};

/**
 * @brief The pad cell this design instantiates.
 *
 * The generator owns every connection to the cell, so a source that names the
 * wrong port fails against the module library instead of elaborating into a
 * legal but wrong netlist.
 */
struct QSocPadCellPlan
{
    QString                  cell;
    QString                  portInputValue;
    QString                  portInputEnable;
    QString                  portOutputValue;
    QString                  portOutputEnable;
    QString                  portPad;
    QSocPadPullPlan          pull;
    QSocPadDrivePlan         drive;
    QList<QSocPadConstraint> constraint;
    /** Port name to direction of the cell, filled from the module library. */
    QMap<QString, QString> cellPorts;

    bool declared() const { return !cell.isEmpty(); }
    bool canPullUp() const { return pull.has(QStringLiteral("up")); }
    bool canPullDown() const { return pull.has(QStringLiteral("down")); }
    /** The cell owns a keeper mode, or one can be woven from the two pulls. */
    bool canKeep() const
    {
        return pull.has(QStringLiteral("keeper"))
               || (canPullUp() && canPullDown() && !pull.isDriver);
    }
    bool keeperIsNative() const { return pull.has(QStringLiteral("keeper")); }

    bool operator==(const QSocPadCellPlan &) const = default;
};

struct QSocIomuxIntegrationPlan
{
    QString         instance;
    QString         clock;
    QString         reset;
    QString         control;
    QString         padInputValue;
    QString         padInputEnable;
    QString         padOutputValue;
    QString         padOutputEnable;
    QString         padIo; /**< Top-level pad net, used when a pad cell is declared */
    QSocPadCellPlan padCell;

    bool operator==(const QSocIomuxIntegrationPlan &) const = default;
};

/**
 * @brief Optional register blocks layered on top of the selector fabric.
 */
struct QSocIomuxOptionPlan
{
    bool gpio       = false;
    bool interrupt  = false;
    bool padControl = false; /**< Pull and drive registers behind a per-pin source bit */
    bool invert     = false; /**< Runtime inversion after every source selector */
    bool rxOverride = false; /**< Per-slot register substitution of the pad input */

    /** Some option owns a field in the per-pin source control word. */
    bool sourceControl() const { return gpio || padControl || rxOverride; }

    bool operator==(const QSocIomuxOptionPlan &) const = default;
};

/**
 * @brief How the selector codes of a pad cell are laid out.
 *
 * Pull rows are numbered with `none` first and the remaining modes in name
 * order, so an all-zero code is the state that drives nothing. Drive rows
 * keep their table order. A woven keeper or oscillator is not a row; it is a
 * flag that overrides the code inside the pad module.
 */
struct QSocPadEncoding
{
    QList<QSocPadTableRow> pullRows;
    QStringList            pullMode; /**< Mode name of each pull row */
    QStringList            driveLevel;
    quint32                pullWidth  = 0; /**< 0 when the cell has no pull table */
    quint32                driveWidth = 0; /**< 0 when the cell has no drive table */
    bool                   weaves     = false;
    int                    upCode     = -1;
    int                    downCode   = -1;

    bool hasPull() const { return pullWidth > 0; }
    bool hasDrive() const { return driveWidth > 0; }
    /** Code of a pull row, or -1 when the cell has no such row. */
    int pullCode(const QString &mode, const QString &strength) const;
    /** Code of a drive row, or -1 when the cell has no such row. */
    int driveCode(const QString &level) const;
    /** The slot code a route asks for, 0 when it asks for nothing reachable. */
    int  routePullCode(const QSocIomuxRoutePlan &route) const;
    int  routeDriveCode(const QSocIomuxRoutePlan &route) const;
    bool routeWeavesKeeper(const QSocIomuxRoutePlan &route) const;
    bool routeWeavesOscillator(const QSocIomuxRoutePlan &route) const;
};

struct QSocIomuxPlan
{
    QString                   moduleName;
    quint32                   pinCount = 0;
    quint32                   hsSlots  = 0;
    QSocIomuxOptionPlan       option;
    QList<QSocIomuxRoutePlan> routes;
    QSocIomuxIntegrationPlan  integration;
    QSocMmioPlan              mmio;

    bool operator==(const QSocIomuxPlan &) const = default;
};

/**
 * @brief One port of the mux core, without its direction suffix.
 */
struct QSocIomuxCorePort
{
    QString name;
    quint32 width = 1;
};

struct QSocIomuxFormalCollateral
{
    QString systemVerilog;
    QString sby;
};

class QSocIomuxGenerator
{
public:
    static bool        isIomux(const QSocModuleDefinition &definition);
    static YAML::Node  createDraftGenerator();
    static QStringList validate(const QSocModuleDefinition &definition);
    static bool        buildPlan(
        const QSocModuleDefinition &definition, QSocIomuxPlan *plan, QStringList *errors = nullptr);
    static QString endpointPortName(quint32 pin, quint32 slot, QSocIomuxRole role);
    /** The selector code layout of the declared pad cell, empty when none. */
    static QSocPadEncoding padEncoding(const QSocPadCellPlan &cell);
    /**
     * @brief The register inputs an option adds to one pin of the core.
     *
     * The core declares them with `_i`, the wrapper wires them with `_w` and
     * the formal harness leaves them free, all from this one list.
     */
    static QList<QSocIomuxCorePort> corePinOptionPorts(const QSocIomuxPlan &plan, quint32 pin);
    /** The pad selector vectors the core drives when a pad cell is declared. */
    static QList<QSocIomuxCorePort> corePadSelectPorts(const QSocIomuxPlan &plan);
    /**
     * @brief Check every declared pad cell port against the library.
     *
     * The caller supplies the port table of the cell named by the source, so a
     * port that does not exist, or exists with the wrong direction, fails
     * before any Verilog is written.
     *
     * @param plan      the plan holding the pad cell declaration
     * @param cellPorts port name to direction, "in", "out" or "inout"
     * @param errors    receives one message per rejected port
     * @return true when the declaration matches the cell
     */
    static bool checkPadCellPorts(
        const QSocIomuxPlan          &plan,
        const QMap<QString, QString> &cellPorts,
        QStringList                  *errors = nullptr);
    static QString generateCoreVerilog(const QSocIomuxPlan &plan);
    static QString generateConnVerilog(const QSocIomuxPlan &plan);
    static QString generateRegsVerilog(const QSocIomuxPlan &plan);
    static QString generateTopVerilog(const QSocIomuxPlan &plan);
    static QString generatePadVerilog(const QSocIomuxPlan &plan);
    /** The constraint body with cell ports rewritten to the nets of one pin. */
    static QString    padConstraintForPin(const QSocIomuxPlan &plan, qsizetype index, quint32 pin);
    static QString    generateFileList(const QSocIomuxPlan &plan);
    static QString    generateIntegrationNetlist(const QSocIomuxPlan &plan);
    static YAML::Node describeModuleYaml(const QSocIomuxPlan &plan);
    static QString    generateReport(const QSocIomuxPlan &plan);
};

#endif // QSOCIOMUXGENERATOR_H
