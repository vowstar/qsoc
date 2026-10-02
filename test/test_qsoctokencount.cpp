// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocrequestusage.h"
#include "common/qllmservice.h"
#include "common/qsocconfig.h"
#include "qsoc_test.h"

#include <cmath>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

using json = nlohmann::json;

namespace {

/* Write the yaml under a fresh XDG_CONFIG_HOME so QSocConfig reads it
 * instead of the developer's real ~/.config/qsoc/qsoc.yml. */
class ScopedConfig
{
public:
    explicit ScopedConfig(const QByteArray &yaml)
    {
        const QString directory = tempDir.filePath(QStringLiteral("qsoc"));
        QDir().mkpath(directory);
        QFile file(QDir(directory).filePath(QStringLiteral("qsoc.yml")));
        if (file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            file.write(yaml);
        }
        previousXdg = qEnvironmentVariable("XDG_CONFIG_HOME");
        qputenv("XDG_CONFIG_HOME", tempDir.path().toUtf8());
    }
    ~ScopedConfig()
    {
        if (previousXdg.isEmpty()) {
            qunsetenv("XDG_CONFIG_HOME");
        } else {
            qputenv("XDG_CONFIG_HOME", previousXdg.toUtf8());
        }
    }
    ScopedConfig(const ScopedConfig &)            = delete;
    ScopedConfig &operator=(const ScopedConfig &) = delete;

private:
    QTemporaryDir tempDir;
    QString       previousXdg;
};

QSocRequestSnapshot request(QSocTokenizer::Mode counter)
{
    QSocRequestSnapshot result;
    result.route    = QStringLiteral("route");
    result.counter  = counter;
    result.messages = json::array(
        {{{"role", "system"}, {"content", "Follow the task."}},
         {{"role", "user"}, {"content", "Count the registers in the clock domain."}}});
    return result;
}

class Test final : public QObject
{
    Q_OBJECT
private slots:
    void tokenizerSettingIsValidated()
    {
        ScopedConfig scope(
            "llm:\n"
            "  model: plain\n"
            "  models:\n"
            "    plain:\n"
            "      url: http://127.0.0.1:9/v1/chat/completions\n"
            "    local:\n"
            "      url: http://127.0.0.1:9/v1/chat/completions\n"
            "      tokenizer: o200k\n"
            "    rough:\n"
            "      url: http://127.0.0.1:9/v1/chat/completions\n"
            "      tokenizer: BYTES\n"
            "    typo:\n"
            "      url: http://127.0.0.1:9/v1/chat/completions\n"
            "      tokenizer: o200\n");
        QSocTestCapture capture;
        QSocConfig      config;
        QLLMService     llm(nullptr, &config);
        QCOMPARE(llm.getModelConfig(QStringLiteral("plain")).tokenizer, QStringLiteral("auto"));
        QCOMPARE(llm.getModelConfig(QStringLiteral("local")).tokenizer, QStringLiteral("o200k"));
        QCOMPARE(llm.getModelConfig(QStringLiteral("rough")).tokenizer, QStringLiteral("bytes"));
        QCOMPARE(llm.getModelConfig(QStringLiteral("typo")).tokenizer, QStringLiteral("auto"));
        QVERIFY(capture.text().contains(QStringLiteral("invalid tokenizer")));
        QCOMPARE(QSocRequestUsage::counterFor(QStringLiteral("bytes")), QSocTokenizer::Mode::Bytes);
        QCOMPARE(QSocRequestUsage::counterFor(QStringLiteral("auto")), QSocTokenizer::Mode::O200k);
    }

    void anchoredEstimateWidensOnlyTheLocalPart()
    {
        QSocRequestUsage usage;
        const auto       first = request(QSocTokenizer::Mode::O200k);
        QVERIFY(usage.complete(usage.begin(first), {{"prompt_tokens", 2000}}));
        const auto exact = usage.estimate(first);
        QCOMPARE(exact.reported, qint64(2000));
        QCOMPARE(exact.counted, qint64(0));
        QCOMPARE(exact.upper(), qint64(2000));
        QVERIFY(!exact.approximate());

        auto next = first;
        next.messages.push_back({{"role", "assistant"}, {"content", "Twelve registers."}});
        const auto   grown = usage.estimate(next);
        const qint64 local = QSocRequestUsage::estimateMessages(
            json::array({next.messages.back()}), next.imageTokens);
        QCOMPARE(grown.reported, qint64(2000));
        QCOMPARE(grown.counted, local);
        QCOMPARE(grown.point(), 2000 + local);
        QCOMPARE(grown.upper(), 2000 + qint64(std::ceil(local * 1.3)));
        QVERIFY(grown.approximate());
    }

    void unanchoredEstimateUsesTheCounterMargin()
    {
        const QSocRequestUsage usage;
        for (const auto counter : {QSocTokenizer::Mode::O200k, QSocTokenizer::Mode::Bytes}) {
            const auto snapshot = request(counter);
            const auto estimate = usage.estimate(snapshot);
            const auto margin   = counter == QSocTokenizer::Mode::Bytes ? 1.5 : 1.3;
            QCOMPARE(estimate.reported, qint64(0));
            QCOMPARE(estimate.counted, QSocRequestUsage::estimateRequest(snapshot));
            QCOMPARE(estimate.margin, margin);
            QCOMPARE(estimate.upper(), qint64(std::ceil(estimate.counted * margin)));
        }
        QVERIFY(
            QSocRequestUsage::estimateRequest(request(QSocTokenizer::Mode::Bytes))
            != QSocRequestUsage::estimateRequest(request(QSocTokenizer::Mode::O200k)));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoctokencount.moc"
