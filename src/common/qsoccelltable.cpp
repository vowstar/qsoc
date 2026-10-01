// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoccelltable.h"

#include <algorithm>
#include <functional>
#include <numeric>
#include <string>
#include <QRegularExpression>
#include <QSet>

#include <slang/diagnostics/Diagnostics.h>
#include <slang/parsing/Parser.h>
#include <slang/parsing/Preprocessor.h>
#include <slang/syntax/AllSyntax.h>
#include <slang/text/SourceManager.h>
#include <slang/util/BumpAllocator.h>

namespace {

using Truth = QSocCellTable::Truth;

bool isLevel(const QString &value)
{
    return value == "0" || value == "1" || value == "x" || value == "X";
}

bool isIdentifier(const QString &name)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z_][A-Za-z0-9_]*$"));
    return pattern.match(name).hasMatch();
}

bool isLabel(const QString &value)
{
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z0-9_][A-Za-z0-9_.]*$"));
    return pattern.match(value).hasMatch() && !isLevel(value);
}

QString direction(const QSocCellPort &port)
{
    if (port.direction == "input") {
        return QStringLiteral("in");
    }
    if (port.direction == "output") {
        return QStringLiteral("out");
    }
    return port.direction;
}

QString raw(const slang::parsing::Token &token)
{
    return QString::fromStdString(std::string(token.rawText()));
}

/* Truth table of a parsed expression; pin() yields an input's table. */
struct Walker
{
    std::function<bool(const QString &, Truth *)> pin;
    Truth                                         ones;
    QString                                       error;

    bool fail(const QString &message)
    {
        error = message;
        return false;
    }

    bool literal(const slang::syntax::ExpressionSyntax &node, Truth *out)
    {
        using namespace slang::syntax;
        QString text;
        if (node.kind == SyntaxKind::IntegerLiteralExpression) {
            text = raw(node.as<LiteralExpressionSyntax>().literal);
        } else {
            const auto &vector = node.as<IntegerVectorExpressionSyntax>();
            const bool  sized  = raw(vector.size) == "1"
                                 && raw(vector.base).compare("'b", Qt::CaseInsensitive) == 0;
            text               = sized ? raw(vector.value) : QString();
        }
        if (text != "0" && text != "1") {
            return fail(QStringLiteral("literal '%1' is not 0, 1, 1'b0 or 1'b1")
                            .arg(QString::fromStdString(node.toString()).trimmed()));
        }
        *out = text == "1" ? ones : Truth();
        return true;
    }

    bool walk(const slang::syntax::ExpressionSyntax &node, Truth *out)
    {
        using namespace slang::syntax;
        Truth a;
        Truth b;
        Truth c;
        switch (node.kind) {
        case SyntaxKind::IdentifierName: {
            const QString name = raw(node.as<IdentifierNameSyntax>().identifier);
            return pin(name, out) || fail(QStringLiteral("'%1' is not an input pin").arg(name));
        }
        case SyntaxKind::IntegerLiteralExpression:
        case SyntaxKind::IntegerVectorExpression:
            return literal(node, out);
        case SyntaxKind::ParenthesizedExpression:
            return walk(*node.as<ParenthesizedExpressionSyntax>().expression, out);
        case SyntaxKind::UnaryLogicalNotExpression:
        case SyntaxKind::UnaryBitwiseNotExpression:
            if (!walk(*node.as<PrefixUnaryExpressionSyntax>().operand, &a)) {
                return false;
            }
            *out = ~a & ones;
            return true;
        case SyntaxKind::BinaryAndExpression:
        case SyntaxKind::LogicalAndExpression:
        case SyntaxKind::BinaryOrExpression:
        case SyntaxKind::LogicalOrExpression:
        case SyntaxKind::BinaryXorExpression: {
            const auto &binary = node.as<BinaryExpressionSyntax>();
            if (!walk(*binary.left, &a) || !walk(*binary.right, &b)) {
                return false;
            }
            const bool isAnd = node.kind == SyntaxKind::BinaryAndExpression
                               || node.kind == SyntaxKind::LogicalAndExpression;
            const bool isOr  = node.kind == SyntaxKind::BinaryOrExpression
                               || node.kind == SyntaxKind::LogicalOrExpression;
            *out             = isAnd ? (a & b) : isOr ? (a | b) : (a ^ b);
            return true;
        }
        case SyntaxKind::ConditionalExpression: {
            const auto &conditional = node.as<ConditionalExpressionSyntax>();
            const auto &conditions  = conditional.predicate->conditions;
            if (conditions.size() != 1 || conditions[0]->matchesClause) {
                return fail(QStringLiteral("a condition takes one plain expression"));
            }
            if (!walk(*conditions[0]->expr, &c) || !walk(*conditional.left, &a)
                || !walk(*conditional.right, &b)) {
                return false;
            }
            *out = (c & a) | (~c & b & ones);
            return true;
        }
        default:
            return fail(QStringLiteral("'%1' is not one of ! ~ & | ^ && || ?:")
                            .arg(QString::fromStdString(node.toString()).trimmed()));
        }
    }
};

} // namespace

quint32 QSocCellTable::portWidth(const QString &type)
{
    static const QRegularExpression range(R"(\[\s*(\d+)\s*:\s*(\d+)\s*\])");
    quint32                         width = 0;
    QRegularExpressionMatchIterator it    = range.globalMatch(type);
    while (it.hasNext()) {
        const QRegularExpressionMatch match     = it.next();
        const int                     msb       = match.captured(1).toInt();
        const int                     lsb       = match.captured(2).toInt();
        const quint32                 dimension = quint32(qAbs(msb - lsb)) + 1;
        width                                   = width == 0 ? dimension : width * dimension;
    }
    return width == 0 ? 1 : width;
}

QSocCellPorts QSocCellTable::portsOf(const YAML::Node &portNode)
{
    QSocCellPorts result;
    if (!portNode || !portNode.IsMap()) {
        return result;
    }
    for (const auto &entry : portNode) {
        QString portDirection;
        QString type;
        if (entry.second["direction"]) {
            portDirection = QString::fromStdString(entry.second["direction"].Scalar());
        }
        if (entry.second["type"]) {
            type = QString::fromStdString(entry.second["type"].Scalar());
        }
        const QSocCellPort port(portDirection, portWidth(type));
        result.insert(
            QString::fromStdString(entry.first.Scalar()), QSocCellPort(direction(port), port.width));
    }
    return result;
}

QSocCellTable QSocCellTable::parse(
    const YAML::Node &node, const QSocCellPorts &ports, const QString &path, Check check)
{
    QSocCellTable cell;
    cell.ports = ports;
    for (auto it = ports.constBegin(); it != ports.constEnd(); ++it) {
        if (direction(it.value()) == "in" && it.value().width == 1) {
            cell.inputPins.append(it.key());
        }
    }
    if (!node || !(node.IsMap() || node.IsSequence()) || node.size() == 0) {
        cell.diagnostics.append(QStringLiteral("%1: expected a row or a list of rows").arg(path));
        return cell;
    }
    if (node.IsMap()) {
        cell.readRow(node, path);
    } else {
        for (std::size_t i = 0; i < node.size(); ++i) {
            cell.readRow(node[i], QStringLiteral("%1[%2]").arg(path).arg(i));
        }
    }
    if (cell.diagnostics.isEmpty() && check == Check::Function) {
        cell.check(path);
    }
    return cell;
}

void QSocCellTable::readRow(const YAML::Node &node, const QString &path)
{
    if (!node.IsMap() || node.size() == 0) {
        diagnostics.append(QStringLiteral("%1: a row is a map of columns to values").arg(path));
        return;
    }
    Row row;
    for (const auto &entry : node) {
        const QString column = QString::fromStdString(entry.first.Scalar());
        const QString where  = QStringLiteral("%1.%2").arg(path, column);
        if (!entry.second.IsScalar()) {
            diagnostics.append(QStringLiteral("%1: value must be a scalar").arg(where));
            continue;
        }
        const QString value = QString::fromStdString(entry.second.Scalar()).trimmed();
        const auto    port  = ports.constFind(column);
        QString       error;
        if (port == ports.constEnd()) {
            if (isLevel(value)) {
                error = QStringLiteral("'%1' is not a port of the cell").arg(column);
            } else if (!isIdentifier(column)) {
                error = QStringLiteral("attribute name '%1' is not an identifier").arg(column);
            } else if (!isLabel(value)) {
                error = QStringLiteral("label '%1' is not a single word").arg(value);
            } else {
                row.sets.insert(column, value);
                if (!attributeNames.contains(column)) {
                    attributeNames.append(column);
                }
            }
        } else if (port->width != 1) {
            error = QStringLiteral("pin %1 is %2 bits wide, a table pin is 1 bit")
                        .arg(column)
                        .arg(port->width);
        } else if (direction(*port) == "in") {
            if (!isLevel(value)) {
                error = QStringLiteral("input pin takes 0, 1 or x, not '%1'").arg(value);
            } else if (value == "0" || value == "1") {
                row.when.insert(column, value == "1");
            }
        } else if (direction(*port) == "out") {
            if (value == "x" || value == "X") {
                error = QStringLiteral("an output pin cannot be x");
            } else if (compile(value, &error)) {
                row.sets.insert(column, value);
                if (!outputPins.contains(column)) {
                    outputPins.append(column);
                }
            }
        } else {
            error
                = QStringLiteral("%1 pin %2 cannot appear in a table").arg(direction(*port), column);
        }
        if (!error.isEmpty()) {
            diagnostics.append(QStringLiteral("%1: %2").arg(where, error));
        }
    }
    if (row.sets.isEmpty()) {
        diagnostics.append(QStringLiteral("%1: row sets no output pin or attribute").arg(path));
    }
    table.append(row);
}

bool QSocCellTable::compile(const QString &text, QString *error)
{
    if (functions.contains(text)) {
        return true;
    }
    if (inputPins.size() > maxInputs) {
        *error = QStringLiteral("%1 input pins exceed the limit of %2 for a Boolean output")
                     .arg(inputPins.size())
                     .arg(maxInputs);
        return false;
    }
    Walker walker;
    walker.ones = all();
    walker.pin  = [this](const QString &name, Truth *out) {
        const qsizetype index = inputPins.indexOf(name);
        if (index < 0) {
            return false;
        }
        out->reset();
        for (int m = 0; m < span(); ++m) {
            out->set(m, (m >> index) & 1);
        }
        return true;
    };
    if (text.contains('`') || text.contains('\\')) {
        *error = QStringLiteral("'%1' is not a plain expression").arg(text);
        return false;
    }
    const std::string            source = text.toStdString();
    slang::SourceManager         sourceManager;
    slang::BumpAllocator         allocator;
    slang::Diagnostics           parseDiagnostics;
    slang::parsing::Preprocessor preprocessor(sourceManager, allocator, parseDiagnostics);
    preprocessor.pushSource(source);
    slang::parsing::Parser parser(preprocessor);
    const auto            &expression = parser.parseExpression();
    const bool             broken     = std::any_of(
        parseDiagnostics.begin(), parseDiagnostics.end(), [](const slang::Diagnostic &diagnostic) {
            return diagnostic.isError();
        });
    if (!parser.isDone() || broken) {
        *error = QStringLiteral("'%1' is not a valid expression").arg(text);
        return false;
    }
    Truth result;
    if (!walker.walk(expression, &result)) {
        *error = walker.error;
        return false;
    }
    functions.insert(text, result);
    return true;
}

QSocCellTable::Truth QSocCellTable::all() const
{
    Truth ones;
    for (int m = 0; m < span(); ++m) {
        ones.set(m);
    }
    return ones;
}

QSocCellTable::Truth QSocCellTable::cube(const QMap<QString, bool> &when) const
{
    Truth result = all();
    for (auto it = when.constBegin(); it != when.constEnd(); ++it) {
        const qsizetype index = inputPins.indexOf(it.key());
        for (int m = 0; m < span(); ++m) {
            if (((m >> index) & 1) != int(it.value())) {
                result.reset(m);
            }
        }
    }
    return result;
}

QString QSocCellTable::describe(int minterm) const
{
    QStringList levels;
    for (qsizetype i = 0; i < inputPins.size(); ++i) {
        levels.append(QStringLiteral("%1=%2").arg(inputPins[i]).arg((minterm >> i) & 1));
    }
    return levels.join(' ');
}

void QSocCellTable::check(const QString &path)
{
    for (const QString &attribute : std::as_const(attributeNames)) {
        QSet<QString> labels;
        for (const Row &row : std::as_const(table)) {
            if (row.sets.contains(attribute)) {
                labels.insert(row.sets.value(attribute));
            }
        }
        if (labels.size() < 2) {
            diagnostics.append(QStringLiteral("%1.%2: the only value is '%3', a constant is a tie")
                                   .arg(path, attribute, *labels.constBegin()));
        }
    }
    const auto rowPath = [&](qsizetype i) {
        return table.size() == 1 ? path : QStringLiteral("%1[%2]").arg(path).arg(i);
    };
    for (qsizetype i = 0; i < table.size(); ++i) {
        for (qsizetype j = i + 1; j < table.size(); ++j) {
            const Row &a       = table[i];
            const Row &b       = table[j];
            bool       overlap = true;
            for (auto it = a.when.constBegin(); it != a.when.constEnd(); ++it) {
                overlap = overlap && b.when.value(it.key(), it.value()) == it.value();
            }
            for (auto it = a.sets.constBegin(); overlap && it != a.sets.constEnd(); ++it) {
                const QString column = it.key();
                if (!b.sets.contains(column)) {
                    continue;
                }
                QString when;
                if (outputPins.contains(column)) {
                    const Truth differ  = (functions[it.value()] ^ functions[b.sets[column]])
                                          & cube(a.when) & cube(b.when);
                    int         minterm = 0;
                    while (minterm < span() && !differ.test(minterm)) {
                        ++minterm;
                    }
                    when = minterm < span() ? describe(minterm) : QString();
                } else if (it.value() != b.sets[column]) {
                    QMap<QString, bool> both = a.when;
                    both.insert(b.when);
                    QStringList levels;
                    for (auto level = both.constBegin(); level != both.constEnd(); ++level) {
                        levels.append(
                            QStringLiteral("%1=%2").arg(level.key()).arg(int(level.value())));
                    }
                    when = levels.isEmpty() ? QStringLiteral("always") : levels.join(' ');
                }
                if (!when.isEmpty()) {
                    diagnostics.append(QStringLiteral("%1 and %2: disagree on %3 when %4")
                                           .arg(rowPath(i), rowPath(j), column, when));
                }
            }
        }
    }
    for (const QString &pin : std::as_const(outputPins)) {
        Truth defined;
        Truth value;
        for (const Row &row : std::as_const(table)) {
            if (row.sets.contains(pin)) {
                const Truth covered = cube(row.when);
                defined |= covered;
                value |= covered & functions[row.sets[pin]];
            }
        }
        const Truth missing = all() & ~defined;
        if (missing.any()) {
            int minterm = 0;
            while (!missing.test(minterm)) {
                ++minterm;
            }
            diagnostics.append(
                QStringLiteral("%1: %2 is undefined when %3").arg(path, pin, describe(minterm)));
        }
        truths.insert(pin, value);
    }
    if (!diagnostics.isEmpty()) {
        truths.clear();
    }
}

QMap<QString, QString> QSocCellTable::evaluate(const QMap<QString, bool> &levels) const
{
    QMap<QString, QString> result;
    if (!isValid()) {
        return result;
    }
    QMap<QString, bool> known;
    for (const QString &pin : inputPins) {
        if (levels.contains(pin)) {
            known.insert(pin, levels.value(pin));
        }
    }
    if (!outputPins.isEmpty()) {
        const Truth region = cube(known);
        for (const QString &pin : outputPins) {
            const Truth high = truths.value(pin) & region;
            if (high == region || high.none()) {
                result.insert(pin, high.none() ? QStringLiteral("0") : QStringLiteral("1"));
            }
        }
    }
    for (const Row &row : table) {
        const bool fixed = std::all_of(row.when.keyBegin(), row.when.keyEnd(), [&](const QString &k) {
            return known.contains(k) && known.value(k) == row.when.value(k);
        });
        for (auto it = row.sets.constBegin(); fixed && it != row.sets.constEnd(); ++it) {
            if (attributeNames.contains(it.key())) {
                result.insert(it.key(), it.value());
            }
        }
    }
    return result;
}

QList<QSocCellTable::Pattern> QSocCellTable::patterns(
    const QString &attribute, const QStringList &pinOrder, QString *error) const
{
    QList<Pattern> result;
    for (const Row &row : table) {
        if (!row.sets.contains(attribute)) {
            continue;
        }
        for (auto it = row.when.constBegin(); it != row.when.constEnd(); ++it) {
            if (!pinOrder.contains(it.key())) {
                if (error) {
                    *error = QStringLiteral("%1: pin %2 is not in the pin order")
                                 .arg(attribute, it.key());
                }
                return {};
            }
        }
        Pattern pattern{QString(pinOrder.size(), 'x'), row.sets.value(attribute)};
        for (qsizetype i = 0; i < pinOrder.size(); ++i) {
            if (row.when.contains(pinOrder[i])) {
                pattern.bits[i] = row.when.value(pinOrder[i]) ? '1' : '0';
            }
        }
        result.append(pattern);
    }
    return result;
}

QSocCellTable QSocCellTable::tied(const QMap<QString, bool> &levels) const
{
    QSocCellTable result;
    result.diagnostics = diagnostics;
    if (!isValid()) {
        return result;
    }
    result.ports      = ports;
    result.outputPins = outputPins;
    for (const QString &pin : inputPins) {
        if (levels.contains(pin)) {
            result.ports.remove(pin);
        } else {
            result.inputPins.append(pin);
        }
    }
    result.functions.insert(QStringLiteral("0"), Truth());
    result.functions.insert(QStringLiteral("1"), result.all());
    for (int m = 0; m < result.span(); ++m) {
        int full = 0;
        Row row;
        for (qsizetype i = 0; i < inputPins.size(); ++i) {
            const qsizetype index = result.inputPins.indexOf(inputPins[i]);
            const bool level = index < 0 ? levels.value(inputPins[i]) : ((m >> index) & 1) != 0;
            full |= int(level) << i;
            if (index >= 0) {
                row.when.insert(inputPins[i], level);
            }
        }
        for (const QString &pin : outputPins) {
            const bool high = truths.value(pin).test(full);
            row.sets.insert(pin, high ? QStringLiteral("1") : QStringLiteral("0"));
            result.truths[pin].set(m, high);
        }
        result.table.append(row);
    }
    return result;
}

bool QSocCellTable::dependsOn(const QString &input) const
{
    const qsizetype index = inputPins.indexOf(input);
    if (index < 0) {
        return false;
    }
    for (const QString &pin : outputPins) {
        const Truth truth = truths.value(pin);
        for (int m = 0; m < span(); ++m) {
            if (truth.test(m) != truth.test(m ^ (1 << index))) {
                return true;
            }
        }
    }
    return false;
}

QList<QSocCellTable::PinMap> QSocCellTable::matches(const QSocCellTable &other) const
{
    QList<PinMap> result;
    if (!isValid() || !other.isValid() || inputPins.size() != other.inputPins.size()
        || outputPins.size() != other.outputPins.size() || inputPins.size() > maxMatchInputs
        || outputPins.isEmpty()) {
        return result;
    }
    QList<int> in(inputPins.size());
    std::iota(in.begin(), in.end(), 0);
    do {
        QList<int> out(outputPins.size());
        std::iota(out.begin(), out.end(), 0);
        do {
            bool same = true;
            for (int m = 0; same && m < span(); ++m) {
                int image = 0;
                for (qsizetype i = 0; i < in.size(); ++i) {
                    image |= ((m >> i) & 1) << in[i];
                }
                for (qsizetype o = 0; same && o < out.size(); ++o) {
                    same = truths.value(outputPins[o]).test(m)
                           == other.truths.value(other.outputPins[out[o]]).test(image);
                }
            }
            if (same) {
                PinMap map;
                for (qsizetype i = 0; i < in.size(); ++i) {
                    map.insert(inputPins[i], other.inputPins[in[i]]);
                }
                for (qsizetype o = 0; o < out.size(); ++o) {
                    map.insert(outputPins[o], other.outputPins[out[o]]);
                }
                result.append(map);
            }
        } while (std::next_permutation(out.begin(), out.end()));
    } while (std::next_permutation(in.begin(), in.end()));
    return result;
}

QString QSocCellTable::verilog(const QString &module, QString *error) const
{
    const auto fail = [&](const QString &message) {
        if (error) {
            *error = message;
        }
        return QString();
    };
    if (!isValid()) {
        return fail(diagnostics.join('\n'));
    }
    QStringList declarations;
    for (auto it = ports.constBegin(); it != ports.constEnd(); ++it) {
        const QString kind = direction(it.value());
        if (kind == "out" && !outputPins.contains(it.key())) {
            return fail(QStringLiteral("output %1 has no function").arg(it.key()));
        }
        if (kind != "in" && kind != "out") {
            return fail(QStringLiteral("%1 port %2 is not combinational").arg(kind, it.key()));
        }
        const QString range = it.value().width > 1
                                  ? QStringLiteral("[%1:0] ").arg(it.value().width - 1)
                                  : QString();
        declarations.append(
            QStringLiteral("    %1 wire %2%3")
                .arg(
                    kind == "in" ? QStringLiteral("input ") : QStringLiteral("output"),
                    range,
                    it.key()));
    }
    QString body;
    for (const QString &pin : outputPins) {
        QStringList terms;
        QString     lone;
        for (const Row &row : table) {
            const QString value = row.sets.value(pin, QStringLiteral("0"));
            if (value == "0") {
                continue;
            }
            QStringList factors;
            for (auto it = row.when.constBegin(); it != row.when.constEnd(); ++it) {
                factors.append(it.value() ? it.key() : QStringLiteral("~") + it.key());
            }
            if (value != "1") {
                lone = factors.isEmpty() ? value : QString();
                factors.append(QStringLiteral("(%1)").arg(value));
            }
            terms.append(factors.isEmpty() ? QStringLiteral("1'b1") : factors.join(" & "));
        }
        body += QStringLiteral("    assign %1 = %2;\n")
                    .arg(
                        pin,
                        terms.isEmpty()                        ? QStringLiteral("1'b0")
                        : terms.size() == 1 && !lone.isEmpty() ? lone
                                                               : terms.join(" | "));
    }
    return QStringLiteral("module %1 (\n%2\n);\n%3endmodule\n")
        .arg(module, declarations.join(",\n"), body);
}
