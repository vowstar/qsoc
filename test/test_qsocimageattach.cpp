// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

/* Magic-byte detection unit test. Image fixtures are generated at
 * runtime via QImage; nothing binary is committed to the repo. */

#include "common/qsocimageattach.h"
#include "qsoc_test.h"

#include <QBuffer>
#include <QByteArray>
#include <QColor>
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
};

} // namespace

QSOC_TEST_MAIN(TestQSocImageAttach)
#include "test_qsocimageattach.moc"
