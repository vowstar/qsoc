// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "moduleportmodel.h"

#include <algorithm>
#include <functional>
#include <QSet>

ModulePortModel::ModulePortModel(QObject *parent)
    : QAbstractTableModel(parent)
{}

int ModulePortModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_ports.size();
}

int ModulePortModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant ModulePortModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_ports.size())
        return QVariant();

    const QSocModulePort &port = m_ports.at(index.row());
    if (index.column() == VisibleColumn && role == Qt::CheckStateRole)
        return port.visible ? Qt::Checked : Qt::Unchecked;

    if (role != Qt::DisplayRole && role != Qt::EditRole)
        return QVariant();

    switch (index.column()) {
    case NameColumn:
        return port.name;
    case DirectionColumn:
        return port.direction;
    case TypeColumn:
        return port.type;
    case VisibleColumn:
        return port.visible;
    case GroupColumn:
        return port.group;
    case DescriptionColumn:
        return port.description;
    case ProblemColumn:
        return problemText(index.row());
    default:
        return QVariant();
    }
}

QVariant ModulePortModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();

    switch (section) {
    case NameColumn:
        return tr("Name");
    case DirectionColumn:
        return tr("Direction");
    case TypeColumn:
        return tr("Type");
    case VisibleColumn:
        return tr("Visible");
    case GroupColumn:
        return tr("Group");
    case DescriptionColumn:
        return tr("Description");
    case ProblemColumn:
        return tr("Problem");
    default:
        return QVariant();
    }
}

Qt::ItemFlags ModulePortModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    Qt::ItemFlags flags = QAbstractTableModel::flags(index);
    if (index.column() == ProblemColumn)
        return flags;
    if (index.column() == VisibleColumn)
        return flags | Qt::ItemIsUserCheckable | Qt::ItemIsEditable;
    return flags | Qt::ItemIsEditable;
}

bool ModulePortModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_ports.size())
        return false;

    QSocModulePort &port = m_ports[index.row()];
    if (index.column() == VisibleColumn && role == Qt::CheckStateRole) {
        const bool visible = value.toInt() == Qt::Checked;
        if (port.hasVisible && port.visible == visible)
            return false;
        port.hasVisible = true;
        port.visible    = visible;
        emitRowChanged(index.row());
        setDirty(true);
        return true;
    }

    if (role != Qt::EditRole || index.column() == ProblemColumn)
        return false;

    const QString text   = value.toString().trimmed();
    QString      *target = nullptr;
    switch (index.column()) {
    case NameColumn:
        target = &port.name;
        break;
    case DirectionColumn:
        target = &port.direction;
        break;
    case TypeColumn:
        target = &port.type;
        break;
    case GroupColumn:
        target = &port.group;
        break;
    case DescriptionColumn:
        target = &port.description;
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

void ModulePortModel::clear()
{
    setPorts({});
}

void ModulePortModel::setPorts(const QList<QSocModulePort> &ports)
{
    beginResetModel();
    m_ports = ports;
    endResetModel();
    setDirty(false);
}

QList<QSocModulePort> ModulePortModel::ports() const
{
    return m_ports;
}

bool ModulePortModel::isDirty() const
{
    return m_dirty;
}

void ModulePortModel::setDirty(bool dirty)
{
    if (m_dirty == dirty)
        return;
    m_dirty = dirty;
    emit dirtyChanged(m_dirty);
}

void ModulePortModel::addRow()
{
    const int row = m_ports.size();
    beginInsertRows(QModelIndex(), row, row);
    QSocModulePort port;
    port.direction = QStringLiteral("in");
    port.type      = QStringLiteral("logic");
    m_ports.append(port);
    endInsertRows();
    setDirty(true);
}

void ModulePortModel::duplicateRows(const QList<int> &rows)
{
    QSet<int>  seen;
    QList<int> uniqueRows;
    for (int row : rows) {
        if (row < 0 || row >= m_ports.size() || seen.contains(row))
            continue;
        seen.insert(row);
        uniqueRows.append(row);
    }
    if (uniqueRows.isEmpty())
        return;

    std::sort(uniqueRows.begin(), uniqueRows.end());
    const int first = m_ports.size();
    const int last  = first + uniqueRows.size() - 1;
    beginInsertRows(QModelIndex(), first, last);
    for (int row : std::as_const(uniqueRows))
        m_ports.append(m_ports.at(row));
    endInsertRows();
    setDirty(true);
}

void ModulePortModel::removeRowIndices(const QList<int> &rows)
{
    QSet<int>  seen;
    QList<int> uniqueRows;
    for (int row : rows) {
        if (row < 0 || row >= m_ports.size() || seen.contains(row))
            continue;
        seen.insert(row);
        uniqueRows.append(row);
    }
    if (uniqueRows.isEmpty())
        return;

    std::sort(uniqueRows.begin(), uniqueRows.end(), std::greater<int>());
    for (int row : std::as_const(uniqueRows)) {
        beginRemoveRows(QModelIndex(), row, row);
        m_ports.removeAt(row);
        endRemoveRows();
    }
    setDirty(true);
}

QString ModulePortModel::problemText(int row) const
{
    if (row < 0 || row >= m_ports.size())
        return {};

    const QSocModulePort &port = m_ports.at(row);
    if (port.name.isEmpty())
        return tr("Missing name");

    int sameNameCount = 0;
    for (const QSocModulePort &candidate : m_ports) {
        if (candidate.name == port.name)
            ++sameNameCount;
    }
    if (sameNameCount > 1)
        return tr("Duplicate name");

    const QString direction = port.direction.toLower();
    if (!direction.isEmpty() && direction != QStringLiteral("in")
        && direction != QStringLiteral("out") && direction != QStringLiteral("inout")) {
        return tr("Invalid direction");
    }
    return {};
}

void ModulePortModel::emitRowChanged(int row)
{
    if (row < 0 || row >= m_ports.size())
        return;
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1), {});
}
