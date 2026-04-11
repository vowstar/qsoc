// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QAGENTINPUTMONITOR_H
#define QAGENTINPUTMONITOR_H

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QRegularExpression>
#include <QSocketNotifier>
#include <QString>

#ifndef _WIN32
#include <termios.h>
#endif

/**
 * @brief Stdin monitor for ESC interrupt and user input during agent execution
 * @details Uses termios raw mode and QSocketNotifier to detect ESC keypress
 *          and buffer user input. Supports full UTF-8 including CJK (3-byte)
 *          and emoji (4-byte) characters. Line editing: backspace, Ctrl-U, Ctrl-W.
 */
class QAgentInputMonitor : public QObject
{
    Q_OBJECT

public:
    explicit QAgentInputMonitor(QObject *parent = nullptr);
    ~QAgentInputMonitor() override;

    /**
     * @brief Start monitoring stdin for ESC key and user input
     * @details Saves terminal settings, enters raw mode, creates QSocketNotifier
     */
    void start();

    /**
     * @brief Stop monitoring and restore terminal settings
     */
    void stop();

    /**
     * @brief Check if monitor is currently active
     * @return True if monitoring is active
     */
    bool isActive() const;

    /**
     * @brief Process raw bytes as if they came from stdin
     * @details Public for testability. Feeds bytes through the same state machine
     *          used by the stdin callback.
     * @param data Raw bytes to process
     * @param len Number of bytes
     */
    void processBytes(const char *data, int len);

signals:
    void escPressed();
    void ctrlCPressed();
    void inputReady(const QString &text);
    void inputChanged(const QString &text);

    /* Mouse events (SGR format: ESC [ < btn ; x ; y M/m) */
    void mouseWheel(int direction); /* 0=up, 1=down */
    void mouseClick(int button, int col, int row, bool pressed);

    /* Arrow keys */
    void arrowKey(int key); /* 'A'=up, 'B'=down, 'C'=right, 'D'=left */

    /**
     * @brief Emitted when Enter or Tab is pressed while submit is blocked
     *        (e.g. completion popup is open). REPL can use this to confirm
     *        the popup selection without the input buffer being wiped.
     * @param key 'E' for Enter, 'T' for Tab
     */
    void submitBlockedKey(int key);

    /**
     * @brief Emitted when the user requests to open the current input in an
     *        external editor. Triggered by Ctrl+X Ctrl+E chord or by Ctrl+G.
     *        The REPL handler should pause the compositor, run the editor,
     *        and rewrite the input buffer with the result.
     */
    void externalEditorRequested();

    /**
     * @brief Emitted on Ctrl+R — request reverse-i-search of the input history.
     *        REPL owns the search state machine; the monitor is stateless.
     *        When already searching, the REPL uses a second Ctrl+R as the
     *        "next older match" command.
     */
    void historySearchRequested();

    /**
     * @brief Emitted on Ctrl+T — request toggling the TODO list visibility.
     */
    void toggleTodosRequested();

    /**
     * @brief Emitted when a bracketed paste completes, carrying the full
     *        decoded paste payload. The REPL decides whether to insert the
     *        text literally or replace it with a "[Pasted text #N +M lines]"
     *        chip reference. Monitor never auto-inserts paste content — the
     *        REPL is responsible for either calling insertText(payload) or
     *        insertText(chipLabel) based on its own size heuristics.
     */
    void pastedReceived(const QString &text);

    /**
     * @brief Emitted on Ctrl+L — request a full compositor repaint. Useful
     *        after a background process scribbles on the alt-screen or when
     *        a resize glitch leaves leftover artifacts.
     */
    void redrawRequested();

private:
    static int  utf8SeqLen(unsigned char lead);
    static bool isUtf8Continuation(unsigned char byte);
    void        insertAtCursor(const QString &decoded);
    int         prevCharStep() const;
    int         nextCharStep() const;
    void        moveCursorLeft();
    void        moveCursorRight();

    /* Undo stack primitives. A snapshot captures the buffer + cursor state
     * that existed BEFORE a mutation. Push callers tag whether the change
     * was a printable-character insertion so consecutive keystrokes can
     * coalesce into a single undo step via a 500ms debounce window. */
    struct UndoSnapshot
    {
        QString text;
        int     cursorPos     = 0;
        bool    fromPrintable = false;
    };
    static constexpr int UNDO_MAX_DEPTH   = 100;
    static constexpr int UNDO_DEBOUNCE_MS = 500;
    void                 pushUndoSnapshot(bool fromPrintable);
    void                 clearUndoStack();

    /**
     * @brief Find an atomic-pattern match whose span overlaps or is adjacent
     *        to the cursor for the given direction.
     * @param forward true for forward Delete, false for Backspace
     * @param outStart out-param: start of the match in inputBuffer (-1 if none)
     * @param outLen   out-param: length of the match in QChars (0 if none)
     * @return true on a hit worth deleting atomically.
     */
    bool findAtomicSpanAtCursor(bool forward, int &outStart, int &outLen) const;

#ifndef _WIN32
    struct termios origTermios;
#endif
    QSocketNotifier    *notifier     = nullptr;
    bool                active       = false;
    bool                termiosSaved = false;
    QString             inputBuffer;
    int                 cursorPos = 0; /* Insertion point in inputBuffer (QChar index) */
    QByteArray          utf8Pending;
    QByteArray          escBuffer;     /* Buffer for ESC sequence parsing */
    QByteArray          pastedBuffer;  /* Accumulator for bracketed paste payload */
    QRegularExpression  atomicPattern; /* Matches glyphs that delete as a unit */
    QList<UndoSnapshot> undoStack;     /* Pre-mutation snapshots for Ctrl+_ */
    QElapsedTimer       undoTimer;     /* Elapsed since the last snapshot push */
    bool                undoTimerValid   = false;
    bool                inEscSeq         = false;
    bool                inBracketedPaste = false; /* True while inside \033[200~ ... \033[201~ */
    bool                submitBlocked    = false; /* Enter/Tab emit submitBlockedKey instead */
    bool                inCtrlXChord     = false; /* True after Ctrl+X, waiting for second key */

    void processEscSequence();

public:
    /* Reset ESC sequence state (call after modal overlay consumes ESC) */
    void resetEscState();

    /* Set input buffer content (for history navigation) */
    void setInputBuffer(const QString &text);

    /* Current cursor position within inputBuffer (QChar index) */
    int getCursorPos() const { return cursorPos; }

    /**
     * @brief Replace the '@query' token ending at cursorPos with a replacement
     *        (typically a file path) and advance the cursor past it.
     * @param atPos       Absolute buffer position of the '@' char to replace
     * @param replacement Text to insert (should NOT start with '@'; '@' is
     *                    prepended automatically so references render as
     *                    '@path/to/file')
     * @param trailing    Optional trailing char (' ' for file, '/' for dir)
     */
    void insertCompletion(int atPos, const QString &replacement, QChar trailing = QLatin1Char(' '));

    /**
     * @brief Insert arbitrary text at the current cursor position.
     * @details Public wrapper around the internal insertAtCursor so the REPL
     *          can push pasted payloads or chip labels without reaching into
     *          private state. Cursor advances by text.size() QChars.
     */
    void insertText(const QString &text);

    /**
     * @brief Register a regex whose matches should behave as atomic glyphs
     *        for Backspace / Delete.
     * @details When set, a Backspace or forward Delete that lands anywhere
     *          inside a match — or immediately adjacent to it — removes the
     *          whole match instead of one character. Used by the REPL to
     *          make "[Pasted text #N +M lines]" chips deletable as a unit.
     *          Pass a default-constructed QRegularExpression to disable.
     */
    void setAtomicPattern(const QRegularExpression &pattern);

    /**
     * @brief When blocked, Enter and Tab emit submitBlockedKey() instead of
     *        inputReady/tab-ignore, so the REPL can own popup confirmation
     *        without the buffer being cleared by the monitor.
     */
    void setSubmitBlocked(bool blocked);
    bool isSubmitBlocked() const { return submitBlocked; }

private:
    void resetEscBuffer();
};

#endif // QAGENTINPUTMONITOR_H
