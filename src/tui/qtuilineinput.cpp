// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuilineinput.h"

#include <QByteArray>

#ifndef Q_OS_WIN
#include <termios.h>
#include <unistd.h>
#endif

QString QTuiLineInput::exec(const QString &prompt)
{
#ifdef Q_OS_WIN
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
    /* Read byte-by-byte so we can handle Esc/Backspace ourselves,
     * but leave ECHO on so each printable byte shows up at the
     * cursor. Disable ICANON so Backspace reaches us before the
     * terminal applies line-editing on its own. */
    struct termios cooked = original;
    cooked.c_lflag &= ~static_cast<tcflag_t>(ICANON);
    cooked.c_lflag |= ECHO | ISIG;
    cooked.c_cc[VMIN]  = 1;
    cooked.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &cooked) != 0) {
        return {};
    }

    const QByteArray promptBytes = prompt.toUtf8();
    (void) write(STDOUT_FILENO, promptBytes.constData(), promptBytes.size());

    QByteArray buffer;
    char       byte     = 0;
    bool       canceled = false;
    while (true) {
        const ssize_t got = read(STDIN_FILENO, &byte, 1);
        if (got <= 0) {
            break;
        }
        if (byte == '\n' || byte == '\r') {
            break;
        }
        if (byte == 0x1B /* Esc */ || byte == 0x03 /* Ctrl+C */) {
            canceled = true;
            break;
        }
        if (byte == 0x7F /* DEL */ || byte == '\b') {
            if (buffer.isEmpty()) {
                continue;
            }
            int trailing = 0;
            while (!buffer.isEmpty()
                   && (static_cast<unsigned char>(buffer.back()) & 0xC0u) == 0x80u) {
                buffer.chop(1);
                ++trailing;
            }
            if (!buffer.isEmpty()) {
                buffer.chop(1);
            }
            /* Visually erase one glyph regardless of how many bytes
             * the code point spanned. Continuation bytes do not
             * advance the cursor, so a single BS SP BS suffices. */
            Q_UNUSED(trailing);
            static const char kErase[] = "\b \b";
            (void) write(STDOUT_FILENO, kErase, sizeof(kErase) - 1);
            continue;
        }
        buffer.append(byte);
    }

    tcsetattr(STDIN_FILENO, TCSANOW, &original);
    static const char kNewline[] = "\n";
    (void) write(STDOUT_FILENO, kNewline, 1);

    if (canceled) {
        return {};
    }
    return QString::fromUtf8(buffer);
#endif
}
