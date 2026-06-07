// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSESSIONTITLE_H
#define QSOCSESSIONTITLE_H

#include <QString>
#include <QStringList>

/**
 * @brief Pure helpers for the auto session-title feature.
 * @details Builds the prompt for a short, scannable session title and
 *          sanitizes the model's reply. Kept side-effect free so the
 *          formatting is unit-testable; the CLI owns the LLM call and the
 *          meta write. The title generation uses the user's configured
 *          model (an optional knob may override); it never binds a cheaper
 *          model on its own.
 */
namespace QSocSessionTitle {

/** @brief System prompt instructing the model to emit a bare short title. */
inline QString systemPrompt()
{
    return QStringLiteral(
        "You write a concise session title summarizing the user's task. "
        "Reply with the title only: 3 to 7 words, Title Case, no surrounding "
        "quotes, no trailing punctuation, no preamble.");
}

/** @brief User message carrying the first prompt to title. */
inline QString buildUserMessage(const QString &firstPrompt)
{
    return QStringLiteral("Title this task:\n%1").arg(firstPrompt);
}

/**
 * @brief Clean a raw model reply into a storable title.
 * @details Collapses whitespace, strips surrounding quotes and trailing
 *          punctuation, and caps the result to 7 words. Returns empty when
 *          nothing usable remains.
 */
inline QString sanitize(const QString &raw)
{
    QString title = raw.simplified();
    /* Strip one layer of matching surrounding quotes. */
    if (title.size() >= 2) {
        const QChar first = title.front();
        const QChar last  = title.back();
        if ((first == QLatin1Char('"') && last == QLatin1Char('"'))
            || (first == QLatin1Char('\'') && last == QLatin1Char('\''))) {
            title = title.mid(1, title.size() - 2).simplified();
        }
    }
    /* Drop trailing sentence punctuation a model sometimes adds. */
    while (!title.isEmpty()) {
        const QChar last = title.back();
        if (last == QLatin1Char('.') || last == QLatin1Char('!') || last == QLatin1Char(':')
            || last == QLatin1Char(',') || last == QLatin1Char(';')) {
            title.chop(1);
            title = title.trimmed();
        } else {
            break;
        }
    }
    /* Cap to 7 words so the picker stays scannable. */
    const QStringList words = title.split(QLatin1Char(' '), Qt::SkipEmptyParts);
    if (words.size() > 7) {
        title = QStringList(words.mid(0, 7)).join(QLatin1Char(' '));
    }
    return title;
}

} // namespace QSocSessionTitle

#endif // QSOCSESSIONTITLE_H
