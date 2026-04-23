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
 * level. ESC cancels and returns an empty QString.
 */
class QTuiPathPicker
{
public:
    using ListDirsFn = std::function<QStringList(const QString &path)>;

    void setTitle(const QString &title);
    void setStartPath(const QString &path);
    void setListDirs(ListDirsFn listDirs);

    /* Blocking interactive loop. Returns the chosen absolute path,
     * or empty QString on cancel. */
    QString exec();

private:
    QString    m_title;
    QString    m_startPath;
    ListDirsFn m_listDirs;
};

#endif // QTUIPATHPICKER_H
