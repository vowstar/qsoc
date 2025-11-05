// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef SCHEMATICRENAMECOMMAND_H
#define SCHEMATICRENAMECOMMAND_H

#include <memory>
#include <QUndoCommand>

class SchematicModule;

/**
 * @brief Undo command for renaming module instance
 */
class SchematicRenameCommand : public QUndoCommand
{
public:
    /**
     * @brief Constructor
     * @param item Module item to rename
     * @param newName New instance name
     * @param parent Parent undo command
     */
    SchematicRenameCommand(
        const std::shared_ptr<SchematicModule> &item,
        const QString                          &newName,
        QUndoCommand                           *parent = nullptr);

    void undo() override;
    void redo() override;

private:
    std::shared_ptr<SchematicModule> m_item;
    QString                          m_oldName;
    QString                          m_newName;
};

#endif // SCHEMATICRENAMECOMMAND_H
