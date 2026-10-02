// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsoctool.h"
#include "common/qllmanthropic.h"
#include "qsoc_test.h"
#include "qsoc_test_pty.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace {

using namespace QSocTestPty;
using QLLMAnthropic::StreamDecoder;

/* The mock speaking /v1/messages, one process per test. */
struct Mock
{
    QTemporaryDir dir{QDir::tempPath() + QStringLiteral("/test_qsoc_anthropic_XXXXXX")};
    std::unique_ptr<BoundedProcess> process = std::make_unique<BoundedProcess>();
    int                             port    = 0;

    QString log() const { return QDir(dir.path()).filePath(QStringLiteral("requests.jsonl")); }

    bool start(const QMap<QString, QString> &variables, const QString &failMode = {})
    {
        port = pickFreePort();
        if (!dir.isValid() || port <= 0) {
            return false;
        }
        auto environment = isolatedEnvironment(dir.path());
        environment.insert(QStringLiteral("MOCK_TTL"), QStringLiteral("120"));
        environment.insert(QStringLiteral("MOCK_REQUEST_LOG"), log());
        for (auto it = variables.constBegin(); it != variables.constEnd(); ++it) {
            environment.insert(it.key(), it.value());
        }
        process->setProcessEnvironment(environment);
        process->setStandardOutputFile(QDir(dir.path()).filePath(QStringLiteral("mock.out")));
        process->setStandardErrorFile(QDir(dir.path()).filePath(QStringLiteral("mock.err")));
        QStringList arguments{QString::number(port)};
        if (!failMode.isEmpty()) {
            arguments.append(failMode);
        }
        process->start(QString::fromUtf8(QSOC_MOCK_LLM_PATH), arguments);
        return process->waitForStarted(5000) && waitForMockReady(*process, port, 45000);
    }

    LLMModelConfig model() const
    {
        LLMModelConfig config;
        config.id            = QStringLiteral("mock");
        config.name          = config.id;
        config.model         = QStringLiteral("claude-mock");
        config.url           = QStringLiteral("http://127.0.0.1:%1/v1/messages").arg(port);
        config.api           = LLMApi::AnthropicMessages;
        config.timeout       = 20000;
        config.contextTokens = 131072;
        return config;
    }

    QList<json> requests() const
    {
        QFile file(log());
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        QList<json> out;
        for (const QByteArray &line : file.readAll().split('\n')) {
            if (!line.trimmed().isEmpty()) {
                out.append(json::parse(line.toStdString()));
            }
        }
        return out;
    }
};

/* Stream one request; the error text stays empty on success. */
void streamError(QLLMService &service, QString &error)
{
    bool done = false;
    QObject::connect(&service, &QLLMService::streamError, &service, [&](const QString &text) {
        error = text;
        done  = true;
    });
    QObject::connect(&service, &QLLMService::streamComplete, &service, [&](const json &) {
        done = true;
    });
    service.sendChatCompletionStream(json::array({{{"role", "user"}, {"content", "hi"}}}));
    QTRY_VERIFY_WITH_TIMEOUT(done, 15000);
}

LLMModelConfig endpoint(int maxOutput = 4096)
{
    LLMModelConfig config;
    config.model           = QStringLiteral("claude-mock");
    config.contextTokens   = 200000;
    config.maxOutputTokens = maxOutput;
    config.api             = LLMApi::AnthropicMessages;
    return config;
}

json build(const json &messages, const json &tools = json::array(), const QString &effort = {})
{
    QLLMAnthropic::RequestOptions options;
    options.effort = effort;
    return QLLMAnthropic::buildRequest(messages, tools, endpoint(), options);
}

json toolCall(const std::string &id, const std::string &arguments)
{
    return {
        {"id", id},
        {"type", "function"},
        {"function", {{"name", "read_file"}, {"arguments", arguments}}}};
}

json feedAll(StreamDecoder &decoder, const json &events, QString *text, QString *reasoning)
{
    StreamDecoder::Delta delta;
    QString              error;
    for (const json &event : events) {
        const auto status = decoder.feed(event, &delta, &error);
        if (status == StreamDecoder::Status::Malformed || status == StreamDecoder::Status::Error) {
            return {{"failed", error.toStdString()}};
        }
        *text += delta.text;
        *reasoning += delta.reasoning;
    }
    return decoder.response();
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void systemMovesToTopLevelWithCacheBreakpoints()
    {
        const json body = build(
            json::array(
                {{{"role", "system"}, {"content", "rules"}},
                 {{"role", "user"}, {"content", "hello"}},
                 {{"role", "assistant"}, {"content", "hi"}},
                 {{"role", "system"}, {"content", "late reminder"}},
                 {{"role", "user"}, {"content", "go on"}}}));
        QCOMPARE(body["system"].size(), size_t(1));
        QCOMPARE(body["system"][0]["text"].get<std::string>(), std::string("rules"));
        QVERIFY(body["system"][0].contains("cache_control"));
        const json &messages = body["messages"];
        QCOMPARE(messages.size(), size_t(3));
        QCOMPARE(messages[2]["role"].get<std::string>(), std::string("user"));
        QCOMPARE(messages[2]["content"].size(), size_t(2));
        QCOMPARE(messages[2]["content"][0]["text"].get<std::string>(), std::string("late reminder"));
        QVERIFY(!messages[2]["content"][0].contains("cache_control"));
        QVERIFY(messages[2]["content"][1].contains("cache_control"));
        QVERIFY(!messages[0]["content"][0].contains("cache_control"));
        QVERIFY(!body.contains("anthropic_version"));
    }

    void consecutiveToolResultsMergeWithFollowingUserText()
    {
        const json body = build(
            json::array(
                {{{"role", "user"}, {"content", "read both"}},
                 {{"role", "assistant"},
                  {"content", nullptr},
                  {"tool_calls",
                   json::array({toolCall("a", R"({"path":"x"})"), toolCall("b", "{}")})}},
                 {{"role", "tool"}, {"tool_call_id", "a"}, {"content", "one"}},
                 {{"role", "tool"}, {"tool_call_id", "b"}, {"content", "two"}},
                 {{"role", "user"}, {"content", "now summarize"}}}));
        const json &messages = body["messages"];
        QCOMPARE(messages.size(), size_t(3));
        const json &assistant = messages[1]["content"];
        QCOMPARE(assistant.size(), size_t(2));
        QCOMPARE(assistant[0]["type"].get<std::string>(), std::string("tool_use"));
        QCOMPARE(assistant[0]["input"], json({{"path", "x"}}));
        QCOMPARE(assistant[1]["input"], json::object());
        const json &user = messages[2]["content"];
        QCOMPARE(user.size(), size_t(3));
        QCOMPARE(user[0]["type"].get<std::string>(), std::string("tool_result"));
        QCOMPARE(user[0]["tool_use_id"].get<std::string>(), std::string("a"));
        QCOMPARE(user[0]["content"].get<std::string>(), std::string("one"));
        QCOMPARE(user[1]["tool_use_id"].get<std::string>(), std::string("b"));
        QCOMPARE(user[2]["text"].get<std::string>(), std::string("now summarize"));
    }

    void missingToolResultIsSynthesized()
    {
        const json body = build(
            json::array(
                {{{"role", "user"}, {"content", "read"}},
                 {{"role", "assistant"},
                  {"content", "checking"},
                  {"tool_calls", json::array({toolCall("a", "{}"), toolCall("b", "{}")})}},
                 {{"role", "tool"}, {"tool_call_id", "a"}, {"content", "one"}},
                 {{"role", "user"}, {"content", "stop"}}}));
        const json &user = body["messages"][2]["content"];
        QCOMPARE(user.size(), size_t(3));
        QCOMPARE(user[1]["tool_use_id"].get<std::string>(), std::string("b"));
        QVERIFY(user[1]["content"].get<std::string>().find("Not executed") != std::string::npos);
        QCOMPARE(user[2]["text"].get<std::string>(), std::string("stop"));
    }

    void orphanToolResultBecomesText()
    {
        const json body = build(
            json::array(
                {{{"role", "user"}, {"content", "hi"}},
                 {{"role", "tool"}, {"tool_call_id", "ghost"}, {"content", "late"}}}));
        const json &user = body["messages"][0]["content"];
        QCOMPARE(user.size(), size_t(2));
        QCOMPARE(user[1]["type"].get<std::string>(), std::string("text"));
        QVERIFY(user[1]["text"].get<std::string>().find("ghost") != std::string::npos);
    }

    void signedThinkingReplaysVerbatim()
    {
        const json details = json::array(
            {{{"type", "reasoning.text"},
              {"text", "plan"},
              {"signature", "sig-1"},
              {"format", "anthropic-claude-v1"}},
             {{"type", "reasoning.encrypted"}, {"data", "opaque"}, {"format", "anthropic-claude-v1"}},
             {{"type", "reasoning.text"}, {"text", "unsigned"}, {"format", "anthropic-claude-v1"}},
             {{"type", "reasoning.text"}, {"text", "foreign"}, {"signature", "x"}}});
        const json body = build(
            json::array(
                {{{"role", "user"}, {"content", "go"}},
                 {{"role", "assistant"},
                  {"content", nullptr},
                  {"reasoning_content", "plan"},
                  {"reasoning_details", details},
                  {"tool_calls", json::array({toolCall("a", "{}")})}},
                 {{"role", "tool"}, {"tool_call_id", "a"}, {"content", "done"}}}));
        const json &assistant = body["messages"][1]["content"];
        QCOMPARE(assistant.size(), size_t(3));
        QCOMPARE(
            assistant[0],
            json({{"type", "thinking"}, {"thinking", "plan"}, {"signature", "sig-1"}}));
        QCOMPARE(assistant[1], json({{"type", "redacted_thinking"}, {"data", "opaque"}}));
        QCOMPARE(assistant[2]["type"].get<std::string>(), std::string("tool_use"));
    }

    void imagesBecomeSources()
    {
        const json content = json::array(
            {{{"type", "text"}, {"text", "look"}},
             {{"type", "image_url"}, {"image_url", {{"url", "data:image/png;base64,QUJD"}}}},
             {{"type", "image_url"}, {"image_url", {{"url", "https://example.com/a.png"}}}}});
        const json  body  = build(json::array({{{"role", "user"}, {"content", content}}}));
        const json &parts = body["messages"][0]["content"];
        QCOMPARE(parts.size(), size_t(3));
        QCOMPARE(
            parts[1]["source"],
            json({{"type", "base64"}, {"media_type", "image/png"}, {"data", "QUJD"}}));
        QCOMPARE(parts[2]["source"], json({{"type", "url"}, {"url", "https://example.com/a.png"}}));
    }

    void toolsUseInputSchema()
    {
        const json tools = json::array(
            {{{"type", "function"},
              {"function",
               {{"name", "read_file"},
                {"description", "Read"},
                {"parameters",
                 {{"type", "object"}, {"properties", {{"path", {{"type", "string"}}}}}}}}}}});
        const json body = build(json::array({{{"role", "user"}, {"content", "x"}}}), tools);
        QCOMPARE(body["tools"].size(), size_t(1));
        QCOMPARE(body["tools"][0]["name"].get<std::string>(), std::string("read_file"));
        QCOMPARE(body["tools"][0]["input_schema"]["type"].get<std::string>(), std::string("object"));
        QVERIFY(!body["tools"][0].contains("function"));
    }

    void effortSelectsAdaptiveThinking()
    {
        const json messages = json::array({{{"role", "user"}, {"content", "x"}}});
        const json off      = build(messages);
        QVERIFY(!off.contains("thinking"));
        QVERIFY(!off.contains("output_config"));
        QVERIFY(off.contains("temperature"));
        const json high = build(messages, json::array(), QStringLiteral("high"));
        QCOMPARE(high["thinking"], json({{"type", "adaptive"}}));
        QCOMPARE(high["output_config"], json({{"effort", "high"}}));
        QVERIFY(!high.contains("temperature"));
    }

    void maxTokensFollowsConfigOrContext()
    {
        const json messages = json::array({{{"role", "user"}, {"content", "x"}}});
        QLLMAnthropic::RequestOptions options;
        QCOMPARE(
            QLLMAnthropic::buildRequest(
                messages, json::array(), endpoint(512), options)["max_tokens"]
                .get<int>(),
            512);
        const json body = QLLMAnthropic::buildRequest(messages, json::array(), endpoint(0), options);
        const int value = body["max_tokens"].get<int>();
        QVERIFY(value < 200000);
        QVERIFY(value > 200000 - 1000);

        LLMModelConfig withImage = endpoint(0);
        withImage.imageMaxTokens = 3000;
        const json image         = json::array(
            {{{"role", "user"},
              {"content",
               json::array(
                   {{{"type", "image_url"},
                     {"image_url",
                      {{"url", "data:image/png;base64," + std::string(400000, 'A')}}}}})}}});
        const int imageCap
            = QLLMAnthropic::buildRequest(image, json::array(), withImage, options)["max_tokens"]
                  .get<int>();
        QVERIFY(imageCap < 200000 - 3000);
        QVERIFY(imageCap > 200000 - 4000);
    }

    void usageIncludesCache()
    {
        const json usage = QLLMAnthropic::toChatUsage(
            {{"input_tokens", 10},
             {"cache_read_input_tokens", 100},
             {"cache_creation_input_tokens", 5},
             {"output_tokens", 7}});
        QCOMPARE(usage["prompt_tokens"].get<int>(), 115);
        QCOMPARE(usage["completion_tokens"].get<int>(), 7);
        QCOMPARE(usage["total_tokens"].get<int>(), 122);
        QCOMPARE(usage["prompt_tokens_details"]["cached_tokens"].get<int>(), 100);
    }

    void stopReasonMaps_data()
    {
        QTest::addColumn<QString>("stop");
        QTest::addColumn<QString>("finish");
        QTest::newRow("end_turn") << "end_turn" << "stop";
        QTest::newRow("tool_use") << "tool_use" << "tool_calls";
        QTest::newRow("max_tokens") << "max_tokens" << "length";
        QTest::newRow("window") << "model_context_window_exceeded" << "length";
        QTest::newRow("refusal") << "refusal" << "content_filter";
        QTest::newRow("pause_turn") << "pause_turn" << "stop";
        QTest::newRow("unknown") << "future_reason" << "stop";
    }

    void stopReasonMaps()
    {
        QFETCH(QString, stop);
        QFETCH(QString, finish);
        QCOMPARE(QString::fromStdString(QLLMAnthropic::toFinishReason(stop.toStdString())), finish);
    }

    void streamAccumulatesThinkingAndToolInput()
    {
        const json events = json::array(
            {{{"type", "message_start"},
              {"message", {{"usage", {{"input_tokens", 3}, {"cache_read_input_tokens", 4}}}}}},
             {{"type", "content_block_start"},
              {"index", 0},
              {"content_block", {{"type", "thinking"}, {"thinking", ""}}}},
             {{"type", "content_block_delta"},
              {"index", 0},
              {"delta", {{"type", "thinking_delta"}, {"thinking", "think "}}}},
             {{"type", "content_block_delta"},
              {"index", 0},
              {"delta", {{"type", "thinking_delta"}, {"thinking", "hard"}}}},
             {{"type", "content_block_delta"},
              {"index", 0},
              {"delta", {{"type", "signature_delta"}, {"signature", "sig"}}}},
             {{"type", "content_block_stop"}, {"index", 0}},
             {{"type", "content_block_start"},
              {"index", 1},
              {"content_block", {{"type", "redacted_thinking"}, {"data", "opaque"}}}},
             {{"type", "content_block_stop"}, {"index", 1}},
             {{"type", "ping"}},
             {{"type", "content_block_start"},
              {"index", 2},
              {"content_block", {{"type", "text"}, {"text", ""}}}},
             {{"type", "content_block_delta"},
              {"index", 2},
              {"delta", {{"type", "text_delta"}, {"text", "Reading."}}}},
             {{"type", "content_block_stop"}, {"index", 2}},
             {{"type", "content_block_start"},
              {"index", 3},
              {"content_block",
               {{"type", "tool_use"},
                {"id", "toolu_1"},
                {"name", "read_file"},
                {"input", json::object()}}}},
             {{"type", "content_block_delta"},
              {"index", 3},
              {"delta", {{"type", "input_json_delta"}, {"partial_json", "{\"pa"}}}},
             {{"type", "content_block_delta"},
              {"index", 3},
              {"delta", {{"type", "input_json_delta"}, {"partial_json", "th\":\"x\"}"}}}},
             {{"type", "content_block_stop"}, {"index", 3}},
             {{"type", "content_block_start"},
              {"index", 4},
              {"content_block",
               {{"type", "tool_use"},
                {"id", "toolu_2"},
                {"name", "list"},
                {"input", json::object()}}}},
             {{"type", "content_block_stop"}, {"index", 4}},
             {{"type", "message_delta"},
              {"delta", {{"stop_reason", "tool_use"}}},
              {"usage", {{"output_tokens", 9}}}}});
        StreamDecoder decoder;
        QString       text;
        QString       reasoning;
        const json    response = feedAll(decoder, events, &text, &reasoning);
        QVERIFY(!response.contains("failed"));
        StreamDecoder::Delta delta;
        QString              error;
        QCOMPARE(
            decoder.feed({{"type", "message_stop"}}, &delta, &error), StreamDecoder::Status::Done);

        QCOMPARE(text, QStringLiteral("Reading."));
        QCOMPARE(reasoning, QStringLiteral("think hard"));
        const json &choice  = response["choices"][0];
        const json &message = choice["message"];
        QCOMPARE(choice["finish_reason"].get<std::string>(), std::string("tool_calls"));
        QCOMPARE(message["content"].get<std::string>(), std::string("Reading."));
        QCOMPARE(message["reasoning_content"].get<std::string>(), std::string("think hard"));
        QCOMPARE(message["reasoning_details"][0]["signature"].get<std::string>(), std::string("sig"));
        QCOMPARE(message["reasoning_details"][1]["data"].get<std::string>(), std::string("opaque"));
        QCOMPARE(
            message["tool_calls"][0]["function"]["arguments"].get<std::string>(),
            std::string(R"({"path":"x"})"));
        QCOMPARE(
            message["tool_calls"][1]["function"]["arguments"].get<std::string>(), std::string("{}"));
        QCOMPARE(response["usage"]["prompt_tokens"].get<int>(), 7);
        QCOMPARE(response["usage"]["completion_tokens"].get<int>(), 9);
        QVERIFY(QLLMService::extractAssistantMessage(response, nullptr));

        /* The stored message replays as the same blocks. */
        const json replay = build(
            json::array(
                {{{"role", "user"}, {"content", "go"}},
                 message,
                 {{"role", "tool"}, {"tool_call_id", "toolu_1"}, {"content", "x"}},
                 {{"role", "tool"}, {"tool_call_id", "toolu_2"}, {"content", "y"}}}));
        const json &blocks = replay["messages"][1]["content"];
        QCOMPARE(blocks.size(), size_t(5));
        QCOMPARE(
            blocks[0],
            json({{"type", "thinking"}, {"thinking", "think hard"}, {"signature", "sig"}}));
        QCOMPARE(blocks[1], json({{"type", "redacted_thinking"}, {"data", "opaque"}}));
        QCOMPARE(blocks[2]["text"].get<std::string>(), std::string("Reading."));
        QCOMPARE(blocks[3]["input"], json({{"path", "x"}}));
        QCOMPARE(blocks[4]["input"], json::object());
    }

    void streamRejectsDeltaWithoutBlock()
    {
        StreamDecoder        decoder;
        StreamDecoder::Delta delta;
        QString              error;
        QCOMPARE(
            decoder.feed(
                {{"type", "content_block_delta"},
                 {"index", 0},
                 {"delta", {{"type", "text_delta"}, {"text", "x"}}}},
                &delta,
                &error),
            StreamDecoder::Status::Malformed);
    }

    void errorEventCarriesStatus_data()
    {
        QTest::addColumn<QString>("type");
        QTest::addColumn<QString>("expected");
        QTest::newRow("overloaded") << "overloaded_error" << "[HTTP 529] Overloaded";
        QTest::newRow("rate") << "rate_limit_error" << "[HTTP 429] Overloaded";
        QTest::newRow("api") << "api_error" << "[HTTP 500] Overloaded";
        QTest::newRow("unknown") << "brand_new_error" << "Overloaded";
    }

    void errorEventCarriesStatus()
    {
        QFETCH(QString, type);
        QFETCH(QString, expected);
        StreamDecoder        decoder;
        StreamDecoder::Delta delta;
        QString              error;
        QCOMPARE(
            decoder.feed(
                {{"type", "error"},
                 {"error", {{"type", type.toStdString()}, {"message", "Overloaded"}}}},
                &delta,
                &error),
            StreamDecoder::Status::Error);
        QCOMPARE(error, expected);
    }

    void syncReplyConverts()
    {
        const json reply
            = {{"type", "message"},
               {"content",
                json::array(
                    {{{"type", "thinking"}, {"thinking", "t"}, {"signature", "s"}},
                     {{"type", "text"}, {"text", "answer"}}})},
               {"stop_reason", "max_tokens"},
               {"usage", {{"input_tokens", 2}, {"output_tokens", 3}}}};
        const json response = QLLMAnthropic::toChatResponse(reply);
        QCOMPARE(response["choices"][0]["finish_reason"].get<std::string>(), std::string("length"));
        QCOMPARE(
            response["choices"][0]["message"]["content"].get<std::string>(), std::string("answer"));
        QCOMPARE(response["usage"]["prompt_tokens"].get<int>(), 2);
        const json error = QLLMAnthropic::toChatResponse(
            {{"type", "error"},
             {"error", {{"type", "invalid_request_error"}, {"message", "prompt is too long"}}}});
        QCOMPARE(error["error"].get<std::string>(), std::string("[HTTP 400] prompt is too long"));
    }

    void agentToolLoopReplaysSignedThinking()
    {
        Mock mock;
        QVERIFY(mock.start(
            {{QStringLiteral("MOCK_REASONING"), QStringLiteral("weighing the options")},
             {QStringLiteral("MOCK_TOOL_NAME"), QStringLiteral("no_such_tool")},
             {QStringLiteral("MOCK_TOOL_ARGS"), QStringLiteral(R"({"path":"a.txt","n":1})")},
             {QStringLiteral("MOCK_TOOL_MAX"), QStringLiteral("1")}}));
        QLLMService service;
        service.setModel(mock.model());
        QString thinking;
        connect(&service, &QLLMService::streamReasoningChunk, this, [&thinking](const QString &c) {
            thinking += c;
        });
        QSocToolRegistry registry;
        QSocAgentConfig  config;
        config.autoLoadMemory      = false;
        config.memoryRecallEnabled = false;
        config.maxRetries          = 0;
        config.effortLevel         = QStringLiteral("high");
        QSocAgent agent(nullptr, &service, &registry, config);
        QString   result;
        QString   failure;
        connect(&agent, &QSocAgent::runComplete, this, [&result](const QString &text) {
            result = text;
        });
        connect(&agent, &QSocAgent::runError, this, [&failure](const QString &text) {
            failure = text;
        });
        agent.runStream(QStringLiteral("use a tool"));
        QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty() || !failure.isEmpty(), 20000);
        QVERIFY2(failure.isEmpty(), qPrintable(failure));
        QCOMPARE(result, QStringLiteral("DONE"));
        QCOMPARE(thinking, QStringLiteral("weighing the optionsweighing the options"));

        const QList<json> requests = mock.requests();
        QCOMPARE(requests.size(), 2);
        for (const json &request : requests) {
            QCOMPARE(request["thinking"], json({{"type", "adaptive"}}));
            QCOMPARE(request["output_config"], json({{"effort", "high"}}));
            QVERIFY(request["max_tokens"].get<int>() > 0);
            QVERIFY(request.contains("system"));
        }
        const json &messages  = requests.at(1)["messages"];
        const json &assistant = messages.at(messages.size() - 2);
        QCOMPARE(assistant["role"].get<std::string>(), std::string("assistant"));
        QCOMPARE(
            assistant["content"][0],
            json(
                {{"type", "thinking"},
                 {"thinking", "weighing the options"},
                 {"signature", "mock-signature-1"}}));
        QCOMPARE(assistant["content"][1]["type"].get<std::string>(), std::string("tool_use"));
        QCOMPARE(assistant["content"][1]["input"], json({{"path", "a.txt"}, {"n", 1}}));
        const json &user = messages.back()["content"];
        QCOMPARE(user[0]["type"].get<std::string>(), std::string("tool_result"));
        QCOMPARE(user[0]["tool_use_id"].get<std::string>(), std::string("toolu_1"));
    }

    void streamFailuresKeepTheirStatus_data()
    {
        QTest::addColumn<QString>("variable");
        QTest::addColumn<QString>("value");
        QTest::addColumn<QString>("failMode");
        QTest::addColumn<QString>("prefix");
        QTest::addColumn<QString>("phrase");
        QTest::newRow("error-event") << "MOCK_STREAM_ERROR" << "overloaded_error" << ""
                                     << "[HTTP 529]" << "Overloaded";
        QTest::newRow("http-529") << "MOCK_FAIL_CODE" << "529" << "always" << "[HTTP 529]"
                                  << "overloaded_error";
        QTest::newRow("overflow") << "MOCK_OVERFLOW_BYTES" << "16" << "" << "[HTTP 400]"
                                  << "prompt is too long";
    }

    void streamFailuresKeepTheirStatus()
    {
        QFETCH(QString, variable);
        QFETCH(QString, value);
        QFETCH(QString, failMode);
        QFETCH(QString, prefix);
        QFETCH(QString, phrase);
        Mock mock;
        QVERIFY(mock.start({{variable, value}}, failMode));
        QLLMService service;
        service.setModel(mock.model());
        QString error;
        streamError(service, error);
        QVERIFY2(error.startsWith(prefix), qPrintable(error));
        QVERIFY2(error.contains(phrase), qPrintable(error));
    }

    void syncPathsSpeakMessages()
    {
        Mock mock;
        QVERIFY(mock.start({{QStringLiteral("MOCK_REASONING"), QStringLiteral("t")}}));
        QLLMService service;
        service.setModel(mock.model());
        const json response = service.sendChatCompletion(
            json::array(
                {{{"role", "system"}, {"content", "rules"}},
                 {{"role", "user"}, {"content", "hi"}}}));
        json message;
        QVERIFY(QLLMService::extractAssistantMessage(response, &message));
        QCOMPARE(message["content"].get<std::string>(), std::string("DONE"));
        QCOMPARE(
            message["reasoning_details"][0]["signature"].get<std::string>(),
            std::string("mock-signature-0"));

        const LLMResponse reply
            = service.sendRequest(QStringLiteral("map it"), QStringLiteral("sys"), 0.2, true);
        QVERIFY2(reply.success, qPrintable(reply.errorMessage));
        QCOMPARE(reply.content, QStringLiteral("DONE"));

        const QList<json> requests = mock.requests();
        QCOMPARE(requests.size(), 2);
        QCOMPARE(requests[0]["system"][0]["text"].get<std::string>(), std::string("rules"));
        QVERIFY(!requests[0].contains("thinking"));
        QVERIFY(requests[0].contains("temperature"));
        QVERIFY(!requests[1].contains("response_format"));
        QCOMPARE(requests[1]["system"].size(), size_t(2));
        QVERIFY(
            requests[1]["system"][1]["text"].get<std::string>().find("JSON") != std::string::npos);
    }

    void configSelectsApi()
    {
        QCOMPARE(llmApiFromName(QStringLiteral("openai-chat")), std::optional(LLMApi::OpenAIChat));
        QCOMPARE(
            llmApiFromName(QStringLiteral("anthropic-messages")),
            std::optional(LLMApi::AnthropicMessages));
        QVERIFY(!llmApiFromName(QStringLiteral("anthropic")).has_value());
        QCOMPARE(llmApiName(LLMApi::AnthropicMessages), QStringLiteral("anthropic-messages"));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qllmanthropic.moc"
