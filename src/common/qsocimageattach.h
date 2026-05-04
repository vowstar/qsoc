// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCIMAGEATTACH_H
#define QSOCIMAGEATTACH_H

#include <QByteArray>
#include <QString>

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

} // namespace QSocImageAttach

#endif // QSOCIMAGEATTACH_H
