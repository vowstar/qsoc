// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#include "bussignalmodemodel.h"

#include <algorithm>
#include <functional>
#include <utility>
#include <QSet>

BusSignalModeModel::BusSignalModeModel(QObject *parent)
    : QAbstractTableModel(parent)
{}

int BusSignalModeModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_definition.rows.size();
}

int BusSignalModeModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant BusSignalModeModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_definition.rows.size())
        return QVariant();

    if (role != Qt::DisplayRole && role != Qt::EditRole)
        return QVariant();

    const QSocBusSignalMode &row = m_definition.rows.at(index.row());
    switch (index.column()) {
    case SignalColumn:
        return row.signal;
    case ModeColumn:
        return row.mode;
    case DirectionColumn:
        return row.direction;
    case WidthColumn:
        return row.width;
    case QualifierColumn:
        return row.qualifier;
    case DescriptionColumn:
        return row.description;
    default:
        return QVariant();
    }
}

QVariant BusSignalModeModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();

    switch (section) {
    case SignalColumn:
        return tr("Signal");
    case ModeColumn:
        return tr("Mode");
    case DirectionColumn:
        return tr("Direction");
    case WidthColumn:
        return tr("Width");
    case QualifierColumn:
        return tr("Qualifier");
    case DescriptionColumn:
        return tr("Description");
    default:
        return QVariant();
    }
}

Qt::ItemFlags BusSignalModeModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return QAbstractTableModel::flags(index) | Qt::ItemIsEditable;
}

bool BusSignalModeModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role != Qt::EditRole || !index.isValid() || index.row() < 0
        || index.row() >= m_definition.rows.size()) {
        return false;
    }

    QSocBusSignalMode &row    = m_definition.rows[index.row()];
    const QString      text   = value.toString().trimmed();
    QString           *target = nullptr;

    switch (index.column()) {
    case SignalColumn:
        target = &row.signal;
        break;
    case ModeColumn:
        target = &row.mode;
        break;
    case DirectionColumn:
        target = &row.direction;
        break;
    case WidthColumn:
        target = &row.width;
        break;
    case QualifierColumn:
        target = &row.qualifier;
        break;
    case DescriptionColumn:
        target = &row.description;
        break;
    default:
        return false;
    }

    if (*target == text)
        return false;

    *target = text;
    emit dataChanged(index, index, {Qt::DisplayRole, Qt::EditRole});
    setDirty(true);
    return true;
}

void BusSignalModeModel::clear()
{
    setDefinition(QSocBusDefinition());
}

void BusSignalModeModel::setDefinition(const QSocBusDefinition &definition)
{
    beginResetModel();
    m_definition = definition;
    endResetModel();
    setDirty(false);
}

QSocBusDefinition BusSignalModeModel::definition() const
{
    return m_definition;
}

bool BusSignalModeModel::isDirty() const
{
    return m_dirty;
}

void BusSignalModeModel::setDirty(bool dirty)
{
    if (m_dirty == dirty)
        return;
    m_dirty = dirty;
    emit dirtyChanged(m_dirty);
}

void BusSignalModeModel::addRow()
{
    const int row = m_definition.rows.size();
    beginInsertRows(QModelIndex(), row, row);
    QSocBusSignalMode item;
    item.direction = QStringLiteral("in");
    item.width     = QStringLiteral("1");
    m_definition.rows.append(item);
    endInsertRows();
    setDirty(true);
}

void BusSignalModeModel::duplicateRows(const QList<int> &rows)
{
    QSet<int>  seen;
    QList<int> uniqueRows;
    for (int row : rows) {
        if (row < 0 || row >= m_definition.rows.size() || seen.contains(row))
            continue;
        seen.insert(row);
        uniqueRows.append(row);
    }
    if (uniqueRows.isEmpty())
        return;

    std::sort(uniqueRows.begin(), uniqueRows.end());
    const int first = m_definition.rows.size();
    const int last  = first + uniqueRows.size() - 1;
    beginInsertRows(QModelIndex(), first, last);
    for (int row : std::as_const(uniqueRows))
        m_definition.rows.append(m_definition.rows.at(row));
    endInsertRows();
    setDirty(true);
}

void BusSignalModeModel::removeRowIndices(const QList<int> &rows)
{
    QSet<int>  seen;
    QList<int> uniqueRows;
    for (int row : rows) {
        if (row < 0 || row >= m_definition.rows.size() || seen.contains(row))
            continue;
        seen.insert(row);
        uniqueRows.append(row);
    }
    if (uniqueRows.isEmpty())
        return;

    std::sort(uniqueRows.begin(), uniqueRows.end(), std::greater<int>());
    for (int row : std::as_const(uniqueRows)) {
        beginRemoveRows(QModelIndex(), row, row);
        m_definition.rows.removeAt(row);
        endRemoveRows();
    }
    setDirty(true);
}
