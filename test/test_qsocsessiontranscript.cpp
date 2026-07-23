// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"

#include "cli/qsocsessiontranscript.h"
#include "tui/qtuiassistanttextblock.h"
#include "tui/qtuiscreen.h"
#include "tui/qtuiscrollview.h"
#include "tui/qtuitoolblock.h"
#include "tui/qtuiuserblock.h"

#include <nlohmann/json.hpp>

#include <QBuffer>
#include <QImage>
#include <QtTest>

using json = nlohmann::json;

namespace {

json toolCall(const char *id, const char *name, const char *arguments)
{
    return {
        {"id", id},
        {"type", "function"},
        {"function", {{"name", name}, {"arguments", arguments}}},
    };
}

QString screenText(const QTuiScreen &screen, int width, int height)
{
    QString text;
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < width; ++col) {
            text += screen.at(col, row).character;
        }
        text += QLatin1Char('\n');
    }
    return text;
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void rendersReadableMessagesInOrder();
    void pairsToolCallsById();
    void rendersRecoveryToolStates();
    void reusesToolIdsAcrossBatches();
    void rejectsMalformedToolHistory();
    void keepsSyntheticMessagesOutOfUserBlocks();
    void skipsAttachmentsWithoutMutatingInput();
    void handlesMalformedInput();
    void usesExistingScrollbackNavigation();
};

void Test::rendersReadableMessagesInOrder()
{
    const json messages = json::array(
        {{{"role", "user"}, {"content", "first request"}},
         {{"role", "assistant"}, {"content", "**first response**"}},
         {{"role", "user"}, {"content", "second request"}}});

    QTuiScrollView view;
    QSocSessionTranscript::appendTo(messages, view);

    QCOMPARE(view.totalLines(), 3);
    QVERIFY(dynamic_cast<QTuiUserBlock *>(view.lastBlock()) != nullptr);
    const QString text = view.toPlainText();
    QVERIFY(
        text.indexOf(QStringLiteral("first request"))
        < text.indexOf(QStringLiteral("**first response**")));
    QVERIFY(
        text.indexOf(QStringLiteral("**first response**"))
        < text.indexOf(QStringLiteral("second request")));
}

void Test::pairsToolCallsById()
{
    const json messages = json::array(
        {{{"role", "assistant"},
          {"content", nullptr},
          {"tool_calls",
           json::array(
               {toolCall("call-a", "read_file", R"({"file_path":"notes.txt"})"),
                toolCall("call-b", "search", R"({"query":"clock tree"})")})}},
         {{"role", "tool"}, {"tool_call_id", "call-b"}, {"content", "two matches"}},
         {{"role", "tool"}, {"tool_call_id", "call-a"}, {"content", "file body"}}});

    QTuiScrollView view;
    QSocSessionTranscript::appendTo(messages, view);

    QCOMPARE(view.totalLines(), 2);
    auto *last = dynamic_cast<QTuiToolBlock *>(view.lastBlock());
    QVERIFY(last != nullptr);
    QVERIFY(!last->isFolded());
    const QString text = view.toPlainText();
    QVERIFY(
        text.indexOf(QStringLiteral("$ search clock tree"))
        < text.indexOf(QStringLiteral("$ read_file notes.txt")));
    QVERIFY(text.contains(QStringLiteral("two matches")));
    QVERIFY(text.contains(QStringLiteral("file body")));
}

void Test::rendersRecoveryToolStates()
{
    const json messages = json::array(
        {{{"role", "assistant"},
          {"tool_calls",
           json::array(
               {toolCall("call-a", "write_file", R"({"file_path":"a.txt"})"),
                toolCall("call-b", "shell", R"({"command":"echo b"})")})}},
         {{"role", "tool"},
          {"tool_call_id", "call-a"},
          {"content", "state must be verified"},
          {"_qsoc_tool_state", "uncertain"}},
         {{"role", "tool"},
          {"tool_call_id", "call-b"},
          {"content", "execution did not start"},
          {"_qsoc_tool_state", "skipped"}}});

    QTuiScrollView view;
    QSocSessionTranscript::appendTo(messages, view);

    QTuiScreen screen(80, 12);
    view.render(screen, 0, 12, 80);
    const QString text = screenText(screen, 80, 12);
    QVERIFY(text.contains(QStringLiteral("? completion uncertain")));
    QVERIFY(text.contains(QStringLiteral("· not executed")));
    QVERIFY(!text.contains(QStringLiteral("✓ done")));
}

void Test::reusesToolIdsAcrossBatches()
{
    const json messages = json::array(
        {{{"role", "assistant"},
          {"tool_calls", json::array({toolCall("call-0", "shell", R"({"command":"first"})")})}},
         {{"role", "tool"}, {"tool_call_id", "call-0"}, {"content", "FIRST_RESULT"}},
         {{"role", "assistant"}, {"content", "between batches"}},
         {{"role", "assistant"},
          {"tool_calls", json::array({toolCall("call-0", "search", R"({"query":"second"})")})}},
         {{"role", "tool"}, {"tool_call_id", "call-0"}, {"content", "SECOND_RESULT"}}});

    QTuiScrollView view;
    QSocSessionTranscript::appendTo(messages, view);

    QCOMPARE(view.totalLines(), 3);
    const QString text = view.toPlainText();
    QVERIFY(text.contains(QStringLiteral("$ shell first\nFIRST_RESULT")));
    QVERIFY(text.contains(QStringLiteral("between batches")));
    QVERIFY(text.contains(QStringLiteral("$ search second\nSECOND_RESULT")));
    QVERIFY(
        text.indexOf(QStringLiteral("FIRST_RESULT"))
        < text.indexOf(QStringLiteral("SECOND_RESULT")));
}

void Test::rejectsMalformedToolHistory()
{
    const json duplicate = toolCall("duplicate", "shell", R"({"command":"true"})");
    const json messages  = json::array(
        {{{"role", "tool"}, {"tool_call_id", "orphan"}, {"content", "ORPHAN"}},
         {{"role", "assistant"},
          {"tool_calls",
           json::array(
               {duplicate,
                duplicate,
                json::object({{"id", "bad"}}),
                json::object({{"id", "mixed"}}),
                toolCall("mixed", "shell", R"({"command":"ambiguous"})")})}},
         {{"role", "tool"}, {"tool_call_id", "duplicate"}, {"content", "DUPLICATE"}},
         {{"role", "tool"}, {"tool_call_id", "mixed"}, {"content", "MIXED_ID"}},
         {{"role", "assistant"},
          {"tool_calls", json::array({toolCall("old", "shell", R"({"command":"old"})")})}},
         {{"role", "assistant"}, {"content", "batch ended"}},
         {{"role", "tool"}, {"tool_call_id", "old"}, {"content", "CROSS_BATCH"}},
         {{"role", "assistant"},
          {"tool_calls", json::array({toolCall("old", "search", R"({"query":"new"})")})}},
         {{"role", "tool"}, {"tool_call_id", "old"}, {"content", "REUSED_ID"}},
         {{"role", "assistant"},
          {"tool_calls", json::array({toolCall("valid", "shell", "not json")})}},
         {{"role", "tool"}, {"tool_call_id", "valid"}, {"content", "VALID"}},
         {{"role", "tool"}, {"tool_call_id", "valid"}, {"content", "REPEATED"}}});

    QTuiScrollView view;
    QSocSessionTranscript::appendTo(messages, view);

    QCOMPARE(view.totalLines(), 3);
    const QString text = view.toPlainText();
    QVERIFY(text.contains(QStringLiteral("batch ended")));
    QVERIFY(text.contains(QStringLiteral("$ search new\nREUSED_ID")));
    QVERIFY(text.contains(QStringLiteral("$ shell\nVALID")));
    QVERIFY(!text.contains(QStringLiteral("ORPHAN")));
    QVERIFY(!text.contains(QStringLiteral("DUPLICATE")));
    QVERIFY(!text.contains(QStringLiteral("MIXED_ID")));
    QVERIFY(!text.contains(QStringLiteral("CROSS_BATCH")));
    QVERIFY(!text.contains(QStringLiteral("REPEATED")));
}

void Test::keepsSyntheticMessagesOutOfUserBlocks()
{
    const json messages = json::array(
        {{{"role", "user"}, {"content", "<task-notification>\ninternal"}},
         {{"role", "user"}, {"content", "<goal_context>\ninternal"}},
         {{"role", "user"},
          {"content", "You are in plan mode and ended your turn without calling a tool."}},
         {{"role", "user"}, {"content", "[System: Context compacted. Continue.]"}},
         {{"role", "user"}, {"content", "[Restored file after compaction: a]\ninternal"}},
         {{"role", "user"}, {"content", "[Referenced file after compaction: b]"}},
         {{"role", "user"}, {"content", "[Skills restored after compaction]\ninternal"}},
         {{"role", "user"},
          {"content", "[Background agents still running after compaction]\ninternal"}},
         {{"role", "user"}, {"content", "[ordinary bracket text]"}},
         {{"role", "user"}, {"content", "[Conversation Summary]\nremember this"}}});

    QTuiScrollView view;
    QSocSessionTranscript::appendTo(messages, view);

    QCOMPARE(view.totalLines(), 2);
    auto *summary = dynamic_cast<QTuiAssistantTextBlock *>(view.lastBlock());
    QVERIFY(summary != nullptr);
    QVERIFY(summary->isDimAll());
    QVERIFY(summary->markdown().contains(QStringLiteral("remember this")));
    QVERIFY(dynamic_cast<QTuiUserBlock *>(view.lastBlock()) == nullptr);
    QVERIFY(view.toPlainText().contains(QStringLiteral("[ordinary bracket text]")));
    QVERIFY(!view.toPlainText().contains(QStringLiteral("internal")));
}

void Test::skipsAttachmentsWithoutMutatingInput()
{
    QImage image(2, 2, QImage::Format_RGB32);
    image.fill(Qt::red);
    QByteArray bytes;
    QBuffer    buffer(&bytes);
    QVERIFY(buffer.open(QIODevice::WriteOnly));
    QVERIFY(image.save(&buffer, "PNG"));
    const QString dataUrl = QStringLiteral("data:image/png;base64,")
                            + QString::fromLatin1(bytes.toBase64());

    const json messages = json::array(
        {{{"role", "user"},
          {"content",
           json::array(
               {{{"type", "text"}, {"text", "attachment"}},
                {{"type", "image_url"}, {"image_url", {{"url", dataUrl.toStdString()}}}}})}},
         {{"role", "user"}, {"content", "visible"}}});
    const json before = messages;

    QTuiScrollView view;
    QSocSessionTranscript::appendTo(messages, view);

    QVERIFY(messages == before);
    QCOMPARE(view.totalLines(), 1);
    QVERIFY(view.toPlainText().contains(QStringLiteral("visible")));
    QVERIFY(!view.toPlainText().contains(dataUrl));
}

void Test::handlesMalformedInput()
{
    QTuiScrollView view;
    QSocSessionTranscript::appendTo(json::object(), view);
    QSocSessionTranscript::appendTo(json::array(), view);
    QSocSessionTranscript::appendTo(
        json::array(
            {nullptr,
             7,
             json::object(),
             json::object({{"role", 4}, {"content", "ignored"}}),
             json::object({{"role", "assistant"}, {"content", json::array()}})}),
        view);
    QCOMPARE(view.totalLines(), 0);
}

void Test::usesExistingScrollbackNavigation()
{
    json messages = json::array();
    for (int index = 0; index < 12; ++index) {
        messages.push_back(
            {{"role", "user"}, {"content", QStringLiteral("marker-%1").arg(index).toStdString()}});
    }

    QTuiScrollView view;
    QSocSessionTranscript::appendTo(messages, view);
    view.scrollToBottom();

    QTuiScreen bottom(32, 4);
    view.render(bottom, 0, 4, 32);
    QVERIFY(screenText(bottom, 32, 4).contains(QStringLiteral("marker-11")));
    QVERIFY(view.isAtBottom());

    view.scrollUp(6);
    QTuiScreen earlier(32, 4);
    view.render(earlier, 0, 4, 32);
    const QString earlierText = screenText(earlier, 32, 4);
    QVERIFY(earlierText.contains(QStringLiteral("marker-2")));
    QVERIFY(!earlierText.contains(QStringLiteral("marker-11")));
    QVERIFY(!view.isAtBottom());

    view.scrollDown(6);
    QVERIFY(view.isAtBottom());
}

QSOC_TEST_MAIN(Test)
#include "test_qsocsessiontranscript.moc"
