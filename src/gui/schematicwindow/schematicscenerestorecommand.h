// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef SCHEMATICSCENERESTORECOMMAND_H
#define SCHEMATICSCENERESTORECOMMAND_H

#include <functional>

#include <qschematic/commands/scene_restore.hpp>

#include <QPointer>
#include <QString>
#include <QStringList>

/**
 * @brief One-step undo for bulk scene edits.
 * @details Extends the QSchematic scene restore command with the editor
 *          state that belongs to the same step: the imported file list Auto
 *          Arrange replays, and the window hooks that suppress reactive
 *          slots while the scene is swapped underneath them.
 */
class SchematicSceneRestoreCommand : public QSchematic::Commands::SceneRestore
{
public:
    using BeginRestore = std::function<void()>;
    using EndRestore   = std::function<void(const QStringList &)>;

    /**
     * @brief Constructor.
     * @param scene Scene the command restores.
     * @param before Scene content captured before the operation.
     * @param filesBefore Imported file list before the operation.
     * @param filesAfter Imported file list after the operation.
     * @param beginRestore Runs before every restore.
     * @param endRestore Runs after every restore with the file list.
     * @param text Label shown in the undo history.
     */
    SchematicSceneRestoreCommand(
        const QPointer<QSchematic::Scene> &scene,
        gpds::container                    before,
        QStringList                        filesBefore,
        QStringList                        filesAfter,
        BeginRestore                       beginRestore,
        EndRestore                         endRestore,
        const QString                     &text);

    void undo() override;
    void redo() override;

private:
    QStringList  m_filesBefore;
    QStringList  m_filesAfter;
    BeginRestore m_beginRestore;
    EndRestore   m_endRestore;
    bool         m_firstRedo = true;
};

#endif // SCHEMATICSCENERESTORECOMMAND_H
