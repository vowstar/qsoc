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

void QAgentInputMonitor::appendToInput(const QString &decoded)
{
    inputBuffer.append(decoded);
    emit inputChanged(inputBuffer);
}

void QAgentInputMonitor::resetEscState()
{
    resetEscBuffer();
}

void QAgentInputMonitor::setInputBuffer(const QString &text)
{
    inputBuffer = text;
    emit inputChanged(inputBuffer);
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

        /* Arrow keys: ESC [ A/B/C/D (exactly 3 bytes) */
        if (escBuffer.size() == 3) {
            char last = escBuffer[2];
            if (last >= 'A' && last <= 'D') {
                emit arrowKey(last);
                resetEscBuffer();
                return;
            }
        }

        /* Other CSI: check if final byte received (0x40-0x7E) */
        char last = escBuffer[escBuffer.size() - 1];
        if (last >= 0x40 && last <= 0x7E) {
            resetEscBuffer(); /* Complete — ignore unknown */
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
                if (utf8Pending.size() >= expected) {
                    QString decoded = QString::fromUtf8(utf8Pending);
                    utf8Pending.clear();
                    if (!decoded.isEmpty()) {
                        appendToInput(decoded);
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
            utf8Pending.clear();
            emit inputChanged(inputBuffer);
            emit ctrlCPressed();
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
            if (!inputBuffer.isEmpty()) {
                QString text = inputBuffer;
                inputBuffer.clear();
                emit inputChanged(inputBuffer);
                emit inputReady(text);
            }
            continue;
        }

        /* Backspace */
        if (byte == 0x7F || byte == '\b') {
            if (!inputBuffer.isEmpty()) {
                int bufLen = inputBuffer.size();
                if (bufLen >= 2 && inputBuffer[bufLen - 1].isLowSurrogate()
                    && inputBuffer[bufLen - 2].isHighSurrogate()) {
                    inputBuffer.chop(2);
                } else {
                    inputBuffer.chop(1);
                }
                emit inputChanged(inputBuffer);
            }
            continue;
        }

        /* Ctrl-U: clear line */
        if (byte == 0x15) {
            inputBuffer.clear();
            emit inputChanged(inputBuffer);
            continue;
        }

        /* Ctrl-W: delete word */
        if (byte == 0x17) {
            inputBuffer   = inputBuffer.trimmed();
            int lastSpace = inputBuffer.lastIndexOf(' ');
            inputBuffer   = (lastSpace >= 0) ? inputBuffer.left(lastSpace + 1) : QString();
            emit inputChanged(inputBuffer);
            continue;
        }

        /* Tab: ignore */
        if (byte == '\t') {
            continue;
        }

        /* UTF-8 multibyte leading byte */
        if (byte >= 0xC0) {
            utf8Pending.append(static_cast<char>(byte));
            continue;
        }

        /* Printable ASCII */
        if (byte >= 0x20 && byte <= 0x7E) {
            appendToInput(QString(QChar(byte)));
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
                inputBuffer.clear();
                emit inputChanged(inputBuffer);
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
        char buf[256];
        auto bytesRead = read(STDIN_FILENO, buf, sizeof(buf));
        if (bytesRead > 0) {
            processBytes(buf, static_cast<int>(bytesRead));
        }
    });

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

    if (termiosSaved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &origTermios);
        termiosSaved = false;
    }

    inputBuffer.clear();
    utf8Pending.clear();
    resetEscBuffer();
    emit inputChanged(QString());

    active = false;
#endif
}

bool QAgentInputMonitor::isActive() const
{
    return active;
}
