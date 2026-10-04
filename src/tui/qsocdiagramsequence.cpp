// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qsocdiagram_p.h"
#include "tui/qtuiwidget.h"
#include <algorithm>

namespace QSocDiagram::Detail {
namespace {
enum class EventKind { Message, Note, Open, Branch, Close };
struct Event
{
    EventKind kind = EventKind::Message;
    int       from = 0;
    int       to   = 0;
    QString   text;
    bool      dashed = false;
    Tip       tip    = Tip::None;
};
struct Participant
{
    QString id;
    QString label;
    bool    declared = false;
};
struct Fragment
{
    QString kind;
    bool    branched = false;
};
struct Timeline
{
    QList<Participant> people;
    QList<Event>       events;
    QList<Fragment>    fragments;
    int                participant(QString &text)
    {
        const auto id = identifier(text);
        for (int i = 0; i < people.size(); ++i)
            if (people[i].id == id)
                return i;
        require(people.size() < 8, Error::Limit);
        people.append({.id = id, .label = id});
        return people.size() - 1;
    }
    void add(const Event &event)
    {
        require(events.size() < 64, Error::Limit);
        events.append(event);
    }
};
QString messageText(QString &text)
{
    require(take(text, ":"));
    text = text.trimmed();
    checkLabel(text);
    return text;
}
void parseLine(Timeline &timeline, QString text)
{
    const bool actor = text.startsWith("actor ");
    if (actor || text.startsWith("participant ")) {
        text.remove(0, actor ? 6 : 12);
        const int index  = timeline.participant(text);
        auto     &person = timeline.people[index];
        require(!person.declared);
        if (take(text, "as ")) {
            person.label = text.trimmed();
            checkLabel(person.label);
        } else
            require(text.trimmed().isEmpty());
        if (actor)
            person.label += QStringLiteral(" (actor)");
        person.declared = true;
        return;
    }
    if (text == "end") {
        require(!timeline.fragments.isEmpty());
        timeline.fragments.removeLast();
        timeline.add({.kind = EventKind::Close});
        return;
    }
    if (take(text, "else ")) {
        require(
            !timeline.fragments.isEmpty() && timeline.fragments.last().kind == "alt"
            && !timeline.fragments.last().branched);
        checkLabel(text);
        timeline.fragments.last().branched = true;
        timeline.add({.kind = EventKind::Branch, .text = "else " + text});
        return;
    }
    const auto word = text.section(' ', 0, 0);
    if (QStringList{"loop", "alt", "opt", "critical", "break"}.contains(word)) {
        require(text.size() > word.size());
        const auto label = text.sliced(word.size() + 1).trimmed();
        checkLabel(label);
        require(timeline.fragments.size() < 4, Error::Limit);
        timeline.fragments.append({.kind = word});
        timeline.add({.kind = EventKind::Open, .text = word + ' ' + label});
        return;
    }
    Event event;
    if (take(text, "Note over ") || take(text, "note over ")) {
        event.kind = EventKind::Note;
        event.from = timeline.participant(text);
        event.to   = take(text, ",") ? timeline.participant(text) : event.from;
    } else {
        event.from = timeline.participant(text);
        bool found = false;
        for (const QString &token : {"-->>", "->>", "-->", "->", "--x", "-x"}) {
            if (!take(text, token))
                continue;
            event.dashed = token.startsWith("--");
            event.tip    = token.endsWith("x")    ? Tip::Cross
                           : token.endsWith(">>") ? Tip::Arrow
                                                  : Tip::None;
            found        = true;
            break;
        }
        require(found);
        event.to = timeline.participant(text);
    }
    event.text = messageText(text);
    timeline.add(event);
}
char32_t arrow(const Event &event, bool left)
{
    if (event.tip == Tip::Cross)
        return U'x';
    if (event.tip == Tip::Arrow)
        return left ? U'◀' : U'▶';
    return event.dashed ? U'┄' : U'─';
}
void drawMessage(Canvas &canvas, const Event &event, int source, int target, int row)
{
    const int left = std::min(source, target), right = std::max(source, target);
    canvas.text(left + 2, row, event.text);
    const auto line = event.dashed ? U'┄' : U'─';
    if (source == target) {
        canvas.horizontal(source, source + 4, row + 1, line);
        canvas.horizontal(source, source + 4, row + 2, line);
        canvas.put(source + 4, row + 1, U'┐');
        canvas.put(source + 4, row + 2, U'┘');
        canvas.put(source, row + 1, U'├');
        canvas.put(source, row + 2, arrow(event, true));
        return;
    }
    canvas.horizontal(left, right, row + 1, line);
    for (int x = left + 1; x < right; ++x)
        if (canvas.at(x, row + 1) == U'╪')
            canvas.put(x, row + 1, U'┼');
    canvas.put(source, row + 1, source < target ? U'├' : U'┤');
    canvas.put(target, row + 1, arrow(event, target < source));
}
} // namespace

Result sequence(const QStringList &body, int width)
{
    Timeline timeline;
    for (const auto &line : body)
        parseLine(timeline, line);
    require(!timeline.people.isEmpty() && timeline.fragments.isEmpty());
    int nameWidth = 0, labelWidth = 0;
    for (const auto &person : timeline.people)
        nameWidth = std::max(nameWidth, QTuiText::visualWidth(person.label));
    for (const auto &event : timeline.events)
        labelWidth = std::max(
            labelWidth, QTuiText::visualWidth(event.text) + (event.kind == EventKind::Note ? 6 : 0));
    const int  boxWidth = nameWidth + 4;
    const int  stride   = std::max(boxWidth + 2, labelWidth + 4);
    QList<int> centers;
    for (int i = 0; i < timeline.people.size(); ++i)
        centers.append(10 + boxWidth / 2 + i * stride);
    const int columns = centers.last() + std::max(boxWidth - boxWidth / 2 + 10, labelWidth + 14);
    const int lines   = 3 + 4 * timeline.events.size();
    Canvas    canvas(columns, lines, width);
    for (int i = 0; i < timeline.people.size(); ++i) {
        canvas.box(centers[i] - boxWidth / 2, 0, boxWidth, 3);
        canvas.text(centers[i] - boxWidth / 2 + 2, 1, timeline.people[i].label, Role::Node);
        if (lines > 3)
            canvas.vertical(centers[i], 3, lines - 1, U'│');
    }
    QList<int> frameStarts;
    for (int i = 0; i < timeline.events.size(); ++i) {
        const auto &event = timeline.events[i];
        const int   row   = 3 + 4 * i;
        if (event.kind == EventKind::Message) {
            drawMessage(canvas, event, centers[event.from], centers[event.to], row);
            continue;
        }
        if (event.kind == EventKind::Note) {
            const int left  = std::min(centers[event.from], centers[event.to]);
            const int right = std::max(
                std::max(centers[event.from], centers[event.to]),
                left + QTuiText::visualWidth(event.text) + 9);
            for (int x = left; x <= right; ++x)
                canvas.put(x, row + 1, U' ', Role::Node);
            canvas.box(left, row, right - left + 1, 3);
            canvas.text(left + 2, row + 1, "Note: " + event.text);
            continue;
        }
        if (event.kind == EventKind::Open)
            frameStarts.append(row);
        const int depth = frameStarts.size() - 1;
        const int left = depth * 2, right = columns - 1 - left;
        canvas.horizontal(left, right, row, U'─', Role::Node);
        if (event.kind == EventKind::Close) {
            const int start = frameStarts.takeLast();
            for (int y = start + 1; y < row; ++y) {
                if (canvas.at(left, y) != U'├')
                    canvas.put(left, y, U'│', Role::Node);
                if (canvas.at(right, y) != U'┤')
                    canvas.put(right, y, U'│', Role::Node);
            }
            canvas.put(left, row, U'└', Role::Node);
            canvas.put(right, row, U'┘', Role::Node);
        } else {
            canvas.put(left, row, event.kind == EventKind::Open ? U'┌' : U'├', Role::Node);
            canvas.put(right, row, event.kind == EventKind::Open ? U'┐' : U'┤', Role::Node);
            canvas.text(left + 2, row, event.text);
        }
    }
    return canvas.result();
}
} // namespace QSocDiagram::Detail
