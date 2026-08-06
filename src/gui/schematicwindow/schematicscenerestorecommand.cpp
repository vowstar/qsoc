// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "schematicscenerestorecommand.h"

#include <utility>

SchematicSceneRestoreCommand::SchematicSceneRestoreCommand(
    const QPointer<QSchematic::Scene> &scene,
    gpds::container                    before,
    QStringList                        filesBefore,
    QStringList                        filesAfter,
    BeginRestore                       beginRestore,
    EndRestore                         endRestore,
    const QString                     &text)
    : QSchematic::Commands::SceneRestore(scene, std::move(before), text)
    , m_filesBefore(std::move(filesBefore))
    , m_filesAfter(std::move(filesAfter))
    , m_beginRestore(std::move(beginRestore))
    , m_endRestore(std::move(endRestore))
{}

void SchematicSceneRestoreCommand::undo()
{
    if (m_beginRestore) {
        m_beginRestore();
    }
    QSchematic::Commands::SceneRestore::undo();
    if (m_endRestore) {
        m_endRestore(m_filesBefore);
    }
}

void SchematicSceneRestoreCommand::redo()
{
    /* QUndoStack::push calls redo() immediately, but the operation has
       already run by then; the base class skips it, so the hooks must too. */
    if (m_firstRedo) {
        m_firstRedo = false;
        QSchematic::Commands::SceneRestore::redo();
        return;
    }
    if (m_beginRestore) {
        m_beginRestore();
    }
    QSchematic::Commands::SceneRestore::redo();
    if (m_endRestore) {
        m_endRestore(m_filesAfter);
    }
}
