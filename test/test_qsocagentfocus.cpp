// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"

#include "agent/protocol/qsocagentruntimeevent.h"
#include "cli/qsocagentfocus.h"
#include "cli/qsocagenttaskmodel.h"
#include "cli/qsoctranscriptrenderer.h"
#include "tui/qtuicompositor.h"
#include "tui/qtuiscreen.h"
#include "tui/qtuitaskoverlay.h"

#include <nlohmann/json.hpp>

#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QStringList>
#include <QtTest>

using json = nlohmann::json;

namespace {

/* A daemon stand-in: answers the focus RPCs and records every method. */
struct FakeDaemon
{
    QStringList methods;
    QStringList ids;
    json        history  = json::array();
    int         pageSize = 1;
    QJsonObject sendResult{{"ok", true}, {"task_id", "a4"}, {"delivery", "woken"}};

    QJsonObject operator()(const QString &method, const QJsonObject &params)
    {
        methods.append(method);
        ids.append(params.value("id").toString());
        if (method == "task_send")
            return {{"result", sendResult}};
        if (method != "task_tail")
            return {{"result", QJsonObject{{"ok", true}}}};
        const int total = static_cast<int>(history.size());
        const int asked = params.value("offset").toInt();
        const int from  = asked <= total ? asked : 0;
        json      slice = json::array();
        for (int i = from; i < total && i < from + pageSize; ++i)
            slice.push_back(history.at(static_cast<size_t>(i)));
        const auto array = QJsonDocument::fromJson(QByteArray::fromStdString(slice.dump())).array();
        return {
            {"result",
             QJsonObject{
                 {"messages", array},
                 {"offset", from},
                 {"next_offset", from + static_cast<int>(slice.size())},
                 {"eof", from + static_cast<int>(slice.size()) >= total},
                 {"found", true}}}};
    }
};

json childHistory()
{
    return json::array(
        {{{"role", "user"}, {"content", "child objective"}},
         {{"role", "assistant"}, {"content", "child answer one"}},
         {{"role", "assistant"}, {"content", "child answer two"}}});
}

QString screenText(const QTuiScreen &screen)
{
    QString text;
    for (int row = 0; row < screen.height(); ++row) {
        for (int col = 0; col < screen.width(); ++col)
            text += screen.at(col, row).text();
        text += QLatin1Char('\n');
    }
    return text;
}

QString transcript(QTuiCompositor &compositor)
{
    return compositor.contentView().toPlainText();
}

QSocAgentFocus::Request forward(FakeDaemon &daemon)
{
    return [&daemon](const QString &method, const QJsonObject &params) {
        return daemon(method, params);
    };
}

QSocTaskRegistry::TaggedRow agentRow(
    const QString &id, const QString &agent, bool live, qint64 startedAt)
{
    QSocTaskRegistry::TaggedRow item;
    item.sourceTag       = QStringLiteral("agent");
    item.row.id          = id;
    item.row.agentId     = agent;
    item.row.kind        = QSocTask::Kind::SubAgent;
    item.row.status      = QSocTask::Status::Completed;
    item.row.live        = live;
    item.row.resumable   = live;
    item.row.startedAtMs = startedAt;
    return item;
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void featuresNeedTheAgentsCapability()
    {
        QVERIFY(!QSocAgentFocus::supports(QJsonObject{{"capabilities", QJsonArray{"smt"}}}));
        QVERIFY(!QSocAgentFocus::supports(QJsonObject{{"daemon", "qsoc-agentd"}}));
        QVERIFY(QSocAgentFocus::supports(QJsonObject{{"capabilities", QJsonArray{"agents"}}}));

        QTuiCompositor         compositor;
        QSocTranscriptRenderer renderer(compositor);
        FakeDaemon             daemon;
        QSocAgentFocus         focus(compositor, renderer, forward(daemon), false);
        focus.enter(QStringLiteral("a1"));
        QVERIFY(!focus.active());
        QVERIFY(daemon.methods.isEmpty());
    }

    void overlayOffersFocusOnlyWhenEnabled()
    {
        QSocAgentTaskModel model([](const QString &, const QJsonObject &) {
            const QJsonObject
                row{{"source", "agent"},
                    {"id", "a1"},
                    {"label", "probe"},
                    {"kind", static_cast<int>(QSocTask::Kind::SubAgent)},
                    {"status", static_cast<int>(QSocTask::Status::Completed)},
                    {"live", true},
                    {"resumable", true}};
            return QJsonObject{{"result", QJsonObject{{"rows", QJsonArray{row}}}}};
        });
        model.refresh();
        QTuiTaskOverlay overlay;
        overlay.setRegistry(&model);
        QSignalSpy focusSpy(&overlay, &QTuiTaskOverlay::focusRequested);
        QSignalSpy sendSpy(&overlay, &QTuiTaskOverlay::messageRequested);

        overlay.open();
        QVERIFY(overlay.handleKey(Qt::Key_F, false));
        QVERIFY(overlay.handleKey(Qt::Key_S, false));
        QCOMPARE(focusSpy.size(), 0);
        QCOMPARE(sendSpy.size(), 0);
        QCOMPARE(overlay.mode(), QTuiTaskOverlay::Mode::List);
        QTuiScreen hidden(120, 30);
        overlay.render(hidden, 0, 120);
        QVERIFY(!screenText(hidden).contains(QStringLiteral("f focus")));

        overlay.setAgentActionsEnabled(true);
        QTuiScreen shown(120, 30);
        overlay.render(shown, 0, 120);
        QVERIFY(screenText(shown).contains(QStringLiteral("f focus")));
        QVERIFY(overlay.handleKey(Qt::Key_F, false));
        QCOMPARE(focusSpy.size(), 1);
        QCOMPARE(focusSpy.first().first().toString(), QStringLiteral("a1"));
        QCOMPARE(overlay.mode(), QTuiTaskOverlay::Mode::Hidden);
        overlay.open();
        QVERIFY(overlay.handleKey(Qt::Key_S, false));
        QCOMPARE(sendSpy.size(), 1);
        overlay.setRegistry(nullptr);
    }

    void focusShowsTheChildAndEscapeReturnsWithoutStopping()
    {
        QTuiCompositor         compositor;
        QSocTranscriptRenderer renderer(compositor);
        FakeDaemon             daemon;
        daemon.history = childHistory();
        QSocAgentFocus focus(compositor, renderer, forward(daemon), true);
        compositor.printContent(QStringLiteral("main line\n"));

        focus.enter(QStringLiteral("a1"));
        QVERIFY(focus.active());
        QCOMPARE(focus.messages().size(), size_t{3});
        QVERIFY(transcript(compositor).contains(QStringLiteral("child answer two")));
        QVERIFY(!transcript(compositor).contains(QStringLiteral("main line")));

        QSocAgentRuntimeEvent late;
        late.kind = QSocAgentRuntimeEvent::Kind::Output;
        late.text = QStringLiteral("main event while away\n");
        focus.route(late);
        QVERIFY(!transcript(compositor).contains(QStringLiteral("main event while away")));

        QVERIFY(focus.escape());
        QVERIFY(!focus.active());
        QVERIFY(!daemon.methods.contains(QStringLiteral("task_kill")));
        QVERIFY(!daemon.methods.contains(QStringLiteral("abort")));
        QVERIFY(transcript(compositor).contains(QStringLiteral("main line")));
        QVERIFY(transcript(compositor).contains(QStringLiteral("main event while away")));
        QVERIFY(!transcript(compositor).contains(QStringLiteral("child answer")));
        QVERIFY(!focus.escape());
    }

    void interruptStopsOnlyTheChildInView()
    {
        QTuiCompositor         compositor;
        QSocTranscriptRenderer renderer(compositor);
        FakeDaemon             daemon;
        QSocAgentFocus         focus(compositor, renderer, forward(daemon), true);
        QVERIFY(!focus.interrupt(false));
        focus.enter(QStringLiteral("a2"));
        daemon.methods.clear();
        QVERIFY(focus.interrupt(true));
        QVERIFY(daemon.methods.isEmpty());
        QVERIFY(focus.interrupt(false));
        QCOMPARE(daemon.methods, QStringList{QStringLiteral("task_kill")});
        QCOMPARE(daemon.ids.last(), QStringLiteral("a2"));
        QVERIFY(focus.active());
    }

    void sendFollowsTheRunThatReadsIt()
    {
        QTuiCompositor         compositor;
        QSocTranscriptRenderer renderer(compositor);
        FakeDaemon             daemon;
        QSocAgentFocus         focus(compositor, renderer, forward(daemon), true);
        focus.enter(QStringLiteral("a1"));
        QCOMPARE(focus.send(QStringLiteral("next step")), QStringLiteral("Sent to a4"));
        QCOMPARE(focus.target(), QStringLiteral("a4"));
        daemon.sendResult = {{"ok", false}, {"status", "error"}, {"error", "not_live"}};
        QCOMPARE(focus.send(QStringLiteral("again")), QStringLiteral("Not sent: not_live"));
        QCOMPARE(focus.target(), QStringLiteral("a4"));
    }

    void historyPagesUntilEndAndRefreshFollowsGrowth()
    {
        QTuiCompositor         compositor;
        QSocTranscriptRenderer renderer(compositor);
        FakeDaemon             daemon;
        daemon.history = childHistory();
        QSocAgentFocus focus(compositor, renderer, forward(daemon), true);
        focus.enter(QStringLiteral("a1"));
        QCOMPARE(daemon.methods.count(QStringLiteral("task_tail")), 3);
        QVERIFY(!focus.refresh());
        daemon.history.push_back({{"role", "assistant"}, {"content", "child answer three"}});
        QVERIFY(focus.refresh());
        QVERIFY(transcript(compositor).contains(QStringLiteral("child answer three")));
        daemon.history = json::array({{{"role", "user"}, {"content", "compacted"}}});
        QVERIFY(focus.refresh());
        QCOMPARE(focus.messages().size(), size_t{1});
    }

    void cycleWalksMainAndLiveChildren()
    {
        /* Newest first, as the task registry may list them. */
        const QList<QSocTaskRegistry::TaggedRow> rows
            = {agentRow("a5", "z", true, 50),
               agentRow("a3", "x", true, 30),
               agentRow("a2", "y", false, 20),
               agentRow("a1", "x", true, 10)};
        QCOMPARE(QSocAgentFocus::cycle({}, rows, 1), QStringLiteral("a3"));
        QCOMPARE(QSocAgentFocus::cycle(QStringLiteral("a1"), rows, 1), QStringLiteral("a5"));
        QCOMPARE(QSocAgentFocus::cycle(QStringLiteral("a5"), rows, 1), QString());
        QCOMPARE(QSocAgentFocus::cycle({}, rows, -1), QStringLiteral("a5"));
        QCOMPARE(QSocAgentFocus::cycle(QStringLiteral("a3"), rows, -1), QString());
        QCOMPARE(QSocAgentFocus::cycle(QStringLiteral("a2"), rows, 1), QStringLiteral("a3"));
        QCOMPARE(QSocAgentFocus::cycle({}, {}, 1), QString());
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocagentfocus.moc"
