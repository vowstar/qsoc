// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qsocmath.h"
#include "tui/qtuiwidget.h"

#include <algorithm>
#include <QHash>
#include <QScopedValueRollback>

namespace QSocMath {
namespace {
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

QString scriptText(const QString &value, bool superscript)
{
    const QString from = superscript
                             ? QStringLiteral(
                                   "0123456789+-=()abcdefghijklmnoprstuvwxyzABDEGHIJKLMNOPRTUVW")
                             : QStringLiteral("0123456789+-=()aehijklmnoprstuvx");
    const QString to   = superscript
                             ? QStringLiteral(
                                   "⁰¹²³⁴⁵⁶⁷⁸⁹⁺⁻⁼⁽⁾ᵃᵇᶜᵈᵉᶠᵍʰⁱʲᵏˡᵐⁿᵒᵖʳˢᵗᵘᵛʷˣʸᶻᴬᴮᴰᴱᴳᴴᴵᴶᴷᴸᴹᴺᴼᴾᴿᵀᵁⱽᵂ")
                             : QStringLiteral("₀₁₂₃₄₅₆₇₈₉₊₋₌₍₎ₐₑₕᵢⱼₖₗₘₙₒₚᵣₛₜᵤᵥₓ");
    QString       result;
    for (QChar ch : value) {
        const int index = from.indexOf(ch);
        if (index < 0)
            return (superscript ? "^{" : "_{") + value + "}";
        result += to[index];
    }
    return result;
}

QString borderPiece(const QString &border, int row, int height)
{
    if (height == 1)
        return border;
    const int piece = row == 0 ? 0 : (row + 1 == height ? 2 : 1);
    if (border == "(" || border == ")")
        return QString(QChar((border == "(" ? 0x239b : 0x239e) + piece));
    if (border == "[" || border == "]")
        return QString(QChar((border == "[" ? 0x23a1 : 0x23a4) + piece));
    if (border == "{" || border == "}") {
        const QString pieces = border == "{" ? QStringLiteral("⎧⎪⎨⎩") : QStringLiteral("⎫⎪⎬⎭");
        const int     index  = row == 0 ? 0 : (row + 1 == height ? 3 : (row == height / 2 ? 2 : 1));
        return QString(pieces[index]);
    }
    return border;
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
        auto result = sequence(0);
        if (!result || pos != source.size() || !result->width || !fits(*result))
            return std::nullopt;
        return result;
    }
    int work = 0;

private:
    enum class AtomKind { Plain, Group, Limits };
    struct GridFormat
    {
        QString    alignment;
        QList<int> bars    = QList<int>(9, 0);
        bool       aligned = false;
    };

    const QString &source;
    bool           display;
    int            maxWidth;
    int            pos    = 0;
    int            cells  = 0;
    bool           inGrid = false;

    bool starts(const QString &value) const
    {
        return QStringView(source).mid(pos).startsWith(value);
    }
    bool commandAt(const QString &name) const
    {
        const int end = pos + name.size() + 1;
        return starts("\\" + name) && (end == source.size() || !source[end].isLetter());
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
    std::optional<Layout> surround(Layout value, const QString &left, const QString &right) const
    {
        const QString lpad = value.rows.size() > 1 && !left.isEmpty() ? " " : "";
        const QString rpad = value.rows.size() > 1 && !right.isEmpty() ? " " : "";
        value.width += QTuiText::visualWidth(left + lpad + rpad + right);
        if (!fits(value))
            return std::nullopt;
        for (int row = 0; row < value.rows.size(); ++row)
            value.rows[row] = borderPiece(left, row, value.rows.size()) + lpad + value.rows[row]
                              + rpad + borderPiece(right, row, value.rows.size());
        return value;
    }
    std::optional<Layout> annotate(Layout base, const Layout &label, bool above) const
    {
        const int width = std::max(base.width, label.width);
        if (base.rows.size() + label.rows.size() > 16 || width > maxWidth)
            return std::nullopt;
        QStringList labels;
        for (const auto &row : label.rows)
            labels.append(padded(row, width, true));
        for (auto &row : base.rows)
            row = padded(row, width, true);
        if (above) {
            base.rows = labels + base.rows;
            base.baseline += labels.size();
        } else {
            base.rows.append(labels);
        }
        base.width = width;
        return base;
    }
    std::optional<Layout> argument(int depth, bool braces = true)
    {
        skipSpace();
        if (pos == source.size() || depth >= 32)
            return std::nullopt;
        if (source[pos] != QLatin1Char('{')) {
            AtomKind kind = AtomKind::Plain;
            return braces ? std::nullopt : atom(depth + 1, kind);
        }
        ++pos;
        auto value = sequence(depth + 1);
        if (!value || pos == source.size() || source[pos++] != QLatin1Char('}'))
            return std::nullopt;
        return value;
    }
    std::optional<Layout> compactArgument(int depth)
    {
        QScopedValueRollback<bool> mode(display, false);
        auto                       value = argument(depth, false);
        return value && value->width && value->rows.size() == 1 ? value : std::nullopt;
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
        skipSpace();
        QString index;
        if (pos < source.size() && source[pos] == QLatin1Char('[')) {
            ++pos;
            QScopedValueRollback<bool> mode(display, false);
            auto                       degree = sequence(depth + 1, false, QLatin1Char(']'));
            if (!degree || !degree->width || pos == source.size()
                || source[pos++] != QLatin1Char(']'))
                return std::nullopt;
            index = scriptText(degree->rows.first(), true);
        }
        auto inner = argument(depth);
        if (!inner || !inner->width)
            return std::nullopt;
        if (!display) {
            auto value = text(index + QStringLiteral("√(") + inner->rows.first() + ")");
            return fits(value) ? std::optional(value) : std::nullopt;
        }
        const int indent = QTuiText::visualWidth(index);
        Layout    value{{}, indent + inner->width + 1, inner->baseline + 1};
        if (!fits(value) || inner->rows.size() + 1 > 16)
            return std::nullopt;
        value.rows.append(
            QString(indent + 1, QLatin1Char(' ')) + QString(inner->width, QChar(0x2500)));
        for (int row = 0; row < inner->rows.size(); ++row)
            value.rows.append(
                (row == inner->baseline ? index + QStringLiteral("√")
                                        : QString(indent + 1, QLatin1Char(' ')))
                + inner->rows[row]);
        return value;
    }
    std::optional<QString> literal()
    {
        skipSpace();
        if (pos == source.size() || source[pos++] != QLatin1Char('{'))
            return std::nullopt;
        QString value;
        while (pos < source.size()) {
            QChar ch = source[pos++];
            ++work;
            if (ch == QLatin1Char('}'))
                return value;
            if (ch == QLatin1Char('\\')) {
                if (pos == source.size() || !QStringLiteral("{}%$_&# ").contains(source[pos]))
                    return std::nullopt;
                value += source[pos++];
            } else if (ch == QLatin1Char('{') || ch.category() == QChar::Other_Control) {
                return std::nullopt;
            } else {
                value += ch;
            }
        }
        return std::nullopt;
    }
    QString readCommand()
    {
        const int begin = pos;
        while (pos < source.size() && source[pos].unicode() < 128 && source[pos].isLetter()) {
            ++pos;
            ++work;
        }
        return source.mid(begin, pos - begin);
    }
    std::optional<QString> delimiter()
    {
        skipSpace();
        if (pos == source.size())
            return std::nullopt;
        const QChar ch = source[pos++];
        if (ch == QLatin1Char('.'))
            return QString();
        if (QStringLiteral("()[]|").contains(ch))
            return QString(ch);
        if (ch != QLatin1Char('\\') || pos == source.size())
            return std::nullopt;
        if (QStringLiteral("{}|").contains(source[pos])) {
            const QChar escaped = source[pos++];
            return escaped == QLatin1Char('|') ? QStringLiteral("||") : QString(escaped);
        }
        static const QHash<QString, QString> borders{
            {"lbrace", "{"},
            {"rbrace", "}"},
            {"langle", "⟨"},
            {"rangle", "⟩"},
            {"lvert", "|"},
            {"rvert", "|"},
            {"vert", "|"},
            {"lVert", "||"},
            {"rVert", "||"},
            {"Vert", "||"},
            {"lfloor", "⌊"},
            {"rfloor", "⌋"},
            {"lceil", "⌈"},
            {"rceil", "⌉"}};
        const auto found = borders.constFind(readCommand());
        return found == borders.cend() ? std::nullopt : std::optional(*found);
    }
    std::optional<Layout> delimited(int depth)
    {
        auto left  = delimiter();
        auto inner = left ? sequence(depth + 1, false, {}, true) : std::nullopt;
        if (!inner || !commandAt("right"))
            return std::nullopt;
        pos += 6;
        auto right = delimiter();
        return right ? surround(*inner, *left, *right) : std::nullopt;
    }
    std::optional<GridFormat> gridFormat(const QString &name)
    {
        GridFormat format;
        format.aligned = name == "aligned";
        if (name != "array")
            return format;
        auto specification = literal();
        if (!specification)
            return std::nullopt;
        for (QChar ch : *specification) {
            if (ch.isSpace())
                continue;
            const int column = format.alignment.size();
            if (ch == QLatin1Char('|')) {
                if (++format.bars[column] > 2)
                    return std::nullopt;
            } else if (QStringLiteral("lcr").contains(ch) && column < 8) {
                format.alignment += ch;
            } else {
                return std::nullopt;
            }
        }
        return format.alignment.isEmpty() ? std::nullopt : std::optional(format);
    }
    std::optional<QList<QList<Layout>>> gridCells(int depth, const QString &closing)
    {
        QList<QList<Layout>> rows;
        QList<Layout>        row;
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
            row.append(cell->width ? *cell : text(" "));
            if (pos < source.size() && source[pos] == QLatin1Char('&')) {
                ++pos;
                continue;
            }
            rows.append(row);
            row.clear();
            if (starts(closing)) {
                pos += closing.size();
                return rows;
            }
            if (!starts("\\\\"))
                return std::nullopt;
            pos += 2;
            skipSpace();
            if (starts(closing)) {
                pos += closing.size();
                return rows;
            }
            if (pos == source.size() || source[pos] == QLatin1Char('['))
                return std::nullopt;
        }
        return std::nullopt;
    }
    std::optional<Layout> gridLine(
        const QList<Layout> &entry, const QList<int> &widths, const GridFormat &format) const
    {
        Layout line = text(QString(format.bars[0], QChar(0x2502)));
        for (int col = 0; col < entry.size(); ++col) {
            const QChar alignment = format.alignment.isEmpty()
                                        ? (format.aligned ? QChar(col % 2 ? 'l' : 'r')
                                                          : QLatin1Char('c'))
                                        : format.alignment[col];
            Layout      cell      = entry[col];
            const int   extra     = widths[col] - cell.width;
            const int   leading   = alignment == QLatin1Char('r')
                                        ? extra
                                        : (alignment == QLatin1Char('c') ? extra / 2 : 0);
            for (auto &value : cell.rows)
                value = QString(leading, QLatin1Char(' ')) + padded(value, widths[col] - leading);
            cell.width    = widths[col];
            auto combined = join(line, cell);
            if (!combined)
                return std::nullopt;
            line = *combined;
            QString separator(format.bars[col + 1], QChar(0x2502));
            if (col + 1 < entry.size())
                separator = separator.isEmpty() ? QString(format.aligned ? 1 : 2, QLatin1Char(' '))
                                                : " " + separator + " ";
            combined = join(line, text(separator));
            if (!combined)
                return std::nullopt;
            line = *combined;
        }
        return line;
    }
    std::optional<Layout> layoutGrid(const QList<QList<Layout>> &rows, const GridFormat &format) const
    {
        const int columns = rows.first().size();
        if (!format.alignment.isEmpty() && format.alignment.size() != columns)
            return std::nullopt;
        QList<int> widths(columns, 1);
        for (const auto &entry : rows) {
            if (entry.size() != columns)
                return std::nullopt;
            for (int col = 0; col < columns; ++col)
                widths[col] = std::max(widths[col], entry[col].width);
        }
        Layout grid{{}, 0, 0};
        for (const auto &entry : rows) {
            auto line = gridLine(entry, widths, format);
            if (!line)
                return std::nullopt;
            const int gap = grid.rows.isEmpty() || format.aligned ? 0 : 1;
            if (grid.rows.size() + gap + line->rows.size() > 16)
                return std::nullopt;
            if (grid.rows.isEmpty())
                grid.baseline = line->baseline;
            if (gap) {
                QList<Layout> empty;
                for (int col = 0; col < columns; ++col)
                    empty.append(text(" "));
                auto blank = gridLine(empty, widths, format);
                if (!blank)
                    return std::nullopt;
                grid.rows.append(blank->rows.first());
            }
            grid.width = line->width;
            grid.rows.append(line->rows);
        }
        if (!format.aligned)
            grid.baseline = (grid.rows.size() - 1) / 2;
        return grid;
    }
    std::optional<Layout> grid(int depth)
    {
        if (!display || inGrid || depth >= 32)
            return std::nullopt;
        auto name = literal();
        if (!name
            || !QStringList{"matrix", "pmatrix", "bmatrix", "vmatrix", "Vmatrix", "array", "aligned"}
                    .contains(*name))
            return std::nullopt;
        auto format = gridFormat(*name);
        if (!format)
            return std::nullopt;
        QScopedValueRollback<bool> active(inGrid, true);
        auto                       rows  = gridCells(depth, "\\end{" + *name + "}");
        auto                       value = rows ? layoutGrid(*rows, *format) : std::nullopt;
        if (!value)
            return std::nullopt;
        if (*name == "pmatrix")
            return surround(*value, "(", ")");
        if (*name == "bmatrix")
            return surround(*value, "[", "]");
        if (*name == "vmatrix" || *name == "Vmatrix") {
            const QString bar = *name == "vmatrix" ? "|" : "||";
            return surround(*value, bar, bar);
        }
        return value;
    }
    std::optional<Layout> box(int depth)
    {
        if (!display)
            return std::nullopt;
        auto value = argument(depth);
        if (!value || !value->width || value->width + 4 > maxWidth || value->rows.size() + 2 > 16)
            return std::nullopt;
        for (auto &row : value->rows)
            row = QStringLiteral("│ ") + row + QStringLiteral(" │");
        const QString rule(value->width + 2, QChar(0x2500));
        value->rows.prepend(QStringLiteral("┌") + rule + QStringLiteral("┐"));
        value->rows.append(QStringLiteral("└") + rule + QStringLiteral("┘"));
        value->width += 4;
        ++value->baseline;
        return value;
    }
    std::optional<Layout> annotation(int depth, bool above)
    {
        auto label = argument(depth);
        auto base  = argument(depth);
        if (!label || !base || !label->width || !base->width)
            return std::nullopt;
        if (display)
            return annotate(*base, *label, above);
        return text(
            "(" + base->rows.first() + ")" + (above ? "^{" : "_{") + label->rows.first() + "}");
    }
    std::optional<Layout> substack(int depth)
    {
        skipSpace();
        if (pos == source.size() || source[pos++] != QLatin1Char('{'))
            return std::nullopt;
        QStringList rows;
        while (pos < source.size() && rows.size() < 8) {
            QScopedValueRollback<bool> mode(display, false);
            auto                       row = sequence(depth + 1, true);
            if (!row || !row->width)
                return std::nullopt;
            rows.append(row->rows.first());
            if (pos < source.size() && source[pos] == QLatin1Char('}')) {
                ++pos;
                return text("(" + rows.join("; ") + ")");
            }
            if (!starts("\\\\"))
                return std::nullopt;
            pos += 2;
        }
        return std::nullopt;
    }
    std::optional<Layout> alphabet(int depth, bool calligraphic)
    {
        auto value = compactArgument(depth);
        if (!value)
            return std::nullopt;
        const QString letters = calligraphic ? "ABCDEFGHIJKLMNOPQRSTUVWXYZ" : "RCNZQP";
        const auto    glyphs  = (calligraphic ? QString::fromUcs4(U"𝒜ℬ𝒞𝒟ℰℱ𝒢ℋℐ𝒥𝒦ℒℳ𝒩𝒪𝒫𝒬ℛ𝒮𝒯𝒰𝒱𝒲𝒳𝒴𝒵")
                                              : QStringLiteral("ℝℂℕℤℚℙ"))
                                    .toUcs4();
        QString       output;
        for (QChar ch : value->rows.first()) {
            const int index = letters.indexOf(ch);
            if (index < 0)
                return std::nullopt;
            const char32_t glyph = glyphs[index];
            output += QString::fromUcs4(&glyph, 1);
        }
        return text(output);
    }
    std::optional<Layout> command(int depth, AtomKind &kind)
    {
        if (pos == source.size())
            return std::nullopt;
        if (QStringLiteral("&%$_{} ").contains(source[pos]))
            return text(QString(source[pos++]));
        if (QStringLiteral(",;:!").contains(source[pos]))
            return text(source[pos++] == QLatin1Char('!') ? "" : " ");
        const QString name = readCommand();
        if (name == "frac" || name == "dfrac" || name == "tfrac" || name == "cfrac") {
            kind = AtomKind::Group;
            return fraction(depth);
        }
        if (name == "sqrt") {
            kind = AtomKind::Group;
            return root(depth);
        }
        if (name == "begin")
            return grid(depth);
        if (name == "left")
            return delimited(depth);
        if (name == "quad" || name == "qquad")
            return text(QString(name == "quad" ? 2 : 4, QLatin1Char(' ')));
        if (name == "text" || name == "operatorname") {
            kind       = AtomKind::Group;
            auto value = literal();
            return value ? std::optional(text(*value)) : std::nullopt;
        }
        if (QStringList{"mathrm", "mathbf", "mathit", "mathsf", "mathtt"}.contains(name)) {
            kind = AtomKind::Group;
            return argument(depth);
        }
        if (name == "mathbb" || name == "mathcal") {
            kind = AtomKind::Group;
            return alphabet(depth, name == "mathcal");
        }
        if (name == "boxed")
            return box(depth);
        if (name == "overset" || name == "underset") {
            kind = AtomKind::Group;
            return annotation(depth, name == "overset");
        }
        if (name == "substack")
            return substack(depth);
        if (QStringList{"sin", "cos", "tan", "log", "ln", "exp", "lim", "max", "min", "det", "dim"}
                .contains(name))
            return text(name);
        if (name == "sum" || name == "prod" || name == "int")
            kind = AtomKind::Limits;
        const auto found = symbols().constFind(name);
        return found == symbols().cend() ? std::nullopt : std::optional(text(*found));
    }
    std::optional<Layout> atom(int depth, AtomKind &kind)
    {
        if (depth > 32 || pos == source.size())
            return std::nullopt;
        if (source[pos] == QLatin1Char('{')) {
            kind = AtomKind::Group;
            return argument(depth);
        }
        const QChar ch = source[pos++];
        ++work;
        if (ch == QLatin1Char('\\'))
            return command(depth, kind);
        if ((ch.unicode() < 128
             && (ch.isLetterOrNumber() || QStringLiteral("+-=<>.,:!()/[]|").contains(ch)))
            || std::any_of(symbols().cbegin(), symbols().cend(), [ch](const QString &value) {
                   return value == QString(ch);
               }))
            return text(QString(ch));
        return std::nullopt;
    }
    std::optional<Layout> scripts(Layout base, AtomKind kind, int depth, bool containsGrid)
    {
        std::optional<Layout> upper;
        std::optional<Layout> lower;
        QString               suffix;
        while (pos < source.size()
               && (source[pos] == QLatin1Char('^') || source[pos] == QLatin1Char('_'))) {
            const bool above = source[pos++] == QLatin1Char('^');
            auto      &label = above ? upper : lower;
            if (containsGrid || label || !base.width)
                return std::nullopt;
            label = compactArgument(depth);
            if (!label)
                return std::nullopt;
            suffix += scriptText(label->rows.first(), above);
            skipSpace();
        }
        if (suffix.isEmpty())
            return base;
        if (kind == AtomKind::Limits && display) {
            const int width = std::max(
                {base.width, upper ? upper->width : 0, lower ? lower->width : 0});
            for (auto &row : base.rows)
                row = padded(row, width, true);
            base.width = width;
            if (upper) {
                auto value = annotate(base, *upper, true);
                if (!value)
                    return std::nullopt;
                base = *value;
            }
            return lower ? annotate(base, *lower, false) : std::optional(base);
        }
        if ((kind == AtomKind::Group && base.width > 1) || base.rows.size() > 1) {
            auto grouped = surround(base, "(", ")");
            if (!grouped)
                return std::nullopt;
            base = *grouped;
        }
        return join(base, text(suffix));
    }
    std::optional<Layout> sequence(int depth, bool cell = false, QChar stop = {}, bool right = false)
    {
        if (depth > 32)
            return std::nullopt;
        Layout result = text("");
        while (pos < source.size()) {
            skipSpace();
            if (pos == source.size() || source[pos] == QLatin1Char('}') || source[pos] == stop)
                break;
            if (right && commandAt("right"))
                break;
            if (cell && (source[pos] == QLatin1Char('&') || starts("\\\\") || starts("\\end{")))
                break;
            const int cellsBefore = cells;
            AtomKind  kind        = AtomKind::Plain;
            auto      value       = atom(depth, kind);
            if (!value)
                return std::nullopt;
            skipSpace();
            value = scripts(*value, kind, depth, cells != cellsBefore);
            if (!value)
                return std::nullopt;
            auto combined = join(result, *value);
            if (!combined)
                return std::nullopt;
            result = *combined;
        }
        return result;
    }
};

} // namespace

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
