// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocimageattach.h"

#include "common/qllmservice.h"
#include "common/qsocimagetokens.h"

#include <nlohmann/json.hpp>
#include <QBuffer>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QImage>
#include <QImageReader>
#include <QSaveFile>
#include <QSize>

#include <algorithm>
#include <memory>

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

QByteArray encodeImage(const QImage &img, const char *format, int qualityOverride = -1)
{
    QByteArray out;
    QBuffer    buf(&out);
    if (!buf.open(QIODevice::WriteOnly)) {
        return {};
    }
    int quality = qualityOverride;
    if (quality < 0) {
        quality = (qstrcmp(format, "JPEG") == 0) ? 80 : -1;
    }
    if (!img.save(&buf, format, quality)) {
        return {};
    }
    return out;
}

} // namespace

CompressOutcome compressWithinBudget(const QImage &img, const QString &sourceMime, int maxBytes)
{
    CompressOutcome outcome;
    const bool      preferPng = sourceMime.contains("png") && img.hasAlphaChannel();
    const char     *preferred = preferPng ? "PNG" : "JPEG";
    if (sourceMime.contains("webp")) {
        preferred = "WEBP";
    }

    outcome.format      = preferred;
    outcome.finalWidth  = img.width();
    outcome.finalHeight = img.height();
    outcome.data        = encodeImage(img, outcome.format);
    if (outcome.data.isEmpty() || maxBytes <= 0 || outcome.data.size() <= maxBytes) {
        return outcome;
    }

    /* Quality ladder for JPEG / WEBP first; PNG with alpha drops to
     * indexed palette before falling all the way to lossy JPEG. */
    if (qstrcmp(outcome.format, "JPEG") == 0 || qstrcmp(outcome.format, "WEBP") == 0) {
        for (int quality : {65, 50, 40}) {
            QByteArray attempt = encodeImage(img, outcome.format, quality);
            if (!attempt.isEmpty() && attempt.size() <= maxBytes) {
                outcome.data         = attempt;
                outcome.finalQuality = quality;
                outcome.downgraded   = true;
                return outcome;
            }
            if (!attempt.isEmpty() && attempt.size() < outcome.data.size()) {
                outcome.data         = attempt;
                outcome.finalQuality = quality;
                outcome.downgraded   = true;
            }
        }
    } else if (preferPng) {
        const QImage paletted = img.convertToFormat(QImage::Format_Indexed8);
        if (!paletted.isNull()) {
            QByteArray attempt = encodeImage(paletted, "PNG");
            if (!attempt.isEmpty() && attempt.size() <= maxBytes) {
                outcome.data       = attempt;
                outcome.format     = "PNG";
                outcome.downgraded = true;
                return outcome;
            }
            if (!attempt.isEmpty() && attempt.size() < outcome.data.size()) {
                outcome.data       = attempt;
                outcome.downgraded = true;
            }
        }
        /* Final PNG escape: drop alpha and ride the JPEG ladder. */
        const QImage opaque = img.convertToFormat(QImage::Format_RGB32);
        for (int quality : {65, 50, 40}) {
            QByteArray attempt = encodeImage(opaque, "JPEG", quality);
            if (!attempt.isEmpty() && attempt.size() <= maxBytes) {
                outcome.data         = attempt;
                outcome.format       = "JPEG";
                outcome.finalQuality = quality;
                outcome.downgraded   = true;
                return outcome;
            }
            if (!attempt.isEmpty() && attempt.size() < outcome.data.size()) {
                outcome.data         = attempt;
                outcome.format       = "JPEG";
                outcome.finalQuality = quality;
                outcome.downgraded   = true;
            }
        }
    }

    if (outcome.data.size() <= maxBytes) {
        return outcome;
    }

    /* Last resort: shrink dimensions and re-run the JPEG ladder. One
     * halving step is sufficient for the budgets a real model ever
     * publishes; deeper recursion would just turn the image to mush. */
    const int    newWidth  = std::max(64, img.width() / 2);
    const int    newHeight = std::max(64, img.height() / 2);
    const QImage shrunk
        = img.scaled(newWidth, newHeight, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    if (shrunk.isNull()) {
        return outcome;
    }
    outcome.finalWidth  = shrunk.width();
    outcome.finalHeight = shrunk.height();
    outcome.downgraded  = true;
    for (int quality : {50, 40, 30}) {
        QByteArray attempt = encodeImage(
            shrunk.hasAlphaChannel() ? shrunk.convertToFormat(QImage::Format_RGB32) : shrunk,
            "JPEG",
            quality);
        if (!attempt.isEmpty() && attempt.size() <= maxBytes) {
            outcome.data         = attempt;
            outcome.format       = "JPEG";
            outcome.finalQuality = quality;
            return outcome;
        }
        if (!attempt.isEmpty() && attempt.size() < outcome.data.size()) {
            outcome.data         = attempt;
            outcome.format       = "JPEG";
            outcome.finalQuality = quality;
        }
    }
    return outcome;
}

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

QString saveFigure(const QString &directory, const QByteArray &bytes, const QString &mime)
{
    static const QHash<QString, QString> extensions
        = {{QStringLiteral("image/png"), QStringLiteral("png")},
           {QStringLiteral("image/jpeg"), QStringLiteral("jpeg")},
           {QStringLiteral("image/gif"), QStringLiteral("gif")},
           {QStringLiteral("image/webp"), QStringLiteral("webp")},
           {QStringLiteral("image/svg+xml"), QStringLiteral("svg")}};
    constexpr qint64 directoryBytes = qint64(1024) * 1024 * 1024;
    const QString    extension      = extensions.value(mime);
    const QDir       dir(directory);
    if (bytes.isEmpty() || extension.isEmpty() || !dir.mkpath(QStringLiteral(".")))
        return {};
    const QString path = dir.filePath(
        QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex())
        + QLatin1Char('.') + extension);
    if (QFileInfo::exists(path))
        return path;
    qint64 used = 0;
    for (const auto &entry : dir.entryInfoList(QDir::Files))
        used += entry.size();
    if (used + bytes.size() > directoryBytes)
        return {};
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit())
        return {};
    QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
    return path;
}

QString wrapAttachmentPayload(const nlohmann::json &payload)
{
    return QString::fromLatin1(attachmentMarkerOpen()) + QString::fromStdString(payload.dump())
           + QString::fromLatin1(attachmentMarkerClose());
}

namespace {

/* Refused images still reach the user. The bound keeps the event below the
 * runtime's preview limit. */
QString displayOnlyMarker(const QString &sourceLabel, const QByteArray &body)
{
    constexpr int previewDimension = 2560;
    constexpr int previewBytes     = 6 * 1024 * 1024;
    QImage        image            = QImage::fromData(body);
    if (image.isNull())
        return {};
    const bool oversize = std::max(image.width(), image.height()) > previewDimension;
    if (!oversize && body.size() <= previewBytes) {
        return wrapAttachmentPayload(
            {{"mime", detectMimeByMagic(body).toStdString()},
             {"data", body.toBase64().toStdString()},
             {"source_url", sourceLabel.toStdString()},
             {"width", image.width()},
             {"height", image.height()},
             {"byte_size", body.size()},
             {"display_only", true}});
    }
    if (oversize) {
        image = image.scaled(
            previewDimension, previewDimension, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    }
    const auto outcome = compressWithinBudget(image, QStringLiteral("image/png"), previewBytes);
    if (outcome.data.isEmpty() || outcome.data.size() > previewBytes)
        return {};
    const QImage encoded = QImage::fromData(outcome.data);
    return wrapAttachmentPayload(
        {{"mime", outboundMimeFor(outcome.format)},
         {"data", outcome.data.toBase64().toStdString()},
         {"source_url", sourceLabel.toStdString()},
         {"width", encoded.width()},
         {"height", encoded.height()},
         {"byte_size", outcome.data.size()},
         {"display_only", true}});
}

} // namespace

nlohmann::json attachmentPayload(const QString &text)
{
    const QString   open  = QString::fromLatin1(attachmentMarkerOpen());
    const QString   close = QString::fromLatin1(attachmentMarkerClose());
    const qsizetype begin = text.indexOf(open);
    const qsizetype end   = text.indexOf(close, begin + open.size());
    if (begin < 0 || end < 0)
        return nlohmann::json(nlohmann::json::value_t::discarded);
    return nlohmann::json::parse(
        text.mid(begin + open.size(), end - begin - open.size()).toStdString(), nullptr, false);
}

namespace {

QString encodedAttachment(
    const QString    &sourceLabel,
    const QByteArray &source,
    const QSize      &sourceSize,
    const QByteArray &encoded,
    const char       *mime,
    const QSize      &size,
    int               tokens,
    bool              resized,
    const QString    &notes,
    const Options    &options)
{
    const auto hash = [](const QByteArray &bytes) {
        return QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex().toStdString();
    };
    nlohmann::json payload
        = {{"mime", mime},
           {"data", encoded.toBase64().toStdString()},
           {"source_url", sourceLabel.toStdString()},
           {"width", size.width()},
           {"height", size.height()},
           {"byte_size", encoded.size()},
           {"est_tokens", tokens},
           {"resized", resized},
           {"source_width", sourceSize.width()},
           {"source_height", sourceSize.height()},
           {"source_sha256", hash(source)},
           {"image_id", hash(encoded)},
           {"preserve_source", options.preserveSource}};
    if (!options.sourceId.isEmpty())
        payload["source_id"] = options.sourceId.toStdString();
    if (!options.viewId.isEmpty())
        payload["view_id"] = options.viewId.toStdString();
    QString visibleSource = sourceLabel;
    visibleSource.replace(QChar(0x01), QStringLiteral("\\u0001"));
    return QString("[image attached: %1x%2 ~%3 tokens (%4 KB %5) from %6%7]\n")
               .arg(size.width())
               .arg(size.height())
               .arg(tokens)
               .arg(encoded.size() / 1024)
               .arg(QString::fromLatin1(mime), visibleSource, notes)
           + wrapAttachmentPayload(payload);
}

/* What the `llm.vision_model` entry says about an image, empty when it cannot say. */
QString describeWithVisionModel(
    const QString    &sourceLabel,
    const QString    &mimeHint,
    const QByteArray &body,
    QLLMService      *llm,
    const Options    &options)
{
    if (llm == nullptr || !llm->getModelConfig(llm->visionModel()).acceptsImage)
        return {};
    const std::unique_ptr<QLLMService> vision(llm->clone());
    vision->setCurrentModel(llm->visionModel());
    const nlohmann::json payload = attachmentPayload(
        buildAttachmentResult(sourceLabel, mimeHint, body, vision.get(), false, options));
    if (!payload.is_object())
        return {};
    nlohmann::json content = nlohmann::json::array(
        {{{"type", "text"},
          {"text",
           "Describe this image for an engineer who cannot see it. Report all visible text and "
           "numbers exactly, and the structure of any diagram or chart."}},
         {{"type", "image_url"},
          {"image_url",
           {{"url",
             "data:" + payload.value("mime", std::string()) + ";base64,"
                 + payload.value("data", std::string())}}}}});
    if (options.preserveSource)
        content[1]["image_url"]["detail"] = "high";
    nlohmann::json message;
    if (!QLLMService::extractAssistantMessage(
            vision->sendChatCompletion(
                nlohmann::json::array({{{"role", "user"}, {"content", content}}}),
                nlohmann::json::array(),
                0.7,
                options.stop,
                options.reasoningEffort),
            &message)
        || !message.value("content", nlohmann::json()).is_string())
        return {};
    QString text = QString::fromStdString(message["content"].get<std::string>());
    text.remove(QChar(0x01));
    return text.trimmed();
}

} // namespace

QString buildAttachmentResult(
    const QString    &sourceLabel,
    const QString    &mimeHint,
    const QByteArray &body,
    QLLMService      *llm,
    bool              previewRefused,
    const Options    &options)
{
    if (options.stop.stop_requested())
        return QStringLiteral("Error: image attachment cancelled");
    if (options.sourceId.size() > 256 || options.viewId.size() > 256)
        return QStringLiteral("Error: image source identity exceeds 256 characters");
    QString visibleSource = sourceLabel;
    visibleSource.replace(QChar(0x01), QStringLiteral("\\u0001"));
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
            .arg(visibleSource, mimeHint)
            .arg(dims.width())
            .arg(dims.height())
            .arg(body.size())
            .arg(estTok)
            .arg(reason);
    };

    if (!accepts) {
        const QString marker = previewRefused ? displayOnlyMarker(sourceLabel, body) : QString();
        QString described    = describeWithVisionModel(sourceLabel, mimeHint, body, llm, options);
        if (options.stop.stop_requested())
            return QStringLiteral("Error: image attachment cancelled");
        described.replace(QChar(0x01), QStringLiteral("\\u0001"));
        const QString summary
            = !described.isEmpty()
                  ? QStringLiteral("[image: %1 described by the vision model %2: %3]")
                        .arg(visibleSource, llm != nullptr ? llm->visionModel() : QString(), described)
                  : fallback(
                        marker.isEmpty() ? QStringLiteral("current model does not accept images")
                                         : QStringLiteral(
                                               "current model does not accept images; the user "
                                               "sees a preview"));
        return marker.isEmpty() ? summary : summary + QLatin1Char('\n') + marker;
    }

    QString sourceMime;
    if (options.preserveSource) {
        if (estTok > modelCfg.imageMaxTokens)
            return fallback(
                QStringLiteral("source image exceeds image_max_tokens; read a smaller region"));
        if (modelCfg.imageMaxDimension > 0
            && std::max(dims.width(), dims.height()) > modelCfg.imageMaxDimension)
            return fallback(
                QStringLiteral("source image exceeds image_max_dimension; read a smaller region"));
        if (modelCfg.imageMaxBytes > 0 && body.size() > modelCfg.imageMaxBytes)
            return fallback(
                QStringLiteral("source image exceeds image_max_bytes; read a smaller region"));
        sourceMime = detectMimeByMagic(body);
        if (sourceMime.isEmpty())
            return fallback(QStringLiteral("source image encoding is unsupported"));
    }

    QImage decoded = reader.read();
    if (decoded.isNull()) {
        /* Header parsed but pixels are bad; do not pretend to inline. */
        return fallback(QStringLiteral("image decode failed"));
    }

    if (options.preserveSource) {
        return encodedAttachment(
            sourceLabel,
            body,
            dims,
            body,
            sourceMime.toLatin1().constData(),
            dims,
            estTok,
            false,
            QStringLiteral(", source preserved"),
            options);
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
    QByteArray  encoded;
    bool        compressDowngrade = false;
    int         compressQuality   = -1;

    if (modelCfg.imageMaxBytes > 0) {
        CompressOutcome outcome = compressWithinBudget(decoded, mimeHint, modelCfg.imageMaxBytes);
        if (outcome.data.isEmpty()) {
            return fallback(QStringLiteral("re-encode failed"));
        }
        if (outcome.data.size() > modelCfg.imageMaxBytes) {
            return fallback(QString("encoded %1 KB > image_max_bytes %2 KB even after cascade")
                                .arg(outcome.data.size() / 1024)
                                .arg(modelCfg.imageMaxBytes / 1024));
        }
        encoded           = outcome.data;
        encFormat         = outcome.format;
        compressDowngrade = outcome.downgraded;
        compressQuality   = outcome.finalQuality;
        if (outcome.finalWidth > 0 && outcome.finalHeight > 0
            && (outcome.finalWidth != finalW || outcome.finalHeight != finalH)) {
            finalW    = outcome.finalWidth;
            finalH    = outcome.finalHeight;
            didResize = true;
            estTok    = QSocImageTokens::estimateImageTokens(finalW, finalH, provider);
        }
    } else {
        encoded = encodeImage(decoded, encFormat);
        if (encoded.isEmpty()) {
            return fallback(QStringLiteral("re-encode failed"));
        }
    }

    const char *outMime = outboundMimeFor(encFormat);
    QString     notes;
    if (didResize) {
        notes += QStringLiteral(", resized");
    }
    if (compressDowngrade) {
        notes += QStringLiteral(", recompressed");
    }
    if (compressQuality > 0) {
        notes += QStringLiteral(" q=%1").arg(compressQuality);
    }
    return encodedAttachment(
        sourceLabel,
        body,
        dims,
        encoded,
        outMime,
        QSize(finalW, finalH),
        estTok,
        didResize,
        notes,
        options);
}

} // namespace QSocImageAttach
