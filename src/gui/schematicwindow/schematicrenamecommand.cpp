// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "schematicrenamecommand.h"
#include "schematicmodule.h"

SchematicRenameCommand::SchematicRenameCommand(
    const std::shared_ptr<SchematicModule> &item, const QString &newName, QUndoCommand *parent)
    : QUndoCommand(parent)
    , m_item(item)
    , m_oldName(item ? item->instanceName() : QString())
    , m_newName(newName)
{
    setText(QStringLiteral("Rename instance"));
}

void SchematicRenameCommand::undo()
{
    if (m_item) {
        m_item->setInstanceName(m_oldName);
    }
}

void SchematicRenameCommand::redo()
{
    if (m_item) {
        m_item->setInstanceName(m_newName);
    }
}
