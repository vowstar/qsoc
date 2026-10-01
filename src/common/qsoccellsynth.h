// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCCELLSYNTH_H
#define QSOCCELLSYNTH_H

#include <QList>
#include <QString>
#include <QStringList>

#include <stop_token>

/* Clock-path roles the synthesizer can build. Role inputs, in order:
 * Buf (a), Inv (a), Mux2 (a, b, s) with out = s ? b : a, Or2 (a, b), Xor2 (a, b). */
enum class QSocCellSynthRole { Buf, Inv, Mux2, Or2, Xor2 };

/* One basis cell. Bit m of table is the output for the input pattern
 * m = sum(input[i] << i), so input[0] is the least significant bit. */
struct QSocCellSynthCell
{
    QString     name;
    QStringList input;
    QString     output;
    quint8      table = 0;
};

struct QSocCellSynthSource
{
    enum class Kind { Constant, Input, Gate };
    Kind kind = Kind::Constant;
    /* Constant value, role input index, or index of an earlier gate. */
    int index = 0;

    bool operator==(const QSocCellSynthSource &other) const = default;
};

struct QSocCellSynthGate
{
    QString                    cell;
    QList<QSocCellSynthSource> pin;

    bool operator==(const QSocCellSynthGate &other) const = default;
};

/* Gates are in topological order; the last gate drives the role output. */
struct QSocCellSynthNetlist
{
    QList<QSocCellSynthGate> gate;
    int                      depth = 0;

    bool operator==(const QSocCellSynthNetlist &other) const = default;
};

struct QSocCellSynthRequest
{
    QSocCellSynthRole        role = QSocCellSynthRole::Inv;
    QList<QSocCellSynthCell> basis;
    int                      maxCells = 8;
    /* Allow gate pins tied to 1'b0 or 1'b1. A signal never drives two pins of one gate. */
    bool                      constantTie          = true;
    static constexpr unsigned defaultResourceLimit = 200000000;
    /* Solver resource budget summed over every query. Zero is unlimited. */
    unsigned resourceLimit = defaultResourceLimit;
};

enum class QSocCellSynthStatus { Found, NoSolution, BudgetExceeded, Cancelled, Invalid };

struct QSocCellSynthResult
{
    QSocCellSynthStatus  status = QSocCellSynthStatus::Invalid;
    QSocCellSynthNetlist netlist;
    QString              reason;
    /* Structures the solver proposed, and how many of them failed the hazard check. */
    int      candidate    = 0;
    int      hazardous    = 0;
    unsigned resourceUsed = 0;
};

class QSocCellSynth
{
public:
    /**
     * @brief Exact synthesis of a clock role from the basis cells.
     * @details Minimum depth first, then minimum cell count, up to maxCells.
     *          Every minimum structure is enumerated and hazard checked. Of
     *          the survivors, the one with the fewest constant-tied pins, then
     *          the fewest cell inputs, then the smallest canonical form is
     *          returned, so the answer does not depend on the basis order.
     */
    static QSocCellSynthResult synthesize(
        const QSocCellSynthRequest &request, std::stop_token stop = {});

    /** Role input count and the truth table of the role over those inputs. */
    static int     roleInputCount(QSocCellSynthRole role);
    static quint8  roleTable(QSocCellSynthRole role);
    static QString roleName(QSocCellSynthRole role);

    /** Evaluate the netlist for one input pattern (bit i is role input i). */
    static bool evaluate(
        const QSocCellSynthNetlist &netlist, const QList<QSocCellSynthCell> &basis, int pattern);

    /**
     * @brief The hazard check synthesize() applies, on any netlist.
     * @details Every gate and wire has its own transport delay. For each input
     *          change the role allows (one input at a time, or both data
     *          inputs of Mux2 with the select held), the output must change
     *          exactly as often as the role function does, zero or one time.
     *          False for a malformed netlist.
     */
    static bool hazardFree(
        QSocCellSynthRole               role,
        const QSocCellSynthNetlist     &netlist,
        const QList<QSocCellSynthCell> &basis);

    /** One line: role, depth, cell count and per-cell usage. */
    static QString report(QSocCellSynthRole role, const QSocCellSynthNetlist &netlist);
};

#endif // QSOCCELLSYNTH_H
