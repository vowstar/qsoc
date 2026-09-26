// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocrequestusage.h"
#include "qsoc_test.h"

#include <limits>
#include <QBuffer>
#include <QImage>
#include <QtTest>

using json = nlohmann::json;

namespace {
QSocRequestSnapshot request()
{
    QSocRequestSnapshot result;
    result.route    = QStringLiteral("runtime-route");
    result.effort   = QStringLiteral("high");
    result.messages = json::array(
        {{{"role", "system"}, {"content", "rules"}}, {{"role", "user"}, {"content", "question"}}});
    result.tools = json::array(
        {{{"type", "function"},
          {"function", {{"name", "probe"}, {"parameters", {{"type", "object"}}}}}}});
    return result;
}

class Test final : public QObject
{
    Q_OBJECT
private slots:
    void splitCacheCountsCannotAnchorTheFullInput()
    {
        QSocRequestUsage    usage;
        QSocRequestSnapshot request;
        request.messages = json::array({{{"role", "user"}, {"content", std::string(400, 'x')}}});
        const auto generation = usage.begin(request);
        QVERIFY(!usage.complete(
            generation,
            {{"input_tokens", 10},
             {"cache_read_input_tokens", 5},
             {"cache_creation_input_tokens", 20}}));
        QCOMPARE(usage.observed().requests, quint64(0));
        QCOMPARE(usage.estimateNext(request), QSocRequestUsage::estimateRequest(request));
        QVERIFY(usage.complete(
            usage.begin(request), {{"prompt_tokens", 35}, {"cache_read_input_tokens", 5}}));
        QCOMPARE(usage.observed().inputTokens, qint64(35));
        QCOMPARE(usage.observed().cachedTokens, qint64(5));
    }

    void arraysCountTextImagesAndCalls()
    {
        QImage image(16, 16, QImage::Format_RGB32);
        image.fill(Qt::green);
        QByteArray encoded;
        QBuffer    buffer(&encoded);
        QVERIFY(buffer.open(QIODevice::WriteOnly));
        QVERIFY(image.save(&buffer, "PNG"));
        const QString imageUrl = QStringLiteral("data:image/png;base64,")
                                 + QString::fromLatin1(encoded.toBase64());
        json          messages = json::array(
            {{{"role", "user"},
              {"content",
               json::array(
                   {{{"type", "text"}, {"text", "abcd"}},
                    {{"type", "image_url"}, {"image_url", {{"url", imageUrl.toStdString()}}}}})}}});
        QCOMPARE(QSocRequestUsage::estimateMessages(messages, 1000), qint64(1012));
        messages[0]["_usage"] = {{"prompt_tokens", 999999}};
        QCOMPARE(QSocRequestUsage::estimateMessages(messages, 1000), qint64(1012));
        messages[0]["content"][0]["text"] = std::string(404, 'a');
        QCOMPARE(QSocRequestUsage::estimateMessages(messages, 1000), qint64(1112));
        messages[0]["tool_calls"] = json::array(
            {{{"id", "call"},
              {"function", {{"name", "probe"}, {"arguments", std::string(400, 'x')}}}}});
        QVERIFY(QSocRequestUsage::estimateMessages(messages, 1000) > 1212);
    }

    void zeroUsageDoesNotReplaceTheRequestEstimate()
    {
        QSocRequestUsage usage;
        const auto       initial = request();
        QVERIFY(usage.complete(usage.begin(initial), {{"prompt_tokens", 0}}));
        QCOMPARE(usage.estimateNext(initial), QSocRequestUsage::estimateRequest(initial));
    }

    void historyKeepsTheAdmittedImageCost()
    {
        const json history = json::array(
            {{{"role", "user"},
              {"content",
               json::array(
                   {{{"type", "text"}, {"text", "caption"}},
                    {{"type", "image_url"}, {"image_url", json::object()}}})},
              {"_img_tokens", 120}}});
        const qint64 expected = 10 + QSocRequestUsage::estimateText(QStringLiteral("caption"))
                                + 120;
        QCOMPARE(QSocRequestUsage::estimateHistory(history), expected);
        QVERIFY(QSocRequestUsage::estimateMessages(history, 10000) > expected);
        QCOMPARE(QSocRequestUsage::estimateHistory(history), expected);
    }

    void anchorRequiresExactRequestPrefix()
    {
        QSocRequestUsage usage;
        const auto       initial    = request();
        const auto       generation = usage.begin(initial);
        QVERIFY(usage.complete(
            generation,
            {{"prompt_tokens", 2000},
             {"completion_tokens", 20},
             {"prompt_tokens_details", {{"cached_tokens", 1600}}}}));
        auto       next  = initial;
        const json reply = {{"role", "assistant"}, {"content", "reply"}};
        next.messages.push_back(reply);
        QCOMPARE(
            usage.estimateNext(next),
            2000 + QSocRequestUsage::estimateMessages(json::array({reply})));
        for (int changed = 0; changed < 6; ++changed) {
            auto different = next;
            switch (changed) {
            case 0:
                different.messages[0]["content"] = "new rules";
                break;
            case 1:
                different.tools.push_back({{"type", "function"}});
                break;
            case 2:
                different.route = QStringLiteral("different-route");
                break;
            case 3:
                different.effort = QStringLiteral("low");
                break;
            case 4:
                different.imageTokens += 1;
                break;
            case 5:
                different.messages.erase(different.messages.begin());
                break;
            }
            QCOMPARE(usage.estimateNext(different), QSocRequestUsage::estimateRequest(different));
        }
        usage.invalidateAnchor();
        QCOMPARE(usage.estimateNext(next), QSocRequestUsage::estimateRequest(next));
    }

    void schemaWireNumberRepresentationInvalidatesAnchor()
    {
        QSocRequestUsage usage;
        auto             initial                              = request();
        initial.tools[0]["function"]["parameters"]["minimum"] = 1;
        QVERIFY(usage.complete(usage.begin(initial), {{"prompt_tokens", 2000}}));
        auto different                                          = initial;
        different.tools[0]["function"]["parameters"]["minimum"] = 1.0;
        QCOMPARE(usage.estimateNext(different), QSocRequestUsage::estimateRequest(different));
    }

    void generationsAndMissingCacheAreNotRecounted()
    {
        QSocRequestUsage usage;
        const auto       first  = usage.begin(request());
        const auto       second = usage.begin(request());
        QVERIFY(!usage.complete(first, {{"prompt_tokens", 100}}));
        QVERIFY(usage.complete(second, {{"prompt_tokens", 200}, {"completion_tokens", 10}}));
        QVERIFY(!usage.complete(second, {{"prompt_tokens", 200}}));
        auto observed = usage.observed();
        QCOMPARE(observed.requests, quint64(1));
        QCOMPARE(observed.cacheReportedRequests, quint64(0));
        const auto third = usage.begin(request());
        usage.discardPending();
        QVERIFY(!usage.complete(third, {{"prompt_tokens", 300}}));
        QVERIFY(usage.complete(
            usage.begin(request()),
            {{"prompt_tokens", 100}, {"prompt_tokens_details", {{"cached_tokens", 0}}}}));
        observed = usage.observed();
        QCOMPARE(observed.inputTokens, qint64(300));
        QCOMPARE(observed.cacheEligibleInputTokens, qint64(100));
        QCOMPARE(observed.cacheReportedRequests, quint64(1));
        QCOMPARE(observed.cachedTokens, qint64(0));
    }

    void rejectsMalformedAndOverflowingUsage()
    {
        QSocRequestUsage  usage;
        const QList<json> invalid
            = {json::object(),
               {{"prompt_tokens", -1}},
               {{"prompt_tokens", 2.5}},
               {{"prompt_tokens", "100"}},
               {{"prompt_tokens", std::numeric_limits<quint64>::max()}},
               {{"prompt_tokens", 100}, {"completion_tokens", -1}}};
        for (const auto &value : invalid) {
            QVERIFY(!usage.complete(usage.begin(request()), value));
        }
        QCOMPARE(usage.observed().requests, quint64(0));
        QVERIFY(usage.complete(
            usage.begin(request()), {{"prompt_tokens", std::numeric_limits<qint64>::max()}}));
        QVERIFY(!usage.complete(usage.begin(request()), {{"prompt_tokens", 1}}));
        QCOMPARE(usage.observed().inputTokens, std::numeric_limits<qint64>::max());
        auto next = request();
        next.messages.push_back({{"role", "user"}, {"content", "tail"}});
        QCOMPARE(usage.estimateNext(next), std::numeric_limits<qint64>::max());
    }
};
} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocrequestusage.moc"
