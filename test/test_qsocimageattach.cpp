// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

/* Magic-byte detection unit test. Image fixtures are generated at
 * runtime via QImage; nothing binary is committed to the repo. */

#include "common/qllmservice.h"
#include "common/qsocimageattach.h"
#include "qsoc_test.h"

#include <QBuffer>
#include <QByteArray>
#include <QColor>
#include <QCryptographicHash>
#include <QImage>
#include <QtTest>

namespace {

QByteArray encode(int width, int height, const QColor &fill, const char *format)
{
    QImage img(width, height, QImage::Format_RGB32);
    img.fill(fill);
    QByteArray out;
    QBuffer    buf(&out);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, format);
    return out;
}

class TestQSocImageAttach : public QObject
{
    Q_OBJECT

private slots:
    void preserveSource_data()
    {
        QTest::addColumn<QString>("limit");
        for (const char *name : {"none", "dimension", "tokens", "bytes", "cancel"})
            QTest::newRow(name) << QString::fromLatin1(name);
    }

    void preserveSource()
    {
        QFETCH(QString, limit);
        const QByteArray source = encode(320, 192, QColor(Qt::cyan), "PNG");
        QVERIFY(!source.isEmpty());
        LLMModelConfig model;
        model.acceptsImage      = true;
        model.imageMaxDimension = limit == "dimension" ? 100 : 4096;
        model.imageMaxTokens    = limit == "tokens" ? 1 : 100000;
        model.imageMaxBytes     = limit == "bytes" ? 32 : 0;
        QLLMService llm;
        llm.setModel(model);
        std::stop_source stop;
        if (limit == "cancel")
            stop.request_stop();
        QSocImageAttach::Options options;
        options.preserveSource = true;
        options.sourceId       = QStringLiteral("generated-document");
        options.viewId         = QStringLiteral("page1-region1");
        options.stop           = stop.get_token();
        const QString result   = QSocImageAttach::buildAttachmentResult(
            "runtime:generated-page", "image/png", source, &llm, false, options);
        const auto payload = QSocImageAttach::attachmentPayload(result);
        if (limit != "none") {
            QVERIFY(!payload.is_object());
            QVERIFY(result.contains(limit == "cancel" ? "cancelled" : "read a smaller region"));
            return;
        }
        QVERIFY(payload.is_object());
        QCOMPARE(payload.at("mime"), nlohmann::json("image/png"));
        QCOMPARE(
            QByteArray::fromBase64(QByteArray::fromStdString(payload.at("data").get<std::string>())),
            source);
        QCOMPARE(payload.at("source_id"), nlohmann::json("generated-document"));
        QCOMPARE(payload.at("view_id"), nlohmann::json("page1-region1"));
        QCOMPARE(payload.at("width"), nlohmann::json(320));
        QCOMPARE(payload.at("height"), nlohmann::json(192));
        QCOMPARE(payload.at("source_width"), payload.at("width"));
        QCOMPARE(payload.at("source_height"), payload.at("height"));
        QCOMPARE(payload.at("source_sha256"), payload.at("image_id"));
        QCOMPARE(
            payload.at("image_id"),
            nlohmann::json(
                QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex().toStdString()));
        QVERIFY(payload.at("preserve_source").get<bool>());
        QVERIFY(!payload.at("resized").get<bool>());
    }

    void ordinaryImagesKeepDefaultEncoding()
    {
        const QByteArray source = encode(320, 192, QColor(Qt::cyan), "PNG");
        LLMModelConfig   model;
        model.acceptsImage = true;
        QLLMService llm;
        llm.setModel(model);
        const auto payload = QSocImageAttach::attachmentPayload(
            QSocImageAttach::buildAttachmentResult(
                "runtime:generated-page", "image/png", source, &llm));
        QCOMPARE(payload.at("mime"), nlohmann::json("image/jpeg"));
        QVERIFY(!payload.at("preserve_source").get<bool>());
        QVERIFY(payload.at("source_sha256") != payload.at("image_id"));
    }

    void preserveSourceRejectsKnownLimitsBeforeDecode_data()
    {
        QTest::addColumn<QString>("limit");
        QTest::newRow("dimension") << QStringLiteral("dimension");
        QTest::newRow("tokens") << QStringLiteral("tokens");
        QTest::newRow("bytes") << QStringLiteral("bytes");
        QTest::newRow("accepted-header-still-validates-pixels") << QStringLiteral("none");
    }

    void preserveSourceRejectsKnownLimitsBeforeDecode()
    {
        QFETCH(QString, limit);
        QByteArray source = encode(320, 192, QColor(Qt::cyan), "PNG");
        QVERIFY(source.size() > 64);
        source.truncate(source.size() / 2);
        LLMModelConfig model;
        model.acceptsImage      = true;
        model.imageMaxDimension = limit == "dimension" ? 100 : 4096;
        model.imageMaxTokens    = limit == "tokens" ? 1 : 100000;
        model.imageMaxBytes     = limit == "bytes" ? 32 : 0;
        QLLMService llm;
        llm.setModel(model);
        QSocImageAttach::Options options;
        options.preserveSource = true;
        const auto result      = QSocImageAttach::buildAttachmentResult(
            "runtime:generated-page", "image/png", source, &llm, false, options);
        QVERIFY(!QSocImageAttach::attachmentPayload(result).is_object());
        if (limit == "none")
            QVERIFY(result.contains(QStringLiteral("image decode failed")));
        else
            QVERIFY(result.contains(QStringLiteral("image_max_%1").arg(limit)));
    }

    void detectsPng()
    {
        const QByteArray bytes = encode(32, 32, QColor(Qt::red), "PNG");
        QCOMPARE(QSocImageAttach::detectMimeByMagic(bytes), QStringLiteral("image/png"));
    }

    void detectsJpeg()
    {
        const QByteArray bytes = encode(32, 32, QColor(Qt::green), "JPEG");
        QCOMPARE(QSocImageAttach::detectMimeByMagic(bytes), QStringLiteral("image/jpeg"));
    }

    /* GIF and WebP encoders are not available in every Qt build, so we
     * synthesize the magic header directly rather than rely on the
     * encoder. The detector only inspects leading bytes anyway. */
    void detectsGif()
    {
        QByteArray bytes("GIF89a", 6);
        bytes.append("\x00\x00\x00\x00", 4);
        QCOMPARE(QSocImageAttach::detectMimeByMagic(bytes), QStringLiteral("image/gif"));
    }

    void detectsWebp()
    {
        QByteArray bytes("RIFF", 4);
        bytes.append("\x10\x00\x00\x00", 4); /* placeholder size */
        bytes.append("WEBP", 4);
        bytes.append("VP8 ", 4);
        QCOMPARE(QSocImageAttach::detectMimeByMagic(bytes), QStringLiteral("image/webp"));
    }

    /* Plain text and short blobs must not false-positive. The four
     * leading byte alternatives cover bash scripts, JSON, YAML, and
     * the kind of garbled binary one accidentally commits when forgetting
     * to run `file` first. */
    void rejectsNonImageBytes()
    {
        QVERIFY(QSocImageAttach::detectMimeByMagic(QByteArray()).isEmpty());
        QVERIFY(QSocImageAttach::detectMimeByMagic(QByteArray("hello world", 11)).isEmpty());
        QVERIFY(QSocImageAttach::detectMimeByMagic(QByteArray("{\"key\":1}", 9)).isEmpty());
        QVERIFY(
            QSocImageAttach::detectMimeByMagic(QByteArray(
                                                   "\x7F"
                                                   "ELF\x01",
                                                   5))
                .isEmpty());
        /* Almost-PNG: right first byte, wrong tail. Detector must not
         * return image/png on partial matches. */
        QVERIFY(QSocImageAttach::detectMimeByMagic(QByteArray("\x89PXX", 4)).isEmpty());
    }

    /* Markers exposed by the helper must round-trip the literal control
     * char so consumers (the agent's extractImageAttachments parser)
     * can match exactly. */
    void markersAreStable()
    {
        const QString openMarker  = QString::fromLatin1(QSocImageAttach::attachmentMarkerOpen());
        const QString closeMarker = QString::fromLatin1(QSocImageAttach::attachmentMarkerClose());
        QVERIFY(openMarker.startsWith(QChar(0x01)));
        QVERIFY(closeMarker.endsWith(QChar(0x01)));
        QVERIFY(openMarker.contains(QStringLiteral("qsoc:image_attachment")));
        QVERIFY(closeMarker.contains(QStringLiteral("qsoc:image_attachment")));
    }

    /* Compression cascade smoke tests. The fixture is a high-entropy
     * gradient so the default JPEG encoder cannot shrink it past the
     * target without dropping quality or dimensions. */
    static QImage gradient(int width, int height)
    {
        QImage img(width, height, QImage::Format_RGB32);
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                const int red   = (x * 255) / width;
                const int green = (y * 255) / height;
                const int blue  = ((x + y) * 255) / (width + height);
                img.setPixel(x, y, qRgb(red, green, blue));
            }
        }
        return img;
    }

    void compressNoBudgetReturnsUnchanged()
    {
        const QImage src = gradient(64, 64);
        const auto out = QSocImageAttach::compressWithinBudget(src, QStringLiteral("image/png"), 0);
        QVERIFY(!out.data.isEmpty());
        QVERIFY(!out.downgraded);
    }

    void compressJpegLadderShrinksUnderTinyBudget()
    {
        const QImage src = gradient(512, 512);
        QBuffer      defaultBuf;
        defaultBuf.open(QIODevice::WriteOnly);
        src.save(&defaultBuf, "JPEG", 80);
        const int baseline = defaultBuf.data().size();

        const int  target = baseline / 2;
        const auto out
            = QSocImageAttach::compressWithinBudget(src, QStringLiteral("image/jpeg"), target);
        QVERIFY2(!out.data.isEmpty(), "cascade produced no data");
        QVERIFY2(out.data.size() <= target, "cascade did not fit budget");
        QVERIFY2(out.downgraded, "cascade should mark downgrade when it dropped quality");
        QCOMPARE(QString::fromLatin1(out.format), QStringLiteral("JPEG"));
    }

    void compressFallsBackToDimensionHalvingForBrutalBudget()
    {
        const QImage src    = gradient(1024, 1024);
        const int    target = 4096;
        const auto   out
            = QSocImageAttach::compressWithinBudget(src, QStringLiteral("image/jpeg"), target);
        QVERIFY(!out.data.isEmpty());
        QVERIFY(out.downgraded);
        QVERIFY2(
            out.finalWidth > 0 && out.finalWidth < src.width(),
            "cascade should shrink dims as last resort");
    }

    void compressPngWithAlphaCanDropToJpeg()
    {
        QImage src(512, 512, QImage::Format_ARGB32);
        for (int y = 0; y < src.height(); ++y) {
            for (int x = 0; x < src.width(); ++x) {
                const int alpha = (x * 255) / src.width();
                src.setPixel(x, y, qRgba(120, 200, 80, alpha));
            }
        }
        QVERIFY(src.hasAlphaChannel());

        QBuffer pngBuf;
        pngBuf.open(QIODevice::WriteOnly);
        src.save(&pngBuf, "PNG");
        const int target = pngBuf.data().size() / 4;
        QVERIFY(target > 32);

        const auto out
            = QSocImageAttach::compressWithinBudget(src, QStringLiteral("image/png"), target);
        QVERIFY(!out.data.isEmpty());
        QVERIFY(out.downgraded);
    }
};

} // namespace

QSOC_TEST_MAIN(TestQSocImageAttach)
#include "test_qsocimageattach.moc"
