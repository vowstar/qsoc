// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiimagepreviewblock.h"

#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void layoutProducesSingleRowPlaceholder();
    void plainTextSummarisesMetadata();
    void markdownEmitsImageSyntax();
    void toAnsiContainsPlaceholderText();
};

void Test::layoutProducesSingleRowPlaceholder()
{
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/foo.png"),
        QStringLiteral("image/png"),
        800,
        600,
        QByteArray("\x89PNG\r\n", 6));
    block.layout(80);
    QCOMPARE(block.rowCount(), 1);
}

void Test::plainTextSummarisesMetadata()
{
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/foo.png"),
        QStringLiteral("image/png"),
        800,
        600,
        QByteArray("\x89PNG\r\n", 6));
    const QString text = block.toPlainText();
    QVERIFY(text.contains(QStringLiteral("/tmp/foo.png")));
    QVERIFY(text.contains(QStringLiteral("png")));
    QVERIFY(text.contains(QStringLiteral("800x600")));
}

void Test::markdownEmitsImageSyntax()
{
    QTuiImagePreviewBlock
        block(QStringLiteral("hello.jpg"), QStringLiteral("image/jpeg"), 100, 50, QByteArray());
    QCOMPARE(block.toMarkdown(), QStringLiteral("![image](hello.jpg)\n"));
}

void Test::toAnsiContainsPlaceholderText()
{
    QTuiImagePreviewBlock block(
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        QByteArray("\x89PNG\r\n", 6));
    const QString out = block.toAnsi(60);
    QVERIFY(out.contains(QStringLiteral("[image: ")));
    QVERIFY(out.contains(QStringLiteral("x.png")));
}

QSOC_TEST_MAIN(Test)
#include "test_qtuiimagepreviewblock.moc"
