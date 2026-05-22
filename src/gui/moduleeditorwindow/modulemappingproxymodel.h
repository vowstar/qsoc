// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef MODULEMAPPINGPROXYMODEL_H
#define MODULEMAPPINGPROXYMODEL_H

#include <QSortFilterProxyModel>

class ModuleMappingProxyModel : public QSortFilterProxyModel
{
public:
    explicit ModuleMappingProxyModel(QObject *parent = nullptr);

    bool showOnlyProblems() const;
    void setShowOnlyProblems(bool enabled);

    bool showEmptyMappings() const;
    void setShowEmptyMappings(bool enabled);

protected:
    bool filterAcceptsRow(int sourceRow, const QModelIndex &sourceParent) const override;

private:
    bool m_showOnlyProblems  = false;
    bool m_showEmptyMappings = false;
};

#endif // MODULEMAPPINGPROXYMODEL_H
