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

/**
 * @brief A pull request of a route: one mode and, on a graded side, a strength.
 */
struct QSocIomuxPullRequest
{
    QString mode;
    QString strength;

    bool empty() const { return mode.isEmpty(); }
    bool operator==(const QSocIomuxPullRequest &) const = default;
};

/**
 * @brief A row chosen at bus speed by a net: `on` while the net is high,
 * `off` while it is low. What an open-drain mode pin or a sleep pull needs.
 */
struct QSocIomuxRouteSelect
{
    QSocIomuxEndpointPlan link;
    QSocIomuxPullRequest  on; /**< For a control only `mode` is used, as the row label */
    QSocIomuxPullRequest  off;

    bool linked() const { return !link.link.isEmpty(); }
    bool operator==(const QSocIomuxRouteSelect &) const = default;
};

/**
 * @brief What a route asks of one control: a fixed row or a net-selected pair.
 */
struct QSocIomuxControlRequest
{
    QString              row;
    QSocIomuxRouteSelect select;

    bool operator==(const QSocIomuxControlRequest &) const = default;
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
    QSocIomuxRouteSelect  pullSelect;   /**< A net-selected pull pair, when the mode is empty */
    QMap<QString, QSocIomuxControlRequest> control; /**< Control name to the request */

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
     * `none`, `up`, `down`, `keeper` and `oscillator` carry meaning to the
     * generator. Any
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
 * @brief One control of a pad cell: a named group of pins and its rows.
 *
 * Drive strength, slew, Schmitt trigger, analog enable, open-drain mode: the
 * generator treats them all alike. The name is the user's, appears as is in
 * ports, fields and the report, and carries no meaning to the generator.
 */
struct QSocPadControlPlan
{
    QString                name;
    QList<QString>         port;
    QList<QSocPadTableRow> row;            /**< Labelled rows, table order */
    int                    defaultRow = 0; /**< Row a slot takes when its route names none */

    bool operator==(const QSocPadControlPlan &) const = default;
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
 * @brief The row every pin takes while `pad_force_i` is high.
 *
 * Isolation, test, and the window before firmware runs all need one state
 * that no register can override. Roles default to 0, the pull to none, and
 * each control to its default row.
 */
struct QSocPadSafePlan
{
    bool                   declared     = false;
    quint8                 inputEnable  = 0;
    quint8                 outputValue  = 0;
    quint8                 outputEnable = 0;
    QSocIomuxPullRequest   pull;
    QMap<QString, QString> control; /**< Control name to row label */

    bool operator==(const QSocPadSafePlan &) const = default;
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
    QString                   cell;
    QString                   portInputValue;
    QString                   portInputEnable;
    QString                   portOutputValue;
    QString                   portOutputEnable;
    QString                   portPad;
    QSocPadPullPlan           pull;
    QList<QSocPadControlPlan> control; /**< Declaration order, which fixes field positions */
    QSocPadSafePlan           safe;
    QList<QSocPadConstraint>  constraint;
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
    /** Whether some control drives this cell pin. */
    bool controlDrives(const QString &port) const
    {
        for (const QSocPadControlPlan &item : control) {
            if (item.port.contains(port)) {
                return true;
            }
        }
        return false;
    }

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
    QString         force; /**< Net that drives pad_force_i, used when safe is declared */
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
    bool padControl = false; /**< Pull and control registers behind per-pin source bits */
    bool invert     = false; /**< Runtime inversion after every source selector */
    bool rxOverride = false; /**< Per-slot register substitution of the pad input */

    /** Some option owns a field in the per-pin source control word. */
    bool sourceControl() const { return gpio || padControl || rxOverride; }

    bool operator==(const QSocIomuxOptionPlan &) const = default;
};

/**
 * @brief How the pad control of a cell is encoded.
 *
 * A pin has one pull mode and one strength per direction. The mode values
 * are fixed so software reads the same field on every design: 0 none, 1 up,
 * 2 down, 3 keeper, 4 oscillator, and the cell's other named rows from 5 in
 * name order. A value the cell has no row for behaves as none. Only up and
 * down carry strength rows; the strength selects index them in table order.
 */
struct QSocPadEncoding
{
    enum Mode : int { None = 0, Up = 1, Down = 2, Keeper = 3, Oscillator = 4, FirstNamed = 5 };

    QSocPadTableRow                noneRow;
    QList<QSocPadTableRow>         upRows;
    QList<QSocPadTableRow>         downRows;
    std::optional<QSocPadTableRow> keeperRow;        /**< Native keeper row, if any */
    std::optional<QSocPadTableRow> oscillatorRow;    /**< Native oscillator row, if any */
    QStringList                    namedMode;        /**< Other modes, name order */
    QList<QSocPadTableRow>         namedRow;         /**< One row per named mode */
    quint32                        modeWidth    = 0; /**< 0 when the cell has no pull table */
    quint32                        upSelWidth   = 0; /**< 0 when up has at most one row */
    quint32                        downSelWidth = 0;
    bool weaves = false; /**< keeper and oscillator are woven from up and down */

    /**
     * @brief One control as software sees it.
     *
     * A single-row control has width 0: nothing to select, no field, no port.
     * Its row is still driven into the pad.
     */
    struct Control
    {
        QString     name;
        QStringList label;
        quint32     width       = 0;
        int         defaultCode = 0;
    };
    QList<Control> control; /**< Declaration order */

    bool hasPull() const { return modeWidth > 0; }
    bool hasUp() const { return !upRows.isEmpty(); }
    bool hasDown() const { return !downRows.isEmpty(); }
    /** Whether the cell reaches this mode, natively or woven. */
    bool supports(int mode) const;
    /** Mode value of a name, or -1 when the cell has no such mode. */
    int modeCode(const QString &mode) const;
    /** Strength index within a direction, or -1. */
    int upSel(const QString &strength) const;
    int downSel(const QString &strength) const;
    /** Declaration index of a control name, or -1. */
    qsizetype controlIndex(const QString &name) const;
    /** Row index of a control label, or -1 when the control has no such row. */
    int controlCode(qsizetype index, const QString &label) const;
    /** The constants a pull request resolves to, 0 when it asks for nothing. */
    int requestMode(const QSocIomuxPullRequest &request) const;
    int requestUpSel(const QSocIomuxPullRequest &request) const;
    int requestDownSel(const QSocIomuxPullRequest &request) const;
    /** What a route's fixed request carries; a linked route reports its off row. */
    int routeMode(const QSocIomuxRoutePlan &route) const;
    int routeUpSel(const QSocIomuxRoutePlan &route) const;
    int routeDownSel(const QSocIomuxRoutePlan &route) const;
    int routeControlCode(const QSocIomuxRoutePlan &route, qsizetype index) const;
    /** Row code of a control label, the default when the label is empty, or -1. */
    int controlCodeOrDefault(qsizetype index, const QString &label) const;
    /** "0 none, 1 up, ..." over the modes this cell reaches. */
    QString modeSummary() const;
    /** The row a mode value selects, with the strength indices, or none. */
    const QSocPadTableRow &row(int mode, int upIndex, int downIndex) const;
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

/**
 * @brief Version of the register layout this generator emits.
 *
 * Software reads it from the first word of every instance. A block appended
 * after the existing ones is a minor step. Any existing offset that moves is
 * a major step.
 */
struct QSocIomuxLayoutVersion
{
    quint32 major = 2;
    quint32 minor = 0;
    quint32 patch = 0;
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
    /** Wrapper input that selects a pull or control row for one slot. */
    static QString selectPortName(quint32 pin, quint32 slot, const QString &group);
    /** The layout contract the identity word reports. */
    static QSocIomuxLayoutVersion layoutVersion();
    /** The type word every instance reports, "IOMX" read as a hex value. */
    static constexpr quint32 kTypeId = 0x494F4D58;
    /** Bytes the identity words occupy before the first selector. */
    static constexpr quint32 kIdentityBytes = 16;
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
