// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/protocol/qsocagentruntimeevent.h"
#include "agent/qsocagent.h"
#include "agent/qsoctool.h"
#include "agent/runtime/qsocagentruntime.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"

#include <nlohmann/json.hpp>

#include <memory>
#include <QDir>
#include <QFile>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QtTest>

using json = nlohmann::json;

namespace {

/* Chat endpoint that records every body. A newest user text holding
 * CHILD_TASK marks a child request. */
class ChatServer : public QObject
{
    Q_OBJECT

public:
    ChatServer()
    {
        connect(&server_, &QTcpServer::newConnection, this, [this]() {
            while (server_.hasPendingConnections())
                serve(server_.nextPendingConnection());
        });
    }

    bool    listen() { return server_.listen(QHostAddress::LocalHost); }
    quint16 port() const { return server_.serverPort(); }

    /* The first recorded request that a child sent. */
    json childRequest() const
    {
        for (const QByteArray &body : bodies_) {
            const json request = json::parse(body.toStdString(), nullptr, false);
            if (lastUserText(request).contains(QStringLiteral("CHILD_TASK")))
                return request;
        }
        return {};
    }

    void clear() { bodies_.clear(); }

    static QString lastUserText(const json &request)
    {
        if (!request.is_object() || !request.contains("messages"))
            return {};
        const json &messages = request["messages"];
        for (auto it = messages.rbegin(); it != messages.rend(); ++it) {
            if (it->value("role", std::string()) != "user" || !(*it)["content"].is_string())
                continue;
            const QString text = QString::fromStdString((*it)["content"].get<std::string>());
            if (!text.startsWith(QStringLiteral("<system-reminder>")))
                return text;
        }
        return {};
    }

private:
    void serve(QTcpSocket *socket)
    {
        connect(socket, &QTcpSocket::disconnected, socket, &QTcpSocket::deleteLater);
        connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
            QByteArray &buffer = buffers_[socket];
            buffer.append(socket->readAll());
            const qsizetype split = buffer.indexOf("\r\n\r\n");
            if (split < 0)
                return;
            const QByteArray head   = buffer.left(split).toLower();
            const qsizetype  at     = head.indexOf("content-length: ");
            const qsizetype  length = at < 0 ? 0
                                             : head.mid(at + 16).split('\r').first().toLongLong();
            if (buffer.size() < split + 4 + length)
                return;
            bodies_.append(buffer.mid(split + 4, length));
            buffers_.remove(socket);
            answer(socket);
        });
    }

    static void answer(QTcpSocket *socket)
    {
        const json chunk{
            {"choices",
             json::array(
                 {{{"index", 0},
                   {"delta", {{"role", "assistant"}, {"content", "ACK"}}},
                   {"finish_reason", nullptr}}})}};
        const json stop{
            {"choices",
             json::array({{{"index", 0}, {"delta", json::object()}, {"finish_reason", "stop"}}})}};
        const QByteArray sse = "data: " + QByteArray::fromStdString(chunk.dump()) + "\n\ndata: "
                               + QByteArray::fromStdString(stop.dump()) + "\n\ndata: [DONE]\n\n";
        socket->write(
            "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nContent-Length: "
            + QByteArray::number(sse.size()) + "\r\nConnection: close\r\n\r\n" + sse);
        socket->flush();
        socket->disconnectFromHost();
    }

    QTcpServer                      server_;
    QHash<QTcpSocket *, QByteArray> buffers_;
    QList<QByteArray>               bodies_;
};

/* Isolated config root, prepared before QCoreApplication exists. */
struct EnvBootstrap
{
    EnvBootstrap()
        : root(QDir(QDir::tempPath())
                   .filePath(
                       QStringLiteral("test_qsocparent-")
                       + QString::number(QCoreApplication::applicationPid())))
    {
        QDir().mkpath(root + QStringLiteral("/config/qsoc"));
        QDir().mkpath(root + QStringLiteral("/.ssh"));
        QFile::setPermissions(
            root, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        qputenv("QSOC_HOME", (root + QStringLiteral("/config/qsoc")).toUtf8());
        qputenv("XDG_CONFIG_HOME", (root + QStringLiteral("/config")).toUtf8());
        qputenv("XDG_RUNTIME_DIR", root.toUtf8());
        qputenv("HOME", root.toUtf8());
    }
    QString root;
};

const EnvBootstrap g_env;

QStringList toolNames(const json &request)
{
    QStringList names;
    for (const json &tool : request.value("tools", json::array()))
        names.append(QString::fromStdString(tool["function"].value("name", std::string())));
    return names;
}

QString spawn(QSocAgentRuntime *runtime, const QString &type)
{
    return runtime->localToolRegistry()->executeTool(
        QStringLiteral("agent"),
        {{"subagent_type", type.toStdString()},
         {"description", "child"},
         {"prompt", "CHILD_TASK reply"},
         {"run_in_background", true}},
        runtime->agent());
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qRegisterMetaType<QSocAgentRuntimeEvent>("QSocAgentRuntimeEvent");
        QVERIFY(server_.listen());
        sshd_.start();
    }

    void cleanupTestCase()
    {
        sshd_.stop();
        sshd_.removeRoot();
        QDir(g_env.root).removeRecursively();
    }

    void init() { server_.clear(); }

    void forkChildRunsOnTheMainContext()
    {
        auto runtime = makeRuntime();
        QVERIFY(runtime->openSession());
        QVERIFY(!runtime->runTurn(QStringLiteral("PARENT_MARKER hello")).error);

        const QString launch = spawn(runtime.get(), QStringLiteral("fork"));
        QVERIFY2(launch.contains(QStringLiteral("task_id")), qPrintable(launch));
        QTRY_COMPARE_WITH_TIMEOUT(runtime->agent()->pendingNotificationCount(), 1, 10000);

        const QByteArray child = QByteArray::fromStdString(server_.childRequest().dump());
        QVERIFY(child.contains("PARENT_MARKER"));
        QVERIFY(child.contains("qsoc-fork-tag"));
    }

    void childInheritsTheLiveModelAndEffort()
    {
        auto runtime = makeRuntime();
        QVERIFY(runtime->openSession());
        QVERIFY(runtime->setCurrentModel(QStringLiteral("alt")));
        runtime->setEffortLevel(QStringLiteral("high"));

        for (const QString &type : {QStringLiteral("general-purpose"), QStringLiteral("fork")}) {
            server_.clear();
            const QString launch = spawn(runtime.get(), type);
            QVERIFY2(launch.contains(QStringLiteral("task_id")), qPrintable(launch));
            QTRY_VERIFY_WITH_TIMEOUT(!server_.childRequest().is_null(), 10000);
            const json child = server_.childRequest();
            QCOMPARE(child.value("model", std::string()), std::string("alt-model"));
            QCOMPARE(child.value("reasoning_effort", std::string()), std::string("high"));
        }
    }

    void childUsesTheMainAgentsCurrentRegistry()
    {
        auto runtime = makeRuntime();
        QVERIFY(runtime->openSession());
        auto            *local = runtime->localToolRegistry();
        QSocToolRegistry swapped;
        for (const char *name : {"agent", "read_file", "agent_list"})
            swapped.registerTool(local->getTool(QString::fromLatin1(name)));
        runtime->agent()->setToolRegistry(&swapped);

        const QString launch = spawn(runtime.get(), QStringLiteral("general-purpose"));
        QVERIFY2(launch.contains(QStringLiteral("task_id")), qPrintable(launch));
        QTRY_VERIFY_WITH_TIMEOUT(!server_.childRequest().is_null(), 10000);
        const QStringList tools = toolNames(server_.childRequest());
        QVERIFY2(tools.contains(QStringLiteral("read_file")), qPrintable(tools.join(',')));
        QVERIFY2(!tools.contains(QStringLiteral("module_list")), qPrintable(tools.join(',')));
        QTRY_COMPARE_WITH_TIMEOUT(runtime->agent()->pendingNotificationCount(), 1, 10000);
        runtime->agent()->setToolRegistry(local);
    }

    void remoteChildUsesTheRemoteRegistry()
    {
        QSOC_REQUIRE_SSHD(sshd_);
        QFile sshConfig(g_env.root + QStringLiteral("/.ssh/config"));
        QVERIFY(sshConfig.open(QIODevice::WriteOnly | QIODevice::Truncate));
        sshConfig.write(QStringLiteral(
                            "Host loopback\n"
                            "  HostName 127.0.0.1\n"
                            "  Port %1\n"
                            "  User %2\n"
                            "  IdentityFile %3\n"
                            "  IdentitiesOnly yes\n"
                            "  StrictHostKeyChecking no\n"
                            "  UserKnownHostsFile /dev/null\n")
                            .arg(sshd_.port())
                            .arg(sshd_.user(), sshd_.keyPath())
                            .toUtf8());
        sshConfig.close();
        QFile::setPermissions(sshConfig.fileName(), QFileDevice::ReadOwner | QFileDevice::WriteOwner);

        auto runtime = makeRuntime(QStringLiteral("loopback"), sshd_.workDir());
        QVERIFY2(runtime->isRemote(), qPrintable(runtime->lastError()));
        QVERIFY(runtime->openSession());
        QCOMPARE(runtime->agent()->getToolRegistry(), runtime->remoteToolRegistry());

        const QString launch = spawn(runtime.get(), QStringLiteral("general-purpose"));
        QVERIFY2(launch.contains(QStringLiteral("task_id")), qPrintable(launch));
        QTRY_VERIFY_WITH_TIMEOUT(!server_.childRequest().is_null(), 10000);
        const QStringList tools = toolNames(server_.childRequest());
        QVERIFY2(!tools.contains(QStringLiteral("module_list")), qPrintable(tools.join(',')));
        for (const char *name : {"agent_list", "agent_inbox", "wait_agent", "send_message"})
            QVERIFY2(tools.contains(QString::fromLatin1(name)), name);
        QTRY_COMPARE_WITH_TIMEOUT(runtime->agent()->pendingNotificationCount(), 1, 10000);
        runtime->disconnectRemote();
    }

private:
    std::unique_ptr<QSocAgentRuntime> makeRuntime(
        const QString &sshTarget = {}, const QString &workspace = {})
    {
        QFile config(g_env.root + QStringLiteral("/config/qsoc/qsoc.yml"));
        if (!config.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text))
            return {};
        config.write(QStringLiteral(
                         "llm:\n"
                         "  models:\n"
                         "    mock: {name: mock, model: mock-model, "
                         "url: \"http://127.0.0.1:%1/v1/chat/completions\", key: none, "
                         "timeout: 10000}\n"
                         "    alt: {name: alt, model: alt-model, "
                         "url: \"http://127.0.0.1:%1/v1/chat/completions\", key: none, "
                         "timeout: 10000}\n"
                         "  model: mock\n"
                         "proxy: {type: none}\n"
                         "agent:\n"
                         "  session_title: false\n"
                         "  away_summary: false\n"
                         "  memory_extract: false\n"
                         "  memory_dream: false\n"
                         "  predict_input: false\n")
                         .arg(server_.port())
                         .toUtf8());
        config.close();
        QSocAgentRuntimeOptions options;
        options.projectDirectory = project_.path();
        options.sshTarget        = sshTarget;
        options.workspace        = workspace;
        return std::make_unique<QSocAgentRuntime>(options);
    }

    ChatServer    server_;
    QSocTestSshd  sshd_;
    QTemporaryDir project_;
};

QSOC_TEST_MAIN(Test)
#include "test_qsocagentruntimeparent.moc"
