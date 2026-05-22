// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "modulebusinterfacemodel.h"

#include <algorithm>
#include <functional>
#include <QSet>

ModuleBusInterfaceModel::ModuleBusInterfaceModel(QObject *parent)
    : QAbstractTableModel(parent)
{}

int ModuleBusInterfaceModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_interfaces.size();
}

int ModuleBusInterfaceModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant ModuleBusInterfaceModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_interfaces.size())
        return QVariant();
    if (role != Qt::DisplayRole && role != Qt::EditRole)
        return QVariant();

    const QSocModuleBusInterface &interface = m_interfaces.at(index.row());
    switch (index.column()) {
    case InterfaceColumn:
        return interface.name;
    case BusColumn:
        return interface.busName;
    case ModeColumn:
        return interface.mode;
    case MappingCountColumn:
        return interface.mapping.size();
    case EmptyCountColumn:
        return emptyMappingCount(interface);
    case StatusColumn:
        return statusText(index.row());
    default:
        return QVariant();
    }
}

QVariant ModuleBusInterfaceModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();

    switch (section) {
    case InterfaceColumn:
        return tr("Interface");
    case BusColumn:
        return tr("Bus");
    case ModeColumn:
        return tr("Mode");
    case MappingCountColumn:
        return tr("Mappings");
    case EmptyCountColumn:
        return tr("Empty");
    case StatusColumn:
        return tr("Status");
    default:
        return QVariant();
    }
}

Qt::ItemFlags ModuleBusInterfaceModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    Qt::ItemFlags flags = QAbstractTableModel::flags(index);
    if (index.column() >= MappingCountColumn)
        return flags;
    return flags | Qt::ItemIsEditable;
}

bool ModuleBusInterfaceModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role != Qt::EditRole || !index.isValid() || index.row() < 0
        || index.row() >= m_interfaces.size() || index.column() >= MappingCountColumn) {
        return false;
    }

    QSocModuleBusInterface &interface = m_interfaces[index.row()];
    const QString           text      = value.toString().trimmed();
    QString                *target    = nullptr;
    switch (index.column()) {
    case InterfaceColumn:
        target = &interface.name;
        break;
    case BusColumn:
        target = &interface.busName;
        break;
    case ModeColumn:
        target = &interface.mode;
        break;
    default:
        return false;
    }

    if (*target == text)
        return false;

    *target = text;
    emitRowChanged(index.row());
    setDirty(true);
    return true;
}

void ModuleBusInterfaceModel::clear()
{
    setBusInterfaces({});
}

void ModuleBusInterfaceModel::setBusInterfaces(const QList<QSocModuleBusInterface> &interfaces)
{
    beginResetModel();
    m_interfaces = interfaces;
    endResetModel();
    setDirty(false);
}

QList<QSocModuleBusInterface> ModuleBusInterfaceModel::busInterfaces() const
{
    return m_interfaces;
}

QSocModuleBusInterface ModuleBusInterfaceModel::interfaceAt(int row) const
{
    if (row < 0 || row >= m_interfaces.size())
        return {};
    return m_interfaces.at(row);
}

bool ModuleBusInterfaceModel::setInterfaceAt(int row, const QSocModuleBusInterface &interface)
{
    if (row < 0 || row >= m_interfaces.size())
        return false;
    m_interfaces[row] = interface;
    emitRowChanged(row);
    setDirty(true);
    return true;
}

bool ModuleBusInterfaceModel::isDirty() const
{
    return m_dirty;
}

void ModuleBusInterfaceModel::setDirty(bool dirty)
{
    if (m_dirty == dirty)
        return;
    m_dirty = dirty;
    emit dirtyChanged(m_dirty);
}

void ModuleBusInterfaceModel::addInterface(const QString &name, const QString &busName)
{
    QSocModuleBusInterface interface;
    interface.name    = name;
    interface.busName = busName;
    interface.mode    = QStringLiteral("master");
    addInterface(interface);
}

void ModuleBusInterfaceModel::addInterface(const QSocModuleBusInterface &interface)
{
    const int row = m_interfaces.size();
    beginInsertRows(QModelIndex(), row, row);
    m_interfaces.append(interface);
    endInsertRows();
    setDirty(true);
}

void ModuleBusInterfaceModel::duplicateRows(const QList<int> &rows)
{
    QSet<int>  seen;
    QList<int> uniqueRows;
    for (int row : rows) {
        if (row < 0 || row >= m_interfaces.size() || seen.contains(row))
            continue;
        seen.insert(row);
        uniqueRows.append(row);
    }
    if (uniqueRows.isEmpty())
        return;

    std::sort(uniqueRows.begin(), uniqueRows.end());
    const int first = m_interfaces.size();
    const int last  = first + uniqueRows.size() - 1;
    beginInsertRows(QModelIndex(), first, last);
    for (int row : std::as_const(uniqueRows))
        m_interfaces.append(m_interfaces.at(row));
    endInsertRows();
    setDirty(true);
}

void ModuleBusInterfaceModel::removeRowIndices(const QList<int> &rows)
{
    QSet<int>  seen;
    QList<int> uniqueRows;
    for (int row : rows) {
        if (row < 0 || row >= m_interfaces.size() || seen.contains(row))
            continue;
        seen.insert(row);
        uniqueRows.append(row);
    }
    if (uniqueRows.isEmpty())
        return;

    std::sort(uniqueRows.begin(), uniqueRows.end(), std::greater<int>());
    for (int row : std::as_const(uniqueRows)) {
        beginRemoveRows(QModelIndex(), row, row);
        m_interfaces.removeAt(row);
        endRemoveRows();
    }
    setDirty(true);
}

QString ModuleBusInterfaceModel::statusText(int row) const
{
    if (row < 0 || row >= m_interfaces.size())
        return {};

    const QSocModuleBusInterface &interface = m_interfaces.at(row);
    if (interface.name.isEmpty())
        return tr("Missing interface");
    if (interface.busName.isEmpty())
        return tr("Missing bus");
    if (interface.mode.isEmpty())
        return tr("Missing mode");

    int sameNameCount = 0;
    for (const QSocModuleBusInterface &candidate : m_interfaces) {
        if (candidate.name == interface.name)
            ++sameNameCount;
    }
    if (sameNameCount > 1)
        return tr("Duplicate interface");
    if (emptyMappingCount(interface) > 0)
        return tr("Incomplete mapping");
    return tr("OK");
}

int ModuleBusInterfaceModel::emptyMappingCount(const QSocModuleBusInterface &interface) const
{
    int count = 0;
    for (const QSocModuleBusMapping &mapping : interface.mapping) {
        if (mapping.modulePort.trimmed().isEmpty())
            ++count;
    }
    return count;
}

void ModuleBusInterfaceModel::emitRowChanged(int row)
{
    if (row < 0 || row >= m_interfaces.size())
        return;
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1), {});
}
