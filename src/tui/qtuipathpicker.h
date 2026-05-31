// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUIPATHPICKER_H
#define QTUIPATHPICKER_H

#include <QString>
#include <QStringList>

#include <functional>

/**
 * @brief Two-column cascading directory picker for local and remote trees.
 * @details Left column lists parent, current-dir, and subdirectory entries
 *          of the active path. Right column previews the highlighted dir
 *          so the user can see where navigating will land before pressing
 *          Enter. Driven by a caller-supplied lambda so the same widget
 *          serves local (QDir) and remote (SFTP) sources without pulling
 *          filesystem dependencies into the tui layer.
 *
 * Keys: Up/Down change highlight, Right/Enter descend into a subdir with
 * children or select one without children. Enter on "." returns the
 * current path. Enter or Left on ".." or the parent entry pops one
 * level. "/" opens a locate prompt: type an absolute, relative, or
 * "~"-prefixed path and Enter navigates to the deepest existing
 * directory along it. ESC cancels and returns an empty QString.
 */
class QTuiPathPicker
{
public:
    using ListDirsFn = std::function<QStringList(const QString &path)>;

    void setTitle(const QString &title);
    void setStartPath(const QString &path);
    void setListDirs(ListDirsFn listDirs);
    /* Home directory used to expand a leading "~" in the locate prompt.
     * Empty (default) leaves "~" untouched. */
    void setHomePath(const QString &path);

    /* Blocking interactive loop. Returns the chosen absolute path,
     * or empty QString on cancel. */
    QString exec();

    /* Resolve a user-typed locate string against an anchor directory.
     * Expands a leading "~" to home, makes relative input absolute under
     * cur, collapses "." / ".." / redundant slashes, then walks the path
     * segment by segment using listDirs and stops at the deepest segment
     * that actually exists. Returns that deepest valid absolute directory
     * (cur itself when input is empty or nothing matches). Pure: no I/O
     * beyond the supplied listDirs callback, so it is unit-testable. */
    static QString resolveJumpTarget(
        const QString    &cur,
        const QString    &input,
        const ListDirsFn &listDirs,
        const QString    &homePath = QString());

private:
    QString    m_title;
    QString    m_startPath;
    QString    m_homePath;
    ListDirsFn m_listDirs;
};

#endif // QTUIPATHPICKER_H
