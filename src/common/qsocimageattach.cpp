// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocimageattach.h"

#include "common/qllmservice.h"
#include "common/qsocimagetokens.h"

#include <nlohmann/json.hpp>
#include <QBuffer>
#include <QImage>
#include <QImageReader>
#include <QSize>

#include <algorithm>

namespace QSocImageAttach {

namespace {

/* Pick the on-wire encoding format. PNG is kept only when the source
 * already has alpha (transparent UI captures); for everything else
 * JPEG quality 80 is roughly an order of magnitude smaller than PNG
 * with negligible model-relevant detail loss. */
const char *encodingFormatFor(const QImage &img, const QString &sourceMime)
{
    if (sourceMime.contains("png") && img.hasAlphaChannel()) {
        return "PNG";
    }
    if (sourceMime.contains("webp")) {
        return "WEBP";
    }
    return "JPEG";
}

const char *outboundMimeFor(const char *encodedFormat)
{
    if (qstrcmp(encodedFormat, "PNG") == 0) {
        return "image/png";
    }
    if (qstrcmp(encodedFormat, "WEBP") == 0) {
        return "image/webp";
    }
    return "image/jpeg";
}

QByteArray encodeImage(const QImage &img, const char *format)
{
    QByteArray out;
    QBuffer    buf(&out);
    if (!buf.open(QIODevice::WriteOnly)) {
        return {};
    }
    const int quality = (qstrcmp(format, "JPEG") == 0) ? 80 : -1;
    if (!img.save(&buf, format, quality)) {
        return {};
    }
    return out;
}

} // namespace

QString detectMimeByMagic(const QByteArray &bytes)
{
    /* PNG: 89 50 4E 47 0D 0A 1A 0A */
    if (bytes.size() >= 8 && static_cast<unsigned char>(bytes[0]) == 0x89 && bytes[1] == 'P'
        && bytes[2] == 'N' && bytes[3] == 'G' && static_cast<unsigned char>(bytes[4]) == 0x0D
        && static_cast<unsigned char>(bytes[5]) == 0x0A
        && static_cast<unsigned char>(bytes[6]) == 0x1A
        && static_cast<unsigned char>(bytes[7]) == 0x0A) {
        return QStringLiteral("image/png");
    }
    /* JPEG: FF D8 FF */
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xFF
        && static_cast<unsigned char>(bytes[1]) == 0xD8
        && static_cast<unsigned char>(bytes[2]) == 0xFF) {
        return QStringLiteral("image/jpeg");
    }
    /* GIF: "GIF87a" or "GIF89a" */
    if (bytes.size() >= 6 && bytes[0] == 'G' && bytes[1] == 'I' && bytes[2] == 'F'
        && bytes[3] == '8' && (bytes[4] == '7' || bytes[4] == '9') && bytes[5] == 'a') {
        return QStringLiteral("image/gif");
    }
    /* WebP: "RIFF" .... "WEBP" */
    if (bytes.size() >= 12 && bytes[0] == 'R' && bytes[1] == 'I' && bytes[2] == 'F'
        && bytes[3] == 'F' && bytes[8] == 'W' && bytes[9] == 'E' && bytes[10] == 'B'
        && bytes[11] == 'P') {
        return QStringLiteral("image/webp");
    }
    return {};
}

const char *attachmentMarkerOpen()
{
    return "\x01<qsoc:image_attachment>";
}

const char *attachmentMarkerClose()
{
    return "</qsoc:image_attachment>\x01";
}

QString buildAttachmentResult(
    const QString &sourceLabel, const QString &mimeHint, const QByteArray &body, QLLMService *llm)
{
    /* Sniff dimensions without decoding pixels. Saves memory for the
     * common case of a 4K screenshot; we only ever decode if we are
     * going to inline. */
    QBuffer buf;
    buf.setData(body);
    if (!buf.open(QIODevice::ReadOnly)) {
        return QString("Error: failed to read image body (mime: %1)").arg(mimeHint);
    }
    QImageReader reader(&buf);
    QSize        dims = reader.size();
    if (!dims.isValid() || dims.width() <= 0 || dims.height() <= 0) {
        return QString("Error: unsupported / corrupt image (mime: %1, %2 bytes)")
            .arg(mimeHint)
            .arg(body.size());
    }

    /* Capability + budget gate. No llm pointer or text-only model means
     * the agent must not see image bytes; emit a textual surrogate so
     * the model still knows the source referenced an image and can
     * decide whether to ask the user for an alternative. */
    LLMModelConfig modelCfg;
    bool           accepts = false;
    if (llm != nullptr) {
        modelCfg = llm->getCurrentModelConfig();
        accepts  = modelCfg.acceptsImage;
    }

    auto provider = QSocImageTokens::parseProvider(modelCfg.imageProviderHint);
    int  estTok   = QSocImageTokens::estimateImageTokens(dims.width(), dims.height(), provider);

    auto fallback = [&](const QString &reason) -> QString {
        return QString("[image: %1 mime=%2 dims=%3x%4 bytes=%5 est_tokens=%6; not inlined: %7]")
            .arg(sourceLabel, mimeHint)
            .arg(dims.width())
            .arg(dims.height())
            .arg(body.size())
            .arg(estTok)
            .arg(reason);
    };

    if (!accepts) {
        return fallback(QStringLiteral("current model does not accept images"));
    }

    QImage decoded = reader.read();
    if (decoded.isNull()) {
        /* Header parsed but pixels are bad; do not pretend to inline. */
        return fallback(QStringLiteral("image decode failed"));
    }

    int  finalW    = dims.width();
    int  finalH    = dims.height();
    bool didResize = false;

    auto needsResize = [&]() {
        if (estTok > modelCfg.imageMaxTokens) {
            return true;
        }
        if (modelCfg.imageMaxDimension > 0
            && std::max(finalW, finalH) > modelCfg.imageMaxDimension) {
            return true;
        }
        return false;
    };

    if (needsResize() && modelCfg.imageMaxDimension > 0) {
        const int target = modelCfg.imageMaxDimension;
        QImage    scaled
            = decoded.scaled(target, target, Qt::KeepAspectRatio, Qt::SmoothTransformation);
        if (!scaled.isNull()) {
            decoded   = scaled;
            finalW    = scaled.width();
            finalH    = scaled.height();
            didResize = true;
            estTok    = QSocImageTokens::estimateImageTokens(finalW, finalH, provider);
        }
    }

    if (estTok > modelCfg.imageMaxTokens) {
        return fallback(QString("est_tokens %1 > image_max_tokens %2 even after resize")
                            .arg(estTok)
                            .arg(modelCfg.imageMaxTokens));
    }

    const char *encFormat = encodingFormatFor(decoded, mimeHint);
    QByteArray  encoded   = encodeImage(decoded, encFormat);
    if (encoded.isEmpty()) {
        return fallback(QStringLiteral("re-encode failed"));
    }

    const QByteArray b64     = encoded.toBase64();
    const char      *outMime = outboundMimeFor(encFormat);
    const QString    summary = QString("[image attached: %1x%2 ~%3 tokens (%4 KB %5) from %6%7]")
                                   .arg(finalW)
                                   .arg(finalH)
                                   .arg(estTok)
                                   .arg(encoded.size() / 1024)
                                   .arg(QString::fromLatin1(outMime))
                                   .arg(sourceLabel)
                                   .arg(didResize ? QStringLiteral(", resized") : QString());

    nlohmann::json payload = {
        {"mime", outMime},
        {"data", b64.toStdString()},
        {"source_url", sourceLabel.toStdString()},
        {"width", finalW},
        {"height", finalH},
        {"byte_size", encoded.size()},
        {"est_tokens", estTok},
        {"resized", didResize},
    };

    QString result;
    result.reserve(static_cast<int>(b64.size()) + 256);
    result += summary;
    result += QLatin1Char('\n');
    result += QString::fromLatin1(attachmentMarkerOpen());
    result += QString::fromStdString(payload.dump());
    result += QString::fromLatin1(attachmentMarkerClose());
    return result;
}

} // namespace QSocImageAttach
