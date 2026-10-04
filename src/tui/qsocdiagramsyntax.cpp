// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qsocdiagram_p.h"
#include "tui/qtuiwidget.h"
#include <QRegularExpression>

namespace QSocDiagram::Detail {
[[noreturn]] void reject(Error error)
{
    throw Failure{error};
}
void require(bool condition, Error error)
{
    if (!condition)
        reject(error);
}

void checkLabel(const QString &text)
{
    require(text.isValidUtf16() && !text.trimmed().isEmpty());
    static const QRegularExpression markup(QStringLiteral("<[A-Za-z/!?]"));
    require(!text.contains(markup));
    static const QRegularExpression joined(QStringLiteral(
        "[\u0644\u06b5-\u06b8\u076a\u08a6\u08c7][\u0622\u0623\u0625\u0627\u0671-"
        "\u0673\u0675\u0773\u0774\u0870-\u0882]"
        "|[\ua4f8-\ua4fb][\ua4fc\ua4fd]"));
    require(!text.contains(joined));
    static const QRegularExpression unsupportedWidth(QStringLiteral(
        "[\u231a-\u231b\u23e9-\u23ec\u23f0\u23f3\u25fd-\u25fe\u2614-\u2615\u2630-\u2637\u2648-"
        "\u2653\u267f\u268a-\u268f\u2693\u26a1\u26aa-\u26ab\u26bd-\u26be\u26c4-"
        "\u26c5\u26ce\u26d4\u26ea\u26f2-\u26f3\u26f5\u26fa\u26fd\u2705\u270a-"
        "\u270b\u2728\u274c\u274e\u2753-\u2755\u2757\u2795-\u2797\u27b0\u27bf\u2b1b-"
        "\u2b1c\u2b50\u2b55\u33c0-\u33ff\u4dc0-\u4dff\ua960-\ua97c\u17a4]"));
    require(!text.contains(unsupportedWidth));
    int      columns  = 0;
    char32_t previous = 0;
    for (const char32_t scalar : text.toUcs4()) {
        const auto category = QChar::category(scalar);
        require(
            !(scalar == 0x16d67
              && (previous == 0x16d63 || previous == 0x16d67 || previous == 0x16d69)));
        previous = scalar;
        require(
            QChar::isPrint(scalar) && category != QChar::Mark_NonSpacing
            && category != QChar::Mark_Enclosing && category != QChar::Mark_SpacingCombining
            && category != QChar::Other_Format && scalar != 0x17d8 && scalar != 0x3164
            && scalar != 0xffa0 && scalar != 0xff9e && scalar != 0xff9f && scalar != 0xa8fa
            && scalar != 0x0d4e && !(scalar >= 0xd7b0 && scalar <= 0xd7ff)
            && !(scalar >= 0x1160 && scalar <= 0x11ff) && !(scalar >= 0x1f3fb && scalar <= 0x1f3ff)
            && !(
                scalar <= 0xffff
                && QStringLiteral("┌┐└┘├┤╪◄").contains(QChar(static_cast<ushort>(scalar)))));
        columns += QTuiText::isWideChar(scalar) ? 2 : 1;
    }
    require(columns <= labelLimit, Error::Limit);
}

QStringList statements(const QString &source)
{
    QStringList                     result;
    bool                            classBody = false;
    static const QRegularExpression entity(QStringLiteral("[#&][A-Za-z0-9_]+;"));
    for (const auto &line : source.split('\n')) {
        QString rest = line.trimmed();
        require(!rest.startsWith("%%{"));
        if (rest.startsWith("%%"))
            continue;
        require(!rest.contains(entity));
        while (!rest.isEmpty()) {
            const auto header = result.value(0);
            if (header == "classDiagram"
                && (classBody
                    || (rest.startsWith("class ") && rest.contains('{')
                        && !rest.first(rest.indexOf('{')).contains(';')))) {
                classBody = rest != "}";
                result.append(rest);
                break;
            }
            const bool flow      = header.startsWith("flowchart") || header.startsWith("graph");
            const bool colonText = header == "classDiagram" || header.startsWith("stateDiagram");
            bool       literal   = header == "sequenceDiagram";
            bool       inQuote   = false;
            QChar      close;
            int        end = rest.size();
            for (int i = 0; i < rest.size(); ++i) {
                const auto ch = rest[i];
                if (ch == '"' && !literal)
                    inQuote = !inQuote;
                if (ch == ':' && colonText && !inQuote)
                    literal = true;
                if (ch == ';' && !inQuote && close.isNull()) {
                    end = i;
                    break;
                }
                if (!flow || inQuote)
                    continue;
                if (ch == close)
                    close = {};
                else if (close.isNull()) {
                    if (ch == '[')
                        close = ']';
                    else if (ch == '{')
                        close = '}';
                    else if (ch == '|')
                        close = '|';
                }
            }
            const auto statement = rest.first(end).trimmed();
            if (!statement.isEmpty())
                result.append(statement);
            rest = end == rest.size() ? QString{} : rest.sliced(end + 1).trimmed();
        }
    }
    return result;
}

bool take(QString &text, const QString &prefix)
{
    text = text.trimmed();
    if (!text.startsWith(prefix))
        return false;
    text.remove(0, prefix.size());
    return true;
}

QString identifier(QString &text)
{
    text = text.trimmed();
    static const QRegularExpression pattern(QStringLiteral("^[A-Za-z][A-Za-z0-9_]*"));
    const auto                      match = pattern.match(text);
    require(match.hasMatch());
    const auto value = match.captured();
    require(value.size() <= labelLimit);
    text.remove(0, value.size());
    return value;
}

QString quoted(QString &text)
{
    require(take(text, "\""));
    const auto end = text.indexOf('"');
    require(end >= 0);
    const auto value = text.first(end);
    text.remove(0, end + 1);
    checkLabel(value);
    return value;
}

QString takeLabel(QString &text, const QString &closing)
{
    QString value;
    if (text.startsWith('"')) {
        value = quoted(text);
        require(!value.startsWith('`') && text.startsWith(closing));
        text.remove(0, closing.size());
    } else {
        const auto end = text.indexOf(closing);
        require(end >= 0);
        value = text.first(end);
        text.remove(0, end + closing.size());
        static const QRegularExpression delimiters(QStringLiteral("[\\[\\]{}|\"]"));
        require(!value.contains(delimiters));
        checkLabel(value);
    }
    return value;
}

Direction direction(const QString &text)
{
    if (text == "TD" || text == "TB")
        return Direction::Down;
    if (text == "BT")
        return Direction::Up;
    if (text == "LR")
        return Direction::Right;
    if (text == "RL")
        return Direction::Left;
    reject();
}

int Graph::node(const QString &id)
{
    for (int i = 0; i < nodes.size(); ++i)
        if (nodes[i].id == id)
            return i;
    require(nodes.size() < nodeLimit, Error::Limit);
    nodes.append({.id = id, .label = id});
    return nodes.size() - 1;
}
void Graph::edge(const Edge &value)
{
    require(edges.size() < edgeLimit, Error::Limit);
    edges.append(value);
}
void Graph::member(int index, const QString &value)
{
    checkLabel(value);
    require(nodes[index].members.size() < 16, Error::Limit);
    nodes[index].members.append(value);
}
} // namespace QSocDiagram::Detail
