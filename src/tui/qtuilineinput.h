// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUILINEINPUT_H
#define QTUILINEINPUT_H

#include <QString>

/**
 * @brief Visible single-line text input for free-form user responses.
 * @details Mirrors `QTuiSecretPrompt::exec` but with terminal echo
 *          **enabled** so the user can see what they type. Used by
 *          the `ask_user` tool's "Other..." branch where the user
 *          wants to type a custom answer that does not match any of
 *          the structured menu options.
 *
 *          The caller is responsible for pausing the enclosing TUI
 *          compositor; this class only manipulates `termios` and
 *          reads / writes stdin / stdout directly.
 *
 *          Esc and Ctrl+C return an empty string so the caller can
 *          treat them as "cancel". Backspace removes one UTF-8 code
 *          point at a time and visually erases the previous glyph
 *          via `BS SP BS`.
 */
class QTuiLineInput
{
public:
    /**
     * @brief Show @p prompt and read a line. ECHO stays on.
     * @return Typed text without the trailing newline; empty on
     *         cancel or non-TTY stdin.
     */
    static QString exec(const QString &prompt);
};

#endif // QTUILINEINPUT_H
