// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoccellsynth.h"

#include <QMap>
#include <QSet>

#include <algorithm>
#include <array>
#include <memory>
#include <tuple>
#include <utility>
#include <vector>

#include <z3++.h>

namespace {

constexpr int kMaxPin = 3;
/* Leaf occurrences switching in one transition; 2^k subsets are walked. */
constexpr int kMaxSwitchingLeaf = 20;

/* Source codes: 0 and 1 are constants, then role inputs, then gates. */
struct Gate
{
    int                      type = 0;
    std::array<int, kMaxPin> src{};
};

using Net = std::vector<Gate>;

struct Ternary
{
    /* Per role input: 0, 1, or -1 for X. */
    std::vector<int> value;
    int              want = 0;
};

struct RoleSpec
{
    int                  input = 1;
    quint8               table = 0;
    std::vector<Ternary> ternary;
    /* Per context and role input: the held value, or -1 when the input switches. */
    std::vector<std::vector<int>> context;
};

RoleSpec roleSpec(QSocCellSynthRole role)
{
    RoleSpec spec;
    spec.input = QSocCellSynth::roleInputCount(role);
    spec.table = QSocCellSynth::roleTable(role);
    switch (role) {
    case QSocCellSynthRole::Buf:
    case QSocCellSynthRole::Inv:
        spec.context = {{-1}};
        break;
    case QSocCellSynthRole::Mux2:
        /* The select is quasi-static: only data transitions under a held select. */
        spec.ternary = {
            {{-1, 0, 1}, 0},
            {{-1, 1, 1}, 1},
            {{0, -1, 0}, 0},
            {{1, -1, 0}, 1},
        };
        spec.context = {{-1, -1, 0}, {-1, -1, 1}};
        break;
    case QSocCellSynthRole::Or2:
        spec.ternary = {{{-1, 1}, 1}, {{1, -1}, 1}};
        spec.context = {{-1, 0}, {-1, 1}, {0, -1}, {1, -1}};
        break;
    case QSocCellSynthRole::Xor2:
        spec.context = {{-1, 0}, {-1, 1}, {0, -1}, {1, -1}};
        break;
    }
    return spec;
}

int arity(const QSocCellSynthCell &cell)
{
    return static_cast<int>(cell.input.size());
}

bool cellValue(const QSocCellSynthCell &cell, int pattern)
{
    return ((cell.table >> pattern) & 1) != 0;
}

/* Pins p and q are interchangeable when swapping them leaves the table unchanged. */
bool pinSwapInvariant(const QSocCellSynthCell &cell, int p, int q)
{
    for (int m = 0; m < (1 << arity(cell)); ++m) {
        const int bp      = (m >> p) & 1;
        const int bq      = (m >> q) & 1;
        const int swapped = (m & ~((1 << p) | (1 << q))) | (bp << q) | (bq << p);
        if (cellValue(cell, m) != cellValue(cell, swapped)) {
            return false;
        }
    }
    return true;
}

QString validate(const QSocCellSynthRequest &request)
{
    if (request.basis.isEmpty()) {
        return QStringLiteral("Basis is empty.");
    }
    if (request.maxCells < 1 || request.maxCells > 8) {
        return QStringLiteral("Cell bound must be 1 to 8.");
    }
    QSet<QString> seen;
    for (const auto &cell : request.basis) {
        if (cell.name.isEmpty() || cell.output.isEmpty()) {
            return QStringLiteral("Basis cell needs a name and an output pin.");
        }
        if (seen.contains(cell.name)) {
            return QStringLiteral("Basis cell %1 is declared twice.").arg(cell.name);
        }
        seen.insert(cell.name);
        if (arity(cell) < 1 || arity(cell) > kMaxPin) {
            return QStringLiteral("Basis cell %1 must have 1 to 3 inputs.").arg(cell.name);
        }
        const QSet<QString> pin(cell.input.cbegin(), cell.input.cend());
        if (pin.size() != cell.input.size() || pin.contains(cell.output)
            || pin.contains(QString())) {
            return QStringLiteral("Basis cell %1 has duplicate or empty pins.").arg(cell.name);
        }
        if (arity(cell) < kMaxPin && (cell.table >> (1 << arity(cell))) != 0) {
            return QStringLiteral("Basis cell %1 table exceeds its inputs.").arg(cell.name);
        }
    }
    return {};
}

/* Evaluate one node of the network unfolded into a tree. Each role input
 * occurrence is a separate leaf consumed in depth-first order. */
bool evalTree(
    const Net                      &net,
    const QList<QSocCellSynthCell> &basis,
    int                             input,
    int                             code,
    const std::vector<char>        &leaf,
    size_t                         &cursor)
{
    if (code < 2) {
        return code == 1;
    }
    if (code < 2 + input) {
        return leaf[cursor++] != 0;
    }
    const Gate              &gate    = net[code - 2 - input];
    const QSocCellSynthCell &cell    = basis[gate.type];
    int                      pattern = 0;
    for (int j = 0; j < arity(cell); ++j) {
        if (evalTree(net, basis, input, gate.src[j], leaf, cursor)) {
            pattern |= 1 << j;
        }
    }
    return cellValue(cell, pattern);
}

void collectLeaf(
    const Net                      &net,
    const QList<QSocCellSynthCell> &basis,
    int                             input,
    int                             code,
    std::vector<int>               &leaf)
{
    if (code < 2) {
        return;
    }
    if (code < 2 + input) {
        leaf.push_back(code - 2);
        return;
    }
    const Gate &gate = net[code - 2 - input];
    for (int j = 0; j < arity(basis[gate.type]); ++j) {
        collectLeaf(net, basis, input, gate.src[j], leaf);
    }
}

struct Unfolded
{
    const Net                      &net;
    const QList<QSocCellSynthCell> &basis;
    int                             input = 0;
    int                             root  = 0;
    /* Role input driving each leaf occurrence, in depth-first order. */
    std::vector<int> leafInput;
};

/* Delay model: every leaf of the unfolded tree has its own pure delay, so
 * the switching leaves change in any order. Every order must move the
 * output exactly as often as the role function says, zero or one time.
 * The subset lattice holds the fewest and most output changes over all
 * orders that reach each set of switched leaves. */
bool transitionClean(const Unfolded &tree, int begin, int end, int want)
{
    std::vector<char>   leaf(tree.leafInput.size());
    std::vector<size_t> moving;
    for (size_t l = 0; l < tree.leafInput.size(); ++l) {
        leaf[l] = static_cast<char>((begin >> tree.leafInput[l]) & 1);
        if (((begin ^ end) >> tree.leafInput[l]) & 1) {
            moving.push_back(l);
        }
    }
    if (moving.size() > static_cast<size_t>(kMaxSwitchingLeaf)) {
        return false;
    }
    const size_t      states = size_t{1} << moving.size();
    std::vector<int>  fewest(states, 1 << 30);
    std::vector<int>  most(states, 0);
    std::vector<char> out(states, 0);
    fewest[0] = 0;
    for (size_t s = 0; s < states; ++s) {
        std::vector<char> now = leaf;
        for (size_t b = 0; b < moving.size(); ++b) {
            now[moving[b]] ^= static_cast<char>((s >> b) & 1);
        }
        size_t cursor = 0;
        out[s]        = static_cast<char>(
            evalTree(tree.net, tree.basis, tree.input, tree.root, now, cursor));
        for (size_t b = 0; b < moving.size(); ++b) {
            const size_t prev = s & ~(size_t{1} << b);
            if (prev == s) {
                continue;
            }
            const int step = out[prev] != out[s] ? 1 : 0;
            fewest[s]      = std::min(fewest[s], fewest[prev] + step);
            most[s]        = std::max(most[s], most[prev] + step);
        }
    }
    return fewest[states - 1] == want && most[states - 1] == want;
}

bool dynamicHazardFree(const Net &net, const QList<QSocCellSynthCell> &basis, const RoleSpec &spec)
{
    Unfolded tree{net, basis, spec.input, 2 + spec.input + static_cast<int>(net.size()) - 1, {}};
    collectLeaf(net, basis, spec.input, tree.root, tree.leafInput);

    for (const auto &held : spec.context) {
        int heldMask = 0;
        int heldHigh = 0;
        for (int i = 0; i < spec.input; ++i) {
            heldMask |= held[i] >= 0 ? 1 << i : 0;
            heldHigh |= held[i] > 0 ? 1 << i : 0;
        }
        const int all = (1 << spec.input) - 1;
        /* Every start pattern and every nonempty change of the free inputs. */
        for (int begin = 0; begin <= all; ++begin) {
            for (int flip = 1; flip <= all; ++flip) {
                if ((begin & heldMask) != heldHigh || (flip & heldMask) != 0) {
                    continue;
                }
                const int end  = begin ^ flip;
                const int want = ((spec.table >> begin) & 1) != ((spec.table >> end) & 1) ? 1 : 0;
                if (!transitionClean(tree, begin, end, want)) {
                    return false;
                }
            }
        }
    }
    return true;
}

/* Canonical key of a gate: level, cell name, then source codes in pin order. */
struct Key
{
    int        level = 0;
    QString    name;
    QList<int> src;

    bool operator<(const Key &other) const
    {
        if (level != other.level) {
            return level < other.level;
        }
        if (name != other.name) {
            return name < other.name;
        }
        return src < other.src;
    }
    bool operator==(const Key &other) const = default;
};

struct Canonical
{
    QList<Key>           key;
    QSocCellSynthNetlist netlist;
};

/* Preference among equally deep and large networks: fewer tied pins, then
 * fewer cell inputs, then the canonical key so the choice is unique. */
std::tuple<int, int, const QList<Key> &> rank(const Canonical &network)
{
    int tied  = 0;
    int input = 0;
    for (const Key &key : network.key) {
        input += static_cast<int>(key.src.size());
        tied += static_cast<int>(
            std::count_if(key.src.cbegin(), key.src.cend(), [](int code) { return code < 2; }));
    }
    return {tied, input, network.key};
}

/* Sort interchangeable pins so pin order carries no information. */
void sortInterchangeable(const QSocCellSynthCell &cell, QList<int> &src)
{
    for (bool moved = true; moved;) {
        moved = false;
        for (int p = 0; p < arity(cell); ++p) {
            for (int q = p + 1; q < arity(cell); ++q) {
                if (src[p] > src[q] && pinSwapInvariant(cell, p, q)) {
                    src.swapItemsAt(p, q);
                    moved = true;
                }
            }
        }
    }
}

/* Key of a gate whose gate sources are all placed, with renamed sources. */
bool readyKey(
    const Gate                     &gate,
    const QList<QSocCellSynthCell> &basis,
    int                             input,
    int                             level,
    const std::vector<int>         &renamed,
    Key                            &key)
{
    const QSocCellSynthCell &cell = basis[gate.type];
    key                           = {level, cell.name, {}};
    for (int j = 0; j < arity(cell); ++j) {
        const int code = gate.src[j];
        const int src  = code - 2 - input;
        if (src >= 0 && renamed[src] < 0) {
            return false;
        }
        key.src.append(src >= 0 ? 2 + input + renamed[src] : code);
    }
    sortInterchangeable(cell, key.src);
    return true;
}

Canonical canonicalise(const Net &net, const QList<QSocCellSynthCell> &basis, int input)
{
    const int        count = static_cast<int>(net.size());
    std::vector<int> level(count, 1);
    for (int g = 0; g < count; ++g) {
        for (int j = 0; j < arity(basis[net[g].type]); ++j) {
            const int src = net[g].src[j] - 2 - input;
            if (src >= 0) {
                level[g] = std::max(level[g], level[src] + 1);
            }
        }
    }

    /* The solver indexes gates topologically, so the lowest unplaced gate
     * is always ready and every step places one gate. */
    std::vector<int> renamed(count, -1);
    Canonical        result;
    for (int step = 0; step < count; ++step) {
        int best = -1;
        Key bestKey;
        for (int g = 0; g < count; ++g) {
            Key key;
            if (renamed[g] < 0 && readyKey(net[g], basis, input, level[g], renamed, key)
                && (best < 0 || key < bestKey)) {
                best    = g;
                bestKey = key;
            }
        }
        if (best < 0) {
            break;
        }
        renamed[best] = step;
        result.key.append(bestKey);
        QSocCellSynthGate gate;
        gate.cell = bestKey.name;
        for (const int code : bestKey.src) {
            if (code < 2) {
                gate.pin.append({QSocCellSynthSource::Kind::Constant, code});
            } else if (code < 2 + input) {
                gate.pin.append({QSocCellSynthSource::Kind::Input, code - 2});
            } else {
                gate.pin.append({QSocCellSynthSource::Kind::Gate, code - 2 - input});
            }
        }
        result.netlist.gate.append(gate);
        result.netlist.depth = std::max(result.netlist.depth, bestKey.level);
    }
    return result;
}

/* Solver encoding for a fixed gate count. The last gate is the output. */
class Encoding
{
public:
    Encoding(
        z3::context &context, const QSocCellSynthRequest &request, const RoleSpec &spec, int count)
        : m_context(context)
        , m_solver(context)
        , m_type(context)
        , m_src(context)
        , m_level(context)
        , m_basis(request.basis)
        , m_input(spec.input)
        , m_count(count)
    {
        m_solver.set("ctrl_c", false);
        build(request, spec);
    }

    z3::solver &solver() { return m_solver; }

    z3::expr depthBound(int depth) { return m_level[m_count - 1] <= depth; }

    Net decode(const z3::model &model) const
    {
        Net net(m_count);
        for (int i = 0; i < m_count; ++i) {
            net[i].type = model.eval(m_type[i], true).get_numeral_int();
            for (int j = 0; j < kMaxPin; ++j) {
                net[i].src[j] = model.eval(src(i, j), true).get_numeral_int();
            }
        }
        return net;
    }

    void block(const Net &net)
    {
        z3::expr_vector differ(m_context);
        for (int i = 0; i < m_count; ++i) {
            differ.push_back(m_type[i] != net[i].type);
            for (int j = 0; j < kMaxPin; ++j) {
                differ.push_back(src(i, j) != net[i].src[j]);
            }
        }
        m_solver.add(z3::mk_or(differ));
    }

private:
    z3::expr src(int gate, int pin) const { return m_src[gate * kMaxPin + pin]; }

    int gateCode(int gate) const { return 2 + m_input + gate; }

    /* Selected value of a pin: OR over the sources it may connect to. */
    z3::expr pick(int gate, int pin, const std::vector<z3::expr> &value) const
    {
        z3::expr_vector term(m_context);
        for (int k = 0; k < gateCode(gate); ++k) {
            term.push_back(src(gate, pin) == k && value[k]);
        }
        return z3::mk_or(term);
    }

    void build(const QSocCellSynthRequest &request, const RoleSpec &spec)
    {
        const int cells   = static_cast<int>(m_basis.size());
        const int minterm = 1 << m_input;

        for (int i = 0; i < m_count; ++i) {
            const QByteArray t = "t" + QByteArray::number(i);
            m_type.push_back(m_context.int_const(t.constData()));
            m_solver.add(m_type[i] >= 0 && m_type[i] < cells);
            const QByteArray l = "l" + QByteArray::number(i);
            m_level.push_back(m_context.int_const(l.constData()));
            m_solver.add(m_level[i] >= 1);
            for (int j = 0; j < kMaxPin; ++j) {
                const QByteArray s = "s" + QByteArray::number(i) + "_" + QByteArray::number(j);
                m_src.push_back(m_context.int_const(s.constData()));
                m_solver.add(src(i, j) >= 0 && src(i, j) < gateCode(i));
                for (int g = 0; g < i; ++g) {
                    m_solver.add(
                        z3::implies(src(i, j) == gateCode(g), m_level[i] >= m_level[g] + 1));
                }
            }
            for (int c = 0; c < cells; ++c) {
                const QSocCellSynthCell &cell = m_basis[c];
                z3::expr_vector          rule(m_context);
                for (int j = 0; j < kMaxPin; ++j) {
                    if (j >= arity(cell)) {
                        rule.push_back(src(i, j) == 0);
                    } else if (!request.constantTie) {
                        rule.push_back(src(i, j) >= 2);
                    }
                    for (int k = j + 1; k < arity(cell); ++k) {
                        rule.push_back(src(i, j) < 2 || src(i, j) != src(i, k));
                        if (pinSwapInvariant(cell, j, k)) {
                            rule.push_back(src(i, j) <= src(i, k));
                        }
                    }
                }
                if (!rule.empty()) {
                    m_solver.add(z3::implies(m_type[i] == c, z3::mk_and(rule)));
                }
            }
        }

        /* Two-valued function over every input pattern. */
        for (int m = 0; m < minterm; ++m) {
            std::vector<z3::expr> value{m_context.bool_val(false), m_context.bool_val(true)};
            for (int k = 0; k < m_input; ++k) {
                value.push_back(m_context.bool_val(((m >> k) & 1) != 0));
            }
            for (int i = 0; i < m_count; ++i) {
                const QByteArray v   = "v" + QByteArray::number(i) + "_" + QByteArray::number(m);
                const z3::expr   out = m_context.bool_const(v.constData());
                addGateFunction(
                    i,
                    [&](int pin, int bit) {
                        const z3::expr in = pick(i, pin, value);
                        return bit != 0 ? in : !in;
                    },
                    out,
                    true);
                value.push_back(out);
            }
            m_solver.add(value.back() == m_context.bool_val(((spec.table >> m) & 1) != 0));
        }

        /* Dual-rail ternary masking: can0/can1 per node, X means both. */
        for (size_t r = 0; r < spec.ternary.size(); ++r) {
            const Ternary        &req = spec.ternary[r];
            std::vector<z3::expr> can0{m_context.bool_val(true), m_context.bool_val(false)};
            std::vector<z3::expr> can1{m_context.bool_val(false), m_context.bool_val(true)};
            for (int k = 0; k < m_input; ++k) {
                can0.push_back(m_context.bool_val(req.value[k] != 1));
                can1.push_back(m_context.bool_val(req.value[k] != 0));
            }
            for (int i = 0; i < m_count; ++i) {
                const QByteArray base = "x" + QByteArray::number(static_cast<int>(r)) + "_"
                                        + QByteArray::number(i);
                const z3::expr   c0   = m_context.bool_const((base + "_0").constData());
                const z3::expr   c1   = m_context.bool_const((base + "_1").constData());
                auto pinCan = [&](int pin, int bit) { return pick(i, pin, bit != 0 ? can1 : can0); };
                addGateFunction(i, pinCan, c1, true);
                addGateFunction(i, pinCan, c0, false);
                can0.push_back(c0);
                can1.push_back(c1);
            }
            m_solver.add(req.want != 0 ? !can0.back() : !can1.back());
        }

        /* Every gate but the output drives a later gate. */
        for (int g = 0; g + 1 < m_count; ++g) {
            z3::expr_vector use(m_context);
            for (int i = g + 1; i < m_count; ++i) {
                for (int j = 0; j < kMaxPin; ++j) {
                    use.push_back(src(i, j) == gateCode(g));
                }
            }
            m_solver.add(z3::mk_or(use));
        }

        /* Adjacent independent gates appear in key order. Choosing each next
         * gate as the smallest ready key yields such an order for any DAG. */
        for (int i = 0; i + 1 < m_count; ++i) {
            z3::expr_vector independent(m_context);
            for (int j = 0; j < kMaxPin; ++j) {
                independent.push_back(src(i + 1, j) != gateCode(i));
            }
            z3::expr ordered = m_type[i] <= m_type[i + 1];
            for (int j = kMaxPin - 1; j >= 0; --j) {
                ordered = src(i, j) < src(i + 1, j) || (src(i, j) == src(i + 1, j) && ordered);
            }
            m_solver.add(z3::implies(z3::mk_and(independent), ordered));
        }
    }

    /* out == OR of the table rows that produce polarity, per basis cell. */
    template<typename PinLiteral>
    void addGateFunction(int gate, PinLiteral literal, const z3::expr &out, bool polarity)
    {
        for (int c = 0; c < static_cast<int>(m_basis.size()); ++c) {
            const QSocCellSynthCell &cell = m_basis[c];
            z3::expr_vector          row(m_context);
            for (int p = 0; p < (1 << arity(cell)); ++p) {
                if (cellValue(cell, p) != polarity) {
                    continue;
                }
                z3::expr_vector term(m_context);
                for (int j = 0; j < arity(cell); ++j) {
                    term.push_back(literal(j, (p >> j) & 1));
                }
                row.push_back(z3::mk_and(term));
            }
            m_solver.add(z3::implies(m_type[gate] == c, out == z3::mk_or(row)));
        }
    }

    z3::context             &m_context;
    z3::solver               m_solver;
    z3::expr_vector          m_type;
    z3::expr_vector          m_src;
    z3::expr_vector          m_level;
    QList<QSocCellSynthCell> m_basis;
    int                      m_input = 0;
    int                      m_count = 0;
};

unsigned resourceCount(const z3::solver &solver)
{
    const z3::stats stats = solver.statistics();
    for (unsigned i = 0; i < stats.size(); ++i) {
        if (stats.key(i) == "rlimit count") {
            return stats.is_uint(i) ? stats.uint_value(i)
                                    : static_cast<unsigned>(stats.double_value(i));
        }
    }
    return 0;
}

/* Depth-major search over gate counts, then enumeration of every minimum
 * structure. Hazardous structures stay blocked in their encoding. */
class Search
{
public:
    Search(
        z3::context                &context,
        const QSocCellSynthRequest &request,
        std::stop_token             stop,
        QSocCellSynthResult        &result)
        : m_context(context)
        , m_request(request)
        , m_spec(roleSpec(request.role))
        , m_stop(std::move(stop))
        , m_result(result)
        , m_encoding(request.maxCells + 1)
        , m_feasible(request.maxCells + 1, -1)
    {}

    void run()
    {
        int pin = 1;
        for (const auto &cell : m_request.basis) {
            pin = std::max(pin, arity(cell));
        }
        /* Gates of height h feed gates of height h - 1, so depth d holds at
         * most 1 + pin + ... + pin^(d-1) gates. */
        int capacity = 0;
        int layer    = 1;
        for (int depth = 1; depth <= m_request.maxCells; ++depth) {
            capacity = std::min(m_request.maxCells, capacity + layer);
            layer    = std::min(m_request.maxCells, layer * pin);
            for (int count = depth; count <= capacity; ++count) {
                if (!solveAt(depth, count)) {
                    return;
                }
            }
        }
        m_result.status = QSocCellSynthStatus::NoSolution;
        m_result.reason
            = QStringLiteral("No hazard-free structure within %1 cells.").arg(m_request.maxCells);
    }

private:
    Encoding &encoding(int count)
    {
        if (!m_encoding[count]) {
            m_encoding[count] = std::make_unique<Encoding>(m_context, m_request, m_spec, count);
        }
        return *m_encoding[count];
    }

    /* One solver call under the shared budget. False ends the search. */
    bool check(Encoding &enc, const z3::expr_vector &assumption, z3::check_result &status)
    {
        if (m_stop.stop_requested()) {
            m_result.status = QSocCellSynthStatus::Cancelled;
            m_result.reason = QStringLiteral("Synthesis cancelled.");
            return false;
        }
        if (m_request.resourceLimit != 0) {
            if (m_result.resourceUsed >= m_request.resourceLimit) {
                m_result.status = QSocCellSynthStatus::BudgetExceeded;
                m_result.reason = QStringLiteral("Resource budget spent.");
                return false;
            }
            enc.solver().set("rlimit", m_request.resourceLimit - m_result.resourceUsed);
        }
        status                = enc.solver().check(assumption);
        m_result.resourceUsed = std::max(m_result.resourceUsed, resourceCount(enc.solver()));
        if (status != z3::unknown) {
            return true;
        }
        m_result.status = m_stop.stop_requested() ? QSocCellSynthStatus::Cancelled
                                                  : QSocCellSynthStatus::BudgetExceeded;
        m_result.reason = QString::fromStdString(enc.solver().reason_unknown());
        return false;
    }

    /* A count with no structure at any depth is skipped at every later depth. */
    bool feasible(int count, bool &ok)
    {
        if (m_feasible[count] < 0) {
            z3::check_result status = z3::unknown;
            ok                      = check(encoding(count), z3::expr_vector(m_context), status);
            m_feasible[count]       = status == z3::sat ? 1 : 0;
        }
        return m_feasible[count] != 0;
    }

    /* False when the search is over: found, cancelled, or out of budget. */
    bool solveAt(int depth, int count)
    {
        bool ok = true;
        if (!feasible(count, ok)) {
            return ok;
        }
        Encoding       &enc = encoding(count);
        z3::expr_vector bound(m_context);
        bound.push_back(enc.depthBound(depth));
        QList<Canonical> survivor;
        z3::check_result status = z3::unknown;
        while (check(enc, bound, status)) {
            if (status == z3::unsat) {
                if (survivor.isEmpty()) {
                    return true;
                }
                found(survivor);
                return false;
            }
            const Net net = enc.decode(enc.solver().get_model());
            enc.block(net);
            ++m_result.candidate;
            if (dynamicHazardFree(net, m_request.basis, m_spec)) {
                survivor.append(canonicalise(net, m_request.basis, m_spec.input));
            } else {
                ++m_result.hazardous;
            }
        }
        return false;
    }

    void found(const QList<Canonical> &survivor)
    {
        const auto best = std::min_element(
            survivor.cbegin(), survivor.cend(), [](const Canonical &l, const Canonical &r) {
                return rank(l) < rank(r);
            });
        m_result.status  = QSocCellSynthStatus::Found;
        m_result.netlist = best->netlist;
    }

    z3::context                           &m_context;
    const QSocCellSynthRequest            &m_request;
    RoleSpec                               m_spec;
    std::stop_token                        m_stop;
    QSocCellSynthResult                   &m_result;
    std::vector<std::unique_ptr<Encoding>> m_encoding;
    std::vector<int>                       m_feasible;
};

const QSocCellSynthCell *findCell(const QList<QSocCellSynthCell> &basis, const QString &name)
{
    for (const auto &cell : basis) {
        if (cell.name == name) {
            return &cell;
        }
    }
    return nullptr;
}

/* Solver source code of a netlist source, or -1 when it is out of range. */
int sourceCode(const QSocCellSynthSource &source, int input, int gates)
{
    switch (source.kind) {
    case QSocCellSynthSource::Kind::Constant:
        return source.index == 0 || source.index == 1 ? source.index : -1;
    case QSocCellSynthSource::Kind::Input:
        return source.index >= 0 && source.index < input ? 2 + source.index : -1;
    case QSocCellSynthSource::Kind::Gate:
        return source.index >= 0 && source.index < gates ? 2 + input + source.index : -1;
    }
    return -1;
}

} // namespace

int QSocCellSynth::roleInputCount(QSocCellSynthRole role)
{
    switch (role) {
    case QSocCellSynthRole::Buf:
    case QSocCellSynthRole::Inv:
        return 1;
    case QSocCellSynthRole::Mux2:
        return 3;
    case QSocCellSynthRole::Or2:
    case QSocCellSynthRole::Xor2:
        return 2;
    }
    return 0;
}

quint8 QSocCellSynth::roleTable(QSocCellSynthRole role)
{
    switch (role) {
    case QSocCellSynthRole::Buf:
        return 0b10;
    case QSocCellSynthRole::Inv:
        return 0b01;
    case QSocCellSynthRole::Mux2:
        /* Pattern bits (a, b, s): s ? b : a. */
        return 0b11001010;
    case QSocCellSynthRole::Or2:
        return 0b1110;
    case QSocCellSynthRole::Xor2:
        return 0b0110;
    }
    return 0;
}

QString QSocCellSynth::roleName(QSocCellSynthRole role)
{
    switch (role) {
    case QSocCellSynthRole::Buf:
        return QStringLiteral("ck_buf");
    case QSocCellSynthRole::Inv:
        return QStringLiteral("ck_inv");
    case QSocCellSynthRole::Mux2:
        return QStringLiteral("ck_mux2");
    case QSocCellSynthRole::Or2:
        return QStringLiteral("ck_or2");
    case QSocCellSynthRole::Xor2:
        return QStringLiteral("ck_xor2");
    }
    return {};
}

QSocCellSynthResult QSocCellSynth::synthesize(
    const QSocCellSynthRequest &request, std::stop_token stop)
{
    QSocCellSynthResult result;
    result.reason = validate(request);
    if (!result.reason.isEmpty()) {
        return result;
    }
    try {
        z3::context context;
        /* The callback must finish before the context leaves scope. */
        std::stop_callback cancel(stop, [&context] { context.interrupt(); });
        Search             search(context, request, stop, result);
        search.run();
    } catch (const z3::exception &error) {
        result.status = stop.stop_requested() ? QSocCellSynthStatus::Cancelled
                                              : QSocCellSynthStatus::Invalid;
        result.reason = QString::fromUtf8(error.msg());
    }
    return result;
}

bool QSocCellSynth::evaluate(
    const QSocCellSynthNetlist &netlist, const QList<QSocCellSynthCell> &basis, int pattern)
{
    QList<bool> value;
    for (const auto &gate : netlist.gate) {
        const QSocCellSynthCell *cell = findCell(basis, gate.cell);
        if (cell == nullptr) {
            return false;
        }
        int in = 0;
        for (int j = 0; j < gate.pin.size(); ++j) {
            const QSocCellSynthSource &source = gate.pin[j];
            bool                       bit    = false;
            switch (source.kind) {
            case QSocCellSynthSource::Kind::Constant:
                bit = source.index != 0;
                break;
            case QSocCellSynthSource::Kind::Input:
                bit = ((pattern >> source.index) & 1) != 0;
                break;
            case QSocCellSynthSource::Kind::Gate:
                bit = value.value(source.index);
                break;
            }
            in |= (bit ? 1 : 0) << j;
        }
        value.append(cellValue(*cell, in));
    }
    return !value.isEmpty() && value.last();
}

bool QSocCellSynth::hazardFree(
    QSocCellSynthRole               role,
    const QSocCellSynthNetlist     &netlist,
    const QList<QSocCellSynthCell> &basis)
{
    const RoleSpec spec = roleSpec(role);
    Net            net;
    for (const auto &gate : netlist.gate) {
        const QSocCellSynthCell *cell = findCell(basis, gate.cell);
        if (cell == nullptr || gate.pin.size() != arity(*cell)) {
            return false;
        }
        Gate code;
        code.type = static_cast<int>(cell - basis.constData());
        for (int j = 0; j < gate.pin.size(); ++j) {
            code.src[j] = sourceCode(gate.pin[j], spec.input, static_cast<int>(net.size()));
            if (code.src[j] < 0) {
                return false;
            }
        }
        net.push_back(code);
    }
    return !net.empty() && dynamicHazardFree(net, basis, spec);
}

QString QSocCellSynth::report(QSocCellSynthRole role, const QSocCellSynthNetlist &netlist)
{
    QMap<QString, int> usage;
    for (const auto &gate : netlist.gate) {
        ++usage[gate.cell];
    }
    QStringList part;
    for (auto it = usage.cbegin(); it != usage.cend(); ++it) {
        part.append(QStringLiteral("%1 x%2").arg(it.key()).arg(it.value()));
    }
    return QStringLiteral("%1: depth %2, %3 cells (%4)")
        .arg(roleName(role))
        .arg(netlist.depth)
        .arg(netlist.gate.size())
        .arg(part.join(QStringLiteral(", ")));
}
