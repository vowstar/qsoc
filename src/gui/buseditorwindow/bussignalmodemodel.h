// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#ifndef BUSSIGNALMODEMODEL_H
#define BUSSIGNALMODEMODEL_H

#include "common/qsocbusmanager.h"

#include <QAbstractTableModel>
#include <QList>

class BusSignalModeModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column {
        SignalColumn,
        ModeColumn,
        DirectionColumn,
        WidthColumn,
        QualifierColumn,
        DescriptionColumn,
        ColumnCount
    };

    explicit BusSignalModeModel(QObject *parent = nullptr);

    int           rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int           columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant      data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant      headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

    void              clear();
    void              setDefinition(const QSocBusDefinition &definition);
    QSocBusDefinition definition() const;
    bool              isDirty() const;
    void              setDirty(bool dirty);

    void addRow();
    void duplicateRows(const QList<int> &rows);
    void removeRowIndices(const QList<int> &rows);

signals:
    void dirtyChanged(bool dirty);

private:
    QSocBusDefinition m_definition;
    bool              m_dirty = false;
};

#endif // BUSSIGNALMODEMODEL_H
