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
    QString                   name; /**< Class name: the key under pad_cells, or the cell */
    QString                   path; /**< Where the source declares it, for messages */
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
    QString instance;
    QString clock;
    QString reset;
    QString control;
    QString padInputValue;
    QString padInputEnable;
    QString padOutputValue;
    QString padOutputEnable;
    QString padIo; /**< Top-level pad net, used when a pad cell is declared */
    QString force; /**< Net that drives pad_force_i, used when safe is declared */

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
    std::optional<QSocPadTableRow> keeperRow;     /**< Native keeper row, if any */
    std::optional<QSocPadTableRow> oscillatorRow; /**< Native oscillator row, if any */
    QStringList                    namedMode;     /**< The model's other modes, name order */
    /** One per named mode, empty where this cell has no such row. */
    QList<std::optional<QSocPadTableRow>> namedRow;
    quint32                               modeWidth  = 0; /**< 0 when the cell has no pull table */
    quint32                               upSelWidth = 0; /**< 0 when up has at most one row */
    quint32                               downSelWidth = 0;
    bool weaves = false; /**< keeper and oscillator are woven from up and down */

    /**
     * @brief One control as software sees it.
     *
     * A single-row control has width 0: nothing to select, no field, no port.
     * Its row is still driven into the pad. A control the cell lacks has no
     * cell index and no rows.
     */
    struct Control
    {
        QString     name;
        QStringList label;
        quint32     width       = 0;
        int         defaultCode = 0;
        qsizetype   cellIndex   = -1; /**< Into the cell's control list, -1 when absent */
    };
    QList<Control> control; /**< Model order */

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

/**
 * @brief What the register map and the core see of the pad cells.
 *
 * The union over every class: a role or a pull the design has when some class
 * has it, each select as wide as the widest table, the controls in first
 * appearance order. With one class it is that class's own shape.
 */
struct QSocPadModel
{
    bool        inputValue   = false;
    bool        inputEnable  = false;
    bool        outputValue  = false;
    bool        outputEnable = false;
    bool        safe         = false; /**< Some class declares a safe row */
    bool        pull         = false; /**< Some class has a pull table */
    quint32     modeWidth    = 0;
    quint32     upSelWidth   = 0;
    quint32     downSelWidth = 0;
    QStringList namedMode; /**< Modes from FirstNamed, name order */
    struct Control
    {
        QString name;
        quint32 width = 0; /**< 0 when no class gives it a choice */

        bool operator==(const Control &) const = default;
    };
    QList<Control> control; /**< First appearance order */

    bool hasPull() const { return pull; }
    /** Whether some select owns a register field. */
    bool selectable() const
    {
        bool result = pull;
        for (const Control &item : control) {
            result = result || item.width > 0;
        }
        return result;
    }

    bool operator==(const QSocPadModel &) const = default;
};

/**
 * @brief One cell of the IO library: what the ring needs beyond its ports.
 */
/**
 * @brief What a cell is on one axis: the module, and its box when it differs.
 *
 * A width or height of zero means the base cell's, which is the usual case:
 * a variant that only redraws the inside keeps the box of the cell it
 * belongs to.
 */
struct QSocIoLibVariant
{
    QString cell;
    double  width  = 0;
    double  height = 0;

    bool operator==(const QSocIoLibVariant &) const = default;
};

/**
 * @brief One cell of the IO library: what the ring needs beyond its ports.
 *
 * The two sides of an axis are always 180 degrees apart, so a cell that sits
 * on both axes has to say what happens across them: `rotates` when one layout
 * serves both, or one entry per axis when the library draws two.
 */
struct QSocIoLibCell
{
    QString name;        /**< Module name, the key under io_lib */
    QString kind;        /**< signal, power, corner, fill, or other */
    double  width   = 0; /**< Along the die edge in its own frame, 0 when not given */
    double  height  = 0;
    bool    rotates = false;              /**< `variant: rotate` */
    QMap<QString, QSocIoLibVariant> axis; /**< `west_east` and `north_south` */

    bool declaresAxes() const { return rotates || !axis.isEmpty(); }

    bool operator==(const QSocIoLibCell &) const = default;
};

/**
 * @brief A cell on the ring that the mux does not drive: an oscillator, the
 * reset pad, a test pad. Its ports become ports of the wrapper.
 */
struct QSocIoRingDirect
{
    QString                key;
    QString                cell;
    QMap<QString, QString> port;      /**< Cell port to wrapper net, or a constant */
    QMap<QString, QString> cellPorts; /**< Port to direction, from the module library */

    bool operator==(const QSocIoRingDirect &) const = default;
};

/**
 * @brief One entry of a side, in placement order.
 */
struct QSocIoRingItem
{
    enum Kind { Pin, Power, Cell, Direct };

    Kind    kind = Pin;
    quint32 pin  = 0;    /**< Pin, for a signal pad */
    QString name;        /**< Power net, plain cell, or direct key */
    int     id = -1;     /**< Power: the instance number, or the running count */
    QString instance;    /**< Cell: an explicit instance name */
    double  offset = -1; /**< Microns from the corner, or -1 to follow the previous item */
    double  gap    = 0;  /**< Microns of space left before this item */

    bool operator==(const QSocIoRingItem &) const = default;
};

/**
 * @brief One placed cell of the ring, in microns from the die origin.
 */
struct QSocIoRingPlacement
{
    QString instance; /**< Path below the wrapper */
    QString cell;     /**< The module this side instantiates */
    QString base;     /**< The name the source wrote, which the library measures */
    QString meaning;
    QString side;
    double  offset = 0; /**< Along the side from its first corner */
    double  width  = 0; /**< Along the side */
    double  x      = 0; /**< Lower left of the oriented box */
    double  y      = 0;
    QString orient;
};

/**
 * @brief Where everything sits, or why it cannot be worked out yet.
 */
struct QSocIoRingGeometry
{
    bool        complete = false;
    QStringList missing; /**< What the geometry still needs */
    /**
     * @brief What no extra input can fix: a side that cannot hold its cells,
     * an offset that reaches back over the item before it.
     *
     * Missing information only leaves the DEF unwritten. A contradiction is
     * a source error and stops generation.
     */
    QStringList                contradiction;
    QList<QSocIoRingPlacement> placement;
};

/**
 * @brief The physical ring: what sits on each side, in order.
 *
 * Only identity is written here. Position is computed, so a pad that moves
 * keeps its instance name.
 */
struct QSocIoRingPlan
{
    bool                                 declared  = false;
    double                               dieWidth  = 0;
    double                               dieHeight = 0;
    QString                              corner;
    QString                              prefix; /**< DEF component prefix, default the instance */
    QMap<QString, QString>               orient; /**< Side or corner to DEF orientation */
    QMap<QString, QString>               power;  /**< Net to the cell that supplies it */
    QList<QSocIoRingDirect>              direct; /**< Declaration order */
    QMap<QString, QList<QSocIoRingItem>> side;   /**< west, south, east, north */

    bool operator==(const QSocIoRingPlan &) const = default;
};

struct QSocIomuxPlan
{
    QString                      moduleName;
    quint32                      pinCount = 0;
    quint32                      hsSlots  = 0;
    quint32                      build    = 0; /**< version[7:0], the design's own number */
    QSocIomuxOptionPlan          option;
    QList<QSocIomuxRoutePlan>    routes;
    QSocIomuxIntegrationPlan     integration;
    QList<QSocPadCellPlan>       padCells;        /**< Declaration order, indexed by pinClass */
    QList<int>                   pinClass;        /**< The class each pin instantiates */
    QStringList                  padModeOrder;    /**< `pad_model.mode`, names that lead */
    QStringList                  padControlOrder; /**< `pad_model.control`, names that lead */
    QSocPadModel                 padModel;        /**< The union the registers and core follow */
    QMap<QString, QSocIoLibCell> ioLib;
    QSocIoRingPlan               ioRing;
    QSocMmioPlan                 mmio;

    bool hasPadCell() const { return !padCells.isEmpty(); }
    /** The class a pin instantiates, or an undeclared cell when there is none. */
    const QSocPadCellPlan &padClass(quint32 pin) const;
    /** The side the ring puts a pin on, empty when the ring does not place it. */
    QString padSide(quint32 pin) const;
    /** The module a pin instantiates: its class's cell, or that cell's variant for its side. */
    QString padModule(quint32 pin) const;
    /** The module a ring cell takes on a side: `io_lib`'s variant for it, or the cell itself. */
    QString ringModule(const QString &cell, const QString &side) const;
    /** The module and the box a cell takes on a side; a zero width means the library has none. */
    QSocIoLibVariant ioLibBox(const QString &cell, const QString &side) const;

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
    /** Bytes the identity words occupy at offset 0. */
    static constexpr quint32 kIdentityBytes = 16;
    /**
     * Byte base of every register block. A block whose option is off leaves
     * its region empty, so an offset means the same thing on every design.
     */
    static constexpr quint64 kBaseSelector      = 0x100;
    static constexpr quint64 kBaseGpio          = 0x200;
    static constexpr quint64 kBaseRxOverride    = 0x300;
    static constexpr quint64 kBaseInterrupt     = 0x400;
    static constexpr quint64 kBaseInvert        = 0x800;
    static constexpr quint64 kBaseSourceControl = 0x1000;
    static constexpr quint64 kBasePadControl    = 0x1800;
    static constexpr quint64 kBaseControlWords  = 0x2000;
    /** Bytes between one `pin_ctl_k` block and the next. */
    static constexpr quint64 kControlWordStride = 0x800;
    /** Bytes the invert region holds: 32 banks of the widest instance. */
    static constexpr quint64 kInvertBytes = 0x400;
    /** Bytes the fixed map spans, so address_width is at least 14. */
    static constexpr quint64 kApertureBytes = 0x4000;
    /**
     * Bits per pin of every pull and control select between the core and the
     * pad module. The lane is fixed, whatever the table needs, so the bus keeps
     * its shape when a table grows and a pin's slice never moves.
     */
    static constexpr quint32 kPadLane = 4;
    /** `[4 * pin + 3 : 4 * pin]`, the slice of one pin in a select bus. */
    static QString padLane(quint32 pin);
    /** `value`, which is `width` bits wide, zero extended to one lane. */
    static QString padLaneValue(quint32 width, const QString &value);
    /**
     * @brief The union of the classes, which the register map and the core follow.
     *
     * Named modes and controls listed in the orders come first, in that order,
     * whether or not a class has them yet; the rest follow in first appearance
     * order. A listed name no class declares holds its number with nothing in it.
     */
    static QSocPadModel padModel(
        const QList<QSocPadCellPlan> &cells,
        const QStringList            &modeOrder    = {},
        const QStringList            &controlOrder = {});
    /** The rows of one class, numbered as the model numbers them. */
    static QSocPadEncoding padEncoding(const QSocPadCellPlan &cell, const QSocPadModel &model);
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
     * @brief Check every port a class declares against the library.
     *
     * The caller supplies the port table of the cell named by the class, so a
     * port that does not exist, or exists with the wrong direction, fails
     * before any Verilog is written.
     *
     * @param cell      the class
     * @param cellPorts port name to direction, "in", "out" or "inout"
     * @param errors    receives one message per rejected port
     * @return true when the declaration matches the cell
     */
    static bool checkPadCellPorts(
        const QSocPadCellPlan        &cell,
        const QMap<QString, QString> &cellPorts,
        QStringList                  *errors = nullptr);
    /**
     * @brief Check the port map of a direct ring cell against the library.
     *
     * Every input of the cell must be named, or the instance would leave it
     * floating; every named port must exist.
     */
    static bool checkDirectPorts(
        const QSocIoRingDirect       &direct,
        const QMap<QString, QString> &cellPorts,
        QStringList                  *errors = nullptr);
    /** The sides in placement order: west, south, east, north. */
    static const QStringList &ringSides();
    /** The axis a side belongs to, `west_east` or `north_south`; empty for a corner. */
    static QString ringAxis(const QString &side);
    /** Whether a DEF orientation turns the cell a quarter, which swaps its footprint. */
    static bool    ringQuarterTurn(const QString &orient);
    static QString generateCoreVerilog(const QSocIomuxPlan &plan);
    static QString generateConnVerilog(const QSocIomuxPlan &plan);
    static QString generateRegsVerilog(const QSocIomuxPlan &plan);
    static QString generateTopVerilog(const QSocIomuxPlan &plan);
    /**
     * @brief The pad shell: decode, cell instances, direct cells, and the ring.
     *
     * A sibling of the wrapper at the chip top, so pads, the reset and clock
     * sources, and everything physical stay out of the digital block. Empty
     * without a pad cell.
     */
    static QString generateIoVerilog(const QSocIomuxPlan &plan);
    /** `<module>_io`, the shell module and the suffix the module manager derives it by. */
    static QString ioModuleName(const QString &moduleName);
    /** The module library view of the shell, empty without a pad cell. */
    static YAML::Node describeIoModuleYaml(const QSocIomuxPlan &plan);
    /** Every ring instance by side, in order, with what it stands for. */
    static QString generateRingReport(const QSocIomuxPlan &plan);
    /**
     * @brief Place every ring cell from the die, the corner, and the widths.
     *
     * Items pack from the first corner of each side in order, an `offset`
     * pins one at a distance, a `gap` leaves space before one, and fill
     * cells from `io_lib` close every gap, largest first. Incomplete input
     * lists what is missing instead of guessing.
     */
    static QSocIoRingGeometry ringGeometry(const QSocIomuxPlan &plan);
    /** The DEF of the ring, empty when the geometry is incomplete. */
    static QString    generateRingDef(const QSocIomuxPlan &plan);
    static QString    generateFileList(const QSocIomuxPlan &plan);
    static QString    generateIntegrationNetlist(const QSocIomuxPlan &plan);
    static YAML::Node describeModuleYaml(const QSocIomuxPlan &plan);
    static QString    generateReport(const QSocIomuxPlan &plan);
};

#endif // QSOCIOMUXGENERATOR_H
