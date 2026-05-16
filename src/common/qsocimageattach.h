// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCIMAGEATTACH_H
#define QSOCIMAGEATTACH_H

#include <QByteArray>
#include <QString>

class QImage;
class QLLMService;

/**
 * @brief Shared helpers for turning fetched image bytes into a tool
 *        result that the agent main loop can lift into a multimodal
 *        request.
 * @details Two tools currently produce these attachments:
 *          - `web_fetch` when the URL response has an image MIME, and
 *          - `read_file` when the local file's leading bytes match a
 *            known raster image magic signature.
 *          Both share the same encode pipeline: dimension sniff via
 *          QImageReader, optional resize down to imageMaxDimension,
 *          re-encode (PNG when the source has alpha, JPEG quality 80
 *          otherwise), base64, then wrapping in an attachment marker
 *          so the conversation history can lift the bytes into an
 *          OpenAI-compatible image_url content part.
 */
namespace QSocImageAttach {

/**
 * @brief Detect a raster image MIME by inspecting magic bytes.
 * @details Magic-byte detection is preferred over filename extension
 *          because a `read_file` caller may pass a misnamed path and
 *          a `web_fetch` server may report a wrong Content-Type. A
 *          single call here is the trusted source of truth.
 * @return Lowercase MIME (`image/png`, `image/jpeg`, `image/gif`,
 *         `image/webp`) on a hit, empty QString when no signature
 *         matches the first bytes.
 */
QString detectMimeByMagic(const QByteArray &bytes);

/**
 * @brief Sentinel marker that wraps a JSON image-attachment payload
 *        in tool results so the agent main loop can lift it into the
 *        next request.
 */
const char *attachmentMarkerOpen();
const char *attachmentMarkerClose();

/**
 * @brief Build the structured tool-result string from raw image bytes.
 * @param sourceLabel  Human-readable origin (URL or absolute file
 *                     path) for the summary line shown to the user.
 * @param mimeHint     MIME from the caller's first-pass detection
 *                     (Content-Type header for HTTP, magic-byte
 *                     detection for local files). Used for the wire
 *                     payload only when the body actually validates
 *                     as a decodable image.
 * @param body         Raw image bytes. Not modified.
 * @param llm          Active LLM service for capability gating; null
 *                     means "treat as text-only model" so the result
 *                     degrades to an alt-text summary.
 * @return Same shape as the prior in-tree implementation:
 *         "[image attached: WxH ~tokens (NN KB mime) from <label>]\n
 *          \x01<qsoc:image_attachment>{...json...}</qsoc:image_attachment>\x01"
 *         on success, or "[image: <label> ... not inlined: <reason>]"
 *         when capability gate / token budget refuses to inline.
 */
QString buildAttachmentResult(
    const QString &sourceLabel, const QString &mimeHint, const QByteArray &body, QLLMService *llm);

/**
 * @brief Outcome of @ref compressWithinBudget.
 * @details `format` may differ from the source MIME when the cascade
 *          had to swap encoders (e.g. PNG -> JPEG to shed alpha for a
 *          screenshot that would not fit). `finalWidth` / `finalHeight`
 *          reflect the post-shrink dimensions when the cascade reached
 *          the dimension-halving step. `data` is empty only on a hard
 *          encoder failure; callers must still check it against
 *          `maxBytes` because the cascade returns the smallest sample
 *          it produced even when the final byte budget still loses.
 */
struct CompressOutcome
{
    QByteArray  data;
    const char *format       = "JPEG";
    bool        downgraded   = false;
    int         finalQuality = -1;
    int         finalWidth   = 0;
    int         finalHeight  = 0;
};

/**
 * @brief Cascade re-encode until the image fits a byte budget.
 * @details Three steps mirroring claude-code's imageResizer cascade:
 *          1. JPEG / WEBP quality ladder (80 -> 65 -> 50 -> 40).
 *          2. For PNG with alpha: indexed-palette PNG, then drop alpha
 *             and ride the JPEG ladder.
 *          3. Halve dimensions once and run a final JPEG ladder at
 *             50 / 40 / 30 quality.
 * @param img       Decoded source image; already dimension-capped by
 *                  the caller's earlier resize step.
 * @param sourceMime MIME hint used only to pick the preferred encoder.
 * @param maxBytes  Hard byte cap. Zero or negative disables the
 *                  cascade and just runs the default encoder once.
 * @return Outcome carrying the encoded bytes, final format, and
 *         downgrade markers for the summary log line.
 */
CompressOutcome compressWithinBudget(const QImage &img, const QString &sourceMime, int maxBytes);

} // namespace QSocImageAttach

#endif // QSOCIMAGEATTACH_H
