// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocsvg.h"
#include "qsoc_test.h"

#include <QImage>
#include <QProcess>
#include <QtTest>

namespace {

QByteArray sanitized(const QByteArray &source, bool light = false)
{
    QString error;
    return QSocSvg::sanitize(source, light, &error);
}

QImage draw(const QByteArray &source, QString *error = nullptr)
{
    QProcess process;
    process.start(QStringLiteral(QSOC_BINARY_PATH), {"--svg-raster", "dark", "32"});
    process.write(source);
    process.closeWriteChannel();
    process.waitForFinished(20000);
    if (error != nullptr)
        *error = QString::fromUtf8(process.readAllStandardError());
    return QImage::fromData(process.readAllStandardOutput());
}

int paintedPixels(const QImage &image, const QRect &area)
{
    int painted = 0;
    for (int y = area.top(); y <= area.bottom(); ++y) {
        for (int x = area.left(); x <= area.right(); ++x)
            painted += qAlpha(image.pixel(x, y)) > 0 ? 1 : 0;
    }
    return painted;
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void keepsDrawingElements();
    void removesActiveContent();
    void removesExternalReferences();
    void dropsEditorNamespaces();
    void keepsInternalReferences();
    void resolvesTokensForTheBackground();
    void rejectsBadDocuments();
    void helperDrawsTextAtCellScale();
    void helperRejectsInvalidInput();
};

void Test::keepsDrawingElements()
{
    const QByteArray out = sanitized(
        "<svg viewBox=\"0 0 9 9\"><g><rect width=\"9\" height=\"9\"/><text "
        "x=\"1\">Hi</text></g></svg>");
    QVERIFY(out.contains("<rect"));
    QVERIFY(out.contains(">Hi</text>"));
    QVERIFY(out.contains("xmlns=\"http://www.w3.org/2000/svg\""));
}

void Test::removesActiveContent()
{
    const QByteArray out = sanitized(
        "<svg><script>alert(1)</script><foreignObject><div>x</div></foreignObject>"
        "<animate attributeName=\"x\"/><rect onclick=\"alert(2)\" width=\"1\"/></svg>");
    QVERIFY(!out.contains("script"));
    QVERIFY(!out.contains("alert"));
    QVERIFY(!out.contains("foreignObject"));
    QVERIFY(!out.contains("animate"));
    QVERIFY(out.contains("<rect"));
}

void Test::removesExternalReferences()
{
    const QByteArray out = sanitized(
        "<svg xmlns:xlink=\"http://www.w3.org/1999/xlink\"><image href=\"file:///etc/passwd\"/>"
        "<use xlink:href=\"http://example.invalid/a.svg#b\"/>"
        "<rect fill=\"url(http://example.invalid/x)\" width=\"1\"/></svg>");
    QVERIFY(!out.contains("passwd"));
    QVERIFY(!out.contains("example.invalid"));
    QVERIFY(sanitized("<svg><style>@import url(http://x/y.css);</style></svg>").isEmpty());
    QVERIFY(
        sanitized("<svg><style><![CDATA[@im]]><![CDATA[port url(http://x/y.css);]]></style></svg>")
            .isEmpty());
    QVERIFY(sanitized("<svg><text>url(not a reference)</text></svg>").contains("url(not"));
}

void Test::dropsEditorNamespaces()
{
    const QByteArray out = sanitized(
        "<svg xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\">"
        "<g inkscape:label=\"a\"><rect width=\"1\"/></g></svg>");
    QVERIFY(!out.contains("inkscape"));
    QVERIFY(sanitized("<svg><style>a{}<g/></style></svg>").isEmpty());
}

void Test::keepsInternalReferences()
{
    const QByteArray out = sanitized(
        "<svg><defs><linearGradient id=\"g\"/></defs><rect id=\"r\" fill=\"url(#g)\"/>"
        "<use href=\"#r\"/></svg>");
    QVERIFY(out.contains("url(#g)"));
    QVERIFY(out.contains("#r"));
}

void Test::resolvesTokensForTheBackground()
{
    const QByteArray source
        = "<svg><rect fill=\"var(--c1)\" stroke=\"var(--missing, red)\"/></svg>";
    QVERIFY(sanitized(source).contains("#458588"));
    QVERIFY(sanitized(source, true).contains("#076678"));
    QVERIFY(sanitized(source).contains("stroke=\"#ebdbb2\""));
    QVERIFY(!sanitized(source).contains("var("));
}

void Test::rejectsBadDocuments()
{
    QVERIFY(sanitized("<html/>").isEmpty());
    QVERIFY(sanitized("<svg><rect").isEmpty());
    QVERIFY(sanitized("<!DOCTYPE svg [<!ENTITY a \"b\">]><svg>&a;</svg>").isEmpty());
    QVERIFY(sanitized(QByteArray(QSocSvg::kMaxSourceBytes + 1, ' ')).isEmpty());
}

void Test::helperDrawsTextAtCellScale()
{
    QString      error;
    const QImage image = draw(
        "<svg viewBox=\"0 0 100 50\"><rect width=\"50\" height=\"25\" fill=\"var(--c1)\"/>"
        "<text x=\"50\" y=\"45\" font-size=\"14\">Label</text></svg>",
        &error);
    QVERIFY2(!image.isNull(), qPrintable(error));
    /* A 32 pixel cell maps 16 units to one row: twice the viewBox. */
    QCOMPARE(image.size(), QSize(200, 100));
    QCOMPARE(QColor(image.pixel(10, 10)), QColor(0x45, 0x85, 0x88));
    QCOMPARE(qAlpha(image.pixel(190, 10)), 0);
    QVERIFY(paintedPixels(image, QRect(100, 60, 100, 40)) > 0);
}

void Test::helperRejectsInvalidInput()
{
    QString error;
    QVERIFY(draw("<not-svg/>", &error).isNull());
    QVERIFY(!error.isEmpty());
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocsvg.moc"
