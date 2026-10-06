// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/protocol/qsocagentruntimeevent.h"
#include "agent/qsocsession.h"
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

QByteArray queueScript()
{
    const QJsonArray steps{step("Read the question.", "FIRSTANSWER"), step({}, "SECONDANSWER")};
    const QJsonArray routes{QJsonObject{{"contains", "QUEUECHECK"}, {"responses", steps}}};
    return QJsonDocument(routes).toJson();
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
    void queuedPromptEchoesWhereItIsRead();
    void shellOutputReachesTheNextRequest();
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

/* A prompt queued while the answer streams is its own message, echoed where
 * the runtime reads it, and the live transcript equals the replay. */
void Test::queuedPromptEchoesWhereItIsRead()
{
    QTemporaryDir fixture(QDir::tempPath() + QStringLiteral("/test_qsoc_runtime_replay_XXXXXX"));
    QVERIFY(fixture.isValid());
    const QString project = QDir(fixture.path()).filePath(QStringLiteral("project"));
    QVERIFY(QDir().mkpath(project));
    const QString scriptPath = QDir(fixture.path()).filePath(QStringLiteral("script.json"));
    QFile         scriptFile(scriptPath);
    QVERIFY(scriptFile.open(QIODevice::WriteOnly));
    scriptFile.write(queueScript());
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
        sessionId   = runtime.sessionId();
        bool queued = false;
        connect(&runtime, &QSocAgentRuntime::eventRaised, &runtime, [&](const Event &event) {
            liveRenderer.apply(event);
            if (event.kind == Kind::ContentChunk && !queued)
                queued = runtime.queueRequest(QStringLiteral("second"));
        });
        const auto result = runtime.runTurn(QStringLiteral("QUEUECHECK first"));
        QVERIFY2(!result.error, qPrintable(result.errorText));
        QVERIFY(result.persistedOk);
        QVERIFY(queued);
    }

    QSocAgentRuntime resumed(options);
    QVERIFY(resumed.openSessionById(sessionId));
    QTuiCompositor         replay;
    QSocTranscriptRenderer replayRenderer(replay);
    replayRenderer.replaceHistory(resumed.messages());

    QStringList users;
    for (const auto &message : resumed.messages())
        if (message.value("role", "") == "user")
            users.append(QString::fromStdString(message.value("content", "")));
    QCOMPARE(users, QStringList({QStringLiteral("QUEUECHECK first"), QStringLiteral("second")}));

    QCOMPARE(live.contentView().toAnsi(100), replay.contentView().toAnsi(100));
    const QString plain = live.contentView().toPlainText();
    QVERIFY2(!plain.contains(QStringLiteral("FIRSTANSWERSECONDANSWER")), qPrintable(plain));
    const auto firstAnswer = plain.indexOf(QStringLiteral("FIRSTANSWER"));
    const auto echo        = plain.indexOf(QStringLiteral("second"));
    QVERIFY2(firstAnswer >= 0 && firstAnswer < echo, qPrintable(plain));
    QVERIFY2(echo < plain.indexOf(QStringLiteral("SECONDANSWER")), qPrintable(plain));
}

/* A `!` result is a user-role block the next request carries; `!!` is not.
 * The script routes on the real prompt, the title is that prompt, and the
 * live transcript equals the replay. */
void Test::shellOutputReachesTheNextRequest()
{
    QTemporaryDir fixture(QDir::tempPath() + QStringLiteral("/test_qsoc_runtime_replay_XXXXXX"));
    QVERIFY(fixture.isValid());
    const QString project = QDir(fixture.path()).filePath(QStringLiteral("project"));
    QVERIFY(QDir().mkpath(project));
    const QString scriptPath = QDir(fixture.path()).filePath(QStringLiteral("script.json"));
    QFile         scriptFile(scriptPath);
    QVERIFY(scriptFile.open(QIODevice::WriteOnly));
    scriptFile.write(
        QJsonDocument(
            QJsonArray{QJsonObject{
                {"contains", "SHELLCHECK"}, {"responses", QJsonArray{step({}, "SHELLANSWER")}}}})
            .toJson());
    scriptFile.close();
    const QString requestLog = QDir(fixture.path()).filePath(QStringLiteral("requests.jsonl"));

    const int port = pickFreePort();
    QVERIFY(port > 0);
    QFile configFile(QDir(g_env.root).filePath(QStringLiteral("config/qsoc/qsoc.yml")));
    QVERIFY(configFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
    configFile.write(config(port));
    configFile.close();

    auto mockEnvironment = isolatedEnvironment(fixture.path());
    mockEnvironment.insert(QStringLiteral("MOCK_TTL"), QStringLiteral("120"));
    mockEnvironment.insert(QStringLiteral("MOCK_SCRIPT"), scriptPath);
    mockEnvironment.insert(QStringLiteral("MOCK_REQUEST_LOG"), requestLog);
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
    QString                sessionPath;
    {
        QSocAgentRuntime runtime(options);
        QVERIFY(runtime.openSession());
        sessionId   = runtime.sessionId();
        sessionPath = runtime.sessionPath();
        connect(&runtime, &QSocAgentRuntime::eventRaised, &runtime, [&](const Event &event) {
            liveRenderer.apply(event);
        });
        QVERIFY(runtime.executeCommand(QStringLiteral("!!echo LOCAL_O6")));
        QVERIFY(runtime.executeCommand(QStringLiteral("!echo MARK_O6")));
        const auto result = runtime.runTurn(QStringLiteral("SHELLCHECK ask"));
        QVERIFY2(!result.error, qPrintable(result.errorText));
        QVERIFY(result.persistedOk);
        QCOMPARE(result.finalText, QStringLiteral("SHELLANSWER"));
    }

    QFile log(requestLog);
    QVERIFY(log.open(QIODevice::ReadOnly));
    const QList<QByteArray> requests = log.readAll().split('\n');
    QVERIFY(!requests.isEmpty());
    const QJsonObject request = QJsonDocument::fromJson(requests.first()).object();
    QString           shell;
    QString           prompt;
    for (const auto &value : request.value(QStringLiteral("messages")).toArray()) {
        const QJsonObject message = value.toObject();
        const QString     content = message.value(QStringLiteral("content")).toString();
        if (message.value(QStringLiteral("role")).toString() != QStringLiteral("user"))
            continue;
        if (content.startsWith(QStringLiteral("<user_shell_command>")))
            shell = content;
        else if (content == QStringLiteral("SHELLCHECK ask"))
            prompt = content;
    }
    QVERIFY2(shell.contains(QStringLiteral("<command>\necho MARK_O6\n</command>")), qPrintable(shell));
    QVERIFY2(shell.contains(QStringLiteral("Output:\nMARK_O6\n")), qPrintable(shell));
    QVERIFY(!prompt.isEmpty());
    const QString system = request.value(QStringLiteral("messages"))
                               .toArray()
                               .first()
                               .toObject()
                               .value(QStringLiteral("content"))
                               .toString();
    QVERIFY(
        system.contains(QStringLiteral("A <user_shell_command> block is a command the user ran")));
    for (const QByteArray &line : requests)
        QVERIFY(!line.contains("LOCAL_O6"));
    QCOMPARE(QSocSession::readInfo(sessionPath).firstPrompt, QStringLiteral("SHELLCHECK ask"));

    QSocAgentRuntime resumed(options);
    QVERIFY(resumed.openSessionById(sessionId));
    QTuiCompositor         replay;
    QSocTranscriptRenderer replayRenderer(replay);
    replayRenderer.replaceHistory(resumed.messages());

    const QString liveText = live.contentView().toAnsi(100);
    const QString plain    = withoutAnsi(liveText);
    QVERIFY2(plain.contains(QStringLiteral("added to the conversation")), qPrintable(plain));
    QVERIFY2(plain.contains(QStringLiteral("!echo MARK_O6")), qPrintable(plain));
    /* The local-only line is on screen live and never in the history. */
    QVERIFY2(plain.contains(QStringLiteral("LOCAL_O6")), qPrintable(plain));
    QVERIFY(!withoutAnsi(replay.contentView().toAnsi(100)).contains(QStringLiteral("LOCAL_O6")));
    const qsizetype cut = liveText.indexOf(QStringLiteral("!echo MARK_O6"));
    QVERIFY(cut > 0);
    const QString replayText = replay.contentView().toAnsi(100);
    QCOMPARE(replayText.mid(replayText.indexOf(QStringLiteral("!echo MARK_O6"))), liveText.mid(cut));
}

QSOC_TEST_MAIN(Test)

#include "test_qsocagentruntimereplay.moc"
