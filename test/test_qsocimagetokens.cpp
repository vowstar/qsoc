// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocimagetokens.h"
#include "qsoc_test.h"

#include <QtTest>

using QSocImageTokens::estimateImageTokens;
using QSocImageTokens::parseProvider;
using QSocImageTokens::Provider;

class TestQSocImageTokens : public QObject
{
    Q_OBJECT

private slots:
    /* Anthropic uses (W*H)/750. Spot-check the canonical sizes a user
     * might inline (small icon up through Claude's 1568px ceiling). */
    void anthropicCanonicalSizes()
    {
        QCOMPARE(estimateImageTokens(256, 256, Provider::Anthropic), 87);
        QCOMPARE(estimateImageTokens(512, 512, Provider::Anthropic), 349);
        QCOMPARE(estimateImageTokens(768, 768, Provider::Anthropic), 786);
        QCOMPARE(estimateImageTokens(1024, 1024, Provider::Anthropic), 1398);
        QCOMPARE(estimateImageTokens(1568, 1568, Provider::Anthropic), 3278);
        QCOMPARE(estimateImageTokens(2048, 2048, Provider::Anthropic), 5592);
    }

    /* Anthropic 1080p screenshot: matches the documented "~2765 tokens"
     * intuition we used in the design doc, off by a few because the
     * docs round; this test pins the exact integer division result. */
    void anthropic1080pScreenshot()
    {
        QCOMPARE(estimateImageTokens(1920, 1080, Provider::Anthropic), 2764);
    }

    /* OpenAI low detail is dimension-independent and always 85. */
    void openaiLowFlatRate()
    {
        QCOMPARE(estimateImageTokens(64, 64, Provider::OpenAILow), 85);
        QCOMPARE(estimateImageTokens(1024, 1024, Provider::OpenAILow), 85);
        QCOMPARE(estimateImageTokens(8192, 8192, Provider::OpenAILow), 85);
    }

    /* OpenAI high detail: scale to fit 2048, then short side -> 768,
     * tile into 512x512. Each tile = 170 tokens, 85 base.
     *
     * 256x256: stays 256x256 (no upscale). 1 tile. 85 + 170 = 255.
     * 512x512: stays 512x512. 1 tile. 255.
     * 768x768: stays 768x768. 4 tiles (ceil(768/512)=2). 85 + 680 = 765.
     * 1024x1024: scales to 768x768 (short side capped). 4 tiles. 765.
     * 2048x2048: same as above after step-2 scale. 765.
     * 1920x1080 (16:9): step1 unchanged, step2 short=1080->768 so width
     *   becomes 1080*1920/1080... 1920*(768/1080)=1365. Tiles ceil(1365/512)=3
     *   x ceil(768/512)=2 = 6. 85 + 6*170 = 1105. */
    void openaiHighTilingMatrix()
    {
        QCOMPARE(estimateImageTokens(256, 256, Provider::OpenAIHigh), 255);
        QCOMPARE(estimateImageTokens(512, 512, Provider::OpenAIHigh), 255);
        QCOMPARE(estimateImageTokens(768, 768, Provider::OpenAIHigh), 765);
        QCOMPARE(estimateImageTokens(1024, 1024, Provider::OpenAIHigh), 765);
        QCOMPARE(estimateImageTokens(2048, 2048, Provider::OpenAIHigh), 765);
        QCOMPARE(estimateImageTokens(1920, 1080, Provider::OpenAIHigh), 1105);
    }

    /* Gemini: ≤384 -> 258. >384 -> tile by 768x768, 258/tile. */
    void geminiTiling()
    {
        QCOMPARE(estimateImageTokens(64, 64, Provider::Gemini), 258);
        QCOMPARE(estimateImageTokens(384, 384, Provider::Gemini), 258);
        /* 385x385 crosses the small-cell threshold and pays for one full tile. */
        QCOMPARE(estimateImageTokens(385, 385, Provider::Gemini), 258);
        /* 1024x1024 = ceil(1024/768)=2 tiles per side -> 4 * 258 = 1032. */
        QCOMPARE(estimateImageTokens(1024, 1024, Provider::Gemini), 1032);
        /* 1920x1080 -> 3 wide * 2 tall = 6 tiles. */
        QCOMPARE(estimateImageTokens(1920, 1080, Provider::Gemini), 1548);
    }

    /* Edge cases: zero / negative inputs must never crash and must
     * return a sentinel ≥ 1 so callers can use the value without
     * defending. */
    void degenerateInputs()
    {
        QCOMPARE(estimateImageTokens(0, 0, Provider::Anthropic), 1);
        QCOMPARE(estimateImageTokens(-100, 100, Provider::Anthropic), 1);
        QCOMPARE(estimateImageTokens(0, 0, Provider::OpenAIHigh), 1);
        QCOMPARE(estimateImageTokens(0, 0, Provider::Gemini), 1);
        /* OpenAILow stays 85 even for zero - it is dimension-independent. */
        QCOMPARE(estimateImageTokens(0, 0, Provider::OpenAILow), 85);
    }

    /* Pathological huge dimensions must not overflow int. Anthropic
     * 100k x 100k = 1.33e10 / 750 = 1.78e7, still well within int range. */
    void hugeDimensionsDoNotOverflow()
    {
        const int tokens = estimateImageTokens(100000, 100000, Provider::Anthropic);
        QVERIFY(tokens > 0);
        QVERIFY(tokens < 100000000);
    }

    void parseProviderRecognisesKnownHints()
    {
        QCOMPARE(parseProvider(QStringLiteral("anthropic")), Provider::Anthropic);
        QCOMPARE(parseProvider(QStringLiteral("Anthropic")), Provider::Anthropic);
        QCOMPARE(parseProvider(QStringLiteral("openai_low")), Provider::OpenAILow);
        QCOMPARE(parseProvider(QStringLiteral("openai-low")), Provider::OpenAILow);
        QCOMPARE(parseProvider(QStringLiteral("openai_high")), Provider::OpenAIHigh);
        QCOMPARE(parseProvider(QStringLiteral("openai-high")), Provider::OpenAIHigh);
        QCOMPARE(parseProvider(QStringLiteral("openai")), Provider::OpenAIHigh);
        QCOMPARE(parseProvider(QStringLiteral("gemini")), Provider::Gemini);
        QCOMPARE(parseProvider(QStringLiteral("google")), Provider::Gemini);
    }

    void parseProviderFallsBackToAnthropicOnUnknown()
    {
        QCOMPARE(parseProvider(QString()), Provider::Anthropic);
        QCOMPARE(parseProvider(QStringLiteral("")), Provider::Anthropic);
        QCOMPARE(parseProvider(QStringLiteral("qwen-vl")), Provider::Anthropic);
        QCOMPARE(parseProvider(QStringLiteral("nonsense")), Provider::Anthropic);
    }
};

QSOC_TEST_MAIN(TestQSocImageTokens)
#include "test_qsocimagetokens.moc"
