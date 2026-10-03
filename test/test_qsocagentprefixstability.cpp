// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocmemorymanager.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctool.h"
#include "agent/tool/qsoctoolplanmode.h"
#include "common/qllmservice.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>

#include <QHash>
#include <QHostAddress>
#include <QQueue>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

using json = nlohmann::json;

namespace {

class RecordingServer final : public QObject
{
public:
    explicit RecordingServer(bool messagesApi)
        : messagesApi_(messagesApi)
    {
        connect(&server_, &QTcpServer::newConnection, this, [this]() {
            while (server_.hasPendingConnections()) {
                QTcpSocket *socket = server_.nextPendingConnection();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { consume(socket); });
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
            }
        });
    }

    bool listen() { return server_.listen(QHostAddress::LocalHost); }

    QString url() const
    {
        return QStringLiteral("http://127.0.0.1:%1/%2")
            .arg(server_.serverPort())
            .arg(messagesApi_ ? QStringLiteral("v1/messages") : QStringLiteral("chat/completions"));
    }

    void text(const char *content)
    {
        if (messagesApi_) {
            reply({{{"type", "text"}, {"text", content}}}, "end_turn");
        } else {
            reply({{"role", "assistant"}, {"content", content}});
        }
    }

    void call(const char *name, const json &arguments = json::object())
    {
        const std::string id = "call_" + std::to_string(++serial_);
        if (messagesApi_) {
            reply(
                {{{"type", "tool_use"}, {"id", id}, {"name", name}, {"input", arguments}}},
                "tool_use");
            return;
        }
        const json call
            = {{"id", id},
               {"type", "function"},
               {"function", {{"name", name}, {"arguments", arguments.dump()}}}};
        reply({{"role", "assistant"}, {"content", nullptr}, {"tool_calls", json::array({call})}});
    }

    bool drained() const { return responses_.isEmpty() && misses_ == 0; }

    QList<json> requests;

private:
    void reply(const json &message)
    {
        responses_.enqueue(
            json{{"choices", json::array({{{"message", message}, {"finish_reason", "stop"}}})}}
                .dump());
    }

    void reply(const json &content, const char *stopReason)
    {
        responses_.enqueue(
            json{
                {"id", "msg_" + std::to_string(++serial_)},
                {"type", "message"},
                {"role", "assistant"},
                {"content", content},
                {"stop_reason", stopReason},
                {"usage", {{"input_tokens", 1}, {"output_tokens", 1}}}}
                .dump());
    }

    void consume(QTcpSocket *socket)
    {
        QByteArray &buffer = buffers_[socket];
        buffer.append(socket->readAll());
        const qsizetype headerEnd = buffer.indexOf("\r\n\r\n");
        if (headerEnd < 0) {
            return;
        }
        qsizetype length = 0;
        for (QByteArray line : buffer.left(headerEnd).split('\n')) {
            line = line.trimmed();
            if (line.toLower().startsWith("content-length:")) {
                length = line.mid(15).trimmed().toLongLong();
            }
        }
        if (buffer.size() < headerEnd + 4 + length) {
            return;
        }
        requests.append(json::parse(buffer.mid(headerEnd + 4, length).toStdString()));
        buffers_.remove(socket);
        misses_ += responses_.isEmpty() ? 1 : 0;
        const QByteArray body = QByteArray::fromStdString(
            responses_.isEmpty() ? std::string(R"({"error":{"message":"empty script"}})")
                                 : responses_.dequeue());
        socket->write(
            "HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nConnection: close\r\n"
            "Content-Length: "
            + QByteArray::number(body.size()) + "\r\n\r\n" + body);
        socket->flush();
        socket->disconnectFromHost();
    }

    bool                            messagesApi_;
    int                             serial_ = 0;
    int                             misses_ = 0;
    QTcpServer                      server_;
    QHash<QTcpSocket *, QByteArray> buffers_;
    QQueue<std::string>             responses_;
};

class ProbeTool final : public QSocTool
{
public:
    ProbeTool(QString name, bool readOnly, std::function<QString()> body, QObject *parent)
        : QSocTool(parent)
        , name_(std::move(name))
        , readOnly_(readOnly)
        , body_(std::move(body))
    {}

    QString getName() const override { return name_; }
    QString getDescription() const override { return QStringLiteral("Probe ") + name_; }
    json    getParametersSchema() const override { return {{"type", "object"}}; }
    bool    isReadOnly() const override { return readOnly_; }
    QString execute(const json &) override { return body_(); }

private:
    QString                  name_;
    bool                     readOnly_;
    std::function<QString()> body_;
};

/* Cache breakpoints move every request by design; they are not content. */
json withoutCacheMarks(json value)
{
    if (value.is_object()) {
        value.erase("cache_control");
        for (auto &item : value) {
            item = withoutCacheMarks(item);
        }
    } else if (value.is_array()) {
        for (auto &item : value) {
            item = withoutCacheMarks(item);
        }
    }
    return value;
}

/* The prompt in render order: system, tools, then each message block. */
QList<QPair<QString, QString>> render(const json &raw)
{
    const json                     request  = withoutCacheMarks(raw);
    const json                    &messages = request.at("messages");
    QList<QPair<QString, QString>> parts;
    size_t                         first = 0;
    if (request.contains("system")) {
        parts.append({QStringLiteral("system"), QString::fromStdString(request["system"].dump())});
    } else if (!messages.empty() && messages.front().value("role", std::string()) == "system") {
        parts.append(
            {QStringLiteral("system"),
             QString::fromStdString(messages.front().value("content", std::string()))});
        first = 1;
    }
    parts.append(
        {QStringLiteral("tools"),
         QString::fromStdString(request.value("tools", json::array()).dump())});
    for (size_t i = first; i < messages.size(); ++i) {
        json          message = messages[i];
        const QString label   = QStringLiteral("message %1 (%2)")
                                    .arg(i)
                                    .arg(QString::fromStdString(message.value("role", "")));
        const json    blocks  = message.contains("content") && message["content"].is_array()
                                    ? message["content"]
                                    : json::array();
        message.erase("role");
        if (!blocks.empty()) {
            message.erase("content");
        }
        parts.append({label, QStringLiteral("[%1]").arg(label)});
        parts.append({label, QString::fromStdString(message.dump())});
        for (const auto &block : blocks) {
            parts.append({label, QString::fromStdString(block.dump())});
        }
    }
    return parts;
}

QString joined(const QList<QPair<QString, QString>> &parts)
{
    QString text;
    for (const auto &part : parts) {
        text += part.second;
    }
    return text;
}

QString section(const QList<QPair<QString, QString>> &parts, qsizetype at)
{
    for (const auto &part : parts) {
        if (at < part.second.size()) {
            return QStringLiteral("%1 at %2").arg(part.first).arg(at);
        }
        at -= part.second.size();
    }
    return QStringLiteral("end");
}

struct StepResult
{
    bool    prefixHeld = true;
    bool    toolsHeld  = true;
    QString where;
};

/* Breaks the current layout still causes. Each fix deletes its rows. */
const QHash<QString, QString> &knownBreaks()
{
    static const QHash<QString, QString> breaks = {
        {QStringLiteral("AGENTS.md edited"), QStringLiteral("system reread every request")},
    };
    return breaks;
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir                      home_;
    QTemporaryDir                      project_;
    QSocProjectManager                 projectManager_;
    std::unique_ptr<QSocMemoryManager> memory_;
    QStringList                        order_;
    QHash<QString, StepResult>         results_;

    /* One scripted session against one wire format. */
    struct Session
    {
        explicit Session(bool messagesApi)
            : server(messagesApi)
        {}

        RecordingServer                server;
        QLLMService                    service;
        QSocToolRegistry               registry;
        bool                           watching = true;
        QSocAgent                     *current  = nullptr;
        QString                        tag;
        QString                        previous;
        QList<QPair<QString, QString>> previousParts;
        json                           previousTools;
        int                            seen = 0;
    };

    QSocAgentConfig config() const
    {
        QSocAgentConfig config;
        config.verbose             = false;
        config.projectPath         = project_.path();
        config.maxIterations       = 6;
        config.memoryRecallEnabled = true;
        config.keepRecentMessages  = 2;
        return config;
    }

    void writeAgentsMd(const QString &rules)
    {
        QFile file(QDir(project_.path()).filePath(QStringLiteral("AGENTS.md")));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        file.write(rules.toUtf8());
    }

    /* Compare every new main-agent request with the one before it. */
    void record(Session &session, const QString &step, bool rebuild)
    {
        StepResult result;
        for (; session.seen < session.server.requests.size(); ++session.seen) {
            const json &request = session.server.requests.at(session.seen);
            if (!request.contains("tools")) {
                continue;
            }
            const auto    parts = render(request);
            const QString text  = joined(parts);
            if (!session.previous.isEmpty() && !text.startsWith(session.previous)) {
                qsizetype common = 0;
                while (common < session.previous.size() && common < text.size()
                       && session.previous.at(common) == text.at(common)) {
                    ++common;
                }
                if (result.prefixHeld) {
                    result.where = section(session.previousParts, common);
                }
                result.prefixHeld = false;
            }
            if (!session.previousTools.is_null() && session.previousTools != request.at("tools")) {
                result.toolsHeld = false;
            }
            session.previous      = text;
            session.previousParts = parts;
            session.previousTools = request.at("tools");
        }
        if (!rebuild) {
            const QString key = session.tag + QLatin1Char('/') + step;
            order_.append(key);
            results_.insert(key, result);
        }
    }

    void turn(Session &session, const QString &step, const QString &prompt, bool rebuild = false)
    {
        QVERIFY(!session.current->run(prompt).isEmpty());
        record(session, step, rebuild);
    }

    QSocAgent *startAgent(Session &session)
    {
        auto *agent = new QSocAgent(this, &session.service, &session.registry, config());
        agent->setMemoryManager(memory_.get());
        agent->setUserWatchingProbe([&session]() { return session.watching; });
        auto *messaging = new QSocSubAgentTaskSource(agent);
        messaging->enableMessaging(agent);
        agent->bindSessionIdentity(QStringLiteral("prefix-session"));
        session.current = agent;
        return agent;
    }

    void runSession(LLMApi api, const QString &tag)
    {
        Session session(api == LLMApi::AnthropicMessages);
        session.tag = tag;
        QVERIFY(session.server.listen());
        LLMModelConfig endpoint;
        endpoint.name            = QStringLiteral("prefix-test");
        endpoint.url             = session.server.url();
        endpoint.model           = QStringLiteral("prefix-model");
        endpoint.timeout         = 5000;
        endpoint.maxOutputTokens = 4096;
        endpoint.api             = api;
        session.service.setModel(endpoint);
        writeAgentsMd(QStringLiteral("# Rules\nAnswer briefly.\n"));

        QSocToolRegistry &registry = session.registry;
        const QString big = QStringLiteral("line: module u connects clk to rst_n\n").repeated(40);
        registry.registerTool(
            new ProbeTool(QStringLiteral("read_probe"), true, [big]() { return big; }, &registry));
        registry.registerTool(new ProbeTool(
            QStringLiteral("write_probe"),
            false,
            []() { return QStringLiteral("written"); },
            &registry));
        registry.registerTool(new ProbeTool(
            QStringLiteral("focus_probe"),
            true,
            [&session]() {
                session.watching = false;
                return QStringLiteral("looked");
            },
            &registry));
        registry.registerTool(new QSocToolEnterPlanMode(&registry, [&session]() {
            auto cfg     = session.current->getConfig();
            cfg.planMode = true;
            session.current->setConfig(cfg);
        }));
        registry.registerTool(new QSocToolExitPlanMode(&registry, [&session](const QString &plan) {
            auto cfg     = session.current->getConfig();
            cfg.planMode = false;
            session.current->setConfig(cfg);
            session.current->setApprovedPlan(plan);
            return QSocPlanApproval{true, {}};
        }));
        QSocAgent       *agent  = startAgent(session);
        RecordingServer &server = session.server;

        server.call("read_probe");
        server.text("T1 done");
        turn(session, QStringLiteral("tool loop"), QStringLiteral("Read the notes and summarize"));
        server.text("T2 done");
        turn(session, QStringLiteral("follow-up turn"), QStringLiteral("Count the lines again"));
        server.text("T3 done");
        turn(session, QStringLiteral("single word turn"), QStringLiteral("ok"));
        server.text("T4 done");
        turn(session, QStringLiteral("recall returns"), QStringLiteral("Check the wiring again"));

        session.watching = false;
        server.text("T5 done");
        turn(session, QStringLiteral("focus lost"), QStringLiteral("Keep going with the notes"));
        server.text("T6 done");
        turn(session, QStringLiteral("unfocused turn"), QStringLiteral("And the next section"));
        session.watching = true;
        server.text("T7 done");
        turn(session, QStringLiteral("focus regained"), QStringLiteral("I am back, continue"));
        server.call("focus_probe");
        server.text("T8 done");
        turn(session, QStringLiteral("focus lost mid-turn"), QStringLiteral("Look at it once"));
        session.watching = true;
        server.text("T9 done");
        turn(session, QStringLiteral("refocused turn"), QStringLiteral("Back again, go on"));

        auto cfg     = agent->getConfig();
        cfg.planMode = true;
        agent->setConfig(cfg);
        server.call("read_probe");
        server.text("T10 plan drafted");
        server.text("T10 after the plan-mode nudge");
        turn(session, QStringLiteral("plan mode on"), QStringLiteral("Plan a fix for the typo"));
        server.text("T11 refined");
        server.text("T11 after the plan-mode nudge");
        turn(session, QStringLiteral("plan turn"), QStringLiteral("Refine the plan a bit"));
        server.call("exit_plan_mode", {{"plan", "1. Fix the typo.\n2. Verify."}});
        server.text("T12 approved");
        turn(session, QStringLiteral("plan approved"), QStringLiteral("Present the plan now"));
        server.call("write_probe");
        server.text("T13 applied");
        turn(session, QStringLiteral("approved plan turn"), QStringLiteral("Apply the plan now"));

        writeAgentsMd(QStringLiteral("# Rules\nAnswer briefly.\nPrefer tables.\n"));
        server.text("T14 done");
        turn(session, QStringLiteral("AGENTS.md edited"), QStringLiteral("Show the result as text"));

        server.text("## Task Overview\n- notes reviewed and fixed\n");
        QVERIFY(agent->compact() > 0);
        server.text("T15 done");
        turn(session, QStringLiteral("compaction"), QStringLiteral("Continue after this"), true);
        server.text("T16 done");
        turn(session, QStringLiteral("turn after compaction"), QStringLiteral("One more check"));

        const json    history = agent->getMessages();
        const QString plan    = agent->approvedPlan();
        delete agent;
        QSocAgent *resumed = startAgent(session);
        resumed->setMessages(history);
        resumed->setApprovedPlan(plan);
        server.text("T17 done");
        turn(session, QStringLiteral("resume"), QStringLiteral("Resumed, show the last line"));
        server.text("T18 done");
        turn(session, QStringLiteral("turn after resume"), QStringLiteral("Thanks, wrap it up"));
        delete resumed;
        QVERIFY(server.drained());
        QCOMPARE(server.requests.back().contains("system"), api == LLMApi::AnthropicMessages);
    }

private slots:
    void initTestCase()
    {
        QVERIFY(home_.isValid());
        QVERIFY(project_.isValid());
        qputenv("QSOC_HOME", home_.path().toUtf8());
        qputenv("XDG_CONFIG_HOME", home_.path().toUtf8());
        projectManager_.setProjectPath(project_.path());
        memory_ = std::make_unique<QSocMemoryManager>(nullptr, &projectManager_);
        for (const char *name : {"wiring", "naming"}) {
            QVERIFY(memory_->writeTopicFile(
                QStringLiteral("project"),
                QString::fromLatin1(name),
                QStringLiteral("project"),
                QStringLiteral("Notes about %1").arg(QString::fromLatin1(name)),
                QStringLiteral("Remember the %1 rule.").arg(QString::fromLatin1(name))));
        }
        runSession(LLMApi::OpenAIChat, QStringLiteral("openai-chat"));
        runSession(LLMApi::AnthropicMessages, QStringLiteral("anthropic-messages"));
    }

    void cleanupTestCase()
    {
        /* QSOC_TEST_MAIN exits without running destructors. */
        home_.remove();
        project_.remove();
    }

    void prefixOnlyGrows_data()
    {
        QTest::addColumn<QString>("step");
        for (const QString &step : std::as_const(order_)) {
            QTest::newRow(step.toUtf8().constData()) << step;
        }
    }

    void prefixOnlyGrows()
    {
        QFETCH(QString, step);
        const StepResult result = results_.value(step);
        const QString    name   = step.section(QLatin1Char('/'), 1);
        if (knownBreaks().contains(name)) {
            QEXPECT_FAIL("", qPrintable(knownBreaks().value(name)), Continue);
        }
        QVERIFY2(result.prefixHeld, qPrintable(result.where));
    }

    void toolsStayFixed_data() { prefixOnlyGrows_data(); }

    void toolsStayFixed()
    {
        QFETCH(QString, step);
        QVERIFY(results_.value(step).toolsHeld);
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocagentprefixstability.moc"
