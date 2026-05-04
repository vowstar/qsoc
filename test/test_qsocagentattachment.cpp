// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

/* C4 unit test: extractImageAttachments must reliably strip the C3
 * sentinel-bracketed JSON, parse fields out of it, and survive
 * malformed input without losing surrounding text. The downstream
 * addToolMessage() path is exercised by the integration suite; here we
 * isolate the parser itself so a regression cannot silently leak base64
 * blobs into the user-facing scrollview. */

#include "agent/qsocagent.h"
#include "agent/tool/qsoctoolweb.h"
#include "qsoc_test.h"

#include <QString>
#include <QtTest>

namespace {

QString openMarker()
{
    return QString::fromLatin1(QSocToolWebFetch::attachmentMarkerOpen());
}
QString closeMarker()
{
    return QString::fromLatin1(QSocToolWebFetch::attachmentMarkerClose());
}

QString wrap(const QString &json)
{
    return openMarker() + json + closeMarker();
}

class TestQSocAgentAttachment : public QObject
{
    Q_OBJECT

private slots:
    /* No marker -> input passes through verbatim, attachments empty. */
    void plainTextRoundTrips()
    {
        QList<QSocAgent::AttachmentSpec> out;
        const QString                    input  = QStringLiteral("just a regular tool result\n");
        const QString                    result = QSocAgent::extractImageAttachments(input, &out);
        QCOMPARE(result, input);
        QCOMPARE(out.size(), 0);
    }

    /* Single well-formed attachment: marker is removed, JSON fields
     * land on the spec, the human-readable summary line is preserved. */
    void singleAttachmentParses()
    {
        const QString json = QStringLiteral(
            "{\"mime\":\"image/jpeg\",\"data\":\"YWJjZA==\","
            "\"source_url\":\"https://e/x.png\",\"width\":1024,\"height\":768,"
            "\"byte_size\":12345,\"est_tokens\":1398,\"resized\":true}");
        const QString input = QStringLiteral("[image attached: 1024x768 ~1398 tokens]\n")
                              + wrap(json) + QStringLiteral("\n");

        QList<QSocAgent::AttachmentSpec> out;
        const QString                    stripped = QSocAgent::extractImageAttachments(input, &out);

        QCOMPARE(stripped, QStringLiteral("[image attached: 1024x768 ~1398 tokens]\n"));
        QCOMPARE(out.size(), 1);
        QCOMPARE(out[0].mime, QStringLiteral("image/jpeg"));
        QCOMPARE(out[0].dataB64, QStringLiteral("YWJjZA=="));
        QCOMPARE(out[0].sourceUrl, QStringLiteral("https://e/x.png"));
        QCOMPARE(out[0].width, 1024);
        QCOMPARE(out[0].height, 768);
        QCOMPARE(out[0].byteSize, 12345);
        QCOMPARE(out[0].estTokens, 1398);
        QCOMPARE(out[0].resized, true);
    }

    /* Two attachments back-to-back (rare but valid: a tool could return
     * a gallery). Both must parse and the strip must collapse cleanly. */
    void multipleAttachmentsParse()
    {
        const QString jsonA = QStringLiteral(
            "{\"mime\":\"image/png\",\"data\":\"AAA=\","
            "\"source_url\":\"https://e/a.png\",\"width\":256,\"height\":256,"
            "\"byte_size\":100,\"est_tokens\":87,\"resized\":false}");
        const QString jsonB = QStringLiteral(
            "{\"mime\":\"image/png\",\"data\":\"BBB=\","
            "\"source_url\":\"https://e/b.png\",\"width\":512,\"height\":512,"
            "\"byte_size\":200,\"est_tokens\":349,\"resized\":false}");
        const QString input = QStringLiteral("first\n") + wrap(jsonA) + QStringLiteral("\nsecond\n")
                              + wrap(jsonB) + QStringLiteral("\ntail");

        QList<QSocAgent::AttachmentSpec> out;
        const QString                    stripped = QSocAgent::extractImageAttachments(input, &out);

        QCOMPARE(stripped, QStringLiteral("first\nsecond\ntail"));
        QCOMPARE(out.size(), 2);
        QCOMPARE(out[0].sourceUrl, QStringLiteral("https://e/a.png"));
        QCOMPARE(out[1].sourceUrl, QStringLiteral("https://e/b.png"));
    }

    /* Malformed JSON inside the marker must not lose the surrounding
     * text; the attachment is dropped silently because the textual
     * summary preceding it still tells the model what was supposed to
     * be there. */
    void malformedJsonDropsAttachmentKeepsText()
    {
        const QString input = QStringLiteral("intro\n") + wrap(QStringLiteral("{not valid json"))
                              + QStringLiteral("\noutro");

        QList<QSocAgent::AttachmentSpec> out;
        const QString                    stripped = QSocAgent::extractImageAttachments(input, &out);

        QCOMPARE(stripped, QStringLiteral("intro\noutro"));
        QCOMPARE(out.size(), 0);
    }

    /* Open marker without a close: keep all text verbatim, do not parse
     * an attachment. Defensive against truncated tool output. */
    void unterminatedMarkerLeavesContentIntact()
    {
        const QString input = QStringLiteral("intro\n") + openMarker()
                              + QStringLiteral("{\"mime\":\"image/png\",\"data\":\"AAA=\"")
                              + QStringLiteral(" : abrupt cutoff");

        QList<QSocAgent::AttachmentSpec> out;
        const QString                    stripped = QSocAgent::extractImageAttachments(input, &out);

        QCOMPARE(stripped, input);
        QCOMPARE(out.size(), 0);
    }

    /* Missing required fields (no mime / no data) must not produce a
     * spec; sending an empty image_url to the model wastes a request. */
    void attachmentMissingRequiredFieldsIsDropped()
    {
        const QString json = QStringLiteral(
            "{\"source_url\":\"https://e/x.png\",\"width\":256,\"height\":256}");
        const QString input = wrap(json);

        QList<QSocAgent::AttachmentSpec> out;
        const QString                    stripped = QSocAgent::extractImageAttachments(input, &out);

        QCOMPARE(stripped, QString());
        QCOMPARE(out.size(), 0);
    }

    /* Caller passes nullptr for the out list: function still strips. */
    void nullOutputListIsAccepted()
    {
        const QString json = QStringLiteral(
            "{\"mime\":\"image/png\",\"data\":\"AAA=\","
            "\"source_url\":\"https://e/x.png\"}");
        const QString input    = QStringLiteral("intro\n") + wrap(json) + QStringLiteral("\noutro");
        const QString stripped = QSocAgent::extractImageAttachments(input, nullptr);
        QCOMPARE(stripped, QStringLiteral("intro\noutro"));
    }
};

} // namespace

QSOC_TEST_MAIN(TestQSocAgentAttachment)
#include "test_qsocagentattachment.moc"
