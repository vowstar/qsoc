// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"

#include "agent/protocol/qsocagentruntimeevent.h"
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
 * existed, reduced to its scrollback writes. Tool events and resume are
 * left out: their header detail, outcome styling and replay changed. */
namespace Reference {

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
    QTuiCompositor &compositor;
    bool            streamedContent = false;

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
    case Kind::SessionStarted:
    case Kind::SessionCleared:
        compositor.printContent(event.text);
        break;
    case Kind::TaskNotification:
        compositor.printContent(
            QStringLiteral("(task: %1)\n").arg(event.text.section(QLatin1Char('\n'), 0, 0).left(160)),
            QTuiScrollView::Dim);
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

QString args(const json &value)
{
    return QString::fromStdString(value.dump());
}

QList<Event> toolScript()
{
    return {
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
        finished("read_file", "c9", QStringLiteral("binary image"), true),
    };
}

QList<Event> script()
{
    const QString longNote = QString(200, QLatin1Char('n')) + QStringLiteral("\nsecond line");
    return {
        make(Kind::SessionStarted, QStringLiteral("Session started.\n")),
        make(Kind::ReasoningChunk, QStringLiteral("Thinking about ")),
        make(Kind::ReasoningChunk, QStringLiteral("the plan.\n")),
        make(Kind::ContentChunk, QStringLiteral("Here is **code**:\n\n```cpp\nint ")),
        make(
            Kind::ContentChunk,
            QStringLiteral("main() { return 0; }\n```\n\n| a | b |\n|---|---|\n")),
        make(Kind::ContentChunk, QStringLiteral("| 1 | 2 |\n\nDone.\n")),
        imagePreview(),
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
        make(Kind::ContentChunk, QStringLiteral("after clear")),
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
    void showsArgumentDetailAndOutcome();
    void echoesUserMessages();
    void queuedPromptMatchesReplay();
    void stripsControlsFromExternalText();
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
    for (const auto &event : toolScript().mid(0, 6))
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
    for (const auto &event : toolScript())
        renderer.apply(event);
    const QString pane = screenText(compositor.todoList(), 80);
    QVERIFY(pane.contains(QStringLiteral("Review")));
    QVERIFY(pane.contains(QStringLiteral("Ship it")));
    QVERIFY(!pane.contains(QStringLiteral("Drop")));
}

void Test::showsArgumentDetailAndOutcome()
{
    QTuiCompositor         compositor;
    QSocTranscriptRenderer renderer(compositor);
    for (const auto &event : toolScript())
        renderer.apply(event);
    const QStringList outcomes{"dispatched", "uncertain", "skipped", "ok"};
    for (const QString &status : outcomes) {
        const QString id = QStringLiteral("s-") + status;
        renderer.apply(
            make(Kind::ToolStarted, args({{"command", status.toStdString()}}), "bash", id));
        Event done = finished("bash", id, status, false);
        done.json  = {{"status", status.toStdString()}};
        renderer.apply(done);
    }
    QTuiScreen screen(100, 200);
    compositor.contentView().render(screen, 0, 200, 100);
    QString text;
    for (int row = 0; row < 200; ++row) {
        for (int col = 0; col < 100; ++col)
            text += screen.at(col, row).character;
        text += QLatin1Char('\n');
    }
    const QString plain = compositor.contentView().toPlainText();
    QVERIFY(plain.contains(QStringLiteral("$ write_file src/hello.c\n")));
    QVERIFY(plain.contains(QStringLiteral("$ bash false && echo unreachable\n")));
    QVERIFY(plain.contains(QStringLiteral("$ todo_update #1\n")));
    QVERIFY(plain.contains(QStringLiteral("$ read_file\n")));
    QVERIFY(!plain.contains(QStringLiteral("$ write_file {")));
    QVERIFY(text.contains(QStringLiteral("dispatched, check task status")));
    QVERIFY(text.contains(QStringLiteral("completion uncertain")));
    QVERIFY(text.contains(QStringLiteral("not executed")));
    QCOMPARE(text.count(QStringLiteral("✗ failed")), 2);
}

void Test::echoesUserMessages()
{
    QTuiCompositor         compositor;
    QSocTranscriptRenderer renderer(compositor);
    renderer.apply(make(Kind::UserMessage, QStringLiteral("typed prompt")));
    renderer.apply(make(Kind::UserMessage, QStringLiteral("queued prompt")));
    QTuiCompositor expected;
    expected.appendUserMessage(QStringLiteral("typed prompt"));
    expected.appendUserMessage(QStringLiteral("queued prompt"));
    QCOMPARE(compositor.contentView().toAnsi(80), expected.contentView().toAnsi(80));
}

/* A prompt taken up inside a run ends the answer before it, as on replay. */
void Test::queuedPromptMatchesReplay()
{
    QTuiCompositor         live;
    QSocTranscriptRenderer renderer(live);
    for (const Event &event :
         {make(Kind::UserMessage, QStringLiteral("first")),
          make(Kind::ReasoningChunk, QStringLiteral("thinking")),
          make(Kind::ContentChunk, QStringLiteral("FIRSTANSWER")),
          make(Kind::ProcessingQueued, QStringLiteral("second")),
          make(Kind::UserMessage, QStringLiteral("second")),
          make(Kind::ContentChunk, QStringLiteral("SECONDANSWER")),
          make(Kind::RunComplete)})
        renderer.apply(event);

    const json history = json::array(
        {{{"role", "user"}, {"content", "first"}},
         {{"role", "assistant"}, {"content", "FIRSTANSWER"}, {"reasoning_content", "thinking"}},
         {{"role", "user"}, {"content", "second"}},
         {{"role", "assistant"}, {"content", "SECONDANSWER"}}});
    QTuiCompositor         replay;
    QSocTranscriptRenderer replayRenderer(replay);
    replayRenderer.replaceHistory(history);

    QCOMPARE(live.contentView().toAnsi(80), replay.contentView().toAnsi(80));
    const QString plain = live.contentView().toPlainText();
    QVERIFY(!plain.contains(QStringLiteral("FIRSTANSWERSECONDANSWER")));
    QVERIFY(plain.indexOf(QStringLiteral("FIRSTANSWER")) < plain.indexOf(QStringLiteral("second")));
    QVERIFY(plain.indexOf(QStringLiteral("second")) < plain.indexOf(QStringLiteral("SECONDANSWER")));
}

void Test::stripsControlsFromExternalText()
{
    const QString          osc52 = QStringLiteral("\x1b]52;c;cHduZWQ=\x07");
    const QString          csi   = QStringLiteral("\x1b[2J");
    QTuiCompositor         compositor;
    QSocTranscriptRenderer renderer(compositor);
    renderer.apply(
        make(Kind::ReasoningChunk, QStringLiteral("think") + osc52 + QStringLiteral("\n")));
    renderer.apply(make(Kind::ContentChunk, QStringLiteral("say") + csi + QStringLiteral("\x1b")));
    renderer.apply(make(Kind::ContentChunk, QStringLiteral("]0;split title\x07 more\n")));
    renderer.apply(make(Kind::ToolStarted, args({{"command", "ls\x1b]0;owned\x07"}}), "bash", "t"));
    renderer.apply(
        make(Kind::ToolOutput, QStringLiteral("line") + osc52 + QStringLiteral("\n"), {}, "t"));
    renderer.apply(
        finished("bash", "t", QStringLiteral("out") + csi + QStringLiteral("put\r"), true));
    renderer.apply(diff(QStringLiteral("a.txt"), "x\n", "y\x1b]52;c;eA==\x07\n"));
    renderer.apply(make(Kind::RunError, QStringLiteral("bad") + osc52));
    const QString ansi = compositor.contentView().toAnsi(100);
    QVERIFY(!ansi.contains(QStringLiteral("\x1b]")));
    QVERIFY(!ansi.contains(QStringLiteral("[2J")));
    QVERIFY(!ansi.contains(QStringLiteral("cHduZWQ")));
    QVERIFY(!ansi.contains(QStringLiteral("owned")));
    QVERIFY(!ansi.contains(QLatin1Char('\r')));
    const QString plain = compositor.contentView().toPlainText();
    QVERIFY(plain.contains(QStringLiteral("$ bash ls\n")));
    QVERIFY(plain.contains(QStringLiteral("output")));
    QVERIFY(plain.contains(QStringLiteral("Error: bad")));

    /* Runtime output carries QSoC's own text and passes through. */
    QTuiCompositor         own;
    QSocTranscriptRenderer ownRenderer(own);
    ownRenderer.apply(
        output(QStringLiteral("\x1b[1mstatus\x1b[0m\n"), QSocAgentRuntimeStyle::Normal));
    QVERIFY(own.contentView().toPlainText().contains(QStringLiteral("\x1b[1mstatus")));
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
