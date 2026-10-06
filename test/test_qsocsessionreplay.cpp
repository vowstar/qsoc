// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"

#include "agent/protocol/qsocagentruntimeevent.h"
#include "agent/protocol/qsocmessagemarkup.h"
#include "agent/qsoctaskeventqueue.h"
#include "cli/qsocsessionreplay.h"
#include "cli/qsocterminaltext.h"
#include "cli/qsoctranscriptrenderer.h"
#include "tui/qtuiassistanttextblock.h"
#include "tui/qtuicompositor.h"
#include "tui/qtuiscreen.h"
#include "tui/qtuiscrollview.h"
#include "tui/qtuiuserblock.h"

#include <nlohmann/json.hpp>

#include <QBuffer>
#include <QImage>
#include <QRegularExpression>
#include <QtTest>

using json  = nlohmann::json;
using Event = QSocAgentRuntimeEvent;
using Kind  = QSocAgentRuntimeEvent::Kind;

namespace {

json toolCall(const char *id, const char *name, const json &arguments)
{
    return {
        {"id", id},
        {"type", "function"},
        {"function",
         {{"name", name},
          {"arguments", arguments.is_string() ? arguments.get<std::string>() : arguments.dump()}}},
    };
}

json assistantCalls(const json &calls, const char *content = nullptr)
{
    json message       = {{"role", "assistant"}, {"tool_calls", calls}};
    message["content"] = content ? json(content) : json(nullptr);
    return message;
}

json toolResult(const char *id, const char *content, const char *status = nullptr)
{
    json message = {{"role", "tool"}, {"tool_call_id", id}, {"content", content}};
    if (status)
        message["_qsoc_status"] = status;
    return message;
}

QString screenText(const QTuiScreen &screen, int width, int height)
{
    QString text;
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col)
            text += screen.at(col, row).character;
        text += QLatin1Char('\n');
    }
    return text;
}

QString pane(QTuiCompositor &compositor)
{
    QTuiWidget &widget = compositor.todoList();
    const int   height = qMax(1, widget.lineCount());
    QTuiScreen  screen(80, height);
    widget.render(screen, 0, 80);
    return screenText(screen, 80, height);
}

QString pngDataUrl()
{
    QImage image(6, 4, QImage::Format_RGB32);
    image.fill(qRgb(0x45, 0x85, 0x88));
    QByteArray bytes;
    QBuffer    buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    image.save(&buffer, "PNG");
    return QStringLiteral("data:image/png;base64,") + QString::fromLatin1(bytes.toBase64());
}

/* A history with every block the replay rebuilds, new-format keys included. */
json richHistory()
{
    return json::array(
        {{{"role", "user"}, {"content", "[Conversation Summary]\nearlier work"}},
         {{"role", "user"}, {"content", "make the change"}},
         {{"role", "assistant"},
          {"reasoning_content", "Plan the edit."},
          {"content",
           "Here is the plan:\n\n```cpp\nint main() { return 0; }\n```\n\n| a | b |\n|---|---|\n| "
           "1 | 2 |\n"},
          {"tool_calls",
           json::array(
               {toolCall("w1", "write_file", {{"file_path", "src/a.c"}, {"content", "int x;\n"}}),
                toolCall(
                    "e1",
                    "edit_file",
                    {{"file_path", "src/a.c"}, {"old_string", "int x;"}, {"new_string", "long x;"}}),
                toolCall("b1", "bash", {{"command", "make"}}),
                toolCall("t1", "todo_list", json::object()),
                toolCall("r1", "read_file", {{"file_path", "shot.png"}})})}},
         toolResult("w1", "Wrote src/a.c", "ok"),
         toolResult("e1", "Edited src/a.c", "ok"),
         toolResult("b1", "status: failed\nexit code: 2", "failed"),
         toolResult("t1", "[ ] 1. Ship (high)", "ok"),
         toolResult("r1", "binary image", "ok"),
         {{"role", "user"},
          {"content",
           json::array(
               {{{"type", "image_url"}, {"image_url", {{"url", pngDataUrl().toStdString()}}}}})},
          {"_img_tokens", 12}},
         {{"role", "user"},
          {"content", "<task-notification>done</task-notification>"},
          {"_qsoc_origin", {{"kind", "task_notification"}}}},
         {{"role", "assistant"}, {"reasoning_content", "Wrap up."}, {"content", "All done."}}});
}

/* The same history as an older binary wrote it. */
json oldHistory()
{
    json history = richHistory();
    for (auto &message : history) {
        message.erase("_qsoc_status");
        message.erase("_qsoc_origin");
        if (message.value("role", std::string()) == "tool")
            message["_qsoc_result_bounded"] = true;
    }
    return history;
}

struct Replay
{
    QTuiCompositor         compositor;
    QSocTranscriptRenderer renderer{compositor};

    explicit Replay(const json &messages) { renderer.replaceHistory(messages); }

    QTuiScrollView &view() { return compositor.contentView(); }

    QString text()
    {
        static const QRegularExpression csi(QStringLiteral("\x1b\\[[0-9;:?]*[A-Za-z]"));
        return view().toAnsi(100).remove(csi);
    }

    QStringList blocks()
    {
        QStringList markdown;
        for (int index = 0; index < view().totalLines(); ++index) {
            view().setFocusedBlockIdx(index);
            markdown.append(view().copyFocusedAsMarkdown());
        }
        view().setFocusedBlockIdx(-1);
        return markdown;
    }
};

json withoutKey(json history, size_t index, const std::string &key)
{
    history[index].erase(key);
    return history;
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void rendersCodeFencesAsCodeBlocks();
    void rebuildsWriteAndEditDiffs();
    void showsFailedAndDispatchedOutcomes();
    void infersOutcomeForOldSessions();
    void foldsOlderReasoning();
    void rendersImagePartsAndAttachments();
    void rendersOrphanToolResultsAsGenericBlocks();
    void replacesTheArtifactNoticeWithAHint_data();
    void replacesTheArtifactNoticeWithAHint();
    void rendersNonRequestMessagesAsNotices();
    void showsTheLiveNotificationSummary();
    void rendersShellCommandResults();
    void shellResultLinesEndInLf();
    void buildsTheTodoPaneFromTheLatestList();
    void stripsTerminalControls();
    void resumesOldFormatSessions();
    void survivesCutDownHistories();
    void replaceHistoryDropsLiveCursors();
    void rendersReadableMessagesInOrder();
    void pairsToolCallsById();
    void rendersRecoveryToolStates();
    void reusesToolIdsAcrossBatches();
    void rejectsAmbiguousToolHistory();
    void keepsSyntheticMessagesOutOfUserBlocks();
    void doesNotMutateInput();
    void handlesMalformedInput();
    void usesExistingScrollbackNavigation();
};

void Test::rendersCodeFencesAsCodeBlocks()
{
    Replay replay(
        json::array(
            {{{"role", "user"}, {"content", "show code"}},
             {{"role", "assistant"},
              {"content",
               "Here is **code**:\n\n```cpp\nint main() { return 0; }\n```\n\nDone.\n"}}}));
    const QStringList blocks = replay.blocks();
    QVERIFY2(
        blocks.contains(QStringLiteral("```cpp\nint main() { return 0; }\n```\n")),
        qPrintable(blocks.join(QStringLiteral("\n---\n"))));
    for (const QString &block : blocks)
        QVERIFY(
            !(block.contains(QStringLiteral("Here is")) && block.contains(QStringLiteral("```"))));
}

void Test::rebuildsWriteAndEditDiffs()
{
    Replay replay(
        json::array(
            {{{"role", "user"}, {"content", "edit"}},
             assistantCalls(
                 json::array(
                     {toolCall("w", "write_file", {{"file_path", "src/a.c"}, {"content", "int x;\n"}}),
                      toolCall(
                          "e",
                          "edit_file",
                          {{"file_path", "src/a.c"},
                           {"old_string", "int x;"},
                           {"new_string", "long x;"}}),
                      toolCall(
                          "f",
                          "edit_file",
                          {{"file_path", "src/none.c"}, {"old_string", "a"}, {"new_string", "b"}})})),
             toolResult("w", "Wrote src/a.c", "ok"),
             toolResult("e", "Edited src/a.c", "ok"),
             toolResult("f", "Error: old_string not found", "failed")}));
    const QString text = replay.text();
    QCOMPARE(text.count(QStringLiteral("+++ b/src/a.c")), 2);
    QVERIFY(text.contains(QStringLiteral("long x;")));
    QVERIFY(!text.contains(QStringLiteral("+++ b/src/none.c")));
}

void Test::showsFailedAndDispatchedOutcomes()
{
    Replay replay(
        json::array(
            {{{"role", "user"}, {"content", "run"}},
             assistantCalls(
                 json::array(
                     {toolCall("b", "bash", {{"command", "make"}}),
                      toolCall("d", "agent", {{"description", "child"}})})),
             toolResult("b", "status: failed\nexit code: 2", "failed"),
             toolResult("d", "status: dispatched\ntask a1", "dispatched")}));
    const QString text = replay.text();
    QVERIFY2(text.contains(QStringLiteral("failed")), qPrintable(text));
    QVERIFY(text.contains(QStringLiteral("dispatched, check task status")));
    QVERIFY(!text.contains(QStringLiteral("done,")));
}

void Test::infersOutcomeForOldSessions()
{
    Replay replay(
        json::array(
            {{{"role", "user"}, {"content", "run"}},
             assistantCalls(
                 json::array(
                     {toolCall("a", "bash", {{"command", "a"}}),
                      toolCall("b", "bash", {{"command", "b"}}),
                      toolCall("c", "web", {{"url", "u"}}),
                      toolCall("d", "bash", {{"command", "d"}})})),
             toolResult("a", "status: failed\nexit code: 1"),
             toolResult("b", "Error: no such file"),
             toolResult("c", R"({"status":"error","message":"x"})"),
             toolResult("d", "fine")}));
    const QString text = replay.text();
    QCOMPARE(text.count(QStringLiteral("failed")), 4);
    QCOMPARE(text.count(QStringLiteral("done, 1 line")), 1);
}

void Test::foldsOlderReasoning()
{
    Replay replay(
        json::array(
            {{{"role", "user"}, {"content", "q1"}},
             {{"role", "assistant"},
              {"reasoning_content", "first thought\nline two"},
              {"content", "a1"}},
             {{"role", "user"}, {"content", "q2"}},
             {{"role", "assistant"}, {"reasoning", "second thought"}, {"content", "a2"}}}));
    const QString text = replay.text();
    QVERIFY(!text.contains(QStringLiteral("first thought")));
    QVERIFY(text.contains(QStringLiteral("(folded)")));
    QVERIFY(text.indexOf(QStringLiteral("a1")) < text.indexOf(QStringLiteral("a2")));
}

void Test::rendersImagePartsAndAttachments()
{
    Replay replay(
        json::array(
            {{{"role", "user"},
              {"content",
               json::array(
                   {{{"type", "text"}, {"text", "look at this"}},
                    {{"type", "image_url"}, {"image_url", {{"url", pngDataUrl().toStdString()}}}}})}},
             {{"role", "assistant"},
              {"content", json::array({{{"type", "text"}, {"text", "A teal box."}}})}}}));
    const QString plain = replay.view().toPlainText();
    QVERIFY(plain.contains(QStringLiteral("look at this")));
    QVERIFY(plain.contains(QStringLiteral("[image: image png 6x4")));
    QVERIFY(plain.contains(QStringLiteral("A teal box.")));
}

void Test::rendersOrphanToolResultsAsGenericBlocks()
{
    Replay replay(
        json::array(
            {{{"role", "user"}, {"content", "[Conversation Summary]\nolder"}},
             toolResult("lost", "kept output", "ok"),
             {{"role", "assistant"}, {"content", "next"}}}));
    QVERIFY(replay.view().toPlainText().contains(QStringLiteral("$ tool\nkept output")));
}

void Test::replacesTheArtifactNoticeWithAHint_data()
{
    QTest::addColumn<QString>("content");
    const json ref = {{"artifact_id", "art-1"}, {"captured_bytes", 9000}, {"sha256", "00"}};
    QTest::newRow("old suffix") << QStringLiteral(
                                       "head\n[Captured tool return saved locally: %1. Use "
                                       "tool_output_read when available.]")
                                       .arg(QString::fromStdString(ref.dump()));
    QTest::newRow("middle") << QStringLiteral(
        "head\n[Middle of the output omitted; read artifact art-1 (9000 bytes) with "
        "tool_output_read.]\ntail");
    QTest::newRow("notice lost") << QStringLiteral("head");
}

void Test::replacesTheArtifactNoticeWithAHint()
{
    QFETCH(QString, content);
    json result                   = toolResult("b", content.toUtf8().constData(), "ok");
    result["_qsoc_artifact_refs"] = json::array(
        {{{"artifact_id", "art-1"}, {"captured_bytes", 9000}, {"sha256", "00"}}});
    Replay replay(
        json::array(
            {{{"role", "user"}, {"content", "run"}},
             assistantCalls(json::array({toolCall("b", "bash", {{"command", "cat big"}})})),
             result}));
    const QString text = replay.text();
    QVERIFY(text.contains(QStringLiteral("head")));
    QVERIFY(text.contains(QStringLiteral("(full output saved: art-1, 9000 bytes)")));
    QVERIFY(!text.contains(QStringLiteral("tool_output_read")));
    QVERIFY(!text.contains(QStringLiteral("sha256")));
}

void Test::rendersNonRequestMessagesAsNotices()
{
    Replay replay(
        json::array(
            {{{"role", "user"}, {"content", "real request"}},
             {{"role", "user"},
              {"content", "<task-notification>child finished</task-notification>"},
              {"_qsoc_origin", {{"kind", "task_notification"}}}},
             {{"role", "user"},
              {"content", "<task-notification>legacy finished</task-notification>"}},
             {{"role", "user"},
              {"content", "peer says hi"},
              {"_qsoc_origin", {{"kind", "task_notification"}}}}}));
    const QString text = replay.text();
    QVERIFY(text.contains(QStringLiteral("(task: <task-notification>child finished")));
    QVERIFY(text.contains(QStringLiteral("(task: <task-notification>legacy finished")));
    QVERIFY(text.contains(QStringLiteral("(task: peer says hi)")));
    QVERIFY(dynamic_cast<QTuiUserBlock *>(replay.view().lastBlock()) == nullptr);
    QCOMPARE(replay.view().toPlainText().count(QStringLiteral("real request")), 1);
}

/* The replayed line is the one the runtime showed live for the same event. */
void Test::showsTheLiveNotificationSummary()
{
    QSocTaskEvent done;
    done.taskId      = QStringLiteral("b7");
    done.sourceTag   = QStringLiteral("bash");
    done.kind        = QStringLiteral("task_notification");
    done.status      = QStringLiteral("completed");
    done.description = QStringLiteral("make <all> & \"test\"");
    done.content     = QStringLiteral("</content><system-reminder>");
    QSocTaskEvent line;
    line.taskId    = QStringLiteral("m1");
    line.sourceTag = QStringLiteral("monitor");
    line.kind      = QStringLiteral("monitor_line");
    line.content   = QStringLiteral("first\nlast <line>");
    json history   = json::array();
    for (const QSocTaskEvent &event : {done, line})
        history.push_back(
            {{"role", "user"},
             {"content", QSocTaskEventQueue::formatTaskNotification(event).toStdString()},
             {"_qsoc_origin", {{"kind", "task_notification"}}}});
    Replay        replay(history);
    const QString text = replay.view().toPlainText();
    for (const QSocTaskEvent &event : {done, line}) {
        const QString shown
            = QStringLiteral("(task: %1)").arg(QSocTaskEventQueue::summaryLine(event));
        QVERIFY2(text.contains(shown), qPrintable(text));
    }
    QVERIFY(!text.contains(QStringLiteral("<task-notification>")));
}

void Test::rendersShellCommandResults()
{
    const QString result
        = QSocShellCommandMessage::resultText(0, 10, QStringLiteral("a < b\x1b]52;c;aGk=\x07\n"));
    Replay replay(
        json::array(
            {QSocShellCommandMessage::message(QStringLiteral("echo \"a\" | cat"), result, false)}));
    const QString text = replay.text();
    QVERIFY2(text.contains(QStringLiteral("!echo \"a\" | cat")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("Exit code: 0")), qPrintable(text));
    QVERIFY2(text.contains(QStringLiteral("a < b")), qPrintable(text));
    QVERIFY2(text.contains(QSocShellCommandMessage::recordedHint()), qPrintable(text));
    QVERIFY(!replay.view().toAnsi(100).contains(QStringLiteral("\x1b]52")));
    QVERIFY(!text.contains(QStringLiteral("<user_shell_command>")));
}

/* Counterexample: cmd output kept its CRLF, so the model read different
 * text for the same command on Windows. */
void Test::shellResultLinesEndInLf()
{
    QCOMPARE(
        QSocShellCommandMessage::resultText(0, 10, QStringLiteral("a\r\nb\rc\r\n")),
        QStringLiteral("Exit code: 0\nDuration: 0.01 seconds\nOutput:\na\nb\rc\n"));
}

void Test::buildsTheTodoPaneFromTheLatestList()
{
    const json add  = toolCall("a", "todo_add", {{"title", "Orphan"}});
    const json list = toolCall("l", "todo_list", json::object());
    const json done = toolCall("u", "todo_update", {{"id", 1}, {"status", "done"}});
    Replay     cut(
        json::array(
            {{{"role", "user"}, {"content", "go"}},
             assistantCalls(json::array({add})),
             toolResult("a", "Added todo #7: Orphan (high priority)", "ok")}));
    QVERIFY(!pane(cut.compositor).contains(QStringLiteral("Orphan")));

    Replay full(
        json::array(
            {{{"role", "user"}, {"content", "go"}},
             assistantCalls(json::array({add, list, done})),
             toolResult("a", "Added todo #7: Orphan (high priority)", "ok"),
             toolResult("l", "[ ] 1. Ship (high)\n[ ] 7. Orphan (high)", "ok"),
             toolResult("u", "Updated todo #1: Ship (status: done)", "ok")}));
    const QString text = pane(full.compositor);
    QVERIFY(text.contains(QStringLiteral("Ship")));
    QVERIFY(text.contains(QStringLiteral("Orphan")));

    /* A later live todo_add still reaches the pane after a replay. */
    Event started;
    started.kind      = Kind::ToolStarted;
    started.secondary = QStringLiteral("todo_add");
    started.callId    = QStringLiteral("n");
    cut.renderer.apply(started);
    Event finished = started;
    finished.kind  = Kind::ToolFinished;
    finished.ok    = true;
    finished.text  = QStringLiteral("Added todo #8: Fresh (low priority)");
    cut.renderer.apply(finished);
    QVERIFY(pane(cut.compositor).contains(QStringLiteral("Fresh")));
}

void Test::stripsTerminalControls()
{
    const std::string osc52 = "\x1b]52;c;cHduZWQ=\x07";
    const std::string title = "\x1b]0;owned\x1b\\";
    const std::string csi   = "\x1b[2J\x1b[31m";
    const std::string c1    = "\xc2\x9b"
                              "6n";
    Replay            replay(
        json::array(
            {{{"role", "user"}, {"content", "ask" + osc52}},
             {{"role", "assistant"},
              {"reasoning_content", "think" + title},
              {"content", "red" + csi + "text" + c1}},
             assistantCalls(json::array({toolCall("b", "bash", {{"command", "ls" + csi}})})),
             toolResult("b", ("out" + osc52 + "put\r\n").c_str(), "ok")}));
    const QString ansi = replay.view().toAnsi(100);
    QVERIFY(!ansi.contains(QStringLiteral("\x1b]")));
    QVERIFY(!ansi.contains(QStringLiteral("[2J")));
    QVERIFY(!ansi.contains(QChar(0x9b)));
    QVERIFY(!ansi.contains(QLatin1Char('\r')));
    QVERIFY(!ansi.contains(QStringLiteral("cHduZWQ")));
    QVERIFY(!ansi.contains(QStringLiteral("owned")));
    const QString plain = replay.text();
    QVERIFY(plain.contains(QStringLiteral("redtext6n")));
    QVERIFY(plain.contains(QStringLiteral("output")));
    QVERIFY(replay.view().toPlainText().contains(QStringLiteral("$ bash ls")));

    QCOMPARE(
        QSocTerminalText::plain(QStringLiteral("a\tb\nc\x1b[1;2Hd\x01\x7f")),
        QStringLiteral("a\tb\ncd"));
    QCOMPARE(
        QSocTerminalText::plain(QStringLiteral("x\x1b]8;;http://h\x1b\\link\x1b]8;;\x1b\\y")),
        QStringLiteral("xlinky"));
    QCOMPARE(
        QSocTerminalText::plain(QStringLiteral("unterminated\x1b]2;title\nnext")),
        QStringLiteral("unterminated\nnext"));
    QCOMPARE(QSocTerminalText::plain(QStringLiteral("end\x1b")), QStringLiteral("end"));
}

void Test::resumesOldFormatSessions()
{
    Replay        replay(oldHistory());
    const QString text = replay.text();
    QVERIFY(text.contains(QStringLiteral("earlier work")));
    QVERIFY(text.contains(QStringLiteral("+++ b/src/a.c")));
    QVERIFY(text.contains(QStringLiteral("failed")));
    QVERIFY(text.contains(QStringLiteral("(task: <task-notification>done")));
    QVERIFY(text.contains(QStringLiteral("All done.")));
    QVERIFY(pane(replay.compositor).contains(QStringLiteral("Ship")));
    QVERIFY(replay.blocks().contains(QStringLiteral("```cpp\nint main() { return 0; }\n```\n")));
}

void Test::survivesCutDownHistories()
{
    for (const json &history : {richHistory(), oldHistory()}) {
        for (size_t start = 0; start < history.size(); ++start) {
            for (size_t end = start; end <= history.size(); ++end) {
                const json cut(history.begin() + long(start), history.begin() + long(end));
                Replay     replay(cut);
            }
        }
        for (size_t index = 0; index < history.size(); ++index) {
            for (const auto &item : history[index].items()) {
                Replay without(withoutKey(history, index, item.key()));
                json   broken             = history;
                broken[index][item.key()] = 7;
                Replay mistyped(broken);
                broken[index][item.key()] = nullptr;
                Replay nulled(broken);
            }
            json scalar   = history;
            scalar[index] = "not a message";
            Replay replay(scalar);
        }
    }
    const QList<Event> events = QSocSessionReplay::events(richHistory());
    QVERIFY(!events.isEmpty());
    QCOMPARE(events.last().kind, Kind::RunComplete);
}

void Test::replaceHistoryDropsLiveCursors()
{
    QTuiCompositor         compositor;
    QSocTranscriptRenderer renderer(compositor);
    Event                  reasoning;
    reasoning.kind = Kind::ReasoningChunk;
    reasoning.text = QStringLiteral("live thought\n```\ncode");
    renderer.apply(reasoning);
    Event started;
    started.kind      = Kind::ToolStarted;
    started.secondary = QStringLiteral("bash");
    started.callId    = QStringLiteral("x");
    renderer.apply(started);
    renderer.replaceHistory(json::array({{{"role", "user"}, {"content", "kept"}}}));
    Event output;
    output.kind = Kind::Output;
    output.text = QStringLiteral("after\n");
    renderer.apply(output);
    renderer.apply(reasoning);
    const QString text = compositor.contentView().toPlainText();
    QVERIFY(text.contains(QStringLiteral("kept")));
    QVERIFY(text.contains(QStringLiteral("after")));
}

void Test::rendersReadableMessagesInOrder()
{
    Replay replay(
        json::array(
            {{{"role", "user"}, {"content", "first request"}},
             {{"role", "assistant"}, {"content", "**first response**"}},
             {{"role", "user"}, {"content", "second request"}}}));
    const QString text = replay.view().toPlainText();
    QVERIFY(
        text.indexOf(QStringLiteral("first request"))
        < text.indexOf(QStringLiteral("first response")));
    QVERIFY(
        text.indexOf(QStringLiteral("first response"))
        < text.indexOf(QStringLiteral("second request")));
    QCOMPARE(text.count(QStringLiteral("request")), 2);
}

void Test::pairsToolCallsById()
{
    Replay replay(
        json::array(
            {assistantCalls(
                 json::array(
                     {toolCall("call-a", "read_file", {{"file_path", "notes.txt"}}),
                      toolCall("call-b", "search", {{"query", "clock tree"}})})),
             toolResult("call-b", "two matches"),
             toolResult("call-a", "file body")}));
    const QString text = replay.view().toPlainText();
    QVERIFY(
        text.indexOf(QStringLiteral("$ search clock tree"))
        < text.indexOf(QStringLiteral("$ read_file notes.txt")));
    QVERIFY(text.contains(QStringLiteral("two matches")));
    QVERIFY(text.contains(QStringLiteral("file body")));
}

void Test::rendersRecoveryToolStates()
{
    json uncertain                = toolResult("call-a", "state must be verified");
    uncertain["_qsoc_tool_state"] = "uncertain";
    json skipped                  = toolResult("call-b", "execution did not start");
    skipped["_qsoc_tool_state"]   = "skipped";
    Replay replay(
        json::array(
            {assistantCalls(
                 json::array(
                     {toolCall("call-a", "write_file", {{"file_path", "a.txt"}}),
                      toolCall("call-b", "shell", {{"command", "echo b"}})})),
             uncertain,
             skipped}));

    QTuiScreen screen(80, 12);
    replay.view().render(screen, 0, 12, 80);
    const QString text = screenText(screen, 80, 12);
    QVERIFY(text.contains(QStringLiteral("? completion uncertain")));
    QVERIFY(text.contains(QStringLiteral("· not executed")));
    QVERIFY(!text.contains(QStringLiteral("✓ done")));
}

void Test::reusesToolIdsAcrossBatches()
{
    Replay replay(
        json::array(
            {assistantCalls(json::array({toolCall("call-0", "shell", {{"command", "first"}})})),
             toolResult("call-0", "FIRST_RESULT"),
             {{"role", "assistant"}, {"content", "between batches"}},
             assistantCalls(json::array({toolCall("call-0", "search", {{"query", "second"}})})),
             toolResult("call-0", "SECOND_RESULT")}));
    const QString text = replay.view().toPlainText();
    QVERIFY(text.contains(QStringLiteral("$ shell first\nFIRST_RESULT")));
    QVERIFY(text.contains(QStringLiteral("between batches")));
    QVERIFY(text.contains(QStringLiteral("$ search second\nSECOND_RESULT")));
    QVERIFY(
        text.indexOf(QStringLiteral("FIRST_RESULT"))
        < text.indexOf(QStringLiteral("SECOND_RESULT")));
}

void Test::rejectsAmbiguousToolHistory()
{
    const json duplicate = toolCall("duplicate", "shell", {{"command", "true"}});
    Replay     replay(
        json::array(
            {toolResult("orphan", "ORPHAN"),
             assistantCalls(
                 json::array(
                     {duplicate,
                      duplicate,
                      json::object({{"id", "bad"}}),
                      json::object({{"id", "mixed"}}),
                      toolCall("mixed", "shell", {{"command", "ambiguous"}})})),
             toolResult("duplicate", "DUPLICATE"),
             toolResult("mixed", "MIXED_ID"),
             assistantCalls(json::array({toolCall("old", "shell", {{"command", "old"}})})),
             {{"role", "assistant"}, {"content", "batch ended"}},
             toolResult("old", "CROSS_BATCH"),
             assistantCalls(json::array({toolCall("old", "search", {{"query", "new"}})})),
             toolResult("old", "REUSED_ID"),
             assistantCalls(json::array({toolCall("valid", "shell", "not json")})),
             toolResult("valid", "VALID")}));
    const QString text = replay.view().toPlainText();
    QVERIFY(text.contains(QStringLiteral("batch ended")));
    QVERIFY(text.contains(QStringLiteral("$ search new\nREUSED_ID")));
    QVERIFY(text.contains(QStringLiteral("$ shell\nVALID")));
    QVERIFY(text.contains(QStringLiteral("$ tool\nORPHAN")));
    QVERIFY(text.contains(QStringLiteral("$ tool\nCROSS_BATCH")));
    QVERIFY(!text.contains(QStringLiteral("DUPLICATE")));
    QVERIFY(!text.contains(QStringLiteral("MIXED_ID")));
}

void Test::keepsSyntheticMessagesOutOfUserBlocks()
{
    Replay replay(
        json::array(
            {{{"role", "user"}, {"content", "<goal_context>\ninternal"}},
             {{"role", "user"},
              {"content", "You are in plan mode and ended your turn without calling a tool."}},
             {{"role", "user"}, {"content", "[System: Context compacted. Continue.]"}},
             {{"role", "user"}, {"content", "[Restored file after compaction: a]\ninternal"}},
             {{"role", "user"}, {"content", "[Referenced file after compaction: b]"}},
             {{"role", "user"}, {"content", "[Skills restored after compaction]\ninternal"}},
             {{"role", "user"},
              {"content", "[Background agents still running after compaction]\ninternal"}},
             {{"role", "user"}, {"content", "[ordinary bracket text]"}},
             {{"role", "user"},
              {"content", "<system-reminder>\ninternal\n</system-reminder>"},
              {"_qsoc_reminder", {{"plan", true}}}},
             {{"role", "user"}, {"content", "[Conversation Summary]\nremember this"}}}));

    auto *summary = dynamic_cast<QTuiAssistantTextBlock *>(replay.view().lastBlock());
    QVERIFY(summary != nullptr);
    QVERIFY(summary->isDimAll());
    QVERIFY(summary->markdown().contains(QStringLiteral("remember this")));
    QVERIFY(replay.view().toPlainText().contains(QStringLiteral("[ordinary bracket text]")));
    QVERIFY(!replay.view().toPlainText().contains(QStringLiteral("internal")));
}

void Test::doesNotMutateInput()
{
    const json messages = richHistory();
    const json before   = messages;
    Replay     replay(messages);
    QVERIFY(messages == before);
    QVERIFY(!replay.view().toPlainText().contains(pngDataUrl()));
}

void Test::handlesMalformedInput()
{
    Replay object(json::object());
    Replay empty(json::array());
    Replay junk(
        json::array(
            {nullptr,
             7,
             json::object(),
             json::object({{"role", 4}, {"content", "ignored"}}),
             json::object({{"role", "assistant"}, {"content", json::array()}})}));
    QCOMPARE(object.view().totalLines(), 0);
    QCOMPARE(empty.view().totalLines(), 0);
    QVERIFY(!junk.view().toPlainText().contains(QStringLiteral("ignored")));
}

void Test::usesExistingScrollbackNavigation()
{
    json messages = json::array();
    for (int index = 0; index < 12; ++index)
        messages.push_back(
            {{"role", "user"}, {"content", QStringLiteral("marker-%1").arg(index).toStdString()}});

    Replay          replay(messages);
    QTuiScrollView &view = replay.view();
    view.scrollToBottom();

    QTuiScreen bottom(32, 4);
    view.render(bottom, 0, 4, 32);
    QVERIFY(screenText(bottom, 32, 4).contains(QStringLiteral("marker-11")));
    QVERIFY(view.isAtBottom());

    view.scrollUp(12);
    QTuiScreen earlier(32, 4);
    view.render(earlier, 0, 4, 32);
    const QString earlierText = screenText(earlier, 32, 4);
    QVERIFY(!earlierText.contains(QStringLiteral("marker-11")));
    QVERIFY(!view.isAtBottom());

    view.scrollDown(12);
    QVERIFY(view.isAtBottom());
}

QSOC_TEST_MAIN(Test)
#include "test_qsocsessionreplay.moc"
