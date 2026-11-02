// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "tui/qtuigraphics.h"

#include <QtTest>

Q_DECLARE_METATYPE(QTuiGraphics::ProbeReply)

using QTuiGraphics::ImageStore;
using QTuiGraphics::parseProbeReply;
using QTuiGraphics::ProbeReply;
using QTuiGraphics::Protocol;
using QTuiGraphics::selectProtocol;

namespace {

QProcessEnvironment envOf(const QStringList &pairs)
{
    QProcessEnvironment env;
    for (qsizetype index = 0; index + 1 < pairs.size(); index += 2)
        env.insert(pairs[index], pairs[index + 1]);
    return env;
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void parsesGhosttyReplies();
    void parsesBackgroundColor();
    void parsesSixelAttribute();
    void keepsTypeAheadBetweenReplies();
    void escapeBeforeAReplyIsTypeAhead();
    void unfinishedReplyIsNotTypeAhead();
    void selectsByPriority_data();
    void selectsByPriority();
    void sixelDescribesEveryPixel();
    void sixelLeavesTransparentPixelsUndrawn();
    void passthroughWrapsOnlyInTmux();
    void storeEvictsOldestUnplaced();
    void storeNeverEvictsPlaced();
};

void Test::parsesGhosttyReplies()
{
    const auto reply = parseProbeReply(
        QByteArray("\x1b[6;42;19t\x1b_Gi=31;OK\x1b\\\x1bP>|ghostty 1.3.1\x1b\\\x1b[?62;22;52c"));
    QVERIFY(reply.answered);
    QVERIFY(reply.kitty);
    QVERIFY(!reply.sixel);
    QCOMPARE(reply.cell, QSize(19, 42));
    QCOMPARE(reply.terminal, QStringLiteral("ghostty 1.3.1"));
    QVERIFY(reply.typeAhead.isEmpty());
}

void Test::parsesBackgroundColor()
{
    QVERIFY(parseProbeReply("\x1b]11;rgb:ffff/fefe/f0f0\x1b\\\x1b[?62c").light);
    QVERIFY(!parseProbeReply("\x1b]11;rgb:2828/2828/2828\a\x1b[?62c").light);
    QVERIFY(parseProbeReply("\x1b]11;rgb:ffff/ffff/ffff\a\x1b[?62c").answered);
}

void Test::parsesSixelAttribute()
{
    QVERIFY(parseProbeReply("\x1b[?65;4;6;18;22;52c").sixel);
    QVERIFY(!parseProbeReply("\x1b[?64;14;22c").sixel);
}

void Test::keepsTypeAheadBetweenReplies()
{
    const auto reply = parseProbeReply("ab\x1b_Gi=31;ENOTSUPPORTED\x1b\\c\x1b[A\x1b[?62c");
    QVERIFY(reply.answered);
    QVERIFY(!reply.kitty);
    QCOMPARE(reply.typeAhead, QByteArray("abc\x1b[A"));
}

void Test::escapeBeforeAReplyIsTypeAhead()
{
    const auto reply = parseProbeReply("\x1b\x1b[6;16;8t\x1b[?62c");
    QVERIFY(reply.answered);
    QCOMPARE(reply.cell, QSize(8, 16));
    QCOMPARE(reply.typeAhead, QByteArray("\x1b"));
}

void Test::unfinishedReplyIsNotTypeAhead()
{
    const auto reply = parseProbeReply("x\x1b_Gi=31;O");
    QVERIFY(!reply.answered);
    QCOMPARE(reply.typeAhead, QByteArray("x"));
}

void Test::selectsByPriority_data()
{
    ProbeReply kitty;
    kitty.kitty = true;
    ProbeReply iterm;
    iterm.terminal = QStringLiteral("iTerm2 3.5.0");
    ProbeReply sixel;
    sixel.sixel = true;
    ProbeReply both;
    both.kitty = true;
    both.sixel = true;

    QTest::addColumn<QStringList>("env");
    QTest::addColumn<ProbeReply>("reply");
    QTest::addColumn<int>("expected");
    const auto row =
        [](const char *name, const QStringList &env, const ProbeReply &reply, Protocol want) {
            QTest::newRow(name) << env << reply << int(want);
        };
    row("no term", {}, {}, Protocol::None);
    row("windows terminal", {"WT_SESSION", "x"}, {}, Protocol::Cells);
    row("dumb", {"TERM", "dumb"}, kitty, Protocol::Kitty);
    row("plain terminal", {"TERM", "xterm-256color"}, {}, Protocol::Cells);
    row("probe wins", {"TERM", "xterm-256color"}, kitty, Protocol::Kitty);
    row("xtversion", {"TERM", "xterm-256color"}, iterm, Protocol::ITerm2);
    row(
        "wezterm answers kitty but keeps images in cells",
        {"TERM", "xterm"},
        [] {
            ProbeReply wez;
            wez.kitty    = true;
            wez.terminal = QStringLiteral("WezTerm 20260929");
            return wez;
        }(),
        Protocol::ITerm2);
    row("identity", {"TERM", "xterm", "TERM_PROGRAM", "ghostty"}, {}, Protocol::Kitty);
    row("survives ssh", {"TERM", "xterm", "LC_TERMINAL", "iTerm2"}, {}, Protocol::ITerm2);
    row("vscode", {"TERM", "xterm", "TERM_PROGRAM", "vscode"}, {}, Protocol::Cells);
    row("tmux ignores leaks",
        {"TERM", "tmux-256color", "TMUX", "s", "KITTY_WINDOW_ID", "1"},
        {},
        Protocol::Cells);
    row("tmux client reports kitty", {"TERM", "tmux-256color", "TMUX", "s"}, kitty, Protocol::Kitty);
    row("sixel reply", {"TERM", "xterm"}, sixel, Protocol::Sixel);
    row("kitty beats sixel", {"TERM", "xterm"}, both, Protocol::Kitty);
    row("force sixel", {"TERM", "xterm", "QSOC_IMAGE_PROTOCOL", "sixel"}, {}, Protocol::Sixel);
    row("opt out", {"TERM", "xterm", "QSOC_NO_IMAGE_GRAPHICS", "1"}, kitty, Protocol::None);
    row("force none", {"TERM", "xterm", "QSOC_IMAGE_PROTOCOL", "none"}, kitty, Protocol::None);
    row("force kitty in tmux",
        {"TERM", "screen", "TMUX", "s", "QSOC_IMAGE_PROTOCOL", "kitty"},
        {},
        Protocol::Kitty);
    row("force iterm2", {"TERM", "xterm", "QSOC_IMAGE_PROTOCOL", "iTerm2"}, {}, Protocol::ITerm2);
    row("force cells", {"TERM", "xterm", "QSOC_IMAGE_PROTOCOL", "cells"}, kitty, Protocol::Cells);
}

void Test::selectsByPriority()
{
    QFETCH(QStringList, env);
    QFETCH(ProbeReply, reply);
    QFETCH(int, expected);
    QCOMPARE(int(selectProtocol(envOf(env), reply)), expected);
}

void Test::sixelDescribesEveryPixel()
{
    QImage image(10, 12, QImage::Format_RGB32);
    image.fill(qRgb(255, 0, 0));
    const QByteArray out = QTuiGraphics::encodeSixel(image);
    QVERIFY(out.startsWith("\x1bP0;1;0q\"1;1;10;12"));
    QVERIFY(out.endsWith("\x1b\\"));
    QVERIFY(out.contains(";2;100;0;0"));
    /* Two full bands of ten columns. */
    QCOMPARE(out.count("!10~"), 2);
    QCOMPARE(out.count('-'), 2);
}

void Test::sixelLeavesTransparentPixelsUndrawn()
{
    QImage image(6, 6, QImage::Format_ARGB32);
    image.fill(Qt::transparent);
    const QByteArray out = QTuiGraphics::encodeSixel(image);
    QVERIFY(!out.contains('~'));
}

void Test::passthroughWrapsOnlyInTmux()
{
    qunsetenv("TMUX");
    QCOMPARE(
        QTuiGraphics::passthrough(QStringLiteral("\x1b_Ga=q\x1b\\")),
        QStringLiteral("\x1b_Ga=q\x1b\\"));
    qputenv("TMUX", "s");
    QCOMPARE(
        QTuiGraphics::passthrough(QStringLiteral("\x1b_Ga=q\x1b\\")),
        QStringLiteral("\x1bPtmux;\x1b\x1b_Ga=q\x1b\x1b\\\x1b\\"));
    qunsetenv("TMUX");
}

void Test::storeEvictsOldestUnplaced()
{
    ImageStore store;
    store.setLimitForTest(100);
    QVERIFY(store.admit(1, 40).isEmpty());
    QVERIFY(store.admit(2, 40).isEmpty());
    store.setPlaced(1, true);
    store.setPlaced(1, false);
    QCOMPARE(store.admit(3, 40), QList<quint32>{2});
    QVERIFY(store.isResident(1));
    QVERIFY(!store.isResident(2));
    QVERIFY(store.isResident(3));
}

void Test::storeNeverEvictsPlaced()
{
    ImageStore store;
    store.setLimitForTest(50);
    store.admit(1, 40);
    store.setPlaced(1, true);
    QVERIFY(store.admit(2, 40).isEmpty());
    QVERIFY(store.isResident(1));
    QVERIFY(store.isResident(2));
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qtuigraphics.moc"
