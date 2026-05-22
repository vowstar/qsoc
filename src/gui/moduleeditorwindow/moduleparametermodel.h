// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef MODULEPARAMETERMODEL_H
#define MODULEPARAMETERMODEL_H

#include "common/qsocmodulemanager.h"

#include <QAbstractTableModel>

class ModuleParameterModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column {
        NameColumn,
        TypeColumn,
        ValueColumn,
        DescriptionColumn,
        ProblemColumn,
        ColumnCount
    };

    explicit ModuleParameterModel(QObject *parent = nullptr);

    int           rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int           columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant      data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant      headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

    void                       clear();
    void                       setParameters(const QList<QSocModuleParameter> &parameters);
    QList<QSocModuleParameter> parameters() const;
    bool                       isDirty() const;
    void                       setDirty(bool dirty);

    void addRow();
    void duplicateRows(const QList<int> &rows);
    void removeRowIndices(const QList<int> &rows);

signals:
    void dirtyChanged(bool dirty);

private:
    QString problemText(int row) const;
    void    emitRowChanged(int row);

    QList<QSocModuleParameter> m_parameters;
    bool                       m_dirty = false;
};

#endif // MODULEPARAMETERMODEL_H
