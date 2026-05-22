// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef MODULEBUSINTERFACEMODEL_H
#define MODULEBUSINTERFACEMODEL_H

#include "common/qsocmodulemanager.h"

#include <QAbstractTableModel>

class ModuleBusInterfaceModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column {
        InterfaceColumn,
        BusColumn,
        ModeColumn,
        MappingCountColumn,
        EmptyCountColumn,
        StatusColumn,
        ColumnCount
    };

    explicit ModuleBusInterfaceModel(QObject *parent = nullptr);

    int           rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int           columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant      data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant      headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

    void                          clear();
    void                          setBusInterfaces(const QList<QSocModuleBusInterface> &interfaces);
    QList<QSocModuleBusInterface> busInterfaces() const;
    QSocModuleBusInterface        interfaceAt(int row) const;
    bool                          setInterfaceAt(int row, const QSocModuleBusInterface &interface);
    bool                          isDirty() const;
    void                          setDirty(bool dirty);

    void addInterface(const QString &name = QString(), const QString &busName = QString());
    void addInterface(const QSocModuleBusInterface &interface);
    void duplicateRows(const QList<int> &rows);
    void removeRowIndices(const QList<int> &rows);

signals:
    void dirtyChanged(bool dirty);

private:
    QString statusText(int row) const;
    int     emptyMappingCount(const QSocModuleBusInterface &interface) const;
    void    emitRowChanged(int row);

    QList<QSocModuleBusInterface> m_interfaces;
    bool                          m_dirty = false;
};

#endif // MODULEBUSINTERFACEMODEL_H
