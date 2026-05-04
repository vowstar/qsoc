// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocimagetokens.h"

#include <QtGlobal>

#include <algorithm>
#include <climits>
#include <cmath>

namespace QSocImageTokens {

namespace {

/* OpenAI tiling parameters for detail="high". Documented at
 * developers.openai.com/api/docs/guides/images-vision. */
constexpr int kOpenAIHighOuterMax  = 2048; /* shortest fit box */
constexpr int kOpenAIHighShortSide = 768;  /* shortest side after scale */
constexpr int kOpenAIHighTile      = 512;
constexpr int kOpenAIHighBase      = 85;
constexpr int kOpenAIHighPerTile   = 170;

/* Gemini cell parameters. Up to 384x384 -> single 258-token cell;
 * larger images are tiled into 768x768 cells, 258 tokens each. */
constexpr int kGeminiSmallEdge     = 384;
constexpr int kGeminiTile          = 768;
constexpr int kGeminiTokensPerTile = 258;

int ceilDiv(int numerator, int denominator)
{
    if (denominator <= 0) {
        return 0;
    }
    return (numerator + denominator - 1) / denominator;
}

int estimateAnthropic(int width, int height)
{
    /* Empty images still cost 1 so callers can divide / compare safely. */
    const qint64 product = static_cast<qint64>(width) * static_cast<qint64>(height);
    const qint64 tokens  = product / 750;
    const qint64 clamped
        = std::min<qint64>(static_cast<qint64>(INT_MAX), std::max<qint64>(1, tokens));
    return static_cast<int>(clamped);
}

int estimateOpenAIHigh(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return 1;
    }
    /* Step 1: scale to fit inside 2048x2048 keeping aspect ratio. */
    const double fitX     = static_cast<double>(kOpenAIHighOuterMax) / width;
    const double fitY     = static_cast<double>(kOpenAIHighOuterMax) / height;
    const double scale1   = std::min({1.0, fitX, fitY});
    const double scaledW1 = width * scale1;
    const double scaledH1 = height * scale1;

    /* Step 2: scale so the shortest side equals 768 (only if currently
     * larger than 768; small images do not get upscaled). */
    const double shortSide = std::min(scaledW1, scaledH1);
    const double scale2    = (shortSide > kOpenAIHighShortSide) ? kOpenAIHighShortSide / shortSide
                                                                : 1.0;
    int          scaledW2  = static_cast<int>(std::lround(scaledW1 * scale2));
    int          scaledH2  = static_cast<int>(std::lround(scaledH1 * scale2));
    if (scaledW2 < 1) {
        scaledW2 = 1;
    }
    if (scaledH2 < 1) {
        scaledH2 = 1;
    }

    /* Step 3: count 512x512 tiles. */
    const int tilesX = ceilDiv(scaledW2, kOpenAIHighTile);
    const int tilesY = ceilDiv(scaledH2, kOpenAIHighTile);
    const int tiles  = tilesX * tilesY;
    return kOpenAIHighBase + kOpenAIHighPerTile * tiles;
}

int estimateGemini(int width, int height)
{
    if (width <= 0 || height <= 0) {
        return 1;
    }
    /* Small image: single 258-token cell. */
    if (width <= kGeminiSmallEdge && height <= kGeminiSmallEdge) {
        return kGeminiTokensPerTile;
    }
    /* Otherwise: tile into 768x768 cells. */
    const int tilesX = ceilDiv(width, kGeminiTile);
    const int tilesY = ceilDiv(height, kGeminiTile);
    return kGeminiTokensPerTile * tilesX * tilesY;
}

} // namespace

int estimateImageTokens(int width, int height, Provider model)
{
    if (width < 0 || height < 0) {
        return (model == Provider::OpenAILow) ? 85 : 1;
    }
    switch (model) {
    case Provider::Anthropic:
        return estimateAnthropic(width, height);
    case Provider::OpenAILow:
        return 85;
    case Provider::OpenAIHigh:
        return estimateOpenAIHigh(width, height);
    case Provider::Gemini:
        return estimateGemini(width, height);
    }
    return estimateAnthropic(width, height);
}

Provider parseProvider(const QString &hint)
{
    const QString lower = hint.trimmed().toLower();
    if (lower == QStringLiteral("openai_low") || lower == QStringLiteral("openai-low")) {
        return Provider::OpenAILow;
    }
    if (lower == QStringLiteral("openai_high") || lower == QStringLiteral("openai-high")
        || lower == QStringLiteral("openai")) {
        return Provider::OpenAIHigh;
    }
    if (lower == QStringLiteral("gemini") || lower == QStringLiteral("google")) {
        return Provider::Gemini;
    }
    /* Empty / "anthropic" / unknown -> conservative Anthropic estimate. */
    return Provider::Anthropic;
}

} // namespace QSocImageTokens
