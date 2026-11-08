// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"
#include "qsoc_test_pty.h"
#include "tui/qtuigraphics.h"

#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
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
    void tmuxClients_data();
    void tmuxClients();
    void terminalProbe_data();
    void terminalProbe();
    void tmuxReconnect_data();
    void tmuxReconnect();
    void emptyTmuxDoesNotRefresh();
    void parsesSixelGeometry();
    void lateRepliesRefreshImages();
    void lateSixelThroughTmux();
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
    QCOMPARE(reply.kitty, QTuiGraphics::Support::Yes);
    QCOMPARE(reply.sixel, QTuiGraphics::Support::No);
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
    QCOMPARE(parseProbeReply("\x1b[?65;4;6;18;22;52c").sixel, QTuiGraphics::Support::Yes);
    QCOMPARE(parseProbeReply("\x1b[?64;14;22c").sixel, QTuiGraphics::Support::No);
}

void Test::keepsTypeAheadBetweenReplies()
{
    const auto reply = parseProbeReply("ab\x1b_Gi=31;ENOTSUPPORTED\x1b\\c\x1b[A\x1b[?62c");
    QVERIFY(reply.answered);
    QCOMPARE(reply.kitty, QTuiGraphics::Support::No);
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
    kitty.kitty = QTuiGraphics::Support::Yes;
    ProbeReply iterm;
    iterm.terminal = QStringLiteral("iTerm2 3.5.0");
    ProbeReply sixel;
    sixel.sixel = QTuiGraphics::Support::Yes;
    ProbeReply both;
    both.kitty = QTuiGraphics::Support::Yes;
    both.sixel = QTuiGraphics::Support::Yes;

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
            wez.kitty    = QTuiGraphics::Support::Yes;
            wez.terminal = QStringLiteral("WezTerm 20260929");
            return wez;
        }(),
        Protocol::ITerm2);
    row("identity", {"TERM", "xterm", "TERM_PROGRAM", "ghostty"}, {}, Protocol::Kitty);
    row("survives ssh", {"TERM", "xterm", "LC_TERMINAL", "iTerm2"}, {}, Protocol::ITerm2);
    row("vscode", {"TERM", "xterm", "TERM_PROGRAM", "vscode"}, {}, Protocol::Cells);
    row("empty kitty marker", {"TERM", "xterm", "KITTY_WINDOW_ID", ""}, {}, Protocol::Cells);
    row("rejected kitty beats identity",
        {"TERM", "xterm", "KITTY_WINDOW_ID", "1"},
        parseProbeReply("\x1b_Gi=31;ENOTSUPPORTED\x1b\\\x1b[?62;4c"),
        Protocol::Sixel);
    row("sixel evidence beats stale identity",
        {"TERM", "xterm", "TERM_PROGRAM", "ghostty"},
        sixel,
        Protocol::Sixel);
    row("tmux term survives stripped markers",
        {"TERM", "tmux-256color", "KITTY_WINDOW_ID", "1"},
        {},
        Protocol::Cells);
    row("terminal name is not a substring",
        {"TERM", "xterm"},
        parseProbeReply("\x1bP>|notkitty 1.0\x1b\\\x1b[?62c"),
        Protocol::Cells);
    row("tmux ignores leaks",
        {"TERM", "tmux-256color", "TMUX", "s", "KITTY_WINDOW_ID", "1"},
        {},
        Protocol::Cells);
    row("pane Kitty reply is not client evidence",
        {"TERM", "tmux-256color", "TMUX", "s"},
        kitty,
        Protocol::Cells);
    row("pane Sixel reply is not client evidence",
        {"TERM", "tmux-256color", "TMUX", "s"},
        sixel,
        Protocol::Cells);
    row("embedder identity is not capability",
        {"TERM", "xterm", "TERM_PROGRAM", "kitty", "PASEO_TERMINAL_ID", "pane"},
        {},
        Protocol::Cells);
    row(
        "tmux client reports kitty",
        {"TERM", "tmux-256color", "TMUX", "s"},
        [] {
            ProbeReply client;
            client.kitty        = QTuiGraphics::Support::Yes;
            client.placeholders = QTuiGraphics::Support::Yes;
            client.passthrough  = QTuiGraphics::Support::Yes;
            client.evidence     = QTuiGraphics::Evidence::Tmux;
            return client;
        }(),
        Protocol::Kitty);
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

bool writeState(const QString &root, const QString &name, const QByteArray &contents)
{
    QFile file(QDir(root).filePath(name));
    return file.open(QIODevice::WriteOnly) && file.write(contents) == contents.size();
}

bool installTmux(const QString &root)
{
    const QByteArray script
        = "#!/bin/sh\n"
          "state=$QSOC_TEST_STATE\n"
          "if [ -f \"$state/fail\" ]; then touch \"$state/failed\"; exit 1; fi\n"
          "case \"$1\" in\n"
          "list-panes) cat \"$state/panes\";;\n"
          "list-clients)\n"
          "  if [ -f \"$state/delay\" ]; then touch \"$state/querying\"; sleep \"$(cat "
          "\"$state/delay\")\"; fi\n"
          "  cat \"$state/clients\";;\n"
          "show-options) cat \"$state/passthrough\";;\n"
          "display-message) cat \"$state/version\";;\n"
          "show-messages) [ \"$4\" != sixel ] || printf '1: Sxl: (flag) true\\n';;\n"
          "*) exit 1;;\n"
          "esac\n";
    if (!writeState(root, QStringLiteral("tmux"), script))
        return false;
    return QFile::setPermissions(
        QDir(root).filePath(QStringLiteral("tmux")),
        QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner);
}

QProcessEnvironment tmuxEnvironment(const QString &root)
{
    auto env = QSocTestPty::isolatedEnvironment(root);
    env.insert(QStringLiteral("PATH"), root + QLatin1Char(':') + qEnvironmentVariable("PATH"));
    env.insert(QStringLiteral("TMUX"), QDir(root).filePath(QStringLiteral("socket")));
    env.insert(QStringLiteral("TMUX_PANE"), QStringLiteral("%7"));
    env.insert(QStringLiteral("QSOC_TEST_STATE"), root);
    return env;
}

QJsonObject readState(const QString &root)
{
    QFile file(QDir(root).filePath(QStringLiteral("state.json")));
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return QJsonDocument::fromJson(file.readAll()).object();
}

void Test::tmuxClients_data()
{
    QTest::addColumn<QString>("clients");
    QTest::addColumn<QString>("panes");
    QTest::addColumn<QString>("passthrough");
    QTest::addColumn<QString>("version");
    QTest::addColumn<int>("expected");
    const QString graphical = QStringLiteral(
        "graphical\t$1\tghostty 1.3.1\txterm-256color\t101\t@0\t%7\t0\t1\n");
    const QString plain = QStringLiteral("plain\t$1\t\txterm-256color\t102\t@0\t%7\t0\t1\n");
    const auto    row   = [](const char    *name,
                             const QString &clients,
                             const QString &pass,
                             Protocol       expected,
                             const QString &panes   = QStringLiteral("%7\t$1\t@0\n"),
                             const QString &version = QStringLiteral("3.8")) {
        QTest::newRow(name) << clients << panes << pass << version << int(expected);
    };
    row("graphical then plain", graphical + plain, QStringLiteral("on"), Protocol::Kitty);
    row("plain then graphical", plain + graphical, QStringLiteral("on"), Protocol::Kitty);
    row("only graphical", graphical, QStringLiteral("on"), Protocol::Kitty);
    row("only plain", plain, QStringLiteral("on"), Protocol::Cells);
    row("no clients", {}, QStringLiteral("on"), Protocol::Cells);
    row("passthrough off", graphical + plain, QStringLiteral("off"), Protocol::Cells);
    row("passthrough all", graphical + plain, QStringLiteral("all"), Protocol::Kitty);
    row("old tmux",
        graphical,
        {},
        Protocol::Kitty,
        QStringLiteral("%7\t$1\t@0\n"),
        QStringLiteral("3.2a"));
    row("unknown empty option", graphical, {}, Protocol::Cells);
    row("rio",
        QStringLiteral("graphical\t$1\trio 0.5.28\txterm-256color\t101\t@0\t%7\t0\t1\n"),
        QStringLiteral("on"),
        Protocol::Kitty);
    row("client term fallback",
        QStringLiteral("graphical\t$1\t\txterm-kitty\t101\t@0\t%7\t0\t1\n"),
        QStringLiteral("on"),
        Protocol::Kitty);
    row("nested client is not direct",
        QStringLiteral("graphical\t$1\ttmux 3.8\txterm-kitty\t101\t@0\t%7\t0\t1\n"),
        QStringLiteral("on"),
        Protocol::Cells);
    row("unknown placeholders",
        QStringLiteral("graphical\t$1\tkonsole 26.0\txterm-256color\t101\t@0\t%7\t0\t1\n"),
        QStringLiteral("on"),
        Protocol::Cells);
    row("wezterm does not imply placeholders",
        QStringLiteral("graphical\t$1\tWezTerm 20260929\twezterm\t101\t@0\t%7\t0\t1\n"),
        QStringLiteral("on"),
        Protocol::Cells);
    row("client views another window",
        QStringLiteral("graphical\t$1\tghostty 1.3.1\txterm-256color\t101\t@1\t%8\t0\t1\n"),
        QStringLiteral("on"),
        Protocol::Cells);
    row("another pane is zoomed",
        QStringLiteral("graphical\t$1\tghostty 1.3.1\txterm-256color\t101\t@0\t%8\t1\t1\n"),
        QStringLiteral("on"),
        Protocol::Cells);
    row("non UTF-8 client",
        QStringLiteral("graphical\t$1\tghostty 1.3.1\txterm-256color\t101\t@0\t%7\t0\t0\n"),
        QStringLiteral("on"),
        Protocol::None);
    row("unknown UTF-8 client",
        QStringLiteral("graphical\t$1\tghostty 1.3.1\txterm-256color\t101\t@0\t%7\t0\t\n"),
        QStringLiteral("on"),
        Protocol::Cells);
    row("sixel after plain",
        plain + QStringLiteral("sixel\t$1\tfoot\txterm-256color\t103\t@0\t%7\t0\t1\n"),
        QStringLiteral("off"),
        Protocol::Sixel);
    row("linked sessions",
        graphical + QStringLiteral("plain\t$2\t\txterm-256color\t102\t@0\t%7\t0\t1\n"),
        QStringLiteral("on"),
        Protocol::Kitty,
        QStringLiteral("%7\t$1\t@0\n%7\t$2\t@0\n"));
    row("unrelated session",
        graphical + QStringLiteral("plain\t$2\t\txterm-256color\t102\t@0\t%7\t0\t1\n"),
        QStringLiteral("on"),
        Protocol::Cells,
        QStringLiteral("%8\t$1\t@0\n%7\t$2\t@0\n"));
}

void Test::tmuxClients()
{
#ifdef Q_OS_UNIX
    QFETCH(QString, clients);
    QFETCH(QString, panes);
    QFETCH(QString, passthrough);
    QFETCH(QString, version);
    QFETCH(int, expected);
    QTemporaryDir root(QDir::tempPath() + QStringLiteral("/test_qsoc_graphics_XXXXXX"));
    QVERIFY(root.isValid());
    QVERIFY(installTmux(root.path()));
    QVERIFY(writeState(root.path(), QStringLiteral("clients"), clients.toUtf8()));
    QVERIFY(writeState(root.path(), QStringLiteral("panes"), panes.toUtf8()));
    QVERIFY(writeState(root.path(), QStringLiteral("passthrough"), passthrough.toUtf8()));
    QVERIFY(writeState(root.path(), QStringLiteral("version"), version.toUtf8()));
    const QString           resultPath = root.filePath(QStringLiteral("result.json"));
    QSocTestPty::PtyProcess process;
    QVERIFY(process.startInPty(
        QStringLiteral(QSOC_TUI_IMAGE_PROBE_PATH),
        {QStringLiteral("--graphics-probe"), resultPath},
        root.path(),
        tmuxEnvironment(root.path())));
    QVERIFY(process.waitForOutput("\x1b[c", 5000));
    QVERIFY(process.writeInput("\x1b[?62;4c"));
    QVERIFY(process.waitForExit(10000));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QCOMPARE(process.exitCode(), 0);
    QFile result(resultPath);
    QVERIFY(result.open(QIODevice::ReadOnly));
    QCOMPARE(
        QJsonDocument::fromJson(result.readAll()).object().value(QStringLiteral("protocol")).toInt(),
        expected);
#else
    QSKIP("Terminal probing requires a Unix terminal");
#endif
}

void Test::parsesSixelGeometry()
{
    QCOMPARE(parseProbeReply("\x1b[?2;0;1200;800S").sixel, QTuiGraphics::Support::Yes);
    QCOMPARE(parseProbeReply("\x1b[?2;1;1200;800S").sixel, QTuiGraphics::Support::Unknown);
    QCOMPARE(parseProbeReply("\x1b[?2;0;0;0S").sixel, QTuiGraphics::Support::Unknown);
    QCOMPARE(parseProbeReply("\x1b[?2;0;1200;800S\x1b[?62c").sixel, QTuiGraphics::Support::Yes);
}

void Test::terminalProbe_data()
{
    QTest::addColumn<QStringList>("environment");
    QTest::addColumn<QByteArray>("reply");
    QTest::addColumn<int>("delay");
    QTest::addColumn<int>("expected");
    QTest::newRow("slow ssh kitty") << QStringList{} << QByteArray("\x1b_Gi=31;OK\x1b\\\x1b[?62c")
                                    << 400 << int(Protocol::Kitty);
    QTest::newRow("zellij native kitty")
        << QStringList{"ZELLIJ", "0"} << QByteArray("\x1b_Gi=31;OK\x1b\\\x1b[?62c") << 0
        << int(Protocol::Kitty);
    QTest::newRow("explicit zellij beats screen TERM")
        << QStringList{"ZELLIJ", "0", "TERM", "screen-256color"} << QByteArray("\x1b[?62;4c") << 0
        << int(Protocol::Sixel);
    QTest::newRow("zellij sixel") << QStringList{"ZELLIJ", "0"} << QByteArray("\x1b[?62;4c") << 0
                                  << int(Protocol::Sixel);
    QTest::newRow("screen remains conservative")
        << QStringList{"STY", "session"} << QByteArray("\x1b[?62;4c") << 0 << int(Protocol::Cells);
    QTest::newRow("sixel geometry only")
        << QStringList{} << QByteArray("\x1b[?2;0;1200;800S\x1b[?62c") << 0 << int(Protocol::Sixel);
    QTest::newRow("negative kitty")
        << QStringList{"KITTY_WINDOW_ID", "1"}
        << QByteArray("\x1b_Gi=31;ENOTSUPPORTED\x1b\\\x1b[?62c") << 0 << int(Protocol::Cells);
}

void Test::terminalProbe()
{
#ifdef Q_OS_UNIX
    QFETCH(QStringList, environment);
    QFETCH(QByteArray, reply);
    QFETCH(int, delay);
    QFETCH(int, expected);
    QTemporaryDir root(QDir::tempPath() + QStringLiteral("/test_qsoc_probe_XXXXXX"));
    QVERIFY(root.isValid());
    auto env = QSocTestPty::isolatedEnvironment(root.path());
    for (int i = 0; i + 1 < environment.size(); i += 2)
        env.insert(environment[i], environment[i + 1]);
    const QString           path = root.filePath(QStringLiteral("result.json"));
    QSocTestPty::PtyProcess process;
    QVERIFY(process.startInPty(
        QStringLiteral(QSOC_TUI_IMAGE_PROBE_PATH),
        {QStringLiteral("--graphics-probe"), path},
        root.path(),
        env));
    QVERIFY(process.waitForOutput("\x1b[c", 5000));
    QTest::qWait(delay);
    QVERIFY(process.writeInput(reply));
    QVERIFY(process.waitForExit(5000));
    QFile result(path);
    QVERIFY(result.open(QIODevice::ReadOnly));
    QCOMPARE(
        QJsonDocument::fromJson(result.readAll()).object().value(QStringLiteral("protocol")).toInt(),
        expected);
#else
    QSKIP("Terminal probing requires a Unix terminal");
#endif
}

void Test::tmuxReconnect_data()
{
    QTest::addColumn<bool>("startsGraphical");
    QTest::newRow("text to bitmap") << false;
    QTest::newRow("bitmap to text") << true;
}

void Test::tmuxReconnect()
{
#ifdef Q_OS_UNIX
    QFETCH(bool, startsGraphical);
    QTemporaryDir root(QDir::tempPath() + QStringLiteral("/test_qsoc_reconnect_XXXXXX"));
    QVERIFY(root.isValid());
    QVERIFY(installTmux(root.path()));
    const QByteArray graphical("graphical\t$1\tghostty 1.3.1\txterm-256color\t101\t@0\t%7\t0\t1\n");
    const QByteArray plain("plain\t$1\t\txterm-256color\t102\t@0\t%7\t0\t1\n");
    QVERIFY(writeState(root.path(), QStringLiteral("clients"), startsGraphical ? graphical : plain));
    QVERIFY(writeState(root.path(), QStringLiteral("panes"), "%7\t$1\t@0\n"));
    QVERIFY(writeState(root.path(), QStringLiteral("passthrough"), "on"));
    QSocTestPty::PtyProcess process;
    QVERIFY(process.startInPty(
        QStringLiteral(QSOC_TUI_IMAGE_PROBE_PATH),
        {QStringLiteral("--graphics-watch"), root.path()},
        root.path(),
        tmuxEnvironment(root.path())));
    QVERIFY(process.waitForOutput("\x1b[c", 5000));
    QVERIFY(process.writeInput("\x1b[?62c"));
    QVERIFY(process.waitUntil([&] { return !readState(root.path()).isEmpty(); }, 5000));
    QCOMPARE(
        readState(root.path()).value("protocol").toInt(),
        int(startsGraphical ? Protocol::Kitty : Protocol::Cells));
    const auto initialRevision = readState(root.path()).value("revision").toInteger();
    QVERIFY(writeState(root.path(), QStringLiteral("clients"), startsGraphical ? plain : graphical));
    QVERIFY(writeState(root.path(), QStringLiteral("delay"), "1.5"));
    QVERIFY(process.waitUntil(
        [&] { return QFile::exists(root.filePath(QStringLiteral("querying"))); }, 5000));
    QVERIFY(process.writeInput("ALIVE"));
    QVERIFY(process.waitUntil(
        [&] { return readState(root.path()).value("input").toString() == QStringLiteral("ALIVE"); },
        1000));
    QVERIFY(process.waitUntil(
        [&] {
            return readState(root.path()).value("protocol").toInt()
                   == int(startsGraphical ? Protocol::Cells : Protocol::Kitty);
        },
        5000));
    QVERIFY(readState(root.path()).value("revision").toInteger() > initialRevision);
    QCOMPARE(readState(root.path()).value("placeholders").toBool(), !startsGraphical);
    QVERIFY(QFile::remove(root.filePath(QStringLiteral("delay"))));
    QVERIFY(writeState(root.path(), QStringLiteral("fail"), {}));
    const auto stableProtocol = readState(root.path()).value("protocol").toInt();
    const auto stableRevision = readState(root.path()).value("revision").toInteger();
    QVERIFY(process.waitUntil(
        [&] { return QFile::exists(root.filePath(QStringLiteral("failed"))); }, 5000));
    QCOMPARE(readState(root.path()).value("protocol").toInt(), stableProtocol);
    QCOMPARE(readState(root.path()).value("revision").toInteger(), stableRevision);
    QVERIFY(QFile::remove(root.filePath(QStringLiteral("fail"))));

    const auto switchedRevision = readState(root.path()).value("revision").toInteger();
    const auto uploads          = process.output().count("a=t,f=100");
    QByteArray replacement      = startsGraphical ? plain : graphical;
    replacement.replace("101", "105");
    replacement.replace("102", "106");
    QVERIFY(writeState(root.path(), QStringLiteral("clients"), replacement));
    QVERIFY(process.waitUntil(
        [&] { return readState(root.path()).value("revision").toInteger() > switchedRevision; },
        5000));
    if (!startsGraphical)
        QVERIFY(
            process.waitUntil([&] { return process.output().count("a=t,f=100") > uploads; }, 2000));
    QVERIFY(writeState(root.path(), QStringLiteral("stop"), {}));
    QVERIFY(process.waitForExit(5000));
#else
    QSKIP("Terminal probing requires a Unix terminal");
#endif
}

void Test::emptyTmuxDoesNotRefresh()
{
#ifdef Q_OS_UNIX
    QTemporaryDir root(QDir::tempPath() + QStringLiteral("/test_qsoc_empty_tmux_XXXXXX"));
    QVERIFY(root.isValid());
    QVERIFY(installTmux(root.path()));
    QVERIFY(writeState(
        root.path(),
        QStringLiteral("clients"),
        "graphical\t$1\tghostty 1.3.1\txterm-256color\t101\t@0\t%7\t0\t1\n"));
    QVERIFY(writeState(root.path(), QStringLiteral("panes"), "%7\t$1\t@0\n"));
    QVERIFY(writeState(root.path(), QStringLiteral("passthrough"), "on"));
    auto env = tmuxEnvironment(root.path());
    env.insert(QStringLiteral("TMUX"), QString{});
    QSocTestPty::PtyProcess process;
    QVERIFY(process.startInPty(
        QStringLiteral(QSOC_TUI_IMAGE_PROBE_PATH),
        {QStringLiteral("--graphics-watch"), root.path()},
        root.path(),
        env));
    QVERIFY(process.waitForOutput("\x1b[c", 5000));
    QVERIFY(process.writeInput("\x1b[?62c"));
    QVERIFY(process.waitUntil([&] { return !readState(root.path()).isEmpty(); }, 5000));
    QTest::qWait(2500);
    QCOMPARE(readState(root.path()).value("protocol").toInt(), int(Protocol::Cells));
    QVERIFY(readState(root.path()).value("evidence").toInt() != int(QTuiGraphics::Evidence::Tmux));
    QVERIFY(writeState(root.path(), QStringLiteral("stop"), {}));
    QVERIFY(process.waitForExit(5000));
#else
    QSKIP("Terminal probing requires a Unix terminal");
#endif
}

void Test::lateRepliesRefreshImages()
{
#ifdef Q_OS_UNIX
    QTemporaryDir root(QDir::tempPath() + QStringLiteral("/test_qsoc_late_graphics_XXXXXX"));
    QVERIFY(root.isValid());
    QSocTestPty::PtyProcess process;
    QVERIFY(process.startInPty(
        QStringLiteral(QSOC_TUI_IMAGE_PROBE_PATH),
        {QStringLiteral("--graphics-watch"), root.path()},
        root.path(),
        QSocTestPty::isolatedEnvironment(root.path())));
    QVERIFY(process.waitForOutput("\x1b[c", 5000));
    QVERIFY(process.writeInput("\x1b[?62c"));
    QVERIFY(process.waitUntil([&] { return !readState(root.path()).isEmpty(); }, 5000));
    QCOMPARE(readState(root.path()).value("protocol").toInt(), int(Protocol::Cells));
    QVERIFY(process.writeInput("\x1b_Gi=31;"));
    QTest::qWait(30);
    QVERIFY(process.writeInput("OK\x1b\\late"));
    QVERIFY(process.waitUntil(
        [&] {
            return readState(root.path()).value("protocol").toInt() == int(Protocol::Kitty)
                   && readState(root.path()).value("input").toString() == QStringLiteral("late");
        },
        2000));
    QCOMPARE(readState(root.path()).value("evidence").toInt(), int(QTuiGraphics::Evidence::Terminal));
    QVERIFY(process.waitUntil([&] { return process.output().contains("a=t,f=100"); }, 2000));
    QVERIFY(writeState(root.path(), QStringLiteral("stop"), {}));
    QVERIFY(process.waitForExit(5000));
#else
    QSKIP("Terminal probing requires a Unix terminal");
#endif
}

void Test::lateSixelThroughTmux()
{
#ifdef Q_OS_UNIX
    QTemporaryDir root(QDir::tempPath() + QStringLiteral("/test_qsoc_late_sixel_XXXXXX"));
    QVERIFY(root.isValid());
    QVERIFY(installTmux(root.path()));
    QVERIFY(writeState(
        root.path(),
        QStringLiteral("clients"),
        "sixel\t$1\tfoot\txterm-256color\t103\t@0\t%7\t0\t1\n"));
    QVERIFY(writeState(root.path(), QStringLiteral("panes"), "%7\t$1\t@0\n"));
    QVERIFY(writeState(root.path(), QStringLiteral("passthrough"), "off"));
    QSocTestPty::PtyProcess process;
    QVERIFY(process.startInPty(
        QStringLiteral(QSOC_TUI_IMAGE_PROBE_PATH),
        {QStringLiteral("--graphics-watch"), root.path()},
        root.path(),
        tmuxEnvironment(root.path())));
    QVERIFY(process.waitForOutput("\x1b[c", 5000));
    QVERIFY(process.writeInput("\x1b[?62c"));
    QVERIFY(process.waitUntil([&] { return !readState(root.path()).isEmpty(); }, 5000));
    QCOMPARE(readState(root.path()).value("protocol").toInt(), int(Protocol::Cells));
    QVERIFY(process.writeInput("\x1b[?62;4c"));
    QVERIFY(process.waitUntil(
        [&] { return readState(root.path()).value("protocol").toInt() == int(Protocol::Sixel); },
        5000));
    QCOMPARE(readState(root.path()).value("evidence").toInt(), int(QTuiGraphics::Evidence::Tmux));
    QVERIFY(writeState(root.path(), QStringLiteral("stop"), {}));
    QVERIFY(process.waitForExit(5000));
#else
    QSKIP("Terminal probing requires a Unix terminal");
#endif
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
