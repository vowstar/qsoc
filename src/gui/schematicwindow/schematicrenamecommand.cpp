// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "instance_rename.h"
#include "gui/schematicwindow/modulelibrary/socmoduleitem.h"

using namespace SchematicCommands;

InstanceRename::InstanceRename(
    const std::shared_ptr<ModuleLibrary::SocModuleItem> &item,
    const QString                                       &newName,
    QUndoCommand                                        *parent)
    : QUndoCommand(parent)
    , m_item(item)
    , m_oldName(item ? item->instanceName() : QString())
    , m_newName(newName)
{
    setText(QStringLiteral("Rename instance"));
}

void InstanceRename::undo()
{
    if (m_item) {
        m_item->setInstanceName(m_oldName);
    }
}

void InstanceRename::redo()
{
    if (m_item) {
        m_item->setInstanceName(m_newName);
    }
}
