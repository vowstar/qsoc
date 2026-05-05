// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCCODEHIGHLIGHTER_H
#define QSOCCODEHIGHLIGHTER_H

#include "tui/qtuiscreen.h"

#include <QList>
#include <QString>

/**
 * @brief Single-line regex-based syntax highlighter.
 * @details Tokenises a code line for one of the supported languages
 *          and returns styled runs that map keyword / string / number
 *          / comment / preproc tokens to the warm retro palette. The
 *          line is treated as standalone — multi-line constructs like
 *          C block comments do not carry across calls; callers that
 *          need full multi-line state can layer it on top.
 *
 *          Supported language tags (matched lower-case, trimmed):
 *          - python / py
 *          - bash / sh / shell / zsh
 *          - c
 *          - cpp / c++ / cxx / cc
 *          - json
 *          - yaml / yml
 *          - verilog / sv / systemverilog
 *          - diff / patch (per-line +/-/@@ coloring)
 *
 *          Anything else falls through with a single default-styled
 *          run so the renderer never errors on unknown tags.
 */
class QSocCodeHighlighter
{
public:
    /**
     * @brief Tokenise one code line and return the colored runs.
     * @param line     The source line, without trailing newline.
     * @param language Lower-case language tag from the fence info.
     * @return One or more styled runs whose concatenated text equals
     *         the input. Runs may share a color where the tokenizer
     *         could not split them further.
     */
    static QList<QTuiStyledRun> highlight(const QString &line, const QString &language);
};

#endif // QSOCCODEHIGHLIGHTER_H
