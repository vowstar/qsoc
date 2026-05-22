// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "modulelibrarymodel.h"

#include <QDir>
#include <QFileInfo>
#include <QMap>

namespace {

QMap<QString, QSocModuleOverlay> overlayMap(QSocModuleManager *manager)
{
    QMap<QString, QSocModuleOverlay> map;
    if (!manager)
        return map;
    for (const QSocModuleOverlay &overlay : manager->scanModuleOverlays())
        map.insert(overlay.moduleName, overlay);
    return map;
}

} // namespace

ModuleLibraryModel::ModuleLibraryModel(QObject *parent)
    : QStandardItemModel(parent)
{
    setHorizontalHeaderLabels(
        {tr("Library"), tr("Enabled"), tr("Path"), tr("Modules"), tr("Status")});
}

void ModuleLibraryModel::setModuleManager(QSocModuleManager *manager)
{
    clear();
    setHorizontalHeaderLabels(
        {tr("Library"), tr("Enabled"), tr("Path"), tr("Modules"), tr("Status")});

    QString modulePath;
    QString projectPath;
    if (manager && manager->getProjectManager()) {
        modulePath  = manager->getProjectManager()->getModulePath();
        projectPath = manager->getProjectManager()->getProjectPath();
    }

    const QString rootPath = projectPath.isEmpty() ? modulePath
                                                   : QDir(projectPath).relativeFilePath(modulePath);
    appendRow(makeRow(
        tr("Project Modules"), RootNode, {}, {}, rootPath, {}, tr("Loaded"), {}, {}, true, false));
    QStandardItem *root = item(0, LibraryColumn);

    if (!manager)
        return;

    const QMap<QString, QSocModuleOverlay> overlays  = overlayMap(manager);
    const QStringList                      libraries = manager->listLoadedLibraries();
    root->setText(tr("Project Modules (%1)").arg(libraries.size()));
    for (const QString &libraryName : libraries) {
        const QStringList modules = manager->listModulesInLibrary(libraryName);
        const QString filePath = QDir(modulePath).filePath(libraryName + QStringLiteral(".soc_mod"));
        const QString displayPath = projectPath.isEmpty()
                                        ? filePath
                                        : QDir(projectPath).relativeFilePath(filePath);
        QString       status      = tr("Loaded");
        if (!QFileInfo::exists(filePath)) {
            status = modules.isEmpty() ? tr("Pending") : tr("Missing");
        } else if (!QFileInfo(filePath).isWritable()) {
            status = tr("Read-only");
        }

        root->appendRow(makeRow(
            libraryName,
            LibraryNode,
            libraryName,
            {},
            displayPath,
            QString::number(modules.size()),
            status,
            {},
            {},
            true,
            true));
        QStandardItem *libraryItem = root->child(root->rowCount() - 1, LibraryColumn);

        for (const QString &moduleName : modules) {
            const QSocModuleDefinition definition
                = manager->getModuleDefinition(libraryName, moduleName);
            const QSocModuleOverlay overlay = overlays.value(moduleName);
            QString                 moduleStatus;
            QStringList             shadowedLibraries;
            if (!overlay.moduleName.isEmpty()) {
                shadowedLibraries = overlay.shadowedLibraries;
                moduleStatus      = overlay.activeLibrary == libraryName ? tr("Active overlay")
                                                                         : tr("Shadowed");
            } else if (definition.isNullDefinition) {
                moduleStatus = tr("Null");
            } else {
                moduleStatus = tr("Loaded");
            }

            libraryItem->appendRow(makeRow(
                moduleName,
                ModuleNode,
                libraryName,
                moduleName,
                {},
                QString::number(
                    definition.ports.size() + definition.parameters.size()
                    + definition.busInterfaces.size()),
                moduleStatus,
                overlay.activeLibrary,
                shadowedLibraries,
                true,
                false));
        }
    }
}

QModelIndex ModuleLibraryModel::indexForModule(
    const QString &libraryName, const QString &moduleName) const
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

        if (moduleName.isEmpty())
            return libraryItem->index();

        for (int j = 0; j < libraryItem->rowCount(); ++j) {
            QStandardItem *moduleItem = libraryItem->child(j, 0);
            if (moduleItem && moduleItem->data(ModuleNameRole).toString() == moduleName)
                return moduleItem->index();
        }
    }

    return QModelIndex();
}

bool ModuleLibraryModel::setLibraryEnabled(const QString &libraryName, bool enabled)
{
    const QModelIndex libraryIndex = indexForModule(libraryName, {});
    if (!libraryIndex.isValid())
        return false;

    const QModelIndex enabledIndex = index(libraryIndex.row(), EnabledColumn, libraryIndex.parent());
    return setData(enabledIndex, enabled ? Qt::Checked : Qt::Unchecked, Qt::CheckStateRole);
}

ModuleLibraryModel::NodeType ModuleLibraryModel::nodeType(const QModelIndex &index)
{
    if (!index.isValid())
        return RootNode;
    return static_cast<NodeType>(roleIndex(index).data(NodeTypeRole).toInt());
}

QString ModuleLibraryModel::libraryName(const QModelIndex &index)
{
    return roleIndex(index).data(LibraryNameRole).toString();
}

QString ModuleLibraryModel::moduleName(const QModelIndex &index)
{
    return roleIndex(index).data(ModuleNameRole).toString();
}

bool ModuleLibraryModel::isEnabled(const QModelIndex &index)
{
    return roleIndex(index).data(EnabledRole).toBool();
}

QString ModuleLibraryModel::status(const QModelIndex &index)
{
    return roleIndex(index).data(StatusRole).toString();
}

bool ModuleLibraryModel::setData(const QModelIndex &index, const QVariant &value, int role)
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

QList<QStandardItem *> ModuleLibraryModel::makeRow(
    const QString     &text,
    NodeType           type,
    const QString     &libraryName,
    const QString     &moduleName,
    const QString     &path,
    const QString     &moduleCount,
    const QString     &status,
    const QString     &activeLibrary,
    const QStringList &shadowedLibraries,
    bool               enabled,
    bool               checkable) const
{
    QList<QStandardItem *> row;
    row.reserve(ColumnCount);
    row << new QStandardItem(text) << new QStandardItem() << new QStandardItem(path)
        << new QStandardItem(moduleCount) << new QStandardItem(status);

    for (QStandardItem *item : row) {
        item->setEditable(false);
        item->setData(static_cast<int>(type), NodeTypeRole);
        item->setData(libraryName, LibraryNameRole);
        item->setData(moduleName, ModuleNameRole);
        item->setData(enabled, EnabledRole);
        item->setData(path, PathRole);
        item->setData(status, StatusRole);
        item->setData(activeLibrary, ActiveLibraryRole);
        item->setData(shadowedLibraries, ShadowedLibrariesRole);
    }

    QStandardItem *enabledItem = row.at(EnabledColumn);
    enabledItem->setTextAlignment(Qt::AlignCenter);
    if (checkable) {
        enabledItem->setCheckable(true);
        enabledItem->setCheckState(enabled ? Qt::Checked : Qt::Unchecked);
    }
    return row;
}

QModelIndex ModuleLibraryModel::roleIndex(const QModelIndex &index)
{
    if (!index.isValid())
        return {};
    return index.sibling(index.row(), LibraryColumn);
}

void ModuleLibraryModel::setRowEnabled(const QModelIndex &index, bool enabled)
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
