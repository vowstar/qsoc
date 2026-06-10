// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCAWAYSUMMARY_H
#define QSOCAWAYSUMMARY_H

#include <QString>

/**
 * @brief Pure helpers for the away-summary ("recap") feature.
 * @details After the terminal loses focus for a configured idle delay, a
 *          short "while you were away" recap of the task and the next step
 *          is generated and printed. These helpers build the prompt and
 *          clean the model reply; kept side-effect free so they are unit
 *          testable. The CLI owns the LLM call, the idle timer, and the
 *          render. Generation uses the user's configured model (an optional
 *          knob may override); it never binds a cheaper model on its own.
 */
namespace QSocAwaySummary {

/** @brief System prompt instructing the model to emit a bare 1-3 sentence recap. */
inline QString systemPrompt()
{
    return QStringLiteral(
        "The user stepped away and is returning to the session. Write 1 to 3 "
        "short sentences recapping where things stand. Open with the high-level "
        "task being worked on, not low-level implementation details. Then state "
        "the concrete next step. Do not greet, do not report status, do not "
        "recap commits. Reply with the recap text only, no preamble.");
}

/** @brief User message carrying the recent transcript to recap. */
inline QString buildUserMessage(const QString &transcript)
{
    return QStringLiteral("Recent conversation:\n%1\nWrite the recap now.").arg(transcript);
}

/**
 * @brief Clean a raw model reply into a single-line recap.
 * @details Collapses whitespace (so it renders as one dim line) and strips
 *          one layer of surrounding quotes. Returns empty when nothing
 *          usable remains.
 */
inline QString sanitize(const QString &raw)
{
    QString text = raw.simplified();
    if (text.size() >= 2) {
        const QChar first = text.front();
        const QChar last  = text.back();
        if ((first == QLatin1Char('"') && last == QLatin1Char('"'))
            || (first == QLatin1Char('\'') && last == QLatin1Char('\''))) {
            text = text.mid(1, text.size() - 2).simplified();
        }
    }
    return text;
}

} // namespace QSocAwaySummary

#endif // QSOCAWAYSUMMARY_H
