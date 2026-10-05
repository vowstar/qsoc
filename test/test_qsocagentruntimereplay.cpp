// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/protocol/qsocagentruntimeevent.h"
#include "agent/runtime/qsocagentruntime.h"
#include "cli/qsocsessionreplay.h"
#include "cli/qsoctranscriptrenderer.h"
#include "qsoc_test.h"
#include "qsoc_test_pty.h"
#include "tui/qtuicompositor.h"
#include "tui/qtuiscreen.h"

#include <nlohmann/json.hpp>

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using json  = nlohmann::json;
using Event = QSocAgentRuntimeEvent;
using Kind  = QSocAgentRuntimeEvent::Kind;

namespace {

using namespace QSocTestPty;

/* The runtime reads its config root once, so isolate it before QCoreApplication. */
struct EnvBootstrap
{
    EnvBootstrap()
        : root(QDir(QDir::tempPath())
                   .filePath(
                       QStringLiteral("test_qsoc_runtime_replay_")
                       + QString::number(QCoreApplication::applicationPid())))
    {
        QDir().mkpath(root + QStringLiteral("/config/qsoc"));
        qputenv("QSOC_HOME", (root + QStringLiteral("/config/qsoc")).toUtf8());
        qputenv("XDG_CONFIG_HOME", (root + QStringLiteral("/config")).toUtf8());
        qputenv("HOME", root.toUtf8());
    }
    QString root;
};

const EnvBootstrap g_env;

QJsonObject step(
    const QString     &reasoning,
    const QString     &content,
    const QString     &tool = {},
    const QJsonObject &args = {})
{
    QJsonObject reply{{"content", content}};
    if (!reasoning.isEmpty())
        reply.insert("reasoning", reasoning);
    if (!tool.isEmpty())
        reply.insert("tools", QJsonArray{QJsonObject{{"name", tool}, {"arguments", args}}});
    return reply;
}

QByteArray script(const QString &file)
{
    const QString answer = QStringLiteral(
        "Done:\n\n```c\nlong x;\nint y;\n```\n\n| name | value |\n|---|---|\n| x | 1 |\n");
    const QJsonArray steps{
        step(
            "Plan the write.",
            "Writing the file.",
            "write_file",
            {{"file_path", file}, {"content", "int x;\nint y;\n"}}),
        step(
            "Now the edit.",
            {},
            "edit_file",
            {{"file_path", file}, {"old_string", "int x;"}, {"new_string", "long x;"}}),
        step({}, "Checking the build.", "bash", {{"command", "echo building; exit 3"}}),
        step({}, {}, "todo_add", {{"title", "Write tests"}, {"priority", "high"}}),
        step({}, {}, "todo_list", {}),
        step("Summarize the result.", answer),
        step({}, "Second answer with `inline` code."),
    };
    const QJsonArray routes{QJsonObject{{"contains", "REPLAYCHECK"}, {"responses", steps}}};
    return QJsonDocument(routes).toJson();
}

QByteArray config(int port)
{
    return QStringLiteral(
               "llm:\n"
               "  model: mock\n"
               "  models:\n"
               "    mock:\n"
               "      name: Mock\n"
               "      url: \"http://127.0.0.1:%1/v1/chat/completions\"\n"
               "      key: placeholder\n"
               "      timeout: 20000\n"
               "      context: 131072\n"
               "      max_output_tokens: 4096\n"
               "agent:\n"
               "  predict_input: false\n"
               "  memory_recall: false\n"
               "  memory_extract: false\n"
               "  memory_dream: false\n"
               "  session_title: false\n"
               "  away_summary: false\n"
               "proxy:\n"
               "  type: none\n")
        .arg(port)
        .toUtf8();
}

QString todoPane(QTuiCompositor &compositor)
{
    QTuiWidget &widget = compositor.todoList();
    const int   height = qMax(1, widget.lineCount());
    QTuiScreen  screen(100, height);
    widget.render(screen, 0, 100);
    QString text;
    for (int row = 0; row < height; ++row) {
        for (int col = 0; col < 100; ++col)
            text += screen.at(col, row).character;
        text += QLatin1Char('\n');
    }
    return text;
}

QString withoutAnsi(QString text)
{
    static const QRegularExpression csi(QStringLiteral("\x1b\\[[0-9;:?]*[A-Za-z]"));
    return text.remove(csi);
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void cleanupTestCase();
    void replayMatchesTheLiveTranscript();
};

void Test::cleanupTestCase()
{
    QDir(g_env.root).removeRecursively();
}

void Test::replayMatchesTheLiveTranscript()
{
    QTemporaryDir fixture(QDir::tempPath() + QStringLiteral("/test_qsoc_runtime_replay_XXXXXX"));
    QVERIFY(fixture.isValid());
    const QString project = QDir(fixture.path()).filePath(QStringLiteral("project"));
    QVERIFY(QDir().mkpath(project));
    const QString scriptPath = QDir(fixture.path()).filePath(QStringLiteral("script.json"));
    QFile         scriptFile(scriptPath);
    QVERIFY(scriptFile.open(QIODevice::WriteOnly));
    scriptFile.write(script(QDir(project).filePath(QStringLiteral("hello.c"))));
    scriptFile.close();

    const int port = pickFreePort();
    QVERIFY(port > 0);
    QFile configFile(QDir(g_env.root).filePath(QStringLiteral("config/qsoc/qsoc.yml")));
    QVERIFY(configFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    configFile.write(config(port));
    configFile.close();

    auto mockEnvironment = isolatedEnvironment(fixture.path());
    mockEnvironment.insert(QStringLiteral("MOCK_TTL"), QStringLiteral("120"));
    mockEnvironment.insert(QStringLiteral("MOCK_SCRIPT"), scriptPath);
    BoundedProcess mock;
    mock.setProcessEnvironment(mockEnvironment);
    mock.setStandardOutputFile(QDir(fixture.path()).filePath(QStringLiteral("mock.out")));
    mock.setStandardErrorFile(QDir(fixture.path()).filePath(QStringLiteral("mock.err")));
    mock.start(QString::fromUtf8(QSOC_MOCK_LLM_PATH), {QString::number(port), QStringLiteral("none")});
    QVERIFY(mock.waitForStarted(5000));
    QVERIFY(waitForMockReady(mock, port, 45000));

    QSocAgentRuntimeOptions options;
    options.projectDirectory = project;

    QTuiCompositor         live;
    QSocTranscriptRenderer liveRenderer(live);
    QString                sessionId;
    {
        QSocAgentRuntime runtime(options);
        QVERIFY(runtime.openSession());
        sessionId = runtime.sessionId();
        QSignalSpy events(&runtime, &QSocAgentRuntime::eventRaised);
        for (const QString &prompt :
             {QStringLiteral("REPLAYCHECK first"), QStringLiteral("second")}) {
            Event echo;
            echo.kind = Kind::UserMessage;
            echo.text = prompt;
            liveRenderer.apply(echo);
            events.clear();
            const auto result = runtime.runTurn(prompt);
            QVERIFY2(!result.error, qPrintable(result.finalText));
            QVERIFY(result.persistedOk);
            for (const QVariantList &args : std::as_const(events))
                liveRenderer.apply(args.first().value<Event>());
        }
    }

    QSocAgentRuntime resumed(options);
    QVERIFY(resumed.openSessionById(sessionId));
    QTuiCompositor         replay;
    QSocTranscriptRenderer replayRenderer(replay);
    replayRenderer.replaceHistory(resumed.messages());

    const QString liveText = live.contentView().toAnsi(100);
    QCOMPARE(replay.contentView().toAnsi(100), liveText);
    QCOMPARE(replay.contentView().toAnsi(48), live.contentView().toAnsi(48));
    QCOMPARE(todoPane(replay), todoPane(live));

    /* The scripted turn exercised every block the replay must rebuild. */
    const QString plain = withoutAnsi(liveText);
    QVERIFY(plain.contains(QStringLiteral("+++ b/")));
    QVERIFY(plain.contains(QStringLiteral("long x;")));
    QVERIFY(plain.contains(QStringLiteral("failed")));
    QVERIFY(plain.contains(QStringLiteral("(folded)")));
    QVERIFY(plain.contains(QStringLiteral("┄┄┄ c ┄┄┄")));
    QVERIFY(plain.contains(QStringLiteral("│ name │ value │")));
    QVERIFY(live.contentView().toPlainText().contains(QStringLiteral("Summarize the result.")));
    QVERIFY(todoPane(live).contains(QStringLiteral("Write tests")));
}

QSOC_TEST_MAIN(Test)

#include "test_qsocagentruntimereplay.moc"
