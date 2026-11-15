// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qllmservice.h"
#include "qsoc_test.h"
#include "qsoc_test_llm.h"

#include <QSignalSpy>
#include <QtTest>

namespace {

const json messages = {{{"role", "user"}, {"content", "hello"}}};

LLMModelConfig endpoint(const QSocTestLlmMock &mock, bool messagesApi)
{
    LLMModelConfig model;
    model.id              = QStringLiteral("test-model");
    model.model           = model.id;
    model.api             = messagesApi ? LLMApi::AnthropicMessages : LLMApi::OpenAIChat;
    model.url             = QStringLiteral("http://127.0.0.1:%1/v1/%2")
                                .arg(mock.port)
                                .arg(messagesApi ? "messages" : "chat/completions");
    model.proxy           = QStringLiteral("none");
    model.timeout         = 10000;
    model.maxOutputTokens = 4096;
    model.effort          = QStringLiteral("high");
    model.effortLevels    = {"low", "medium", "high", "xhigh", "max"};
    return model;
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void configuration_data()
    {
        QTest::addColumn<QByteArray>("fields");
        QTest::addColumn<bool>("valid");
        QTest::newRow("legacy") << QByteArray("effort: max\n") << true;
        QTest::newRow("declared") << QByteArray("effort: max\neffort_levels: [low, high, max]\n")
                                  << true;
        QTest::newRow("normalized")
            << QByteArray("effort: ' HIGH '\neffort_levels: [' LOW ', HIGH]\n") << true;
        QTest::newRow("omit-effort")
            << QByteArray("effort: off\neffort_levels: [off, high]\n") << true;
        QTest::newRow("implicit-off") << QByteArray("effort_levels: [off, high]\n") << true;
        QTest::newRow("plain") << QByteArray("reasoning: false\neffort_levels: [off]\n") << true;
        QTest::newRow("provider-level")
            << QByteArray("effort: minimal\neffort_levels: [minimal, high]\n") << true;
        QTest::newRow("scalar") << QByteArray("effort_levels: high\n") << false;
        QTest::newRow("empty") << QByteArray("effort_levels: []\n") << false;
        QTest::newRow("null") << QByteArray("effort_levels: null\n") << false;
        QTest::newRow("duplicate")
            << QByteArray("effort: high\neffort_levels: [high, HIGH]\n") << false;
        QTest::newRow("empty-level") << QByteArray("effort_levels: ['']\n") << false;
        QTest::newRow("nested") << QByteArray("effort_levels: [{high: true}]\n") << false;
        QTest::newRow("invalid-default")
            << QByteArray("effort: medium\neffort_levels: [low, high]\n") << false;
        QTest::newRow("missing-default") << QByteArray("effort_levels: [low, high]\n") << false;
        QTest::newRow("plain-with-effort")
            << QByteArray("reasoning: false\neffort: high\neffort_levels: [high]\n") << false;
    }

    void configuration()
    {
        QFETCH(QByteArray, fields);
        QFETCH(bool, valid);
        const auto node = YAML::Load(
            (QByteArray("url: http://127.0.0.1:9/v1/messages\n") + fields).toStdString());
        const auto model = QLLMService::parseModelEntry(QStringLiteral("test-model"), node);
        QCOMPARE(model.has_value(), valid);
        if (model)
            QVERIFY(model->effortError(model->effort).isEmpty());
    }

    void legacyMenuIncludesItsConfiguredDefault()
    {
        LLMModelConfig model;
        model.effort = QStringLiteral("max");
        QCOMPARE(model.availableEffortLevels(), QStringList({"off", "low", "medium", "high", "max"}));
        QVERIFY(model.effortError(QStringLiteral("custom")).isEmpty());
        model.reasoning = false;
        QCOMPARE(model.availableEffortLevels(), QStringList({"off"}));
    }

    void wireEncoding_data()
    {
        QTest::addColumn<bool>("messagesApi");
        QTest::addColumn<QString>("level");
        for (bool api : {false, true}) {
            for (const auto &level : {"low", "medium", "high", "xhigh", "max"})
                QTest::newRow(
                    qPrintable(QStringLiteral("%1-%2").arg(api ? "messages" : "chat", level)))
                    << api << QString::fromLatin1(level);
        }
    }

    void wireEncoding()
    {
        QFETCH(bool, messagesApi);
        QFETCH(QString, level);
        QSocTestLlmMock mock;
        QVERIFY(mock.start({}));
        auto model   = endpoint(mock, messagesApi);
        model.effort = level;
        QLLMService service;
        service.setModel(model);
        QVERIFY(service.sendRequest(QStringLiteral("hello")).success);
        QVERIFY(!service.sendChatCompletion(messages).contains("error"));
        QVERIFY(
            !service.sendChatCompletion(messages, json::array(), 0.2, {}, level).contains("error"));
        QVERIFY(!service.sendChatCompletionTo(model, messages, json::array(), 0.2, {}, level)
                     .contains("error"));

        bool done = false, succeeded = false;
        service.sendRequestAsync(
            QStringLiteral("hello"),
            [&](const LLMResponse &response) {
                succeeded = response.success;
                done      = true;
            },
            QString(),
            0.2,
            false,
            level);
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        QVERIFY(succeeded);
        done = false;
        service.sendRequestAsync(QStringLiteral("hello"), [&](const LLMResponse &response) {
            succeeded = response.success;
            done      = true;
        });
        QTRY_VERIFY_WITH_TIMEOUT(done, 10000);
        QVERIFY(succeeded);

        QSignalSpy completed(&service, &QLLMService::streamComplete);
        QSignalSpy failed(&service, &QLLMService::streamError);
        service.sendChatCompletionStream(messages, json::array(), 0.2, level);
        QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty() || !failed.isEmpty(), 10000);
        QCOMPARE(failed.size(), 0);
        completed.clear();
        service.sendChatCompletionStream(messages);
        QTRY_VERIFY_WITH_TIMEOUT(!completed.isEmpty() || !failed.isEmpty(), 10000);
        QCOMPARE(failed.size(), 0);

        const auto requests = mock.requests();
        QCOMPARE(requests.size(), 8);
        for (const auto &request : requests) {
            QCOMPARE(request.at("max_tokens").get<int>(), model.maxOutputTokens);
            if (messagesApi) {
                QCOMPARE(
                    request.at("output_config").at("effort").get<std::string>(),
                    level.toStdString());
                QCOMPARE(
                    request.at("thinking").at("type").get<std::string>(), std::string("adaptive"));
                QVERIFY(!request.contains("reasoning_effort"));
            } else {
                QCOMPARE(request.at("reasoning_effort").get<std::string>(), level.toStdString());
                QVERIFY(!request.contains("reasoning"));
                QVERIFY(!request.contains("output_config"));
            }
            QVERIFY(!request.contains("temperature"));
        }
    }

    void rejectsBeforePosting_data()
    {
        QTest::addColumn<bool>("messagesApi");
        QTest::newRow("chat") << false;
        QTest::newRow("messages") << true;
    }

    void rejectsBeforePosting()
    {
        QFETCH(bool, messagesApi);
        QSocTestLlmMock mock;
        QVERIFY(mock.start({}));
        auto model         = endpoint(mock, messagesApi);
        model.effortLevels = {"low", "high", "max"};
        QLLMService service;
        service.setModel(model);
        const QString unsupported = QStringLiteral("medium");
        const auto chat = service.sendChatCompletion(messages, json::array(), 0.2, {}, unsupported);
        QVERIFY(chat.contains("error"));
        QVERIFY(QString::fromStdString(chat.at("error").get<std::string>()).contains("medium"));
        QVERIFY(service.sendChatCompletionTo(model, messages, json::array(), 0.2, {}, unsupported)
                    .contains("error"));
        QSignalSpy failed(&service, &QLLMService::streamError);
        service.sendChatCompletionStream(messages, json::array(), 0.2, unsupported);
        QCOMPARE(failed.size(), 1);
        bool rejected = false;
        service.sendRequestAsync(
            QStringLiteral("hello"),
            [&](const LLMResponse &response) {
                rejected = !response.success && response.errorMessage.contains("medium");
            },
            QString(),
            0.2,
            false,
            unsupported);
        QVERIFY(rejected);
        QString error;
        QVERIFY(!service.countTokens(model, messages, json::array(), unsupported, {}, &error));
        QVERIFY(error.contains("medium"));
        model.effort = unsupported;
        service.setModel(model);
        QVERIFY(!service.sendRequest(QStringLiteral("hello")).success);
        QCOMPARE(mock.requests().size(), 0);
    }

    void explicitOffDoesNotUseTheDefault_data() { rejectsBeforePosting_data(); }

    void explicitOffDoesNotUseTheDefault()
    {
        QFETCH(bool, messagesApi);
        QSocTestLlmMock mock;
        QVERIFY(mock.start({}));
        auto model         = endpoint(mock, messagesApi);
        model.effortLevels = {"off", "high"};
        QLLMService service;
        service.setModel(model);
        QVERIFY(!service.sendChatCompletion(messages).contains("error"));
        QVERIFY(!service.sendChatCompletion(messages, json::array(), 0.2, {}, QString())
                     .contains("error"));
        QVERIFY(!service.sendChatCompletion(messages, json::array(), 0.2, {}, QStringLiteral("off"))
                     .contains("error"));
        const auto requests = mock.requests();
        QCOMPARE(requests.size(), 3);
        const char *field = messagesApi ? "thinking" : "reasoning_effort";
        QVERIFY(requests.at(0).contains(field));
        QVERIFY(!requests.at(1).contains(field));
        QVERIFY(!requests.at(2).contains(field));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qllmeffort.moc"
