// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCIMAGETOKENS_H
#define QSOCIMAGETOKENS_H

#include <QString>

#include <cstdint>

/**
 * @brief Pure-logic estimator for how many input tokens a remote multimodal
 *        model will charge for an image of given dimensions.
 * @details Each provider counts vision tokens differently. The client uses
 *          this to decide, before sending, whether an image is small enough
 *          to inline at all and what dimension to resize to. Bytes are NOT
 *          a useful proxy for token cost: a 1 MB PNG can be 700 tokens or
 *          22 000 tokens depending on the model family.
 */
namespace QSocImageTokens {

enum class Provider : std::uint8_t {
    /* Anthropic Messages API: tokens ≈ (W * H) / 750. The most conservative
     * mainstream estimate; we use it as the default when the provider hint
     * is empty or unknown. */
    Anthropic,
    /* OpenAI Chat / Responses with detail="low": fixed 85 tokens per image
     * regardless of dimensions (older fidelity-low models charge 85 base +
     * 129 per tile; we report the lower bound). */
    OpenAILow,
    /* OpenAI Chat / Responses with detail="high": image scaled to fit
     * 2048x2048, then shortest side scaled to ≤768, then billed as
     * 85 + 170 * tile_count where tiles are 512x512. */
    OpenAIHigh,
    /* Google Gemini generateContent: 258 tokens for images that fit inside
     * 384x384, otherwise tiled into 768x768 cells at 258 tokens per tile. */
    Gemini,
};

/**
 * @brief Estimate the number of input tokens an image will consume.
 * @param width  Source image width in pixels (must be ≥ 0).
 * @param height Source image height in pixels (must be ≥ 0).
 * @param model  Provider model family for the formula.
 * @return Estimated token count, always ≥ 1 for any non-empty image so
 *         caller code can use the value directly as a divisor or
 *         comparison without a separate empty-image check.
 */
int estimateImageTokens(int width, int height, Provider model);

/**
 * @brief Map a YAML config hint string to a Provider enum value.
 * @details Recognised values: "anthropic", "openai_low", "openai_high",
 *          "gemini". Empty string and unrecognised values fall through to
 *          Anthropic so the estimator overstates cost rather than
 *          understating it; conservative is the right default for budget
 *          gating.
 */
Provider parseProvider(const QString &hint);

} // namespace QSocImageTokens

#endif // QSOCIMAGETOKENS_H
