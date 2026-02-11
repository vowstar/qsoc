// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qagentinputmonitor.h"

#ifndef _WIN32
#include <unistd.h>
#endif

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
        return 1; /* 0xxxxxxx -> ASCII */
    }
    if ((lead & 0xE0) == 0xC0) {
        return 2; /* 110xxxxx -> 2-byte */
    }
    if ((lead & 0xF0) == 0xE0) {
        return 3; /* 1110xxxx -> 3-byte (CJK) */
    }
    if ((lead & 0xF8) == 0xF0) {
        return 4; /* 11110xxx -> 4-byte (emoji) */
    }
    return 1; /* continuation/invalid -> treat as 1 */
}

bool QAgentInputMonitor::isUtf8Continuation(unsigned char byte)
{
    return (byte & 0xC0) == 0x80; /* 10xxxxxx */
}

void QAgentInputMonitor::appendToInput(const QString &decoded)
{
    inputBuffer.append(decoded);
    emit inputChanged(inputBuffer);
}

void QAgentInputMonitor::processBytes(const char *data, int len)
{
    for (int i = 0; i < len; i++) {
        auto byte = static_cast<unsigned char>(data[i]);

        /* Assembling UTF-8 multibyte sequence */
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
                /* Not a continuation byte - discard incomplete, reprocess */
                utf8Pending.clear();
                i--;
            }
            continue;
        }

        /* ESC: abort (clears input buffer first) */
        if (byte == 0x1B) {
            inputBuffer.clear();
            emit inputChanged(inputBuffer);
            emit escPressed();
            return;
        }

        /* Enter: submit queued input */
        if (byte == '\r' || byte == '\n') {
            if (!inputBuffer.isEmpty()) {
                QString text = inputBuffer;
                inputBuffer.clear();
                emit inputChanged(inputBuffer);
                emit inputReady(text);
            }
            continue;
        }

        /* Backspace: delete last Unicode character (surrogate-pair-aware) */
        if (byte == 0x7F || byte == '\b') {
            if (!inputBuffer.isEmpty()) {
                int bufLen = inputBuffer.size();
                if (bufLen >= 2 && inputBuffer[bufLen - 1].isLowSurrogate()
                    && inputBuffer[bufLen - 2].isHighSurrogate()) {
                    inputBuffer.chop(2); /* 4-byte UTF-8 / emoji */
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

        /* UTF-8 multibyte leading byte */
        if (byte >= 0xC0) {
            utf8Pending.append(static_cast<char>(byte));
            continue;
        }

        /* Printable ASCII (0x20-0x7E) */
        if (byte >= 0x20 && byte <= 0x7E) {
            appendToInput(QString(QChar(byte)));
            continue;
        }

        /* Other control characters: ignore */
    }
}

void QAgentInputMonitor::start()
{
    if (active) {
        return;
    }

#ifndef _WIN32
    /* Save original terminal settings */
    if (tcgetattr(STDIN_FILENO, &origTermios) == 0) {
        termiosSaved = true;

        /* Enter raw mode: non-canonical, no echo */
        struct termios raw = origTermios;
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    /* Create socket notifier for stdin */
    notifier = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
    connect(notifier, &QSocketNotifier::activated, this, [this]() {
        char buf[256];
        auto n = read(STDIN_FILENO, buf, sizeof(buf));
        if (n > 0) {
            processBytes(buf, static_cast<int>(n));
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
    /* Destroy notifier first */
    if (notifier) {
        delete notifier;
        notifier = nullptr;
    }

    /* Restore terminal settings */
    if (termiosSaved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &origTermios);
        termiosSaved = false;
    }

    /* Clear input state */
    inputBuffer.clear();
    utf8Pending.clear();
    emit inputChanged(QString());

    active = false;
#endif
}

bool QAgentInputMonitor::isActive() const
{
    return active;
}
