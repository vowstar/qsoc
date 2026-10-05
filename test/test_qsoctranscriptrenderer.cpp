// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"

#include "agent/protocol/qsocagentruntimeevent.h"
#include "cli/qsocsessiontranscript.h"
#include "cli/qsoctranscriptrenderer.h"
#include "common/qsoclinediff.h"
#include "tui/qtuicompositor.h"
#include "tui/qtuidiffblock.h"
#include "tui/qtuiimagepreviewblock.h"
#include "tui/qtuiscreen.h"
#include "tui/qtuiscrollview.h"
#include "tui/qtuitodoblock.h"

#include <nlohmann/json.hpp>

#include <QBuffer>
#include <QImage>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QtTest>

#include <memory>

using json  = nlohmann::json;
using Event = QSocAgentRuntimeEvent;
using Kind  = QSocAgentRuntimeEvent::Kind;

namespace {

/* Reference: the client's event switch as it was before the renderer
 * existed, reduced to its scrollback and todo pane writes. */
namespace Reference {

QList<QTuiTodoList::TodoItem> parseTodoListResult(const QString &result)
{
    QList<QTuiTodoList::TodoItem> items;
    QRegularExpression            regex(R"(\[([ x])\]\s*(\d+)\.\s*(.+?)\s*\((\w+)\))");
    const QStringList             lines = result.split('\n');
    for (const QString &line : lines) {
        QRegularExpressionMatch match = regex.match(line);
        if (match.hasMatch()) {
            QTuiTodoList::TodoItem item;
            item.status   = (match.captured(1) == "x") ? "done" : "pending";
            item.id       = match.captured(2).toInt();
            item.title    = match.captured(3).trimmed();
            item.priority = match.captured(4);
            items.append(item);
        }
    }
    return items;
}

QTuiTodoList::TodoItem parseTodoAddResult(const QString &result)
{
    QTuiTodoList::TodoItem item;
    item.id = -1;
    QRegularExpression      regex(R"(Added todo #(\d+):\s*(.+?)\s*\((\w+)(?:\s+priority)?\))");
    QRegularExpressionMatch match = regex.match(result);
    if (match.hasMatch()) {
        item.id       = match.captured(1).toInt();
        item.title    = match.captured(2).trimmed();
        item.priority = match.captured(3);
        item.status   = "pending";
    }
    return item;
}

QPair<int, QString> parseTodoUpdateResult(const QString &result)
{
    QRegularExpression      regex(R"(Updated todo #(\d+):\s*.+?\(status:\s*(\w+)\))");
    QRegularExpressionMatch match = regex.match(result);
    if (match.hasMatch())
        return qMakePair(match.captured(1).toInt(), match.captured(2));
    return qMakePair(-1, QString());
}

QTuiScrollView::LineStyle styleFor(QSocAgentRuntimeStyle style)
{
    switch (style) {
    case QSocAgentRuntimeStyle::Dim:
        return QTuiScrollView::Dim;
    case QSocAgentRuntimeStyle::Bold:
        return QTuiScrollView::Bold;
    case QSocAgentRuntimeStyle::Warning:
        return QTuiScrollView::DiffHunk;
    case QSocAgentRuntimeStyle::Normal:
        break;
    }
    return QTuiScrollView::Normal;
}

struct Client
{
    QTuiCompositor             &compositor;
    QHash<QString, QJsonObject> pendingTools;
    bool                        streamedContent = false;

    void apply(const Event &event);
};

void Client::apply(const Event &event)
{
    switch (event.kind) {
    case Kind::ContentChunk:
        streamedContent = true;
        compositor.appendAssistantChunk(event.text);
        break;
    case Kind::ReasoningChunk:
        compositor.appendReasoningChunk(event.text);
        break;
    case Kind::ToolStarted:
        pendingTools.insert(event.callId, QJsonDocument::fromJson(event.text.toUtf8()).object());
        compositor.beginToolUse(event.secondary, event.text.left(60), event.callId);
        break;
    case Kind::ToolOutput:
        compositor.appendToolUseBody(event.text, event.callId);
        break;
    case Kind::ToolFinished: {
        const auto    args = pendingTools.take(event.callId);
        const QString name = event.secondary;
        if (name == "todo_list") {
            const auto items = parseTodoListResult(event.text);
            compositor.todoList().setItems(items);
            compositor.contentView().appendBlock(std::make_unique<QTuiTodoBlock>(items));
        } else if (name == "todo_add") {
            const auto item = parseTodoAddResult(event.text);
            if (item.id >= 0)
                compositor.todoList().addItem(item);
        } else if (name == "todo_update" || name == "todo_delete") {
            const auto [id, status] = parseTodoUpdateResult(event.text);
            if (id >= 0) {
                if (name == "todo_delete")
                    compositor.todoList().removeItem(id);
                else
                    compositor.todoList().updateStatus(id, status);
            }
        }
        if (event.ok && (name == "edit_file" || name == "write_file")) {
            const QString path  = args.value("file_path").toString();
            const auto    lines = QSocLineDiff::computeLineDiff(
                args.value("old_string").toString(),
                args.value(name == "edit_file" ? "new_string" : "content").toString());
            auto block = std::make_unique<QTuiDiffBlock>("--- a/" + path, "+++ b/" + path);
            for (const auto &line : lines) {
                const auto kind = line.kind == QSocLineDiff::Kind::Add   ? QTuiDiffBlock::Kind::Add
                                  : line.kind == QSocLineDiff::Kind::Del ? QTuiDiffBlock::Kind::Del
                                  : line.kind == QSocLineDiff::Kind::Hunk
                                      ? QTuiDiffBlock::Kind::Hunk
                                      : QTuiDiffBlock::Kind::Context;
                block->addRow(kind, line.text);
            }
            compositor.contentView().appendBlock(std::move(block));
        }
        compositor.replaceToolUseBody(event.text, event.callId);
        compositor.finishToolUse(
            event.ok ? QTuiToolBlock::Status::Success : QTuiToolBlock::Status::Failure,
            {},
            event.callId);
        break;
    }
    case Kind::RunComplete:
        if (!streamedContent && !event.text.isEmpty())
            compositor.appendAssistantChunk(event.text);
        streamedContent = false;
        compositor.finishStream();
        compositor.resetExecution();
        compositor.printContent(QStringLiteral("\n"));
        break;
    case Kind::RunError:
        streamedContent = false;
        compositor.finishStream();
        compositor.resetExecution();
        compositor.printContent(QStringLiteral("\nError: %1\n").arg(event.text));
        break;
    case Kind::RunAborted:
        compositor.resetExecution();
        compositor.printContent(QStringLiteral("\n%1\n").arg(
            event.text.isEmpty() ? QStringLiteral("(interrupted)") : event.text));
        break;
    case Kind::ImagePreview:
        if (event.json.is_object()) {
            compositor.contentView().appendBlock(
                std::make_unique<QTuiImagePreviewBlock>(
                    event.text,
                    QString::fromStdString(event.json.value("mime", std::string())),
                    event.json.value("width", 0),
                    event.json.value("height", 0),
                    QByteArray::fromBase64(
                        QByteArray::fromStdString(event.json.value("data", std::string())))));
            compositor.invalidate();
        }
        break;
    case Kind::Diff: {
        const QString before = QString::fromStdString(event.json.value("before", std::string()));
        const QString after  = QString::fromStdString(event.json.value("after", std::string()));
        auto block = std::make_unique<QTuiDiffBlock>("--- a/" + event.text, "+++ b/" + event.text);
        for (const auto &line : QSocLineDiff::computeLineDiff(before, after)) {
            const auto kind = line.kind == QSocLineDiff::Kind::Add   ? QTuiDiffBlock::Kind::Add
                              : line.kind == QSocLineDiff::Kind::Del ? QTuiDiffBlock::Kind::Del
                              : line.kind == QSocLineDiff::Kind::Hunk
                                  ? QTuiDiffBlock::Kind::Hunk
                                  : QTuiDiffBlock::Kind::Context;
            block->addRow(kind, line.text);
        }
        compositor.contentView().appendBlock(std::move(block));
        compositor.invalidate();
        break;
    }
    case Kind::Output:
        compositor.printContent(event.text, styleFor(event.style));
        break;
    case Kind::SessionResumed:
        if (event.json.is_object() && event.json.contains("messages")) {
            compositor.contentView().clear();
            QSocSessionTranscript::appendTo(event.json["messages"], compositor.contentView());
        }
        [[fallthrough]];
    case Kind::SessionStarted:
    case Kind::SessionCleared:
        compositor.printContent(event.text);
        break;
    case Kind::TaskNotification:
        compositor.printContent(
            QStringLiteral("(task: %1)\n").arg(event.text.left(80)), QTuiScrollView::Dim);
        break;
    default:
        break;
    }
}

} // namespace Reference

Event make(Kind kind, const QString &text = {}, const QString &secondary = {}, const QString &id = {})
{
    Event event;
    event.kind      = kind;
    event.text      = text;
    event.secondary = secondary;
    event.callId    = id;
    return event;
}

Event finished(const QString &name, const QString &id, const QString &text, bool ok)
{
    Event event = make(Kind::ToolFinished, text, name, id);
    event.ok    = ok;
    return event;
}

Event output(const QString &text, QSocAgentRuntimeStyle style)
{
    Event event = make(Kind::Output, text);
    event.style = style;
    return event;
}

Event imagePreview()
{
    QImage image(8, 6, QImage::Format_RGB32);
    image.fill(qRgb(0xd6, 0x5d, 0x0e));
    QByteArray png;
    QBuffer    buffer(&png);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    Event event = make(Kind::ImagePreview, QStringLiteral("shot.png"));
    event.json
        = {{"mime", "image/png"},
           {"width", image.width()},
           {"height", image.height()},
           {"data", png.toBase64().toStdString()}};
    return event;
}

Event diff(const QString &path, const char *before, const char *after)
{
    Event event = make(Kind::Diff, path);
    event.json  = {{"before", before}, {"after", after}};
    return event;
}

Event resumed()
{
    Event event = make(Kind::SessionResumed, QStringLiteral("Resumed session.\n"));
    event.json  = {
        {"messages",
         json::array(
             {{{"role", "user"}, {"content", "earlier question"}},
              {{"role", "assistant"}, {"content", "earlier answer"}}})}};
    return event;
}

QString args(const json &value)
{
    return QString::fromStdString(value.dump());
}

QList<Event> script()
{
    const QString longNote = QString(120, QLatin1Char('n'));
    return {
        make(Kind::SessionStarted, QStringLiteral("Session started.\n")),
        make(Kind::ReasoningChunk, QStringLiteral("Thinking about ")),
        make(Kind::ReasoningChunk, QStringLiteral("the plan.\n")),
        make(Kind::ContentChunk, QStringLiteral("Here is **code**:\n\n```cpp\nint ")),
        make(
            Kind::ContentChunk,
            QStringLiteral("main() { return 0; }\n```\n\n| a | b |\n|---|---|\n")),
        make(Kind::ContentChunk, QStringLiteral("| 1 | 2 |\n\nDone.\n")),
        make(
            Kind::ToolStarted,
            args({{"file_path", "src/hello.c"}, {"content", "int x;\nint y;\n"}}),
            "write_file",
            "c1"),
        finished("write_file", "c1", QStringLiteral("Wrote src/hello.c"), true),
        make(
            Kind::ToolStarted,
            args({{"file_path", "src/hello.c"}, {"old_string", "int x;"}, {"new_string", "long x;"}}),
            "edit_file",
            "c2"),
        finished("edit_file", "c2", QStringLiteral("Edited src/hello.c"), true),
        make(
            Kind::ToolStarted,
            args({{"file_path", "src/none.c"}, {"old_string", "a"}, {"new_string", "b"}}),
            "edit_file",
            "c3"),
        finished("edit_file", "c3", QStringLiteral("Error: old_string not found"), false),
        make(Kind::ToolStarted, args({{"command", "false && echo unreachable"}}), "bash", "c4"),
        make(Kind::ToolOutput, QStringLiteral("line one\n"), {}, "c4"),
        make(Kind::ToolOutput, QStringLiteral("line two\n"), {}, "c4"),
        finished("bash", "c4", QStringLiteral("status: failed\nexit code: 1"), false),
        make(Kind::ToolStarted, args(json::object()), "todo_list", "c5"),
        finished(
            "todo_list",
            "c5",
            QStringLiteral("[ ] 1. Write tests (high)\n[x] 2. Ship it (low)\n[ ] 3. Drop (medium)"),
            true),
        make(Kind::ToolStarted, args({{"title", "Review"}}), "todo_add", "c6"),
        finished("todo_add", "c6", QStringLiteral("Added todo #4: Review (high priority)"), true),
        make(Kind::ToolStarted, args({{"id", 1}}), "todo_update", "c7"),
        finished("todo_update", "c7", QStringLiteral("Updated todo #1: Write (status: done)"), true),
        make(Kind::ToolStarted, args({{"id", 3}}), "todo_delete", "c8"),
        finished("todo_delete", "c8", QStringLiteral("Updated todo #3: Drop (status: x)"), true),
        make(Kind::ToolStarted, QStringLiteral("not json"), "read_file", "c9"),
        imagePreview(),
        finished("read_file", "c9", QStringLiteral("binary image"), true),
        diff(QStringLiteral("doc/notes.txt"), "a\nb\nc\n", "a\nB\nc\nd\n"),
        output(QStringLiteral("plain line\n"), QSocAgentRuntimeStyle::Normal),
        output(QStringLiteral("dim line\n"), QSocAgentRuntimeStyle::Dim),
        output(QStringLiteral("bold line\n"), QSocAgentRuntimeStyle::Bold),
        output(QStringLiteral("warning line\n"), QSocAgentRuntimeStyle::Warning),
        make(Kind::TaskNotification, longNote),
        make(Kind::Status, QStringLiteral("Working")),
        make(Kind::Tokens),
        make(Kind::RunComplete, QStringLiteral("final text ignored after streaming")),
        make(Kind::RunComplete, QStringLiteral("final text shown without streaming")),
        make(Kind::ContentChunk, QStringLiteral("partial")),
        make(Kind::RunError, QStringLiteral("provider failed")),
        make(Kind::RunComplete, QStringLiteral("final text after an error")),
        make(Kind::ContentChunk, QStringLiteral("partial again")),
        make(Kind::RunAborted),
        make(Kind::RunComplete, QStringLiteral("final text after an abort")),
        make(Kind::RunAborted, QStringLiteral("Stopped by user.")),
        make(Kind::SessionCleared, QStringLiteral("Cleared.\n")),
        resumed(),
        make(Kind::ContentChunk, QStringLiteral("after resume")),
        make(Kind::RunComplete),
    };
}

QString screenText(QTuiWidget &widget, int width)
{
    const int  height = qMax(1, widget.lineCount());
    QTuiScreen screen(width, height);
    widget.render(screen, 0, width);
    QString text;
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col)
            text += screen.at(col, row).character;
        text += QLatin1Char('\n');
    }
    return text;
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void matchesThePreviousClientOutput_data();
    void matchesThePreviousClientOutput();
    void rendersEditAndWriteDiffsOnlyOnSuccess();
    void fillsTheTodoPane();
    void replaceHistoryClearsTheScrollback();
};

void Test::matchesThePreviousClientOutput_data()
{
    QTest::addColumn<int>("prefix");
    const int total = int(script().size());
    for (int prefix = 1; prefix <= total; ++prefix)
        QTest::addRow("%d", prefix) << prefix;
}

void Test::matchesThePreviousClientOutput()
{
    QFETCH(int, prefix);
    const auto events = script().mid(0, prefix);

    QTuiCompositor    expected;
    Reference::Client client{expected};
    for (const auto &event : events)
        client.apply(event);

    QTuiCompositor         actual;
    QSocTranscriptRenderer renderer(actual);
    for (const auto &event : events)
        renderer.apply(event);

    for (const int width : {100, 48}) {
        QCOMPARE(actual.contentView().toAnsi(width), expected.contentView().toAnsi(width));
        QCOMPARE(screenText(actual.todoList(), width), screenText(expected.todoList(), width));
    }
    QCOMPARE(actual.contentView().toPlainText(), expected.contentView().toPlainText());
}

void Test::rendersEditAndWriteDiffsOnlyOnSuccess()
{
    QTuiCompositor         compositor;
    QSocTranscriptRenderer renderer(compositor);
    for (const auto &event : script().mid(0, 12))
        renderer.apply(event);
    const QString text = compositor.contentView().toPlainText();
    QCOMPARE(text.count(QStringLiteral("+++ b/src/hello.c")), 2);
    QVERIFY(text.contains(QStringLiteral("long x;")));
    QVERIFY(!text.contains(QStringLiteral("+++ b/src/none.c")));
}

void Test::fillsTheTodoPane()
{
    QTuiCompositor         compositor;
    QSocTranscriptRenderer renderer(compositor);
    for (const auto &event : script())
        renderer.apply(event);
    const QString pane = screenText(compositor.todoList(), 80);
    QVERIFY(pane.contains(QStringLiteral("Review")));
    QVERIFY(pane.contains(QStringLiteral("Ship it")));
    QVERIFY(!pane.contains(QStringLiteral("Drop")));
}

void Test::replaceHistoryClearsTheScrollback()
{
    QTuiCompositor         compositor;
    QSocTranscriptRenderer renderer(compositor);
    renderer.apply(make(Kind::Output, QStringLiteral("stale line\n")));
    renderer.replaceHistory(json::array({{{"role", "user"}, {"content", "kept question"}}}));
    const QString text = compositor.contentView().toPlainText();
    QVERIFY(!text.contains(QStringLiteral("stale line")));
    QVERIFY(text.contains(QStringLiteral("kept question")));
}

QSOC_TEST_MAIN(Test)

#include "test_qsoctranscriptrenderer.moc"
