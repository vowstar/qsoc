// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qagentinputmonitor.h"

#ifndef _WIN32
#include <unistd.h>
#endif

#include <QTimer>

QAgentInputMonitor::QAgentInputMonitor(QObject *parent)
    : QObject(parent)
{}

QAgentInputMonitor::~QAgentInputMonitor()
{
    stop();
}

int QAgentInputMonitor::utf8SeqLen(unsigned char lead)
{
    if ((lead & 0x80) == 0) {
        return 1;
    }
    if ((lead & 0xE0) == 0xC0) {
        return 2;
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3;
    }
    if ((lead & 0xF8) == 0xF0) {
        return 4;
    }
    return 1;
}

bool QAgentInputMonitor::isUtf8Continuation(unsigned char byte)
{
    return (byte & 0xC0) == 0x80;
}

void QAgentInputMonitor::insertAtCursor(const QString &decoded)
{
    inputBuffer.insert(cursorPos, decoded);
    cursorPos += decoded.size();
    emit inputChanged(inputBuffer);
}

void QAgentInputMonitor::resetEscState()
{
    resetEscBuffer();
}

void QAgentInputMonitor::setInputBuffer(const QString &text)
{
    inputBuffer = text;
    cursorPos   = inputBuffer.size();
    emit inputChanged(inputBuffer);
}

int QAgentInputMonitor::prevCharStep() const
{
    if (cursorPos >= 2 && inputBuffer[cursorPos - 1].isLowSurrogate()
        && inputBuffer[cursorPos - 2].isHighSurrogate()) {
        return 2;
    }
    return (cursorPos > 0) ? 1 : 0;
}

int QAgentInputMonitor::nextCharStep() const
{
    if (cursorPos + 1 < inputBuffer.size() && inputBuffer[cursorPos].isHighSurrogate()
        && inputBuffer[cursorPos + 1].isLowSurrogate()) {
        return 2;
    }
    return (cursorPos < inputBuffer.size()) ? 1 : 0;
}

void QAgentInputMonitor::moveCursorLeft()
{
    int step = prevCharStep();
    if (step > 0) {
        cursorPos -= step;
        emit inputChanged(inputBuffer);
    }
}

void QAgentInputMonitor::moveCursorRight()
{
    int step = nextCharStep();
    if (step > 0) {
        cursorPos += step;
        emit inputChanged(inputBuffer);
    }
}

void QAgentInputMonitor::insertCompletion(int atPos, const QString &replacement, QChar trailing)
{
    if (atPos < 0 || atPos >= static_cast<int>(inputBuffer.size())) {
        return;
    }
    /* Remove '@query' (from '@' at atPos up to current cursor) and
     * insert '@' + replacement + optional trailing char at atPos. */
    int removeLen = cursorPos - atPos;
    if (removeLen <= 0) {
        return;
    }
    inputBuffer.remove(atPos, removeLen);
    QString inserted = QLatin1Char('@') + replacement;
    if (!trailing.isNull()) {
        inserted.append(trailing);
    }
    inputBuffer.insert(atPos, inserted);
    cursorPos = atPos + static_cast<int>(inserted.size());
    emit inputChanged(inputBuffer);
}

void QAgentInputMonitor::setSubmitBlocked(bool blocked)
{
    submitBlocked = blocked;
}

void QAgentInputMonitor::resetEscBuffer()
{
    escBuffer.clear();
    inEscSeq = false;
}

void QAgentInputMonitor::processEscSequence()
{
    if (escBuffer.size() < 2) {
        /* Only ESC so far — don't decide yet, wait for more data.
         * A 50ms timer will emit escPressed if nothing follows. */
        return;
    }

    /* ESC [ ... sequences (CSI) */
    if (escBuffer[1] == '[') {
        /* Need at least 3 bytes to determine CSI type */
        if (escBuffer.size() < 3) {
            return; /* Wait for more */
        }

        /* SGR mouse: ESC [ < btn ; x ; y M/m */
        if (escBuffer[2] == '<') {
            for (int idx = 3; idx < escBuffer.size(); idx++) {
                char chr = escBuffer[idx];
                if (chr == 'M' || chr == 'm') {
                    QByteArray        params = escBuffer.mid(3, idx - 3);
                    QList<QByteArray> parts  = params.split(';');
                    if (parts.size() == 3) {
                        int  btnFlags = parts[0].toInt();
                        int  mouseCol = parts[1].toInt();
                        int  mouseRow = parts[2].toInt();
                        bool pressed  = (chr == 'M');
                        bool isWheel  = (btnFlags & 64) != 0;

                        if (isWheel) {
                            emit mouseWheel(btnFlags & 3);
                        } else {
                            emit mouseClick(btnFlags & 3, mouseCol, mouseRow, pressed);
                        }
                    }
                    resetEscBuffer();
                    return;
                }
            }
            /* Incomplete SGR — wait for more */
            if (escBuffer.size() > 32) {
                resetEscBuffer(); /* Safety: drop oversized */
            }
            return;
        }

        /* Arrow keys and Home/End: ESC [ A/B/C/D/H/F (exactly 3 bytes).
         * Up/Down ('A'/'B') emit arrowKey for history navigation.
         * Left/Right ('C'/'D') move the cursor within the input buffer.
         * Home/End ('H'/'F') jump to start/end of current line. */
        if (escBuffer.size() == 3) {
            char last = escBuffer[2];
            if (last == 'A' || last == 'B') {
                emit arrowKey(last);
                resetEscBuffer();
                return;
            }
            if (last == 'C') {
                moveCursorRight();
                resetEscBuffer();
                return;
            }
            if (last == 'D') {
                moveCursorLeft();
                resetEscBuffer();
                return;
            }
            if (last == 'H') {
                int oldPos = cursorPos;
                while (cursorPos > 0 && inputBuffer[cursorPos - 1] != QLatin1Char('\n')) {
                    cursorPos--;
                }
                if (cursorPos != oldPos) {
                    emit inputChanged(inputBuffer);
                }
                resetEscBuffer();
                return;
            }
            if (last == 'F') {
                int oldPos = cursorPos;
                while (cursorPos < static_cast<int>(inputBuffer.size())
                       && inputBuffer[cursorPos] != QLatin1Char('\n')) {
                    cursorPos++;
                }
                if (cursorPos != oldPos) {
                    emit inputChanged(inputBuffer);
                }
                resetEscBuffer();
                return;
            }
        }

        /* Other CSI: check if final byte received (0x40-0x7E) */
        char last = escBuffer[escBuffer.size() - 1];
        if (last >= 0x40 && last <= 0x7E) {
            /* Bracketed paste markers: ESC [ 200 ~ (start), ESC [ 201 ~ (end) */
            if (escBuffer == QByteArray("\033[200~")) {
                inBracketedPaste = true;
                resetEscBuffer();
                return;
            }
            if (escBuffer == QByteArray("\033[201~")) {
                inBracketedPaste = false;
                resetEscBuffer();
                return;
            }
            /* ESC [ 1 ~ = Home */
            if (escBuffer == QByteArray("\033[1~")) {
                int oldPos = cursorPos;
                while (cursorPos > 0 && inputBuffer[cursorPos - 1] != QLatin1Char('\n')) {
                    cursorPos--;
                }
                if (cursorPos != oldPos) {
                    emit inputChanged(inputBuffer);
                }
                resetEscBuffer();
                return;
            }
            /* ESC [ 4 ~ = End */
            if (escBuffer == QByteArray("\033[4~")) {
                int oldPos = cursorPos;
                while (cursorPos < static_cast<int>(inputBuffer.size())
                       && inputBuffer[cursorPos] != QLatin1Char('\n')) {
                    cursorPos++;
                }
                if (cursorPos != oldPos) {
                    emit inputChanged(inputBuffer);
                }
                resetEscBuffer();
                return;
            }
            /* ESC [ 3 ~ = Delete (delete char at cursor, surrogate-aware) */
            if (escBuffer == QByteArray("\033[3~")) {
                int step = nextCharStep();
                if (step > 0) {
                    inputBuffer.remove(cursorPos, step);
                    emit inputChanged(inputBuffer);
                }
                resetEscBuffer();
                return;
            }
            resetEscBuffer(); /* Complete — unknown CSI ignored */
            return;
        }

        /* Still incomplete */
        if (escBuffer.size() > 32) {
            resetEscBuffer();
        }
        return;
    }

    /* ESC + non-[ : unknown sequence — treat as bare ESC */
    emit escPressed();
    resetEscBuffer();
}

void QAgentInputMonitor::processBytes(const char *data, int len)
{
    for (int i = 0; i < len; i++) {
        auto byte = static_cast<unsigned char>(data[i]);

        /* Ctrl+X chord: waiting for second key of the two-key sequence.
         * Ctrl+X Ctrl+E → external editor. Any other byte cancels the
         * chord and falls through to normal processing (reprocess via i--). */
        if (inCtrlXChord) {
            inCtrlXChord = false;
            if (byte == 0x05) { /* Ctrl+E */
                emit externalEditorRequested();
                continue;
            }
            /* Not a recognized chord — reprocess this byte normally */
            i--;
            continue;
        }

        /* Continue accumulating ESC sequence */
        if (inEscSeq) {
            escBuffer.append(static_cast<char>(byte));
            processEscSequence();
            continue;
        }

        /* UTF-8 multibyte assembly */
        if (!utf8Pending.isEmpty()) {
            if (isUtf8Continuation(byte)) {
                utf8Pending.append(static_cast<char>(byte));
                int expected = utf8SeqLen(static_cast<unsigned char>(utf8Pending[0]));
                if (static_cast<int>(utf8Pending.size()) >= expected) {
                    QString decoded = QString::fromUtf8(utf8Pending);
                    utf8Pending.clear();
                    if (!decoded.isEmpty()) {
                        insertAtCursor(decoded);
                    }
                }
            } else {
                utf8Pending.clear();
                i--;
            }
            continue;
        }

        /* Ctrl+C: interrupt — use continue, NOT return, so multiple 0x03 in one
         * read() can all be processed (critical for double Ctrl+C detection) */
        if (byte == 0x03) {
            inputBuffer.clear();
            cursorPos = 0;
            utf8Pending.clear();
            emit inputChanged(inputBuffer);
            emit ctrlCPressed();
            continue;
        }

        /* Ctrl+X: start two-key chord (Ctrl+X Ctrl+E → external editor).
         * The next byte is interpreted by the chord branch at the top. */
        if (byte == 0x18) {
            inCtrlXChord = true;
            continue;
        }

        /* Ctrl+G: direct external editor trigger (readline-compatible alias) */
        if (byte == 0x07) {
            emit externalEditorRequested();
            continue;
        }

        /* ESC: start sequence buffering */
        if (byte == 0x1B) {
            inEscSeq = true;
            escBuffer.clear();
            escBuffer.append(static_cast<char>(byte));
            continue;
        }

        /* Enter */
        if (byte == '\r' || byte == '\n') {
            /* In bracketed paste: newline becomes literal content, never submit */
            if (inBracketedPaste) {
                insertAtCursor(QStringLiteral("\n"));
                continue;
            }
            /* Submit blocked (completion popup open): REPL owns Enter semantics */
            if (submitBlocked) {
                emit submitBlockedKey('E');
                continue;
            }
            /* Backslash line continuation: trailing '\' + Enter → newline inserted, no submit.
             * Mirrors bash behavior: the backslash must be the last character of the buffer. */
            if (inputBuffer.endsWith(QLatin1Char('\\'))) {
                inputBuffer.chop(1);
                inputBuffer.append(QLatin1Char('\n'));
                int maxPos = static_cast<int>(inputBuffer.size());
                cursorPos  = qMin(cursorPos, maxPos);
                emit inputChanged(inputBuffer);
                continue;
            }
            /* Normal submit */
            if (!inputBuffer.isEmpty()) {
                QString text = inputBuffer;
                inputBuffer.clear();
                cursorPos = 0;
                emit inputChanged(inputBuffer);
                emit inputReady(text);
            }
            continue;
        }

        /* Backspace: delete char at cursorPos-1 (surrogate-aware) */
        if (byte == 0x7F || byte == '\b') {
            int step = prevCharStep();
            if (step > 0) {
                inputBuffer.remove(cursorPos - step, step);
                cursorPos -= step;
                emit inputChanged(inputBuffer);
            }
            continue;
        }

        /* Ctrl-A: move to start of current line */
        if (byte == 0x01) {
            int oldPos = cursorPos;
            while (cursorPos > 0 && inputBuffer[cursorPos - 1] != QLatin1Char('\n')) {
                cursorPos--;
            }
            if (cursorPos != oldPos) {
                emit inputChanged(inputBuffer);
            }
            continue;
        }

        /* Ctrl-E: move to end of current line */
        if (byte == 0x05) {
            int oldPos = cursorPos;
            while (cursorPos < static_cast<int>(inputBuffer.size())
                   && inputBuffer[cursorPos] != QLatin1Char('\n')) {
                cursorPos++;
            }
            if (cursorPos != oldPos) {
                emit inputChanged(inputBuffer);
            }
            continue;
        }

        /* Ctrl-K: kill from cursor to end of current line */
        if (byte == 0x0B) {
            int end = cursorPos;
            while (end < static_cast<int>(inputBuffer.size())
                   && inputBuffer[end] != QLatin1Char('\n')) {
                end++;
            }
            if (end > cursorPos) {
                inputBuffer.remove(cursorPos, end - cursorPos);
                emit inputChanged(inputBuffer);
            }
            continue;
        }

        /* Ctrl-U: kill from start of current line to cursor */
        if (byte == 0x15) {
            int start = cursorPos;
            while (start > 0 && inputBuffer[start - 1] != QLatin1Char('\n')) {
                start--;
            }
            if (start < cursorPos) {
                inputBuffer.remove(start, cursorPos - start);
                cursorPos = start;
                emit inputChanged(inputBuffer);
            }
            continue;
        }

        /* Ctrl-W: delete word back from cursor */
        if (byte == 0x17) {
            int end = cursorPos;
            /* Skip trailing spaces */
            while (end > 0 && inputBuffer[end - 1].isSpace()
                   && inputBuffer[end - 1] != QLatin1Char('\n')) {
                end--;
            }
            /* Skip non-space word chars */
            while (end > 0 && !inputBuffer[end - 1].isSpace()) {
                end--;
            }
            if (end < cursorPos) {
                inputBuffer.remove(end, cursorPos - end);
                cursorPos = end;
                emit inputChanged(inputBuffer);
            }
            continue;
        }

        /* Tab: when submit is blocked (completion popup open), Tab confirms.
         * Otherwise ignored — @file completion is triggered live by the REPL
         * on inputChanged, not by Tab. */
        if (byte == '\t') {
            if (submitBlocked) {
                emit submitBlockedKey('T');
            }
            continue;
        }

        /* UTF-8 multibyte leading byte */
        if (byte >= 0xC0) {
            utf8Pending.append(static_cast<char>(byte));
            continue;
        }

        /* Printable ASCII */
        if (byte >= 0x20 && byte <= 0x7E) {
            insertAtCursor(QString(QChar(byte)));
            continue;
        }
    }

    /* After processing all bytes: if we're in an ESC sequence with only the
     * ESC byte, schedule a delayed check. If no more bytes arrive within 50ms,
     * this was a bare ESC keypress. */
    if (inEscSeq && escBuffer.size() == 1) {
        /* Use guarded connection: if 'this' is destroyed before timer fires,
         * the connection is broken and lambda won't execute. */
        auto *guard = this;
        QTimer::singleShot(50, guard, [this]() {
            if (active && inEscSeq && escBuffer.size() == 1) {
                /* When submit is blocked (completion popup open), Esc only
                 * dismisses the popup — keep the input buffer intact. */
                if (!submitBlocked) {
                    inputBuffer.clear();
                    cursorPos = 0;
                    emit inputChanged(inputBuffer);
                }
                emit escPressed();
                resetEscBuffer();
            }
        });
    }
}

void QAgentInputMonitor::start()
{
    if (active) {
        return;
    }

#ifndef _WIN32
    if (tcgetattr(STDIN_FILENO, &origTermios) == 0) {
        termiosSaved = true;

        struct termios raw = origTermios;
        raw.c_iflag &= ~static_cast<tcflag_t>(ICRNL | INLCR | IXON);
        raw.c_oflag &= ~static_cast<tcflag_t>(OPOST);
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO | ISIG);
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    notifier = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
    connect(notifier, &QSocketNotifier::activated, this, [this]() {
        char buf[4096];
        auto bytesRead = read(STDIN_FILENO, buf, sizeof(buf));
        if (bytesRead > 0) {
            processBytes(buf, static_cast<int>(bytesRead));
        }
    });

    /* Enable bracketed paste mode: terminal wraps pasted text with
     * \033[200~ ... \033[201~ so we can distinguish typed Enter from pasted newline. */
    {
        const char *seq     = "\033[?2004h";
        ssize_t     written = write(STDOUT_FILENO, seq, 8);
        (void) written;
    }

    active = true;
#endif
}

void QAgentInputMonitor::stop()
{
    if (!active) {
        return;
    }

#ifndef _WIN32
    if (notifier) {
        delete notifier;
        notifier = nullptr;
    }

    /* Disable bracketed paste mode before restoring termios */
    {
        const char *seq     = "\033[?2004l";
        ssize_t     written = write(STDOUT_FILENO, seq, 8);
        (void) written;
    }

    if (termiosSaved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &origTermios);
        termiosSaved = false;
    }

    inputBuffer.clear();
    cursorPos = 0;
    utf8Pending.clear();
    resetEscBuffer();
    inBracketedPaste = false;
    inCtrlXChord     = false;
    emit inputChanged(QString());

    active = false;
#endif
}

bool QAgentInputMonitor::isActive() const
{
    return active;
}
