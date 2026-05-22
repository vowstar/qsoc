// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "moduleparametermodel.h"

#include <algorithm>
#include <functional>
#include <QSet>

ModuleParameterModel::ModuleParameterModel(QObject *parent)
    : QAbstractTableModel(parent)
{}

int ModuleParameterModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_parameters.size();
}

int ModuleParameterModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant ModuleParameterModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_parameters.size())
        return QVariant();
    if (role != Qt::DisplayRole && role != Qt::EditRole)
        return QVariant();

    const QSocModuleParameter &parameter = m_parameters.at(index.row());
    switch (index.column()) {
    case NameColumn:
        return parameter.name;
    case TypeColumn:
        return parameter.type;
    case ValueColumn:
        return parameter.value;
    case DescriptionColumn:
        return parameter.description;
    case ProblemColumn:
        return problemText(index.row());
    default:
        return QVariant();
    }
}

QVariant ModuleParameterModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();

    switch (section) {
    case NameColumn:
        return tr("Name");
    case TypeColumn:
        return tr("Type");
    case ValueColumn:
        return tr("Value");
    case DescriptionColumn:
        return tr("Description");
    case ProblemColumn:
        return tr("Problem");
    default:
        return QVariant();
    }
}

Qt::ItemFlags ModuleParameterModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    Qt::ItemFlags flags = QAbstractTableModel::flags(index);
    if (index.column() == ProblemColumn)
        return flags;
    return flags | Qt::ItemIsEditable;
}

bool ModuleParameterModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role != Qt::EditRole || !index.isValid() || index.row() < 0
        || index.row() >= m_parameters.size() || index.column() == ProblemColumn) {
        return false;
    }

    QSocModuleParameter &parameter = m_parameters[index.row()];
    const QString        text      = value.toString().trimmed();
    QString             *target    = nullptr;
    switch (index.column()) {
    case NameColumn:
        target = &parameter.name;
        break;
    case TypeColumn:
        target = &parameter.type;
        break;
    case ValueColumn:
        target = &parameter.value;
        break;
    case DescriptionColumn:
        target = &parameter.description;
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

void ModuleParameterModel::clear()
{
    setParameters({});
}

void ModuleParameterModel::setParameters(const QList<QSocModuleParameter> &parameters)
{
    beginResetModel();
    m_parameters = parameters;
    endResetModel();
    setDirty(false);
}

QList<QSocModuleParameter> ModuleParameterModel::parameters() const
{
    return m_parameters;
}

bool ModuleParameterModel::isDirty() const
{
    return m_dirty;
}

void ModuleParameterModel::setDirty(bool dirty)
{
    if (m_dirty == dirty)
        return;
    m_dirty = dirty;
    emit dirtyChanged(m_dirty);
}

void ModuleParameterModel::addRow()
{
    const int row = m_parameters.size();
    beginInsertRows(QModelIndex(), row, row);
    m_parameters.append(QSocModuleParameter());
    endInsertRows();
    setDirty(true);
}

void ModuleParameterModel::duplicateRows(const QList<int> &rows)
{
    QSet<int>  seen;
    QList<int> uniqueRows;
    for (int row : rows) {
        if (row < 0 || row >= m_parameters.size() || seen.contains(row))
            continue;
        seen.insert(row);
        uniqueRows.append(row);
    }
    if (uniqueRows.isEmpty())
        return;

    std::sort(uniqueRows.begin(), uniqueRows.end());
    const int first = m_parameters.size();
    const int last  = first + uniqueRows.size() - 1;
    beginInsertRows(QModelIndex(), first, last);
    for (int row : std::as_const(uniqueRows))
        m_parameters.append(m_parameters.at(row));
    endInsertRows();
    setDirty(true);
}

void ModuleParameterModel::removeRowIndices(const QList<int> &rows)
{
    QSet<int>  seen;
    QList<int> uniqueRows;
    for (int row : rows) {
        if (row < 0 || row >= m_parameters.size() || seen.contains(row))
            continue;
        seen.insert(row);
        uniqueRows.append(row);
    }
    if (uniqueRows.isEmpty())
        return;

    std::sort(uniqueRows.begin(), uniqueRows.end(), std::greater<int>());
    for (int row : std::as_const(uniqueRows)) {
        beginRemoveRows(QModelIndex(), row, row);
        m_parameters.removeAt(row);
        endRemoveRows();
    }
    setDirty(true);
}

QString ModuleParameterModel::problemText(int row) const
{
    if (row < 0 || row >= m_parameters.size())
        return {};

    const QSocModuleParameter &parameter = m_parameters.at(row);
    if (parameter.name.isEmpty())
        return tr("Missing name");

    int sameNameCount = 0;
    for (const QSocModuleParameter &candidate : m_parameters) {
        if (candidate.name == parameter.name)
            ++sameNameCount;
    }
    if (sameNameCount > 1)
        return tr("Duplicate name");
    return {};
}

void ModuleParameterModel::emitRowChanged(int row)
{
    if (row < 0 || row >= m_parameters.size())
        return;
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1), {});
}
