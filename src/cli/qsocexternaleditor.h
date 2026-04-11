// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCEXTERNALEDITOR_H
#define QSOCEXTERNALEDITOR_H

#include <QString>

/**
 * @brief External editor invocation helper for the agent REPL.
 * @details Writes the current prompt text to a temporary file, spawns
 *          the user's editor (EDITOR env, falling back to 'vi'), and
 *          returns the edited file contents on success. Synchronous —
 *          the caller must pause the TUI compositor and the raw-mode
 *          input monitor before calling, and restore them afterwards.
 */
class QSocExternalEditor
{
public:
    /**
     * @brief Open the given text in an external editor, return the edited text.
     * @param current  Initial text to write into the tempfile.
     * @param result   Out-parameter: edited text on success, unchanged on failure.
     * @param error    Out-parameter: human-readable error message on failure.
     * @return True on success (editor exited 0, file readable). False on any failure.
     */
    static bool editText(const QString &current, QString &result, QString &error);

    /**
     * @brief Resolve the editor command to run.
     * @details Returns $EDITOR if set and non-empty, otherwise 'vi'.
     */
    static QString resolveEditor();
};

#endif // QSOCEXTERNALEDITOR_H
