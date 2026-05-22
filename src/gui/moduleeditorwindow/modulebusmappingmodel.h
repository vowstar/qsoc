// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef MODULEBUSMAPPINGMODEL_H
#define MODULEBUSMAPPINGMODEL_H

#include "common/qsocbusmanager.h"
#include "common/qsocmodulemanager.h"

#include <QAbstractTableModel>

class ModuleBusMappingModel : public QAbstractTableModel
{
    Q_OBJECT

public:
    enum Column {
        BusSignalColumn,
        ExpectedDirectionColumn,
        ExpectedWidthColumn,
        ExpectedQualifierColumn,
        ModulePortColumn,
        ActualDirectionColumn,
        ActualTypeColumn,
        ProblemColumn,
        ColumnCount
    };

    explicit ModuleBusMappingModel(QObject *parent = nullptr);

    int           rowCount(const QModelIndex &parent = QModelIndex()) const override;
    int           columnCount(const QModelIndex &parent = QModelIndex()) const override;
    QVariant      data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    QVariant      headerData(int section, Qt::Orientation orientation, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

    void clear();
    void setContext(
        const QSocModuleBusInterface &interface,
        const QSocBusDefinition      &busDefinition,
        const QList<QSocModulePort>  &ports);
    void                   setInterface(const QSocModuleBusInterface &interface);
    void                   setBusDefinition(const QSocBusDefinition &busDefinition);
    void                   setPorts(const QList<QSocModulePort> &ports);
    QSocModuleBusInterface interfaceDefinition() const;
    bool                   isDirty() const;
    void                   setDirty(bool dirty);

    void addRow();
    void duplicateRows(const QList<int> &rows);
    void removeRowIndices(const QList<int> &rows);
    void rebuildRowsFromBusDefinition(bool markDirty = true);
    void autoMatchByName();
    void autoMatchByInterfacePrefix();
    bool rowHasProblem(int row) const;
    bool rowHasEmptyMapping(int row) const;
    bool rowHasUnknownBusSignal(int row) const;
    void removeUnknownBusSignalRows();

signals:
    void dirtyChanged(bool dirty);

private:
    QSocBusSignalMode expectedRow(const QString &busSignal) const;
    QSocModulePort    actualPort(const QString &modulePort) const;
    QString           problemText(int row) const;
    QString           matchPortForSignal(const QString &busSignal, bool useInterfacePrefix) const;
    void              emitRowChanged(int row);

    QSocModuleBusInterface m_interface;
    QSocBusDefinition      m_busDefinition;
    QList<QSocModulePort>  m_ports;
    bool                   m_dirty = false;
};

#endif // MODULEBUSMAPPINGMODEL_H
