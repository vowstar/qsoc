// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "modulebusmappingmodel.h"

#include <algorithm>
#include <functional>
#include <QMap>
#include <QSet>

namespace {

QString normalizedName(const QString &name)
{
    QString out;
    out.reserve(name.size());
    for (const QChar ch : name) {
        if (ch.isLetterOrNumber())
            out.append(ch.toLower());
    }
    return out;
}

bool directionsConflict(const QString &expected, const QString &actual)
{
    if (expected.isEmpty() || actual.isEmpty())
        return false;
    return expected.compare(actual, Qt::CaseInsensitive) != 0;
}

} // namespace

ModuleBusMappingModel::ModuleBusMappingModel(QObject *parent)
    : QAbstractTableModel(parent)
{}

int ModuleBusMappingModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return m_interface.mapping.size();
}

int ModuleBusMappingModel::columnCount(const QModelIndex &parent) const
{
    if (parent.isValid())
        return 0;
    return ColumnCount;
}

QVariant ModuleBusMappingModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_interface.mapping.size())
        return QVariant();
    if (role != Qt::DisplayRole && role != Qt::EditRole)
        return QVariant();

    const QSocModuleBusMapping &mapping  = m_interface.mapping.at(index.row());
    const QSocBusSignalMode     expected = expectedRow(mapping.busSignal);
    const QSocModulePort        actual   = actualPort(mapping.modulePort);

    switch (index.column()) {
    case BusSignalColumn:
        return mapping.busSignal;
    case ExpectedDirectionColumn:
        return expected.direction;
    case ExpectedWidthColumn:
        return expected.width;
    case ExpectedQualifierColumn:
        return expected.qualifier;
    case ModulePortColumn:
        return mapping.modulePort;
    case ActualDirectionColumn:
        return actual.direction;
    case ActualTypeColumn:
        return actual.type;
    case ProblemColumn:
        return problemText(index.row());
    default:
        return QVariant();
    }
}

QVariant ModuleBusMappingModel::headerData(int section, Qt::Orientation orientation, int role) const
{
    if (orientation != Qt::Horizontal || role != Qt::DisplayRole)
        return QVariant();

    switch (section) {
    case BusSignalColumn:
        return tr("Bus Signal");
    case ExpectedDirectionColumn:
        return tr("Expected Direction");
    case ExpectedWidthColumn:
        return tr("Expected Width");
    case ExpectedQualifierColumn:
        return tr("Expected Qualifier");
    case ModulePortColumn:
        return tr("Module Port");
    case ActualDirectionColumn:
        return tr("Actual Direction");
    case ActualTypeColumn:
        return tr("Actual Type");
    case ProblemColumn:
        return tr("Problem");
    default:
        return QVariant();
    }
}

Qt::ItemFlags ModuleBusMappingModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    Qt::ItemFlags flags = QAbstractTableModel::flags(index);
    if (index.column() == BusSignalColumn || index.column() == ModulePortColumn)
        return flags | Qt::ItemIsEditable;
    return flags;
}

bool ModuleBusMappingModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role != Qt::EditRole || !index.isValid() || index.row() < 0
        || index.row() >= m_interface.mapping.size()) {
        return false;
    }

    QSocModuleBusMapping &mapping = m_interface.mapping[index.row()];
    const QString         text    = value.toString().trimmed();
    QString              *target  = nullptr;
    switch (index.column()) {
    case BusSignalColumn:
        target = &mapping.busSignal;
        break;
    case ModulePortColumn:
        target = &mapping.modulePort;
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

void ModuleBusMappingModel::clear()
{
    setContext({}, {}, {});
}

void ModuleBusMappingModel::setContext(
    const QSocModuleBusInterface &interface,
    const QSocBusDefinition      &busDefinition,
    const QList<QSocModulePort>  &ports)
{
    beginResetModel();
    m_interface     = interface;
    m_busDefinition = busDefinition;
    m_ports         = ports;
    endResetModel();
    setDirty(false);
}

void ModuleBusMappingModel::setInterface(const QSocModuleBusInterface &interface)
{
    beginResetModel();
    m_interface = interface;
    endResetModel();
    setDirty(false);
}

void ModuleBusMappingModel::setBusDefinition(const QSocBusDefinition &busDefinition)
{
    beginResetModel();
    m_busDefinition = busDefinition;
    endResetModel();
}

void ModuleBusMappingModel::setPorts(const QList<QSocModulePort> &ports)
{
    beginResetModel();
    m_ports = ports;
    endResetModel();
}

QSocModuleBusInterface ModuleBusMappingModel::interfaceDefinition() const
{
    return m_interface;
}

bool ModuleBusMappingModel::isDirty() const
{
    return m_dirty;
}

void ModuleBusMappingModel::setDirty(bool dirty)
{
    if (m_dirty == dirty)
        return;
    m_dirty = dirty;
    emit dirtyChanged(m_dirty);
}

void ModuleBusMappingModel::addRow()
{
    const int row = m_interface.mapping.size();
    beginInsertRows(QModelIndex(), row, row);
    m_interface.mapping.append(QSocModuleBusMapping());
    endInsertRows();
    setDirty(true);
}

void ModuleBusMappingModel::duplicateRows(const QList<int> &rows)
{
    QSet<int>  seen;
    QList<int> uniqueRows;
    for (int row : rows) {
        if (row < 0 || row >= m_interface.mapping.size() || seen.contains(row))
            continue;
        seen.insert(row);
        uniqueRows.append(row);
    }
    if (uniqueRows.isEmpty())
        return;

    std::sort(uniqueRows.begin(), uniqueRows.end());
    const int first = m_interface.mapping.size();
    const int last  = first + uniqueRows.size() - 1;
    beginInsertRows(QModelIndex(), first, last);
    for (int row : std::as_const(uniqueRows))
        m_interface.mapping.append(m_interface.mapping.at(row));
    endInsertRows();
    setDirty(true);
}

void ModuleBusMappingModel::removeRowIndices(const QList<int> &rows)
{
    QSet<int>  seen;
    QList<int> uniqueRows;
    for (int row : rows) {
        if (row < 0 || row >= m_interface.mapping.size() || seen.contains(row))
            continue;
        seen.insert(row);
        uniqueRows.append(row);
    }
    if (uniqueRows.isEmpty())
        return;

    std::sort(uniqueRows.begin(), uniqueRows.end(), std::greater<int>());
    for (int row : std::as_const(uniqueRows)) {
        beginRemoveRows(QModelIndex(), row, row);
        m_interface.mapping.removeAt(row);
        endRemoveRows();
    }
    setDirty(true);
}

void ModuleBusMappingModel::rebuildRowsFromBusDefinition(bool markDirty)
{
    QMap<QString, QString> existingTargets;
    for (const QSocModuleBusMapping &mapping : m_interface.mapping)
        existingTargets.insert(mapping.busSignal, mapping.modulePort);

    QList<QSocModuleBusMapping> rebuilt;
    QSet<QString>               generatedSignals;
    for (const QSocBusSignalMode &row : m_busDefinition.rows) {
        if (!m_interface.mode.isEmpty()
            && row.mode.compare(m_interface.mode, Qt::CaseInsensitive) != 0) {
            continue;
        }
        if (row.signal.isEmpty() || generatedSignals.contains(row.signal))
            continue;
        generatedSignals.insert(row.signal);
        rebuilt.append({row.signal, existingTargets.value(row.signal)});
    }

    for (const QSocModuleBusMapping &mapping : std::as_const(m_interface.mapping)) {
        if (!generatedSignals.contains(mapping.busSignal))
            rebuilt.append(mapping);
    }

    beginResetModel();
    m_interface.mapping = rebuilt;
    endResetModel();
    if (markDirty)
        setDirty(true);
}

void ModuleBusMappingModel::autoMatchByName()
{
    bool changed = false;
    for (QSocModuleBusMapping &mapping : m_interface.mapping) {
        if (!mapping.modulePort.isEmpty())
            continue;
        const QString matched = matchPortForSignal(mapping.busSignal, false);
        if (matched.isEmpty())
            continue;
        mapping.modulePort = matched;
        changed            = true;
    }
    if (!changed)
        return;
    emit dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1), {});
    setDirty(true);
}

void ModuleBusMappingModel::autoMatchByInterfacePrefix()
{
    bool changed = false;
    for (QSocModuleBusMapping &mapping : m_interface.mapping) {
        if (!mapping.modulePort.isEmpty())
            continue;
        const QString matched = matchPortForSignal(mapping.busSignal, true);
        if (matched.isEmpty())
            continue;
        mapping.modulePort = matched;
        changed            = true;
    }
    if (!changed)
        return;
    emit dataChanged(index(0, 0), index(rowCount() - 1, ColumnCount - 1), {});
    setDirty(true);
}

bool ModuleBusMappingModel::rowHasProblem(int row) const
{
    return !problemText(row).isEmpty();
}

bool ModuleBusMappingModel::rowHasEmptyMapping(int row) const
{
    if (row < 0 || row >= m_interface.mapping.size())
        return false;
    return m_interface.mapping.at(row).modulePort.isEmpty();
}

bool ModuleBusMappingModel::rowHasUnknownBusSignal(int row) const
{
    if (row < 0 || row >= m_interface.mapping.size())
        return false;
    const QSocModuleBusMapping &mapping = m_interface.mapping.at(row);
    return mapping.busSignal.isEmpty() || expectedRow(mapping.busSignal).signal.isEmpty();
}

void ModuleBusMappingModel::removeUnknownBusSignalRows()
{
    QList<int> rows;
    for (int row = 0; row < m_interface.mapping.size(); ++row) {
        if (rowHasUnknownBusSignal(row))
            rows.append(row);
    }
    removeRowIndices(rows);
}

QSocBusSignalMode ModuleBusMappingModel::expectedRow(const QString &busSignal) const
{
    for (const QSocBusSignalMode &row : m_busDefinition.rows) {
        if (row.signal != busSignal)
            continue;
        if (m_interface.mode.isEmpty()
            || row.mode.compare(m_interface.mode, Qt::CaseInsensitive) == 0) {
            return row;
        }
    }
    return {};
}

QSocModulePort ModuleBusMappingModel::actualPort(const QString &modulePort) const
{
    for (const QSocModulePort &port : m_ports) {
        if (port.name == modulePort)
            return port;
    }
    return {};
}

QString ModuleBusMappingModel::problemText(int row) const
{
    if (row < 0 || row >= m_interface.mapping.size())
        return {};

    const QSocModuleBusMapping &mapping = m_interface.mapping.at(row);
    if (mapping.busSignal.isEmpty())
        return tr("Missing bus signal");

    const QSocBusSignalMode expected = expectedRow(mapping.busSignal);
    if (expected.signal.isEmpty())
        return tr("Unknown bus signal");

    if (mapping.modulePort.isEmpty())
        return tr("Empty mapping");

    const QSocModulePort actual = actualPort(mapping.modulePort);
    if (actual.name.isEmpty())
        return tr("Unknown module port");

    if (directionsConflict(expected.direction, actual.direction))
        return tr("Direction mismatch");
    return {};
}

QString ModuleBusMappingModel::matchPortForSignal(
    const QString &busSignal, bool useInterfacePrefix) const
{
    const QString normalizedSignal = normalizedName(busSignal);
    const QString normalizedPrefix = normalizedName(m_interface.name);

    for (const QSocModulePort &port : m_ports) {
        if (normalizedName(port.name) == normalizedSignal)
            return port.name;
    }

    if (!useInterfacePrefix || normalizedPrefix.isEmpty())
        return {};

    const QString prefixed = normalizedPrefix + normalizedSignal;
    for (const QSocModulePort &port : m_ports) {
        const QString normalizedPort = normalizedName(port.name);
        if (normalizedPort == prefixed || normalizedPort.endsWith(prefixed))
            return port.name;
    }
    return {};
}

void ModuleBusMappingModel::emitRowChanged(int row)
{
    if (row < 0 || row >= m_interface.mapping.size())
        return;
    emit dataChanged(index(row, 0), index(row, ColumnCount - 1), {});
}
