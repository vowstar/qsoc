// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef INSTANCE_RENAME_H
#define INSTANCE_RENAME_H

#include <memory>
#include <QUndoCommand>

namespace ModuleLibrary {
class SocModuleItem;
}

namespace SchematicCommands {

/**
 * @brief Undo command for renaming module instance
 */
class InstanceRename : public QUndoCommand
{
public:
    /**
     * @brief Constructor
     * @param item Module item to rename
     * @param newName New instance name
     * @param parent Parent undo command
     */
    InstanceRename(
        const std::shared_ptr<ModuleLibrary::SocModuleItem> &item,
        const QString                                       &newName,
        QUndoCommand                                        *parent = nullptr);

    void undo() override;
    void redo() override;

private:
    std::shared_ptr<ModuleLibrary::SocModuleItem> m_item;
    QString                                       m_oldName;
    QString                                       m_newName;
};

} // namespace SchematicCommands

#endif // INSTANCE_RENAME_H
