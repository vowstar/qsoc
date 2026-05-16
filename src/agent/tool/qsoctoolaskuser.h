// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLASKUSER_H
#define QSOCTOOLASKUSER_H

#include "agent/qsoctool.h"

#include <functional>

/**
 * @brief One menu entry presented to the user by `ask_user`.
 */
struct QSocAskUserOption
{
    QString label;       /**< Short text shown in the menu row. */
    QString description; /**< Optional one-line hint. */
};

/**
 * @brief Result of a single `ask_user` round-trip.
 */
struct QSocAskUserResult
{
    QString choice; /**< Picked option label, the literal "Other", or empty on cancel. */
    QString text;   /**< Free-form text when @c choice is "Other"; empty otherwise. */
    bool    canceled = false;
};

/**
 * @brief LLM-facing tool that asks the user to pick from 2-4 options
 *        with an automatic "Other..." escape hatch for custom text.
 * @details Mirrors claude-code's `AskUserQuestion`: the tool guarantees
 *          a free-form path even when the model could not enumerate it.
 *          The CLI layer installs a callback that drives the TUI; the
 *          sub-agent dispatch path leaves the callback unset so a
 *          mid-LLM-turn child cannot block waiting on user input.
 */
class QSocToolAskUser : public QSocTool
{
    Q_OBJECT

public:
    using Callback = std::function<QSocAskUserResult(
        const QString &question, const QString &header, const QList<QSocAskUserOption> &options)>;

    QSocToolAskUser(QObject *parent, Callback callback);

    /**
     * @brief Install or replace the TUI-driving callback. Used by the
     *        REPL once the compositor and input monitor exist; left
     *        unset on the sub-agent dispatch path so a child cannot
     *        block mid-LLM-turn waiting on user input.
     */
    void setCallback(Callback callback);

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    Callback callback_;
};

#endif // QSOCTOOLASKUSER_H
