// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUISECRETPROMPT_H
#define QTUISECRETPROMPT_H

#include <QString>

/**
 * @brief Hidden-input single-line prompt for SSH secrets.
 * @details Reads a UTF-8 line from stdin with terminal echo disabled
 *          so the typed bytes never appear on screen and are not
 *          captured by terminal history. The pause/resume of any
 *          enclosing compositor is the caller's responsibility:
 *          this class only touches `termios` and stdin / stdout.
 *
 *          On POSIX, `termios` is flipped to disable `ECHO` and
 *          `ICANON`; the original mode is restored on return even
 *          when the user cancels with Ctrl+C. Backspace removes one
 *          UTF-8 codepoint from the in-memory buffer (no echo).
 *          Enter terminates the prompt and returns the accumulated
 *          string. Esc returns an empty string so the caller can
 *          treat it as "cancel".
 *
 *          The returned QString is the only place the secret lives;
 *          do not log it, store it, or print it. The class never
 *          writes the secret to the terminal, even partially.
 */
class QTuiSecretPrompt
{
public:
    /**
     * @brief Show @p prompt and read a hidden line from stdin.
     * @param prompt One-line UI-safe label (e.g. "Password: ").
     * @return Typed bytes as a UTF-8 string. Empty when the user
     *         cancels with Esc or stdin closes before any byte.
     */
    static QString exec(const QString &prompt);
};

#endif // QTUISECRETPROMPT_H
