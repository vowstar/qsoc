// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsoctaskforecast.h"
#include "common/qllmservice.h"
#include "qsoc_test.h"

#include <QDateTime>
#include <QSignalSpy>
#include <QTcpServer>
#include <QTcpSocket>
#include <QUuid>
#include <QtTest>

namespace {
using json = nlohmann::json;

class Source : public QSocTaskSource
{
public:
    QList<QSocTask::Row> rows;
    QString              tail = QStringLiteral("[result] one check passed; integration remains");
    QString              sourceTag() const override { return QStringLiteral("agent"); }
    QList<QSocTask::Row> listTasks() const override { return rows; }
    QString              tailFor(const QString &, int) const override { return tail; }
    bool                 killTask(const QString &) override { return false; }
    void                 add(int count = 1)
    {
        for (int i = 0; i < count; ++i)
            rows.append(
                {QString::number(i),
                 QStringLiteral("Check interface"),
                 {},
                 QSocTask::Kind::SubAgent,
                 QSocTask::Status::Running,
                 QDateTime::currentMSecsSinceEpoch(),
                 true,
                 QStringLiteral("Verify all interfaces")});
        emit tasksChanged();
    }
};

class Server : public QObject
{
public:
    QList<json>                    requests;
    QList<QPointer<QTcpSocket>>    sockets;
    QMap<QTcpSocket *, QByteArray> buffers;
    QTcpServer                     server;
    Server()
    {
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection();
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() { read(socket); });
                connect(socket, &QTcpSocket::disconnected, this, [this, socket]() {
                    buffers.remove(socket);
                    socket->deleteLater();
                });
            }
        });
    }
    void read(QTcpSocket *socket)
    {
        auto &buffer = buffers[socket];
        buffer += socket->readAll();
        const auto split = buffer.indexOf("\r\n\r\n");
        if (split < 0)
            return;
        int length = 0;
        for (const auto &line : buffer.left(split).split('\n')) {
            if (line.toLower().startsWith("content-length:"))
                length = line.mid(15).trimmed().toInt();
        }
        if (buffer.size() < split + 4 + length)
            return;
        requests.append(json::parse(buffer.mid(split + 4, length).toStdString()));
        sockets.append(socket);
        buffers.remove(socket);
    }
    json estimates(int index) const
    {
        const auto snapshot = json::parse(
            requests.at(index)["messages"][1]["content"].get<std::string>());
        json result = json::array();
        for (const auto &task : snapshot["task_estimate_snapshot"])
            result.push_back(
                {{"key", task["key"]},
                 {"revision", task["revision"]},
                 {"summary", "checking integration"},
                 {"remaining", {"integration tests"}},
                 {"evidence", {"state", "tail"}},
                 {"unknowns", {"unobserved failures"}},
                 {"progress", {25, 60}},
                 {"eta_seconds", nullptr},
                 {"revision_reason", "integration is not yet verified"}});
        return result;
    }
    void reply(int index, const json &value, int status = 200)
    {
        auto socket = sockets.at(index);
        if (!socket)
            return;
        if (status != 200) {
            socket->write(
                "HTTP/1.1 429 Too Many Requests\r\nContent-Length: 0\r\nConnection: close\r\n\r\n");
        } else {
            const json chunk
                = {{"choices", json::array({{{"delta", {{"content", value.dump()}}}}})},
                   {"usage", {{"prompt_tokens", 13}, {"completion_tokens", 7}}}};
            socket->write(
                "HTTP/1.1 200 OK\r\nContent-Type: text/event-stream\r\nConnection: close\r\n\r\n");
            socket->write(
                "data: " + QByteArray::fromStdString(chunk.dump()) + "\n\ndata: [DONE]\n\n");
        }
        socket->disconnectFromHost();
    }
};

struct Fixture
{
    Server           server;
    QLLMService      service;
    QSocAgent        agent{nullptr, &service};
    Source           source;
    QSocTaskRegistry registry;
    QSocTaskForecast forecast{&registry, &agent};
    Fixture()
    {
        if (!server.server.listen(QHostAddress::LocalHost))
            qFatal("Cannot listen for forecast test");
        LLMModelConfig model;
        model.id      = QStringLiteral("primary");
        model.model   = model.id;
        model.url     = QStringLiteral("http://%1:%2/chat/completions")
                            .arg(server.server.serverAddress().toString())
                            .arg(server.server.serverPort());
        model.key     = QUuid::createUuid().toString(QUuid::WithoutBraces);
        model.effort  = QStringLiteral("high");
        model.timeout = 3000;
        service.setModel(model);
        auto config        = agent.getConfig();
        config.effortLevel = QStringLiteral("high");
        agent.setConfig(config);
        registry.registerSource(&source);
    }
};

class Test : public QObject
{
    Q_OBJECT
private slots:
    void destroyingAnInFlightEvaluatorLeavesTaskStateIntact()
    {
        Fixture f;
        f.forecast.setEnabled(false);
        auto evaluator = std::make_unique<QSocTaskForecast>(&f.registry, &f.agent);
        f.source.add();
        QTRY_COMPARE(f.server.requests.size(), 1);
        evaluator.reset();
        f.server.reply(0, f.server.estimates(0));
        QTest::qWait(50);
        QVERIFY(f.registry.estimateFor("agent", "0").summary.isEmpty());
        QCOMPARE(f.source.rows[0].status, QSocTask::Status::Running);
    }

    void inheritsModelEffortAndHasNoTools()
    {
        Fixture    f;
        QSignalSpy usageSpy(&f.agent, &QSocAgent::tokenUsage);
        f.source.add();
        QTRY_COMPARE(f.server.requests.size(), 1);
        const auto request = f.server.requests.first();
        QCOMPARE(request["model"], json("primary"));
        QCOMPARE(request["reasoning_effort"], json("high"));
        QVERIFY(!request.contains("tools") || request["tools"].empty());
        const auto snapshot = json::parse(request["messages"][1]["content"].get<std::string>());
        QCOMPARE(
            snapshot["task_estimate_snapshot"][0]["evidence"]["state"]["objective"],
            json("Verify all interfaces"));
        f.server.reply(0, f.server.estimates(0));
        QTRY_COMPARE(
            f.registry.estimateFor("agent", "0").summary, QStringLiteral("checking integration"));
        QCOMPARE(f.source.rows.first().status, QSocTask::Status::Running);
        QCOMPARE(f.registry.estimateFor("agent", "0").secondsLow, -1);
        QCOMPARE(usageSpy.size(), 1);
        QCOMPARE(usageSpy.first().at(0).toLongLong(), qint64(13));
        QCOMPARE(usageSpy.first().at(1).toLongLong(), qint64(7));
    }

    void coalescesUpdatesAndRejectsSupersededResults()
    {
        Fixture f;
        f.source.add(2);
        QTRY_COMPARE(f.server.requests.size(), 1);
        for (int i = 0; i < 20; ++i) {
            f.source.tail = QString::number(i);
            emit f.source.taskEvidenceChanged("0");
        }
        QCOMPARE(f.server.requests.size(), 1);
        f.server.reply(0, f.server.estimates(0));
        QTRY_COMPARE(f.server.requests.size(), 2);
        const auto request = json::parse(
            f.server.requests[1]["messages"][1]["content"].get<std::string>());
        QCOMPARE(request["task_estimate_snapshot"][0]["evidence"]["tail"], json("19"));
        QCOMPARE(f.registry.estimateFor("agent", "0").summary, QStringLiteral("estimating"));
        f.server.reply(1, f.server.estimates(1));
        QTRY_COMPARE(
            f.registry.estimateFor("agent", "0").summary, QStringLiteral("checking integration"));
        f.forecast.refresh();
        QTest::qWait(300);
        QCOMPARE(f.server.requests.size(), 2);
    }

    void cancellationAndModelSwitchDiscardOldForecasts()
    {
        Fixture f;
        f.source.add();
        QTRY_COMPARE(f.server.requests.size(), 1);
        f.source.rows[0].status = QSocTask::Status::Aborted;
        emit f.source.tasksChanged();
        f.server.reply(0, f.server.estimates(0));
        QTest::qWait(50);
        QVERIFY(f.registry.estimateFor("agent", "0").summary.isEmpty());
        f.source.rows[0].status = QSocTask::Status::Running;
        ++f.source.rows[0].startedAtMs;
        emit f.source.tasksChanged();
        QTRY_COMPARE(f.server.requests.size(), 2);
        auto model  = f.service.getCurrentModelConfig();
        model.id    = QStringLiteral("selected");
        model.model = model.id;
        f.service.setModel(model);
        QTRY_COMPARE(f.server.requests.size(), 3);
        QCOMPARE(f.server.requests[2]["model"], json("selected"));
        QCOMPARE(f.server.requests[2]["reasoning_effort"], json("high"));
        f.server.reply(1, f.server.estimates(1));
        f.server.reply(2, f.server.estimates(2));
        QTRY_COMPARE(
            f.registry.estimateFor("agent", "0").summary, QStringLiteral("checking integration"));
        QCOMPARE(f.source.rows.first().status, QSocTask::Status::Running);
    }

    void rejectsMalformedClaimsAndRateLimitsWithoutChangingState()
    {
        Fixture f;
        f.source.add();
        QTRY_COMPARE(f.server.requests.size(), 1);
        auto bad           = f.server.estimates(0);
        bad[0]["progress"] = {100, 100};
        f.server.reply(0, bad);
        QTRY_COMPARE(f.registry.estimateFor("agent", "0").summary, QStringLiteral("unavailable"));
        emit f.registry.estimateRefreshRequested();
        QTRY_COMPARE(f.server.requests.size(), 2);
        f.server.reply(1, {}, 429);
        QTRY_COMPARE(f.registry.estimateFor("agent", "0").summary, QStringLiteral("unavailable"));
        QCOMPARE(f.source.rows[0].status, QSocTask::Status::Running);
        QTest::qWait(350);
        QCOMPARE(f.server.requests.size(), 2);
    }

    void parserRejectsInjectedAndUnboundText()
    {
        const json good
            = {{"key", "agent/0"},
               {"revision", quint64(1)},
               {"summary", "checking"},
               {"remaining", json::array()},
               {"evidence", {"state"}},
               {"unknowns", json::array()},
               {"progress", nullptr},
               {"eta_seconds", nullptr},
               {"revision_reason", ""}};
        QSocTask::Estimate estimate;
        QVERIFY(QSocTaskForecast::parseEstimate(good, &estimate));
        for (const auto &change : json::array(
                 {{{"summary", std::string(1, char(27)) + "[2Jdone"}},
                  {{"summary", "first\nsecond"}},
                  {{"evidence", {"invented"}}},
                  {{"progress", {70, 20}}},
                  {{"eta_seconds", {-1, 10}}},
                  {{"progress", {0, 100}}},
                  {{"summary", std::string(81, 'a')}},
                  {{"eta_seconds", {0, 999999999999ULL}}},
                  {{"progress", {0.5, 1}}},
                  {{"accepted", true}}})) {
            auto bad = good;
            bad.update(change);
            QVERIFY(!QSocTaskForecast::parseEstimate(bad, &estimate));
        }
    }

    void batchesLargeSetsAndLeavesPendingTasksStatic()
    {
        Fixture f;
        f.source.add(17);
        f.source.rows[16].status = QSocTask::Status::Pending;
        emit f.source.tasksChanged();
        QTRY_COMPARE(f.server.requests.size(), 1);
        QCOMPARE(f.server.estimates(0).size(), size_t(8));
        f.server.reply(0, f.server.estimates(0));
        QTRY_COMPARE(f.server.requests.size(), 2);
        QCOMPARE(f.server.estimates(1).size(), size_t(8));
        f.server.reply(1, f.server.estimates(1));
        QTest::qWait(350);
        QCOMPARE(f.server.requests.size(), 2);
        QVERIFY(f.registry.estimateFor("agent", "16").summary.isEmpty());
        f.forecast.setEnabled(false);
        QVERIFY(f.registry.estimateFor("agent", "0").summary.isEmpty());
    }
};
} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoctaskforecast.moc"
