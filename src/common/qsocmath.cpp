// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocmath.h"
#include "tui/qtuiwidget.h"

#include <algorithm>
#include <QHash>

namespace QSocMath {
namespace {
/* Consume complete source lines. Excluded ranges use UTF-16 offsets. */
class Scanner
{
public:
    QList<Span>         feedLine(const QString &line, const QList<Range> &excluded = {});
    std::optional<Span> unfinished() const;

private:
    int  offset        = 0;
    int  excludedIndex = 0;
    int  start         = -1;
    bool display       = false;
};

bool escaped(const QString &text, int at)
{
    int count = 0;
    while (at > 0 && text[--at] == QLatin1Char('\\'))
        ++count;
    return count % 2 != 0;
}

bool boundary(QChar ch)
{
    return !ch.isLetterOrNumber() && ch != QLatin1Char('_') && ch != QLatin1Char('$');
}

Layout text(const QString &value)
{
    return {{value}, QTuiText::visualWidth(value), 0};
}

QString padded(const QString &value, int width, bool center = false)
{
    const int extra = width - QTuiText::visualWidth(value);
    const int left  = center ? extra / 2 : 0;
    return QString(left, QLatin1Char(' ')) + value + QString(extra - left, QLatin1Char(' '));
}

const QHash<QString, QString> &symbols()
{
    static const QHash<QString, QString> values = {
        {"alpha", "α"},      {"beta", "β"},      {"gamma", "γ"},    {"delta", "δ"},
        {"epsilon", "ε"},    {"zeta", "ζ"},      {"eta", "η"},      {"theta", "θ"},
        {"iota", "ι"},       {"kappa", "κ"},     {"lambda", "λ"},   {"mu", "μ"},
        {"nu", "ν"},         {"xi", "ξ"},        {"pi", "π"},       {"rho", "ρ"},
        {"sigma", "σ"},      {"tau", "τ"},       {"upsilon", "υ"},  {"phi", "φ"},
        {"chi", "χ"},        {"psi", "ψ"},       {"omega", "ω"},    {"Gamma", "Γ"},
        {"Delta", "Δ"},      {"Theta", "Θ"},     {"Lambda", "Λ"},   {"Xi", "Ξ"},
        {"Pi", "Π"},         {"Sigma", "Σ"},     {"Upsilon", "Υ"},  {"Phi", "Φ"},
        {"Psi", "Ψ"},        {"Omega", "Ω"},     {"times", "×"},    {"cdot", "·"},
        {"div", "÷"},        {"pm", "±"},        {"le", "≤"},       {"leq", "≤"},
        {"ge", "≥"},         {"geq", "≥"},       {"ne", "≠"},       {"neq", "≠"},
        {"approx", "≈"},     {"equiv", "≡"},     {"infty", "∞"},    {"in", "∈"},
        {"notin", "∉"},      {"subset", "⊂"},    {"subseteq", "⊆"}, {"cup", "∪"},
        {"cap", "∩"},        {"emptyset", "∅"},  {"forall", "∀"},   {"exists", "∃"},
        {"neg", "¬"},        {"land", "∧"},      {"lor", "∨"},      {"to", "→"},
        {"rightarrow", "→"}, {"leftarrow", "←"}, {"partial", "∂"},  {"nabla", "∇"},
        {"sum", "∑"},        {"prod", "∏"},      {"int", "∫"},      {"ldots", "…"},
        {"cdots", "⋯"},
    };
    return values;
}

class Parser
{
public:
    Parser(const QString &input, bool block, int columns)
        : source(input)
        , display(block)
        , maxWidth(std::min(columns, 256))
    {}

    std::optional<Layout> parse()
    {
        auto result = sequence(0, false);
        if (!result || pos != source.size() || result->width == 0)
            return std::nullopt;
        return result;
    }
    int work = 0;

private:
    const QString &source;
    bool           display;
    int            maxWidth;
    int            pos      = 0;
    int            cells    = 0;
    bool           inMatrix = false;

    bool starts(const QString &value) const
    {
        return QStringView(source).mid(pos).startsWith(value);
    }
    void skipSpace()
    {
        while (pos < source.size() && source[pos].isSpace()) {
            ++pos;
            ++work;
        }
    }
    bool fits(const Layout &value) const
    {
        return value.rows.size() <= 16 && value.width <= maxWidth;
    }
    std::optional<Layout> join(const Layout &left, const Layout &right) const
    {
        const int above = std::max(left.baseline, right.baseline);
        const int below = std::max(
            int(left.rows.size()) - left.baseline - 1, int(right.rows.size()) - right.baseline - 1);
        Layout result{{}, left.width + right.width, above};
        if (above + below + 1 > 16 || result.width > maxWidth)
            return std::nullopt;
        for (int row = 0; row <= above + below; ++row) {
            const int     lrow = row - above + left.baseline;
            const int     rrow = row - above + right.baseline;
            const QString l    = lrow >= 0 && lrow < left.rows.size() ? left.rows[lrow] : QString();
            const QString r = rrow >= 0 && rrow < right.rows.size() ? right.rows[rrow] : QString();
            result.rows.append(padded(l, left.width) + padded(r, right.width));
        }
        return result;
    }
    std::optional<Layout> argument(int depth)
    {
        skipSpace();
        if (pos >= source.size() || source[pos++] != QLatin1Char('{'))
            return std::nullopt;
        auto value = sequence(depth + 1, false);
        if (!value || pos >= source.size() || source[pos++] != QLatin1Char('}'))
            return std::nullopt;
        return value;
    }
    std::optional<Layout> fraction(int depth)
    {
        auto numerator   = argument(depth);
        auto denominator = argument(depth);
        if (!numerator || !denominator || !numerator->width || !denominator->width)
            return std::nullopt;
        if (!display) {
            auto value = text(
                "(" + numerator->rows.first() + ")/(" + denominator->rows.first() + ")");
            return fits(value) ? std::optional(value) : std::nullopt;
        }
        const int width = std::max(numerator->width, denominator->width) + 2;
        Layout    value{{}, width, int(numerator->rows.size())};
        if (width > maxWidth || numerator->rows.size() + denominator->rows.size() + 1 > 16)
            return std::nullopt;
        for (const auto &row : numerator->rows)
            value.rows.append(padded(row, width, true));
        value.rows.append(QString(width, QChar(0x2500)));
        for (const auto &row : denominator->rows)
            value.rows.append(padded(row, width, true));
        return value;
    }
    std::optional<Layout> root(int depth)
    {
        auto inner = argument(depth);
        if (!inner || !inner->width)
            return std::nullopt;
        if (!display) {
            auto value = text(QStringLiteral("√(") + inner->rows.first() + ")");
            return fits(value) ? std::optional(value) : std::nullopt;
        }
        Layout value{{}, inner->width + 1, inner->baseline + 1};
        if (value.width > maxWidth || inner->rows.size() + 1 > 16)
            return std::nullopt;
        value.rows.append(" " + QString(inner->width, QChar(0x2500)));
        for (int row = 0; row < inner->rows.size(); ++row)
            value.rows.append(
                (row == inner->baseline ? QStringLiteral("√") : QStringLiteral(" "))
                + inner->rows[row]);
        return value;
    }
    std::optional<Layout> matrix(int depth)
    {
        if (!display || inMatrix || depth >= 32 || pos >= source.size()
            || source[pos] != QLatin1Char('{'))
            return std::nullopt;
        const int end = source.indexOf(QLatin1Char('}'), pos + 1);
        if (end < 0)
            return std::nullopt;
        const QString name = source.mid(pos + 1, end - pos - 1);
        if (name != "matrix" && name != "pmatrix" && name != "bmatrix" && name != "vmatrix"
            && name != "Vmatrix")
            return std::nullopt;
        pos                          = end + 1;
        const QString        closing = "\\end{" + name + "}";
        QList<QList<Layout>> rows;
        QList<Layout>        row;
        inMatrix = true;
        skipSpace();
        if (starts(closing))
            return std::nullopt;
        while (pos < source.size()) {
            if (row.size() >= 8 || rows.size() >= 8 || cells >= 64)
                return std::nullopt;
            ++cells;
            auto cell = sequence(depth + 1, true);
            if (!cell)
                return std::nullopt;
            if (!cell->width)
                *cell = text(" ");
            row.append(*cell);
            if (pos < source.size() && source[pos] == QLatin1Char('&')) {
                ++pos;
                continue;
            }
            rows.append(row);
            row.clear();
            if (starts(closing)) {
                pos += closing.size();
                break;
            }
            if (!starts(QStringLiteral("\\\\")))
                return std::nullopt;
            pos += 2;
            skipSpace();
            if (starts(closing)) {
                pos += closing.size();
                break;
            }
            if (pos == source.size() || source[pos] == QLatin1Char('['))
                return std::nullopt;
        }
        inMatrix = false;
        if (rows.isEmpty() || !row.isEmpty()
            || source.mid(pos - closing.size(), closing.size()) != closing)
            return std::nullopt;
        const int  columns = rows.first().size();
        QList<int> widths(columns, 1);
        for (const auto &entry : rows) {
            if (entry.size() != columns)
                return std::nullopt;
            for (int col = 0; col < columns; ++col)
                widths[col] = std::max(widths[col], entry[col].width);
        }
        Layout grid{{}, 2 * (columns - 1), 0};
        for (int value : widths)
            grid.width += value;
        if (grid.width > maxWidth)
            return std::nullopt;
        for (const auto &entry : rows) {
            Layout line = text("");
            for (int col = 0; col < columns; ++col) {
                Layout    cell    = entry[col];
                const int leading = (widths[col] - cell.width) / 2;
                for (auto &value : cell.rows)
                    value = QString(leading, QLatin1Char(' '))
                            + padded(value, widths[col] - leading);
                cell.width = widths[col];
                if (col > 0) {
                    auto separated = join(line, text("  "));
                    if (!separated)
                        return std::nullopt;
                    line = *separated;
                }
                auto combined = join(line, cell);
                if (!combined)
                    return std::nullopt;
                line = *combined;
            }
            const int gap = grid.rows.isEmpty() ? 0 : 1;
            if (grid.rows.size() + gap + line.rows.size() > 16)
                return std::nullopt;
            if (gap)
                grid.rows.append(QString(grid.width, QLatin1Char(' ')));
            grid.rows.append(line.rows);
        }
        grid.baseline = (grid.rows.size() - 1) / 2;
        if (name == "matrix")
            return grid;
        const bool tall   = grid.rows.size() > 1;
        const int  border = name == "Vmatrix" ? 2 : 1;
        if (grid.width + 2 * border + (tall ? 2 : 0) > maxWidth)
            return std::nullopt;
        for (int index = 0; index < grid.rows.size(); ++index) {
            QString left  = name == "Vmatrix" ? "||" : "|";
            QString right = left;
            if (name == "pmatrix" || name == "bmatrix") {
                const bool round = name == "pmatrix";
                left             = round ? "(" : "[";
                right            = round ? ")" : "]";
                if (tall) {
                    const int piece = index == 0 ? 0 : (index + 1 == grid.rows.size() ? 2 : 1);
                    left            = QString(QChar((round ? 0x239b : 0x23a1) + piece));
                    right           = QString(QChar((round ? 0x239e : 0x23a4) + piece));
                }
            }
            const QString margin = tall ? " " : "";
            grid.rows[index]     = left + margin + grid.rows[index] + margin + right;
        }
        grid.width += 2 * border + (tall ? 2 : 0);
        return grid;
    }
    std::optional<Layout> atom(int depth, bool &wasMatrix)
    {
        if (source[pos] == QLatin1Char('{'))
            return argument(depth);
        const QChar ch = source[pos++];
        ++work;
        if (ch == QLatin1Char('\\')) {
            if (pos == source.size())
                return std::nullopt;
            if (QStringLiteral("&%$_{} ").contains(source[pos]))
                return text(QString(source[pos++]));
            const int begin = pos;
            while (pos < source.size() && source[pos].isLetter() && source[pos].unicode() < 128) {
                ++pos;
                ++work;
            }
            const QString command = source.mid(begin, pos - begin);
            if (command == "frac")
                return fraction(depth);
            if (command == "sqrt")
                return root(depth);
            if (command == "begin") {
                wasMatrix = true;
                return matrix(depth);
            }
            const auto found = symbols().constFind(command);
            return found == symbols().cend() ? std::nullopt : std::optional(text(*found));
        }
        if ((ch.unicode() < 128
             && (ch.isLetterOrNumber() || QStringLiteral("+-=<>.,:!()/[]|").contains(ch)))
            || std::any_of(symbols().cbegin(), symbols().cend(), [ch](const QString &value) {
                   return value == QString(ch);
               }))
            return text(QString(ch));
        return std::nullopt;
    }
    std::optional<Layout> script(bool superscript, int depth)
    {
        ++pos;
        skipSpace();
        if (pos == source.size())
            return std::nullopt;
        QString digits;
        if (source[pos] == QLatin1Char('{')) {
            if (depth >= 32)
                return std::nullopt;
            const int end = source.indexOf(QLatin1Char('}'), pos + 1);
            if (end < 0)
                return std::nullopt;
            digits = source.mid(pos + 1, end - pos - 1);
            pos    = end + 1;
        } else {
            digits = source.mid(pos++, 1);
        }
        if (digits.isEmpty())
            return std::nullopt;
        const QString from = QStringLiteral("0123456789+-=()");
        const QString to   = superscript ? QStringLiteral("⁰¹²³⁴⁵⁶⁷⁸⁹⁺⁻⁼⁽⁾")
                                         : QStringLiteral("₀₁₂₃₄₅₆₇₈₉₊₋₌₍₎");
        QString       mapped;
        for (QChar digit : digits) {
            const int index = from.indexOf(digit);
            if (index < 0)
                return std::nullopt;
            mapped.append(to[index]);
        }
        return text(mapped);
    }
    std::optional<Layout> sequence(int depth, bool cell)
    {
        if (depth > 32)
            return std::nullopt;
        Layout result = text("");
        while (pos < source.size()) {
            skipSpace();
            if (pos == source.size() || source[pos] == QLatin1Char('}'))
                break;
            if (cell && (source[pos] == QLatin1Char('&') || starts("\\\\") || starts("\\end{")))
                break;
            const int cellsBefore = cells;
            bool      wasMatrix   = false;
            auto      value       = atom(depth, wasMatrix);
            wasMatrix             = wasMatrix || cells != cellsBefore;
            if (!value)
                return std::nullopt;
            skipSpace();
            bool hadSuper = false;
            bool hadSub   = false;
            while (pos < source.size()
                   && (source[pos] == QLatin1Char('^') || source[pos] == QLatin1Char('_'))) {
                const bool superscript = source[pos] == QLatin1Char('^');
                if (wasMatrix || (superscript ? hadSuper : hadSub))
                    return std::nullopt;
                (superscript ? hadSuper : hadSub) = true;
                auto suffix                       = script(superscript, depth);
                if (!suffix)
                    return std::nullopt;
                value = join(*value, *suffix);
                if (!value)
                    return std::nullopt;
                skipSpace();
            }
            auto combined = join(result, *value);
            if (!combined)
                return std::nullopt;
            result = *combined;
        }
        return result;
    }
};

QList<Span> Scanner::feedLine(const QString &line, const QList<Range> &excluded)
{
    QList<Span> result;
    for (int at = 0; at < line.size(); ++at) {
        const QChar ch       = line[at];
        const int   absolute = offset + at;
        if (start < 0) {
            while (excludedIndex < excluded.size() && excluded[excludedIndex].end <= absolute)
                ++excludedIndex;
            if (excludedIndex < excluded.size() && excluded[excludedIndex].begin <= absolute) {
                at = std::min(int(line.size()), excluded[excludedIndex].end - offset) - 1;
                continue;
            }
        }
        if (ch == QLatin1Char('\n') && start >= 0 && !display) {
            result.append({start, absolute, false, false});
            start = -1;
        }
        if (ch != QLatin1Char('$') || escaped(line, at))
            continue;
        int dollars = 1;
        while (at + dollars < line.size() && line[at + dollars] == ch)
            ++dollars;
        if (dollars > 2) {
            at += dollars - 1;
            continue;
        }
        const bool pair = dollars == 2;
        if (start >= 0) {
            if (display && pair) {
                result.append({start, absolute + 2, true, true});
                start = -1;
                ++at;
            } else if (
                !display && !pair && at > 0 && !line[at - 1].isSpace()
                && (at + 1 == line.size() || boundary(line[at + 1]))) {
                result.append({start, absolute + 1, false, true});
                start = -1;
            } else if (pair) {
                ++at;
            }
            continue;
        }
        if (at > 0 && !boundary(line[at - 1]))
            continue;
        if (!pair
            && (at + 1 == line.size() || line[at + 1].isSpace()
                || QStringLiteral("{(?").contains(line[at + 1])))
            continue;
        if (!pair && line[at + 1].isLetterOrNumber()) {
            int close = line.indexOf(QLatin1Char('$'), at + 1);
            while (close >= 0 && escaped(line, close))
                close = line.indexOf(QLatin1Char('$'), close + 1);
            const bool closes = close > at + 1 && !line[close - 1].isSpace()
                                && (close + 1 == line.size() || boundary(line[close + 1]));
            if (!closes) {
                if (line[at + 1].isDigit())
                    continue;
                int end = at + 1;
                while (end < line.size()
                       && (line[end].isLetterOrNumber() || line[end] == QLatin1Char('_')))
                    ++end;
                if (end == line.size() || line[end].isSpace()
                    || QStringLiteral(".,:;!").contains(line[end])) {
                    result.append({absolute, offset + end, false, false});
                    at = end - 1;
                    continue;
                }
            }
        }
        start   = absolute;
        display = pair;
        if (pair)
            ++at;
    }
    offset += line.size();
    return result;
}

std::optional<Span> Scanner::unfinished() const
{
    return start >= 0 ? std::optional(Span{start, offset, display, false}) : std::nullopt;
}

} // namespace

QList<Span> spans(const QString &source, const QList<Range> &excluded)
{
    Scanner     scanner;
    QList<Span> result;
    int         start = 0;
    while (start < source.size()) {
        int end = source.indexOf(QLatin1Char('\n'), start);
        end     = end < 0 ? source.size() : end + 1;
        result.append(scanner.feedLine(source.mid(start, end - start), excluded));
        start = end;
    }
    if (auto tail = scanner.unfinished())
        result.append(*tail);
    return result;
}

QString body(const QString &source, const Span &span)
{
    const int delimiter = span.display ? 2 : 1;
    QString   content   = source.mid(span.begin + delimiter, span.end - span.begin - 2 * delimiter);
    if (!span.display || !content.contains(QLatin1Char('\n')))
        return content;
    const int     lineStart  = source.lastIndexOf(QLatin1Char('\n'), span.begin) + 1;
    const QString prefix     = source.mid(lineStart, span.begin - lineStart);
    int           quoteCount = 0;
    int           prefixAt   = 0;
    while (prefixAt < prefix.size()) {
        if (prefix[prefixAt].isSpace()) {
            ++prefixAt;
            continue;
        }
        if (prefix[prefixAt] == QLatin1Char('>')) {
            ++quoteCount;
            ++prefixAt;
            continue;
        }
        int markerEnd = prefixAt;
        if (QStringLiteral("-*+").contains(prefix[markerEnd])) {
            ++markerEnd;
        } else {
            while (markerEnd < prefix.size() && prefix[markerEnd].isDigit())
                ++markerEnd;
            if (markerEnd == prefixAt || markerEnd == prefix.size()
                || !QStringLiteral(".)").contains(prefix[markerEnd]))
                break;
            ++markerEnd;
        }
        if (markerEnd == prefix.size() || !prefix[markerEnd].isSpace())
            break;
        prefixAt = markerEnd + 1;
    }
    QStringList lines = content.split(QLatin1Char('\n'));
    for (int idx = 1; idx < lines.size(); ++idx) {
        QString &line = lines[idx];
        int      at   = 0;
        for (int quote = 0; quote < quoteCount; ++quote) {
            while (at < line.size() && line[at] == QLatin1Char(' '))
                ++at;
            if (at < line.size() && line[at] == QLatin1Char('>'))
                ++at;
        }
        line = line.mid(at);
    }
    return lines.join(QLatin1Char('\n'));
}

std::optional<Layout> render(const QString &source, bool display, int width, int *steps)
{
    if (steps)
        *steps = 0;
    if (source.size() > 4096 || source.toUtf8().size() > 4096 || width < 1)
        return std::nullopt;
    Parser parser(source, display, width);
    auto   result = parser.parse();
    if (steps)
        *steps = parser.work;
    return result;
}
} // namespace QSocMath
