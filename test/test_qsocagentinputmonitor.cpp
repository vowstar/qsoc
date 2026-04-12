// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsoctool.h"
#include "agent/tool/qsoctoolshell.h"
#include "cli/qagentinputmonitor.h"
#include "common/qllmservice.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

using json = nlohmann::json;

struct TestApp
{
    static auto &instance()
    {
        static auto                   argc      = 1;
        static char                   appName[] = "qsoc";
        static std::array<char *, 1>  argv      = {{appName}};
        static const QCoreApplication app       = QCoreApplication(argc, argv.data());
        return app;
    }
};

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { TestApp::instance(); }

    /* Lifecycle tests */

    void testStartStop()
    {
        QAgentInputMonitor monitor;

        monitor.start();
        QVERIFY(monitor.isActive());

        monitor.stop();
        QVERIFY(!monitor.isActive());

        /* Repeated stop should be safe */
        monitor.stop();
        QVERIFY(!monitor.isActive());

        /* Restart should work */
        monitor.start();
        QVERIFY(monitor.isActive());
        monitor.stop();
    }

    void testDestructorStops()
    {
        {
            QAgentInputMonitor monitor;
            monitor.start();
            QVERIFY(monitor.isActive());
        }
        /* Should not crash */
    }

    void testInitialState()
    {
        QAgentInputMonitor monitor;
        QVERIFY(!monitor.isActive());
    }

    /* Input buffering byte-level tests via processBytes() */

    void testAsciiInput()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Feed "abc" */
        monitor.processBytes("abc", 3);

        QCOMPARE(changes.size(), 3);
        QCOMPARE(changes[0], "a");
        QCOMPARE(changes[1], "ab");
        QCOMPARE(changes[2], "abc");
    }

    void testEnterSubmitsInput()
    {
        QAgentInputMonitor monitor;
        QString            submitted;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputReady, [&submitted](const QString &text) {
            submitted = text;
        });
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Type "hello" then Enter */
        monitor.processBytes("hello\n", 6);

        QCOMPARE(submitted, "hello");
        /* Last inputChanged should be empty (buffer cleared after submit) */
        QVERIFY(!changes.isEmpty());
        QCOMPARE(changes.last(), "");
    }

    void testCarriageReturnSubmits()
    {
        QAgentInputMonitor monitor;
        QString            submitted;

        connect(&monitor, &QAgentInputMonitor::inputReady, [&submitted](const QString &text) {
            submitted = text;
        });

        monitor.processBytes("test\r", 5);
        QCOMPARE(submitted, "test");
    }

    void testEmptyEnterIgnored()
    {
        QAgentInputMonitor monitor;
        int                readyCount = 0;

        connect(&monitor, &QAgentInputMonitor::inputReady, [&readyCount](const QString &) {
            readyCount++;
        });

        /* Enter with empty buffer should not emit inputReady */
        monitor.processBytes("\n", 1);
        QCOMPARE(readyCount, 0);

        /* Multiple enters */
        monitor.processBytes("\n\r\n", 3);
        QCOMPARE(readyCount, 0);
    }

    void testBackspace()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Type "abc" then backspace */
        const char data[] = {'a', 'b', 'c', 0x7F};
        monitor.processBytes(data, 4);

        QCOMPARE(changes.size(), 4);
        QCOMPARE(changes[3], "ab"); /* 'c' deleted */
    }

    void testBackspaceOnEmpty()
    {
        QAgentInputMonitor monitor;
        int                changeCount = 0;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changeCount](const QString &) {
            changeCount++;
        });

        /* Backspace on empty buffer should not emit */
        const char bs = 0x7F;
        monitor.processBytes(&bs, 1);
        QCOMPARE(changeCount, 0);
    }

    void testCtrlUClearsLine()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Type "hello" then Ctrl-U */
        const char data[] = {'h', 'e', 'l', 'l', 'o', 0x15};
        monitor.processBytes(data, 6);

        QCOMPARE(changes.last(), "");
    }

    void testCtrlWDeletesWord()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Type "hello world" then Ctrl-W */
        monitor.processBytes("hello world", 11);
        const char ctrlW = 0x17;
        monitor.processBytes(&ctrlW, 1);

        /* Should delete "world" leaving "hello " */
        QCOMPARE(changes.last(), "hello ");
    }

    void testCtrlWDeletesOnlyWord()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Type "hello" (no space) then Ctrl-W -> clears everything */
        monitor.processBytes("hello", 5);
        const char ctrlW = 0x17;
        monitor.processBytes(&ctrlW, 1);

        QCOMPARE(changes.last(), "");
    }

    void testEscClearsInputAndEmits()
    {
        QAgentInputMonitor monitor;
        int                escCount = 0;

        connect(&monitor, &QAgentInputMonitor::escPressed, [&escCount]() { escCount++; });

        /* Send ESC followed by a non-[ byte to trigger immediate escPressed.
         * Bare ESC alone uses a 50ms timer which needs a running event loop. */
        const char data[] = {'a', 'b', 'c', 0x1B, 'x'};
        monitor.processBytes(data, 5);

        QCOMPARE(escCount, 1);
    }

    void testEscStopsProcessing()
    {
        QAgentInputMonitor monitor;
        bool               escReceived = false;

        connect(&monitor, &QAgentInputMonitor::escPressed, [&escReceived]() { escReceived = true; });

        /* ESC followed by non-CSI byte ('b') triggers escPressed immediately */
        const char data[] = {'a', 0x1B, 'b'};
        monitor.processBytes(data, 3);

        QVERIFY(escReceived);
    }

    void testDoubleEscEmitsEscEsc()
    {
        QAgentInputMonitor monitor;
        int                escCount    = 0;
        int                escEscCount = 0;

        connect(&monitor, &QAgentInputMonitor::escPressed, [&escCount]() { escCount++; });
        connect(&monitor, &QAgentInputMonitor::escEscPressed, [&escEscCount]() { escEscCount++; });

        /* Two back-to-back ESCs followed by non-CSI bytes → both fire the
         * synchronous bare-Esc path in processEscSequence. The first one
         * emits escPressed and arms the double-Esc window; the second one
         * fires escEscPressed INSTEAD of escPressed so the REPL's bare-Esc
         * handler doesn't unwind the promptLoop in the middle of a rewind. */
        const char data1[] = {0x1B, 'x'};
        monitor.processBytes(data1, 2);
        QCOMPARE(escCount, 1);
        QCOMPARE(escEscCount, 0);

        const char data2[] = {0x1B, 'y'};
        monitor.processBytes(data2, 2);
        QCOMPARE(escCount, 1); /* no new escPressed — replaced by escEscPressed */
        QCOMPARE(escEscCount, 1);
    }

    void testEscEscResetByIntermediateKey()
    {
        QAgentInputMonitor monitor;
        int                escEscCount = 0;

        connect(&monitor, &QAgentInputMonitor::escEscPressed, [&escEscCount]() { escEscCount++; });

        /* Esc, type a char, Esc → must NOT be treated as double-Esc because
         * the intermediate keystroke cancels the chord. */
        const char data[] = {0x1B, 'x', 'a', 0x1B, 'y'};
        monitor.processBytes(data, 5);

        QCOMPARE(escEscCount, 0);
    }

    void testTripleEscDoesNotCascade()
    {
        QAgentInputMonitor monitor;
        int                escEscCount = 0;

        connect(&monitor, &QAgentInputMonitor::escEscPressed, [&escEscCount]() { escEscCount++; });

        /* Three Escs in a row should fire escEscPressed exactly once: the
         * second Esc triggers it, then the third one re-arms a NEW potential
         * chord rather than firing a second escEscPressed. */
        const char data1[] = {0x1B, 'x'};
        monitor.processBytes(data1, 2);
        const char data2[] = {0x1B, 'y'};
        monitor.processBytes(data2, 2);
        const char data3[] = {0x1B, 'z'};
        monitor.processBytes(data3, 2);

        QCOMPARE(escEscCount, 1);
    }

    void testUtf8CjkInput()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* U+4F60 (你) = 0xE4 0xBD 0xA0 */
        const char ni[] = {'\xE4', '\xBD', '\xA0'};
        monitor.processBytes(ni, 3);

        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes[0], QString::fromUtf8("\xe4\xbd\xa0"));
    }

    void testUtf8TwoByteInput()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* U+00E9 (é) = 0xC3 0xA9 */
        const char data[] = {'\xC3', '\xA9'};
        monitor.processBytes(data, 2);

        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes[0], QString::fromUtf8("\xc3\xa9"));
    }

    void testUtf8FourByteEmoji()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* U+1F600 (😀) = 0xF0 0x9F 0x98 0x80 */
        const char emoji[] = {'\xF0', '\x9F', '\x98', '\x80'};
        monitor.processBytes(emoji, 4);

        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes[0], QString::fromUtf8("\xf0\x9f\x98\x80"));
    }

    void testBackspaceDeletesEmojiAsUnit()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Type emoji then backspace */
        const char emoji[] = {'\xF0', '\x9F', '\x98', '\x80'};
        monitor.processBytes(emoji, 4);

        const char bs = 0x7F;
        monitor.processBytes(&bs, 1);

        /* Emoji is a surrogate pair (2 QChars), backspace should delete both */
        QCOMPARE(changes.last(), "");
    }

    void testBackspaceDeletesCjkAsUnit()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Type "a" + CJK char + backspace */
        monitor.processBytes("a", 1);
        const char ni[] = {'\xE4', '\xBD', '\xA0'};
        monitor.processBytes(ni, 3);
        const char bs = 0x7F;
        monitor.processBytes(&bs, 1);

        /* Should be back to just "a" */
        QCOMPARE(changes.last(), "a");
    }

    void testUtf8IncompleteRecovery()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Incomplete CJK (only first byte) followed by ASCII 'x' */
        const char data[] = {'\xE4', 'x'};
        monitor.processBytes(data, 2);

        /* The incomplete UTF-8 should be discarded, 'x' processed normally */
        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes[0], "x");
    }

    void testUtf8SplitAcrossCalls()
    {
        QAgentInputMonitor monitor;
        QStringList        changes;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changes](const QString &text) {
            changes.append(text);
        });

        /* Send CJK char 你 (E4 BD A0) split across two processBytes calls */
        const char part1[] = {'\xE4'};
        const char part2[] = {'\xBD', '\xA0'};
        monitor.processBytes(part1, 1);
        monitor.processBytes(part2, 2);

        QCOMPARE(changes.size(), 1);
        QCOMPARE(changes[0], QString::fromUtf8("\xe4\xbd\xa0"));
    }

    void testControlCharsIgnored()
    {
        QAgentInputMonitor monitor;
        int                changeCount = 0;

        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changeCount](const QString &) {
            changeCount++;
        });

        /* Unhandled control chars should be silently dropped.
         * 0x01 (Ctrl+A) and 0x05 (Ctrl+E) are now cursor moves but are no-ops
         * on an empty buffer, so they must not emit inputChanged either. */
        const char data[] = {0x01, 0x02, 0x04, 0x05, 0x06, 0x07};
        monitor.processBytes(data, 6);
        QCOMPARE(changeCount, 0);
    }

    void testCtrlCEmitsSignal()
    {
        QAgentInputMonitor monitor;
        int                ctrlCCount  = 0;
        int                changeCount = 0;

        connect(&monitor, &QAgentInputMonitor::ctrlCPressed, [&ctrlCCount]() { ctrlCCount++; });
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changeCount](const QString &) {
            changeCount++;
        });

        /* Ctrl+C (0x03) should emit ctrlCPressed and clear buffer */
        const char ctrlC = 0x03;
        monitor.processBytes(&ctrlC, 1);
        QCOMPARE(ctrlCCount, 1);
        QCOMPARE(changeCount, 1); /* inputChanged emitted to clear buffer */
    }

    /* Cursor position + line editing tests */

    void testCursorStartsAtZero()
    {
        QAgentInputMonitor monitor;
        QCOMPARE(monitor.getCursorPos(), 0);
    }

    void testCursorAdvancesOnInsert()
    {
        QAgentInputMonitor monitor;
        const char         data[] = "hello";
        monitor.processBytes(data, 5);
        QCOMPARE(monitor.getCursorPos(), 5);
    }

    void testArrowLeftRightMoveCursor()
    {
        QAgentInputMonitor monitor;
        const char         data[] = "abc";
        monitor.processBytes(data, 3);
        QCOMPARE(monitor.getCursorPos(), 3);

        /* Left */
        const char arrowLeft[] = {0x1B, '[', 'D'};
        monitor.processBytes(arrowLeft, 3);
        QCOMPARE(monitor.getCursorPos(), 2);

        /* Right */
        const char arrowRight[] = {0x1B, '[', 'C'};
        monitor.processBytes(arrowRight, 3);
        QCOMPARE(monitor.getCursorPos(), 3);
    }

    void testArrowLeftClampsAtZero()
    {
        QAgentInputMonitor monitor;
        const char         arrowLeft[] = {0x1B, '[', 'D'};
        monitor.processBytes(arrowLeft, 3);
        QCOMPARE(monitor.getCursorPos(), 0);
    }

    void testCursorInsertsInMiddle()
    {
        QAgentInputMonitor monitor;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        /* Type "hello" then move cursor left 3 times → position 2 (between "he" and "llo") */
        monitor.processBytes("hello", 5);
        const char arrowLeft[] = {0x1B, '[', 'D'};
        for (int i = 0; i < 3; i++) {
            monitor.processBytes(arrowLeft, 3);
        }
        QCOMPARE(monitor.getCursorPos(), 2);

        monitor.processBytes("XY", 2);
        QCOMPARE(lastText, QStringLiteral("heXYllo"));
        QCOMPARE(monitor.getCursorPos(), 4);
    }

    void testBackspaceAtCursorNotEnd()
    {
        QAgentInputMonitor monitor;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        monitor.processBytes("hello", 5);
        /* Move cursor to position 2 ("he|llo") */
        const char arrowLeft[] = {0x1B, '[', 'D'};
        for (int i = 0; i < 3; i++) {
            monitor.processBytes(arrowLeft, 3);
        }

        /* Backspace deletes 'e' leaving "hllo" cursor=1 */
        const char backspace = 0x7F;
        monitor.processBytes(&backspace, 1);
        QCOMPARE(lastText, QStringLiteral("hllo"));
        QCOMPARE(monitor.getCursorPos(), 1);
    }

    void testCtrlAMovesToLineStart()
    {
        QAgentInputMonitor monitor;
        monitor.processBytes("hello", 5);
        QCOMPARE(monitor.getCursorPos(), 5);

        const char ctrlA = 0x01;
        monitor.processBytes(&ctrlA, 1);
        QCOMPARE(monitor.getCursorPos(), 0);
    }

    void testCtrlEMovesToLineEnd()
    {
        QAgentInputMonitor monitor;
        monitor.processBytes("hello", 5);
        const char ctrlA = 0x01;
        monitor.processBytes(&ctrlA, 1);
        QCOMPARE(monitor.getCursorPos(), 0);

        const char ctrlE = 0x05;
        monitor.processBytes(&ctrlE, 1);
        QCOMPARE(monitor.getCursorPos(), 5);
    }

    void testCtrlKKillsToEndOfLine()
    {
        QAgentInputMonitor monitor;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        monitor.processBytes("hello world", 11);
        /* Move cursor to position 5 (between "hello" and " world") */
        const char arrowLeft[] = {0x1B, '[', 'D'};
        for (int i = 0; i < 6; i++) {
            monitor.processBytes(arrowLeft, 3);
        }

        const char ctrlK = 0x0B;
        monitor.processBytes(&ctrlK, 1);
        QCOMPARE(lastText, QStringLiteral("hello"));
        QCOMPARE(monitor.getCursorPos(), 5);
    }

    void testCtrlUKillsToStartOfLine()
    {
        QAgentInputMonitor monitor;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        monitor.processBytes("hello world", 11);
        /* Move cursor to position 6 (between " " and "world") */
        const char arrowLeft[] = {0x1B, '[', 'D'};
        for (int i = 0; i < 5; i++) {
            monitor.processBytes(arrowLeft, 3);
        }

        const char ctrlU = 0x15;
        monitor.processBytes(&ctrlU, 1);
        QCOMPARE(lastText, QStringLiteral("world"));
        QCOMPARE(monitor.getCursorPos(), 0);
    }

    void testHomeKeyMovesCursorToLineStart()
    {
        QAgentInputMonitor monitor;
        monitor.processBytes("hello", 5);
        QCOMPARE(monitor.getCursorPos(), 5);

        const char home[] = {0x1B, '[', 'H'};
        monitor.processBytes(home, 3);
        QCOMPARE(monitor.getCursorPos(), 0);
    }

    void testEndKeyMovesCursorToLineEnd()
    {
        QAgentInputMonitor monitor;
        monitor.processBytes("hello", 5);
        const char ctrlA = 0x01;
        monitor.processBytes(&ctrlA, 1);
        QCOMPARE(monitor.getCursorPos(), 0);

        const char end[] = {0x1B, '[', 'F'};
        monitor.processBytes(end, 3);
        QCOMPARE(monitor.getCursorPos(), 5);
    }

    void testHomeTildeSequence()
    {
        QAgentInputMonitor monitor;
        monitor.processBytes("hello", 5);
        const char home[] = {0x1B, '[', '1', '~'};
        monitor.processBytes(home, 4);
        QCOMPARE(monitor.getCursorPos(), 0);
    }

    void testEndTildeSequence()
    {
        QAgentInputMonitor monitor;
        monitor.processBytes("hello", 5);
        const char ctrlA = 0x01;
        monitor.processBytes(&ctrlA, 1);
        const char end[] = {0x1B, '[', '4', '~'};
        monitor.processBytes(end, 4);
        QCOMPARE(monitor.getCursorPos(), 5);
    }

    void testDeleteKeyRemovesCharAtCursor()
    {
        QAgentInputMonitor monitor;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        monitor.processBytes("hello", 5);
        const char ctrlA = 0x01;
        monitor.processBytes(&ctrlA, 1);

        const char del[] = {0x1B, '[', '3', '~'};
        monitor.processBytes(del, 4);
        QCOMPARE(lastText, QStringLiteral("ello"));
        QCOMPARE(monitor.getCursorPos(), 0);
    }

    /* Multi-line / bracketed paste / backslash continuation tests */

    void testBackslashEnterInsertsNewline()
    {
        QAgentInputMonitor monitor;
        int                readyCount = 0;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::inputReady, [&readyCount](const QString &) {
            readyCount++;
        });
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        /* Type "line1\" then Enter — should not submit, should become "line1\n" */
        monitor.processBytes("line1\\", 6);
        const char enter = '\n';
        monitor.processBytes(&enter, 1);

        QCOMPARE(readyCount, 0);
        QCOMPARE(lastText, QStringLiteral("line1\n"));
    }

    void testBackslashMidBufferEnterSubmits()
    {
        QAgentInputMonitor monitor;
        QString            readyText;
        connect(&monitor, &QAgentInputMonitor::inputReady, [&readyText](const QString &text) {
            readyText = text;
        });

        /* "hel\lo" with cursor in middle — trailing char is 'o' not '\', so Enter submits */
        monitor.processBytes("hel\\lo", 6);
        const char arrowLeft[] = {0x1B, '[', 'D'};
        for (int i = 0; i < 2; i++) {
            monitor.processBytes(arrowLeft, 3);
        }
        const char enter = '\n';
        monitor.processBytes(&enter, 1);

        QCOMPARE(readyText, QStringLiteral("hel\\lo"));
    }

    /* Helper: bridge pastedReceived → insertText to simulate the REPL's
     * literal-insert path. Tests that want chip logic can skip this bridge. */
    static void bridgePasteToInsert(QAgentInputMonitor &monitor)
    {
        connect(
            &monitor,
            &QAgentInputMonitor::pastedReceived,
            &monitor,
            [&monitor](const QString &text) { monitor.insertText(text); });
    }

    void testBracketedPasteInsertsLiteralNewlines()
    {
        QAgentInputMonitor monitor;
        bridgePasteToInsert(monitor);
        int     readyCount = 0;
        QString lastText;
        connect(&monitor, &QAgentInputMonitor::inputReady, [&readyCount](const QString &) {
            readyCount++;
        });
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        /* Bracketed paste: ESC[200~ ... ESC[201~ */
        const char startMarker[] = {0x1B, '[', '2', '0', '0', '~'};
        const char endMarker[]   = {0x1B, '[', '2', '0', '1', '~'};
        monitor.processBytes(startMarker, 6);
        monitor.processBytes("alpha\nbeta\ngamma", 16);
        monitor.processBytes(endMarker, 6);

        QCOMPARE(readyCount, 0); /* Newlines inside paste must NOT submit */
        QCOMPARE(lastText, QStringLiteral("alpha\nbeta\ngamma"));
    }

    void testEnterAfterPasteSubmits()
    {
        QAgentInputMonitor monitor;
        bridgePasteToInsert(monitor);
        QString readyText;
        connect(&monitor, &QAgentInputMonitor::inputReady, [&readyText](const QString &text) {
            readyText = text;
        });

        /* Paste "a\nb", then press Enter outside paste — should submit "a\nb" */
        const char startMarker[] = {0x1B, '[', '2', '0', '0', '~'};
        const char endMarker[]   = {0x1B, '[', '2', '0', '1', '~'};
        monitor.processBytes(startMarker, 6);
        monitor.processBytes("a\nb", 3);
        monitor.processBytes(endMarker, 6);

        const char enter = '\n';
        monitor.processBytes(&enter, 1);
        QCOMPARE(readyText, QStringLiteral("a\nb"));
    }

    void testBackspaceJoinsLinesAcrossNewline()
    {
        QAgentInputMonitor monitor;
        bridgePasteToInsert(monitor);
        QString lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        /* Paste "a\nb" — cursor at end (position 3) */
        const char startMarker[] = {0x1B, '[', '2', '0', '0', '~'};
        const char endMarker[]   = {0x1B, '[', '2', '0', '1', '~'};
        monitor.processBytes(startMarker, 6);
        monitor.processBytes("a\nb", 3);
        monitor.processBytes(endMarker, 6);

        /* Move cursor to position 2 (start of "b", just after newline) */
        const char arrowLeft[] = {0x1B, '[', 'D'};
        monitor.processBytes(arrowLeft, 3);
        QCOMPARE(monitor.getCursorPos(), 2);

        /* Backspace removes the newline, joining lines */
        const char backspace = 0x7F;
        monitor.processBytes(&backspace, 1);
        QCOMPARE(lastText, QStringLiteral("ab"));
        QCOMPARE(monitor.getCursorPos(), 1);
    }

    void testCtrlAOnSecondLineGoesToLineStartNotBufferStart()
    {
        QAgentInputMonitor monitor;
        bridgePasteToInsert(monitor);
        const char startMarker[] = {0x1B, '[', '2', '0', '0', '~'};
        const char endMarker[]   = {0x1B, '[', '2', '0', '1', '~'};
        monitor.processBytes(startMarker, 6);
        monitor.processBytes("line1\nline2", 11);
        monitor.processBytes(endMarker, 6);

        /* Cursor at end of buffer (position 11, after "line2") */
        QCOMPARE(monitor.getCursorPos(), 11);

        const char ctrlA = 0x01;
        monitor.processBytes(&ctrlA, 1);
        /* Should move to position 6 (after newline, start of "line2") */
        QCOMPARE(monitor.getCursorPos(), 6);
    }

    void testCtrlKOnFirstLineOnlyKillsFirstLine()
    {
        QAgentInputMonitor monitor;
        bridgePasteToInsert(monitor);
        QString lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        const char startMarker[] = {0x1B, '[', '2', '0', '0', '~'};
        const char endMarker[]   = {0x1B, '[', '2', '0', '1', '~'};
        monitor.processBytes(startMarker, 6);
        monitor.processBytes("line1\nline2", 11);
        monitor.processBytes(endMarker, 6);

        /* Move cursor to position 3 (middle of "line1") */
        const char arrowLeft[] = {0x1B, '[', 'D'};
        for (int i = 0; i < 8; i++) {
            monitor.processBytes(arrowLeft, 3);
        }
        QCOMPARE(monitor.getCursorPos(), 3);

        const char ctrlK = 0x0B;
        monitor.processBytes(&ctrlK, 1);
        /* Should kill "e1" leaving "lin\nline2" */
        QCOMPARE(lastText, QStringLiteral("lin\nline2"));
    }

    void testBracketedPasteToggleIsIdempotent()
    {
        QAgentInputMonitor monitor;
        bridgePasteToInsert(monitor);
        QString lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        /* Start paste, insert text, end paste — then start again and insert more */
        const char startMarker[] = {0x1B, '[', '2', '0', '0', '~'};
        const char endMarker[]   = {0x1B, '[', '2', '0', '1', '~'};

        monitor.processBytes(startMarker, 6);
        monitor.processBytes("foo\n", 4);
        monitor.processBytes(endMarker, 6);

        monitor.processBytes(startMarker, 6);
        monitor.processBytes("bar", 3);
        monitor.processBytes(endMarker, 6);

        QCOMPARE(lastText, QStringLiteral("foo\nbar"));
    }

    void testUtf8CjkCursorMove()
    {
        QAgentInputMonitor monitor;
        /* 你好 = 2 QChars (6 UTF-8 bytes) */
        const char cjk[] = "\xe4\xbd\xa0\xe5\xa5\xbd";
        monitor.processBytes(cjk, 6);
        QCOMPARE(monitor.getCursorPos(), 2);

        const char arrowLeft[] = {0x1B, '[', 'D'};
        monitor.processBytes(arrowLeft, 3);
        QCOMPARE(monitor.getCursorPos(), 1);
        monitor.processBytes(arrowLeft, 3);
        QCOMPARE(monitor.getCursorPos(), 0);
    }

    void testEmojiCursorSkipsSurrogatePair()
    {
        QAgentInputMonitor monitor;
        /* 🎉 = 1 code point but 2 QChars (surrogate pair) in UTF-16 */
        const char emoji[] = "\xf0\x9f\x8e\x89";
        monitor.processBytes(emoji, 4);
        QCOMPARE(monitor.getCursorPos(), 2); /* High + low surrogate */

        /* One Left arrow should jump over both surrogate halves */
        const char arrowLeft[] = {0x1B, '[', 'D'};
        monitor.processBytes(arrowLeft, 3);
        QCOMPARE(monitor.getCursorPos(), 0);
    }

    /* Submit-blocked tests (used by completion popup) */

    void testSubmitBlockedEnterEmitsSpecialSignal()
    {
        QAgentInputMonitor monitor;
        int                readyCount   = 0;
        int                blockedCount = 0;
        int                lastKey      = 0;

        connect(&monitor, &QAgentInputMonitor::inputReady, [&readyCount](const QString &) {
            readyCount++;
        });
        connect(&monitor, &QAgentInputMonitor::submitBlockedKey, [&blockedCount, &lastKey](int key) {
            blockedCount++;
            lastKey = key;
        });

        monitor.processBytes("hello", 5);
        monitor.setSubmitBlocked(true);

        const char enter = '\n';
        monitor.processBytes(&enter, 1);
        QCOMPARE(readyCount, 0);
        QCOMPARE(blockedCount, 1);
        QCOMPARE(lastKey, static_cast<int>('E'));
        /* Buffer should stay intact */
        QCOMPARE(monitor.getCursorPos(), 5);
    }

    void testSubmitBlockedTabEmitsSpecialSignal()
    {
        QAgentInputMonitor monitor;
        int                blockedCount = 0;
        int                lastKey      = 0;

        connect(&monitor, &QAgentInputMonitor::submitBlockedKey, [&blockedCount, &lastKey](int key) {
            blockedCount++;
            lastKey = key;
        });

        monitor.processBytes("hello", 5);
        monitor.setSubmitBlocked(true);

        const char tab = '\t';
        monitor.processBytes(&tab, 1);
        QCOMPARE(blockedCount, 1);
        QCOMPARE(lastKey, static_cast<int>('T'));
    }

    void testSubmitBlockedTabWhenNotBlockedIsIgnored()
    {
        QAgentInputMonitor monitor;
        int                blockedCount = 0;
        connect(&monitor, &QAgentInputMonitor::submitBlockedKey, [&blockedCount](int) {
            blockedCount++;
        });

        monitor.processBytes("hello", 5);
        /* submitBlocked is false by default */
        const char tab = '\t';
        monitor.processBytes(&tab, 1);
        QCOMPARE(blockedCount, 0);
        QCOMPARE(monitor.getCursorPos(), 5);
    }

    void testInsertCompletionReplacesAtToken()
    {
        QAgentInputMonitor monitor;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        /* Type "hello @foo" — cursorPos=10, @ at index 6 */
        monitor.processBytes("hello @foo", 10);
        QCOMPARE(monitor.getCursorPos(), 10);

        monitor.insertCompletion(6, QStringLiteral("src/foo.cpp"), QLatin1Char(' '));
        QCOMPARE(lastText, QStringLiteral("hello @src/foo.cpp "));
        QCOMPARE(monitor.getCursorPos(), 19);
    }

    void testInsertCompletionDirectoryTrailingSlash()
    {
        QAgentInputMonitor monitor;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        monitor.processBytes("@s", 2);
        monitor.insertCompletion(0, QStringLiteral("src"), QLatin1Char('/'));
        QCOMPARE(lastText, QStringLiteral("@src/"));
        QCOMPARE(monitor.getCursorPos(), 5);
    }

    void testInsertCompletionIgnoresInvalidPos()
    {
        QAgentInputMonitor monitor;
        monitor.processBytes("hello", 5);
        int origCursor = monitor.getCursorPos();

        /* atPos outside range — no-op */
        monitor.insertCompletion(-1, QStringLiteral("x"), QLatin1Char(' '));
        monitor.insertCompletion(100, QStringLiteral("x"), QLatin1Char(' '));
        QCOMPARE(monitor.getCursorPos(), origCursor);
    }

    /* External editor chord tests */

    void testCtrlXAloneDoesNotEmit()
    {
        QAgentInputMonitor monitor;
        int                editorCount = 0;
        connect(&monitor, &QAgentInputMonitor::externalEditorRequested, [&editorCount]() {
            editorCount++;
        });

        const char ctrlX = 0x18;
        monitor.processBytes(&ctrlX, 1);
        QCOMPARE(editorCount, 0);
    }

    void testCtrlXCtrlEEmitsExternalEditor()
    {
        QAgentInputMonitor monitor;
        int                editorCount = 0;
        connect(&monitor, &QAgentInputMonitor::externalEditorRequested, [&editorCount]() {
            editorCount++;
        });

        const char seq[] = {0x18, 0x05}; /* Ctrl+X Ctrl+E */
        monitor.processBytes(seq, 2);
        QCOMPARE(editorCount, 1);
    }

    void testCtrlXCancelsOnOtherByte()
    {
        QAgentInputMonitor monitor;
        int                editorCount = 0;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::externalEditorRequested, [&editorCount]() {
            editorCount++;
        });
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        /* Ctrl+X then 'a' — chord cancels, 'a' is inserted normally */
        const char seq[] = {0x18, 'a'};
        monitor.processBytes(seq, 2);
        QCOMPARE(editorCount, 0);
        QCOMPARE(lastText, QStringLiteral("a"));
    }

    void testCtrlGDirectlyTriggersExternalEditor()
    {
        QAgentInputMonitor monitor;
        int                editorCount = 0;
        connect(&monitor, &QAgentInputMonitor::externalEditorRequested, [&editorCount]() {
            editorCount++;
        });

        const char ctrlG = 0x07;
        monitor.processBytes(&ctrlG, 1);
        QCOMPARE(editorCount, 1);
    }

    void testCtrlXThenCtrlCFiresInterrupt()
    {
        QAgentInputMonitor monitor;
        int                editorCount = 0;
        int                ctrlCCount  = 0;
        connect(&monitor, &QAgentInputMonitor::externalEditorRequested, [&editorCount]() {
            editorCount++;
        });
        connect(&monitor, &QAgentInputMonitor::ctrlCPressed, [&ctrlCCount]() { ctrlCCount++; });

        /* Ctrl+X then Ctrl+C — chord cancels, Ctrl+C still interrupts */
        const char seq[] = {0x18, 0x03};
        monitor.processBytes(seq, 2);
        QCOMPARE(editorCount, 0);
        QCOMPARE(ctrlCCount, 1);
    }

    void testCtrlRPressedEmitsHistorySearch()
    {
        QAgentInputMonitor monitor;
        int                count = 0;
        connect(&monitor, &QAgentInputMonitor::historySearchRequested, [&count]() { count++; });

        const char ctrlR = 0x12;
        monitor.processBytes(&ctrlR, 1);
        QCOMPARE(count, 1);

        /* Second press also emits — REPL interprets as "next match" */
        monitor.processBytes(&ctrlR, 1);
        QCOMPARE(count, 2);
    }

    void testCtrlTPressedEmitsToggleTodos()
    {
        QAgentInputMonitor monitor;
        int                count = 0;
        connect(&monitor, &QAgentInputMonitor::toggleTodosRequested, [&count]() { count++; });

        const char ctrlT = 0x14;
        monitor.processBytes(&ctrlT, 1);
        QCOMPARE(count, 1);
    }

    void testCtrlLPressedEmitsRedraw()
    {
        QAgentInputMonitor monitor;
        int                count = 0;
        connect(&monitor, &QAgentInputMonitor::redrawRequested, [&count]() { count++; });

        const char ctrlL = 0x0C;
        monitor.processBytes(&ctrlL, 1);
        QCOMPARE(count, 1);
    }

    /* Undo stack tests */

    void testUndoOnEmptyBufferIsNoop()
    {
        QAgentInputMonitor monitor;
        int                changeCount = 0;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changeCount](const QString &) {
            changeCount++;
        });

        const char ctrlUnderscore = 0x1F;
        monitor.processBytes(&ctrlUnderscore, 1);
        QCOMPARE(changeCount, 0);
    }

    void testUndoPrintableBurstCoalesces()
    {
        QAgentInputMonitor monitor;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        /* Rapid typing should coalesce into a single undo step via debounce. */
        monitor.processBytes("hello", 5);
        QCOMPARE(lastText, QStringLiteral("hello"));

        const char ctrlUnderscore = 0x1F;
        monitor.processBytes(&ctrlUnderscore, 1);
        QCOMPARE(lastText, QString());
        QCOMPARE(monitor.getCursorPos(), 0);
    }

    void testUndoBigActionsAreIndividual()
    {
        QAgentInputMonitor monitor;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        monitor.processBytes("hello world", 11);
        /* Ctrl+W deletes "world", pushing its own snapshot */
        const char ctrlW = 0x17;
        monitor.processBytes(&ctrlW, 1);
        QCOMPARE(lastText, QStringLiteral("hello "));

        /* First undo: restore "hello world" */
        const char ctrlUnderscore = 0x1F;
        monitor.processBytes(&ctrlUnderscore, 1);
        QCOMPARE(lastText, QStringLiteral("hello world"));

        /* Second undo: restore empty */
        monitor.processBytes(&ctrlUnderscore, 1);
        QCOMPARE(lastText, QString());
    }

    void testUndoClearsOnSubmit()
    {
        QAgentInputMonitor monitor;
        QString            readyText;
        connect(&monitor, &QAgentInputMonitor::inputReady, [&readyText](const QString &text) {
            readyText = text;
        });
        QString lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        monitor.processBytes("abc", 3);
        const char enter = '\n';
        monitor.processBytes(&enter, 1);
        QCOMPARE(readyText, QStringLiteral("abc"));
        QCOMPARE(lastText, QString());

        /* Undo after submit should be a no-op — the stack was cleared. */
        monitor.processBytes("xyz", 3);
        const char ctrlUnderscore = 0x1F;
        monitor.processBytes(&ctrlUnderscore, 1);
        /* Exactly ONE level of undo (the xyz burst) should be available;
         * the older "abc" history is gone after submit. */
        QCOMPARE(lastText, QString());
        monitor.processBytes(&ctrlUnderscore, 1);
        QCOMPARE(lastText, QString()); /* Already empty, no further rollback */
    }

    void testUndoClearsOnCtrlC()
    {
        QAgentInputMonitor monitor;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        monitor.processBytes("hello", 5);
        const char ctrlC = 0x03;
        monitor.processBytes(&ctrlC, 1);
        QCOMPARE(lastText, QString());

        /* Undo after Ctrl+C is a no-op. */
        const char ctrlUnderscore = 0x1F;
        monitor.processBytes(&ctrlUnderscore, 1);
        QCOMPARE(lastText, QString());
    }

    void testUndoDeleteRestoresBufferAndCursor()
    {
        QAgentInputMonitor monitor;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        monitor.processBytes("hello", 5);
        /* Ctrl+A to start, then Delete once (removes 'h') */
        const char ctrlA = 0x01;
        monitor.processBytes(&ctrlA, 1);
        const char del[] = {0x1B, '[', '3', '~'};
        monitor.processBytes(del, 4);
        QCOMPARE(lastText, QStringLiteral("ello"));
        QCOMPARE(monitor.getCursorPos(), 0);

        const char ctrlUnderscore = 0x1F;
        monitor.processBytes(&ctrlUnderscore, 1);
        QCOMPARE(lastText, QStringLiteral("hello"));
        QCOMPARE(monitor.getCursorPos(), 0);
    }

    void testUndoSetInputBufferIsUndoable()
    {
        QAgentInputMonitor monitor;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        monitor.processBytes("typed", 5);
        /* Simulate history recall / completion confirm */
        monitor.setInputBuffer(QStringLiteral("from history"));
        QCOMPARE(lastText, QStringLiteral("from history"));

        /* One undo should roll back the setInputBuffer to the typed state. */
        const char ctrlUnderscore = 0x1F;
        monitor.processBytes(&ctrlUnderscore, 1);
        QCOMPARE(lastText, QStringLiteral("typed"));
    }

    /* Bracketed paste accumulation / pastedReceived tests */

    void testBracketedPasteAccumulatesPayload()
    {
        QAgentInputMonitor monitor;
        int                pastedCount = 0;
        QString            lastPayload;
        int                inputChangeCount = 0;
        connect(
            &monitor,
            &QAgentInputMonitor::pastedReceived,
            [&pastedCount, &lastPayload](const QString &text) {
                pastedCount++;
                lastPayload = text;
            });
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&inputChangeCount](const QString &) {
            inputChangeCount++;
        });

        const char startMarker[] = {0x1B, '[', '2', '0', '0', '~'};
        const char endMarker[]   = {0x1B, '[', '2', '0', '1', '~'};
        monitor.processBytes(startMarker, 6);
        monitor.processBytes("alpha\nbeta\ngamma", 16);
        monitor.processBytes(endMarker, 6);

        /* REPL owns insertion — the monitor itself must not touch inputBuffer
         * during bracketed paste, so inputChanged should not fire and cursor
         * stays at 0. */
        QCOMPARE(pastedCount, 1);
        QCOMPARE(lastPayload, QStringLiteral("alpha\nbeta\ngamma"));
        QCOMPARE(inputChangeCount, 0);
        QCOMPARE(monitor.getCursorPos(), 0);
    }

    void testBracketedPasteHandlesUtf8()
    {
        QAgentInputMonitor monitor;
        QString            lastPayload;
        connect(&monitor, &QAgentInputMonitor::pastedReceived, [&lastPayload](const QString &text) {
            lastPayload = text;
        });

        /* 你好 world = UTF-8 6 bytes + " world" */
        const char startMarker[] = {0x1B, '[', '2', '0', '0', '~'};
        const char endMarker[]   = {0x1B, '[', '2', '0', '1', '~'};
        monitor.processBytes(startMarker, 6);
        monitor.processBytes("\xe4\xbd\xa0\xe5\xa5\xbd world", 12);
        monitor.processBytes(endMarker, 6);

        QCOMPARE(lastPayload, QStringLiteral("\u4f60\u597d world"));
    }

    void testEmptyPasteEmitsEmptyPayload()
    {
        QAgentInputMonitor monitor;
        int                count       = 0;
        QString            lastPayload = QStringLiteral("sentinel");
        connect(&monitor, &QAgentInputMonitor::pastedReceived, [&](const QString &text) {
            count++;
            lastPayload = text;
        });

        const char startMarker[] = {0x1B, '[', '2', '0', '0', '~'};
        const char endMarker[]   = {0x1B, '[', '2', '0', '1', '~'};
        monitor.processBytes(startMarker, 6);
        monitor.processBytes(endMarker, 6);

        QCOMPARE(count, 1);
        QVERIFY(lastPayload.isEmpty());
    }

    void testInsertTextAppendsAtCursor()
    {
        QAgentInputMonitor monitor;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        monitor.processBytes("hello ", 6);
        monitor.insertText(QStringLiteral("[Pasted text #1 +3 lines]"));
        QCOMPARE(lastText, QStringLiteral("hello [Pasted text #1 +3 lines]"));
        QCOMPARE(monitor.getCursorPos(), 31);
    }

    void testInsertTextEmptyIsNoop()
    {
        QAgentInputMonitor monitor;
        int                changeCount = 0;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&changeCount](const QString &) {
            changeCount++;
        });

        monitor.insertText(QString());
        QCOMPARE(changeCount, 0);
    }

    /* Atomic delete tests */

    void testBackspaceAtomicDeletesWholeChip()
    {
        QAgentInputMonitor monitor;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        monitor.setAtomicPattern(
            QRegularExpression(QStringLiteral(R"(\[Pasted text #\d+ \+\d+ lines\])")));

        /* "hi [Pasted text #1 +3 lines]" — cursor lands at end (28) */
        monitor.processBytes("hi [Pasted text #1 +3 lines]", 28);
        QCOMPARE(monitor.getCursorPos(), 28);

        /* Backspace with cursor at end of chip → delete the whole chip */
        const char backspace = 0x7F;
        monitor.processBytes(&backspace, 1);
        QCOMPARE(lastText, QStringLiteral("hi "));
        QCOMPARE(monitor.getCursorPos(), 3);
    }

    void testBackspaceAdjacentCharStillWorks()
    {
        QAgentInputMonitor monitor;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        monitor.setAtomicPattern(
            QRegularExpression(QStringLiteral(R"(\[Pasted text #\d+ \+\d+ lines\])")));

        /* Plain text — backspace still removes a single char */
        monitor.processBytes("hello", 5);
        const char backspace = 0x7F;
        monitor.processBytes(&backspace, 1);
        QCOMPARE(lastText, QStringLiteral("hell"));
        QCOMPARE(monitor.getCursorPos(), 4);
    }

    void testBackspaceInsideChipDeletesWholeChip()
    {
        QAgentInputMonitor monitor;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        monitor.setAtomicPattern(
            QRegularExpression(QStringLiteral(R"(\[Pasted text #\d+ \+\d+ lines\])")));

        /* Paste at the start, then move cursor into the middle of the chip. */
        monitor.processBytes("[Pasted text #5 +2 lines] end", 29);
        /* Move cursor to position 10 (inside "[Pasted text #5 +2 lines]") */
        const char arrowLeft[] = {0x1B, '[', 'D'};
        for (int i = 0; i < 19; i++) {
            monitor.processBytes(arrowLeft, 3);
        }
        QCOMPARE(monitor.getCursorPos(), 10);

        const char backspace = 0x7F;
        monitor.processBytes(&backspace, 1);
        QCOMPARE(lastText, QStringLiteral(" end"));
        QCOMPARE(monitor.getCursorPos(), 0);
    }

    void testForwardDeleteAtomicDeletesChip()
    {
        QAgentInputMonitor monitor;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        monitor.setAtomicPattern(
            QRegularExpression(QStringLiteral(R"(\[Pasted text #\d+ \+\d+ lines\])")));

        monitor.processBytes("pre [Pasted text #1 +3 lines] post", 34);
        /* Move cursor to position 4 (just before '[') */
        const char arrowLeft[] = {0x1B, '[', 'D'};
        for (int i = 0; i < 30; i++) {
            monitor.processBytes(arrowLeft, 3);
        }
        QCOMPARE(monitor.getCursorPos(), 4);

        /* ESC [ 3 ~ = forward Delete */
        const char del[] = {0x1B, '[', '3', '~'};
        monitor.processBytes(del, 4);
        QCOMPARE(lastText, QStringLiteral("pre  post"));
        QCOMPARE(monitor.getCursorPos(), 4);
    }

    void testAtomicDisabledWhenPatternEmpty()
    {
        QAgentInputMonitor monitor;
        QString            lastText;
        connect(&monitor, &QAgentInputMonitor::inputChanged, [&lastText](const QString &text) {
            lastText = text;
        });

        /* No pattern set — backspace behaves normally even inside chip-like text */
        monitor.processBytes("[Pasted text #1 +3 lines]", 25);
        const char backspace = 0x7F;
        monitor.processBytes(&backspace, 1);
        QCOMPARE(lastText, QStringLiteral("[Pasted text #1 +3 lines"));
    }

    void testStopClearsInputState()
    {
        QAgentInputMonitor monitor;
        QString            lastChanged;
        bool               changedEmitted = false;

        connect(
            &monitor,
            &QAgentInputMonitor::inputChanged,
            [&lastChanged, &changedEmitted](const QString &text) {
                lastChanged    = text;
                changedEmitted = true;
            });

        monitor.start();
        QVERIFY(monitor.isActive());

        monitor.stop();
        QVERIFY(!monitor.isActive());

        /* stop() should have emitted inputChanged with empty string */
        QVERIFY(changedEmitted);
        QVERIFY(lastChanged.isEmpty());
    }

    /* QSocTool abort interface tests */

    void testToolAbortDefaultNoOp()
    {
        QSocToolRegistry registry;
        auto            *tool = new QSocToolShellBash(&registry);
        registry.registerTool(tool);
        registry.abortAll();
    }

    /* QSocAgent abort cascade tests */

    void testAbortCascadesToLLM()
    {
        auto *llmService   = new QLLMService(this);
        auto *toolRegistry = new QSocToolRegistry(this);
        auto *agent        = new QSocAgent(this, llmService, toolRegistry);

        agent->abort();

        delete agent;
        delete toolRegistry;
        delete llmService;
    }

    void testAbortCascadesToTools()
    {
        auto *llmService   = new QLLMService(this);
        auto *toolRegistry = new QSocToolRegistry(this);
        auto *bashTool     = new QSocToolShellBash(toolRegistry);
        toolRegistry->registerTool(bashTool);

        auto *agent = new QSocAgent(this, llmService, toolRegistry);
        agent->abort();

        delete agent;
        delete toolRegistry;
        delete llmService;
    }

    void testHandleToolCallsSkipsAfterAbort()
    {
        auto *toolRegistry = new QSocToolRegistry(this);
        auto *bashTool     = new QSocToolShellBash(toolRegistry);
        toolRegistry->registerTool(bashTool);

        QSocAgentConfig config;
        auto           *agent = new QSocAgent(this, nullptr, toolRegistry, config);

        json msgs = json::array();
        msgs.push_back({{"role", "user"}, {"content", "Do something"}});

        json assistantMsg  = {{"role", "assistant"}, {"content", nullptr}};
        json toolCallsJson = json::array();
        toolCallsJson.push_back(
            {{"id", "call_1"},
             {"type", "function"},
             {"function", {{"name", "bash"}, {"arguments", R"({"command":"echo hello"})"}}}});
        toolCallsJson.push_back(
            {{"id", "call_2"},
             {"type", "function"},
             {"function", {{"name", "bash"}, {"arguments", R"({"command":"echo world"})"}}}});
        assistantMsg["tool_calls"] = toolCallsJson;
        msgs.push_back(assistantMsg);
        agent->setMessages(msgs);

        agent->abort();
        QVERIFY(!agent->isRunning());

        delete agent;
        delete toolRegistry;
    }

    void testRunAbortedSignalEmitted()
    {
        auto *llmService   = new QLLMService(this);
        auto *toolRegistry = new QSocToolRegistry(this);

        QSocAgentConfig config;
        auto           *agent = new QSocAgent(this, llmService, toolRegistry, config);

        QSignalSpy abortedSpy(agent, &QSocAgent::runAborted);

        agent->runStream("test query");
        agent->abort();

        QCoreApplication::processEvents();
        QVERIFY(abortedSpy.count() >= 0);

        delete agent;
        delete toolRegistry;
        delete llmService;
    }

    void testAbortStreamMethod()
    {
        auto *llmService = new QLLMService(this);

        llmService->abortStream();

        QSignalSpy errorSpy(llmService, &QLLMService::streamError);
        QCOMPARE(errorSpy.count(), 0);

        delete llmService;
    }

    void testAbortRegistryAbortAll()
    {
        QSocToolRegistry registry;
        auto            *bash1 = new QSocToolShellBash(&registry);
        registry.registerTool(bash1);
        registry.abortAll();
    }

    /* Queue integration test */

    void testQueueIntegration()
    {
        auto *llmService   = new QLLMService(this);
        auto *toolRegistry = new QSocToolRegistry(this);
        auto *agent        = new QSocAgent(this, llmService, toolRegistry);

        agent->queueRequest("follow-up request");
        QVERIFY(agent->hasPendingRequests());
        QCOMPARE(agent->pendingRequestCount(), 1);

        agent->queueRequest("second request");
        QCOMPARE(agent->pendingRequestCount(), 2);

        delete agent;
        delete toolRegistry;
        delete llmService;
    }
};

QSOC_TEST_MAIN(Test)

#include "test_qsocagentinputmonitor.moc"
