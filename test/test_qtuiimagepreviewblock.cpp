// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuiimagepreviewblock.h"

#include <QtTest>

namespace {

void clearGraphicsEnv()
{
    /* Wipe every signal detectProtocol() looks at so each test starts
     * from a clean slate regardless of what host env shell launched
     * the suite. */
    qunsetenv("KITTY_WINDOW_ID");
    qunsetenv("GHOSTTY_RESOURCES_DIR");
    qunsetenv("WEZTERM_EXECUTABLE");
    qunsetenv("KONSOLE_VERSION");
    qunsetenv("TERM");
    qunsetenv("TERM_PROGRAM");
}

QTuiImagePreviewBlock makePngBlock()
{
    return {
        QStringLiteral("/tmp/x.png"),
        QStringLiteral("image/png"),
        320,
        240,
        QByteArray("\x89PNG\r\n", 6)};
}

constexpr auto kKittyEscapePrefix  = "\x1b_G";
constexpr auto kITerm2EscapePrefix = "\x1b]1337;File=";

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void init() { clearGraphicsEnv(); }
    void cleanup() { clearGraphicsEnv(); }

    void layoutProducesSingleRowPlaceholder();
    void plainTextSummarisesMetadata();
    void markdownEmitsImageSyntax();
    void toAnsiContainsPlaceholderText();

    /* Protocol routing */
    void emptyEnvProducesNoEscape();
    void kittyWindowIdSelectsKitty();
    void ghosttyResourcesDirSelectsKitty();
    void ghosttyTermProgramSelectsKitty();
    void wezTermExecutableSelectsKitty();
    void wezTermProgramSelectsKitty();
    void rioTermProgramSelectsKitty();
    void konsoleVersionSelectsKitty();
    void xtermKittySelectsKitty();
    void xtermGhosttySelectsKitty();
    void itermSelectsITerm2();
    void vscodeSelectsITerm2();
    void minttySelectsITerm2();
    void termProgramCaseInsensitive();
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

void Test::emptyEnvProducesNoEscape()
{
    QTuiImagePreviewBlock block = makePngBlock();
    const QString         out   = block.toAnsi(60);
    QVERIFY(!out.contains(QLatin1String(kKittyEscapePrefix)));
    QVERIFY(!out.contains(QLatin1String(kITerm2EscapePrefix)));
    QVERIFY(out.contains(QStringLiteral("[image: ")));
}

void Test::kittyWindowIdSelectsKitty()
{
    qputenv("KITTY_WINDOW_ID", "1");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kKittyEscapePrefix)));
}

void Test::ghosttyResourcesDirSelectsKitty()
{
    qputenv("GHOSTTY_RESOURCES_DIR", "/usr/share/ghostty");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kKittyEscapePrefix)));
}

void Test::ghosttyTermProgramSelectsKitty()
{
    qputenv("TERM_PROGRAM", "ghostty");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kKittyEscapePrefix)));
}

void Test::wezTermExecutableSelectsKitty()
{
    qputenv("WEZTERM_EXECUTABLE", "/usr/bin/wezterm");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kKittyEscapePrefix)));
}

void Test::wezTermProgramSelectsKitty()
{
    qputenv("TERM_PROGRAM", "WezTerm");
    QTuiImagePreviewBlock block = makePngBlock();
    const QString         out   = block.toAnsi(60);
    QVERIFY(out.contains(QLatin1String(kKittyEscapePrefix)));
    QVERIFY(!out.contains(QLatin1String(kITerm2EscapePrefix)));
}

void Test::rioTermProgramSelectsKitty()
{
    qputenv("TERM_PROGRAM", "rio");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kKittyEscapePrefix)));
}

void Test::konsoleVersionSelectsKitty()
{
    qputenv("KONSOLE_VERSION", "230400");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kKittyEscapePrefix)));
}

void Test::xtermKittySelectsKitty()
{
    qputenv("TERM", "xterm-kitty");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kKittyEscapePrefix)));
}

void Test::xtermGhosttySelectsKitty()
{
    qputenv("TERM", "xterm-ghostty");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kKittyEscapePrefix)));
}

void Test::itermSelectsITerm2()
{
    qputenv("TERM_PROGRAM", "iTerm.app");
    QTuiImagePreviewBlock block = makePngBlock();
    const QString         out   = block.toAnsi(60);
    QVERIFY(out.contains(QLatin1String(kITerm2EscapePrefix)));
    QVERIFY(!out.contains(QLatin1String(kKittyEscapePrefix)));
}

void Test::vscodeSelectsITerm2()
{
    qputenv("TERM_PROGRAM", "vscode");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kITerm2EscapePrefix)));
}

void Test::minttySelectsITerm2()
{
    qputenv("TERM", "mintty");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kITerm2EscapePrefix)));
}

void Test::termProgramCaseInsensitive()
{
    /* Real terminals advertise mixed case (iTerm.app, WezTerm). The
     * detector lowercases TERM_PROGRAM so capitalisation can't gate
     * detection. */
    qputenv("TERM_PROGRAM", "GHOSTTY");
    QTuiImagePreviewBlock block = makePngBlock();
    QVERIFY(block.toAnsi(60).contains(QLatin1String(kKittyEscapePrefix)));
}

QSOC_TEST_MAIN(Test)
#include "test_qtuiimagepreviewblock.moc"
