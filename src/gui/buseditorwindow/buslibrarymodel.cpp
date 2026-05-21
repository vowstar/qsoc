// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#include "buslibrarymodel.h"

#include <QDir>
#include <QFileInfo>

BusLibraryModel::BusLibraryModel(QObject *parent)
    : QStandardItemModel(parent)
{
    setHorizontalHeaderLabels({tr("Library"), tr("Enabled"), tr("Path"), tr("Buses"), tr("Status")});
}

void BusLibraryModel::setBusManager(QSocBusManager *manager)
{
    clear();
    setHorizontalHeaderLabels({tr("Library"), tr("Enabled"), tr("Path"), tr("Buses"), tr("Status")});

    QString busPath;
    QString projectPath;
    if (manager && manager->getProjectManager()) {
        busPath     = manager->getProjectManager()->getBusPath();
        projectPath = manager->getProjectManager()->getProjectPath();
    }

    const QString rootPath = projectPath.isEmpty() ? busPath
                                                   : QDir(projectPath).relativeFilePath(busPath);
    appendRow(
        makeRow(tr("Project Buses"), RootNode, {}, {}, rootPath, {}, tr("Loaded"), true, false));
    QStandardItem *root = item(0, LibraryColumn);

    if (!manager)
        return;

    const QStringList libraries = manager->listLoadedLibraries();
    root->setText(tr("Project Buses (%1)").arg(libraries.size()));
    for (const QString &libraryName : libraries) {
        const QStringList buses = manager->listBusesInLibrary(libraryName);
        const QString filePath  = QDir(busPath).filePath(libraryName + QStringLiteral(".soc_bus"));
        const QString displayPath = projectPath.isEmpty()
                                        ? filePath
                                        : QDir(projectPath).relativeFilePath(filePath);
        QString       status      = tr("Loaded");
        if (!QFileInfo::exists(filePath)) {
            status = buses.isEmpty() ? tr("Pending") : tr("Missing");
        } else if (!QFileInfo(filePath).isWritable()) {
            status = tr("Read-only");
        }

        root->appendRow(makeRow(
            libraryName,
            LibraryNode,
            libraryName,
            {},
            displayPath,
            QString::number(buses.size()),
            status,
            true,
            true));
        QStandardItem *libraryItem = root->child(root->rowCount() - 1, LibraryColumn);

        for (const QString &busName : buses) {
            const int rowCount = manager->getBusDefinition(libraryName, busName).rows.size();
            libraryItem->appendRow(makeRow(
                busName,
                BusNode,
                libraryName,
                busName,
                {},
                QString::number(rowCount),
                tr("Loaded"),
                true,
                false));
        }
    }
}

QModelIndex BusLibraryModel::indexForBus(const QString &libraryName, const QString &busName) const
{
    if (rowCount() == 0)
        return QModelIndex();

    QStandardItem *root = item(0, 0);
    if (!root)
        return QModelIndex();

    for (int i = 0; i < root->rowCount(); ++i) {
        QStandardItem *libraryItem = root->child(i, 0);
        if (!libraryItem || libraryItem->data(LibraryNameRole).toString() != libraryName)
            continue;

        if (busName.isEmpty())
            return libraryItem->index();

        for (int j = 0; j < libraryItem->rowCount(); ++j) {
            QStandardItem *busItem = libraryItem->child(j, 0);
            if (busItem && busItem->data(BusNameRole).toString() == busName)
                return busItem->index();
        }
    }

    return QModelIndex();
}

bool BusLibraryModel::setLibraryEnabled(const QString &libraryName, bool enabled)
{
    const QModelIndex libraryIndex = indexForBus(libraryName, {});
    if (!libraryIndex.isValid())
        return false;

    const QModelIndex enabledIndex = index(libraryIndex.row(), EnabledColumn, libraryIndex.parent());
    return setData(enabledIndex, enabled ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
}

BusLibraryModel::NodeType BusLibraryModel::nodeType(const QModelIndex &index)
{
    if (!index.isValid())
        return RootNode;
    return static_cast<NodeType>(roleIndex(index).data(NodeTypeRole).toInt());
}

QString BusLibraryModel::libraryName(const QModelIndex &index)
{
    return roleIndex(index).data(LibraryNameRole).toString();
}

QString BusLibraryModel::busName(const QModelIndex &index)
{
    return roleIndex(index).data(BusNameRole).toString();
}

bool BusLibraryModel::isEnabled(const QModelIndex &index)
{
    return roleIndex(index).data(EnabledRole).toBool();
}

bool BusLibraryModel::setData(const QModelIndex &index, const QVariant &value, int role)
{
    if (role == Qt::CheckStateRole && index.column() == EnabledColumn
        && nodeType(index) == LibraryNode) {
        const bool changed = QStandardItemModel::setData(index, value, role);
        const bool enabled = value.toInt() == Qt::Checked;
        setRowEnabled(index, enabled);
        return changed;
    }
    return QStandardItemModel::setData(index, value, role);
}

QList<QStandardItem *> BusLibraryModel::makeRow(
    const QString &text,
    NodeType       type,
    const QString &libraryName,
    const QString &busName,
    const QString &path,
    const QString &busCount,
    const QString &status,
    bool           enabled,
    bool           checkable) const
{
    QList<QStandardItem *> row;
    row.reserve(ColumnCount);
    row << new QStandardItem(text) << new QStandardItem() << new QStandardItem(path)
        << new QStandardItem(busCount) << new QStandardItem(status);

    for (QStandardItem *item : row) {
        item->setEditable(false);
        item->setData(static_cast<int>(type), NodeTypeRole);
        item->setData(libraryName, LibraryNameRole);
        item->setData(busName, BusNameRole);
        item->setData(enabled, EnabledRole);
        item->setData(path, PathRole);
        item->setData(status, StatusRole);
    }

    QStandardItem *enabledItem = row.at(EnabledColumn);
    enabledItem->setTextAlignment(Qt::AlignCenter);
    if (checkable) {
        enabledItem->setCheckable(true);
        enabledItem->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
    }
    return row;
}

QModelIndex BusLibraryModel::roleIndex(const QModelIndex &index)
{
    if (!index.isValid())
        return {};
    return index.sibling(index.row(), LibraryColumn);
}

void BusLibraryModel::setRowEnabled(const QModelIndex &index, bool enabled)
{
    const QModelIndex libraryIndex = roleIndex(index);
    if (!libraryIndex.isValid())
        return;

    for (int column = 0; column < ColumnCount; ++column) {
        QStandardItem *item = itemFromIndex(libraryIndex.sibling(libraryIndex.row(), column));
        if (!item)
            continue;
        item->setData(enabled, EnabledRole);
        if (column != EnabledColumn)
            item->setEnabled(enabled);
    }

    QStandardItem *libraryItem = itemFromIndex(libraryIndex);
    if (!libraryItem)
        return;
    for (int row = 0; row < libraryItem->rowCount(); ++row) {
        for (int column = 0; column < ColumnCount; ++column) {
            QStandardItem *child = libraryItem->child(row, column);
            if (!child)
                continue;
            child->setData(enabled, EnabledRole);
            child->setEnabled(enabled);
        }
    }
}
