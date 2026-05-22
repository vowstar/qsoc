// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "modulemappingproxymodel.h"

#include "modulebusmappingmodel.h"

ModuleMappingProxyModel::ModuleMappingProxyModel(QObject *parent)
    : QSortFilterProxyModel(parent)
{}

bool ModuleMappingProxyModel::showOnlyProblems() const
{
    return m_showOnlyProblems;
}

void ModuleMappingProxyModel::setShowOnlyProblems(bool enabled)
{
    if (m_showOnlyProblems == enabled)
        return;
    beginFilterChange();
    m_showOnlyProblems = enabled;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

bool ModuleMappingProxyModel::showEmptyMappings() const
{
    return m_showEmptyMappings;
}

void ModuleMappingProxyModel::setShowEmptyMappings(bool enabled)
{
    if (m_showEmptyMappings == enabled)
        return;
    beginFilterChange();
    m_showEmptyMappings = enabled;
    endFilterChange(QSortFilterProxyModel::Direction::Rows);
}

bool ModuleMappingProxyModel::filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const
{
    if (!QSortFilterProxyModel::filterAcceptsRow(sourceRow, sourceParent))
        return false;

    const auto *mappingModel = qobject_cast<const ModuleBusMappingModel *>(sourceModel());
    if (!mappingModel)
        return true;

    if (m_showOnlyProblems && !mappingModel->rowHasProblem(sourceRow))
        return false;
    if (m_showEmptyMappings && !mappingModel->rowHasEmptyMapping(sourceRow))
        return false;
    return true;
}
