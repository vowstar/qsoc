// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuisecretprompt.h"

#include <QByteArray>

#include <cstdio>

#ifndef Q_OS_WIN
#include <termios.h>
#include <unistd.h>
#endif

QString QTuiSecretPrompt::exec(const QString &prompt)
{
#ifdef Q_OS_WIN
    /* No echo control on Windows console in this code path. The
     * caller already pauses the compositor; without termios we
     * cannot guarantee hidden input here, so refuse to read rather
     * than leak typed bytes. */
    Q_UNUSED(prompt);
    return {};
#else
    if (!isatty(STDIN_FILENO)) {
        return {};
    }

    struct termios original = {};
    if (tcgetattr(STDIN_FILENO, &original) != 0) {
        return {};
    }
    struct termios silent = original;
    silent.c_lflag &= ~static_cast<tcflag_t>(ECHO | ICANON);
    silent.c_lflag |= ISIG; /* keep Ctrl+C / Ctrl+Z working */
    silent.c_cc[VMIN]  = 1;
    silent.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &silent) != 0) {
        return {};
    }

    /* Write prompt on its own line; do not advance to the next line
     * until the user finishes typing. */
    const QByteArray promptBytes = prompt.toUtf8();
    (void) write(STDOUT_FILENO, promptBytes.constData(), promptBytes.size());

    QByteArray buffer;
    char       ch       = 0;
    bool       canceled = false;
    while (true) {
        const ssize_t got = read(STDIN_FILENO, &ch, 1);
        if (got <= 0) {
            break;
        }
        if (ch == '\n' || ch == '\r') {
            break;
        }
        if (ch == 0x1B /* Esc */) {
            canceled = true;
            break;
        }
        if (ch == 0x03 /* Ctrl+C */) {
            canceled = true;
            break;
        }
        if (ch == 0x7F /* Backspace / DEL */ || ch == '\b') {
            if (!buffer.isEmpty()) {
                /* Pop one full UTF-8 code point: trailing bytes
                 * carry the 10xxxxxx pattern, the leading byte
                 * starts with anything other than 10xxxxxx. */
                while (!buffer.isEmpty()
                       && (static_cast<unsigned char>(buffer.back()) & 0xC0u) == 0x80u) {
                    buffer.chop(1);
                }
                if (!buffer.isEmpty()) {
                    buffer.chop(1);
                }
            }
            continue;
        }
        buffer.append(ch);
    }

    /* Restore termios before printing the newline so the trailing
     * '\n' lands in the user's normal echo mode. */
    tcsetattr(STDIN_FILENO, TCSANOW, &original);
    static const char kNewline[] = "\n";
    (void) write(STDOUT_FILENO, kNewline, 1);

    if (canceled) {
        return {};
    }
    return QString::fromUtf8(buffer);
#endif
}
