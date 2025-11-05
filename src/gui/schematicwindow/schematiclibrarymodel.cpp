// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "modulemodel.h"
#include "common/qsocmodulemanager.h"
#include "socmoduleitem.h"

#include <qschematic/items/item.hpp>
#include <qschematic/items/itemmimedata.hpp>
#include <qschematic/items/label.hpp>
#include <qschematic/items/node.hpp>

#include <QDebug>
#include <QIcon>
#include <QMimeData>
#include <QRegularExpression>

using namespace ModuleLibrary;

/* ModuleModuleTreeItem implementation */

ModuleModuleTreeItem::ModuleModuleTreeItem(
    int type, const ModuleInfo *data, ModuleModuleTreeItem *parent)
    : type_(type)
    , data_(data)
    , parent_(parent)
{}

ModuleModuleTreeItem::~ModuleModuleTreeItem()
{
    qDeleteAll(children_);
    delete data_;
}

void ModuleModuleTreeItem::appendChild(ModuleModuleTreeItem *child)
{
    children_.append(child);
}

ModuleModuleTreeItem *ModuleModuleTreeItem::child(int row) const
{
    if (row < 0 || row >= children_.size())
        return nullptr;
    return children_.at(row);
}

int ModuleModuleTreeItem::childCount() const
{
    return static_cast<int>(children_.size());
}

int ModuleModuleTreeItem::row() const
{
    if (parent_)
        return static_cast<int>(
            parent_->children_.indexOf(const_cast<ModuleModuleTreeItem *>(this)));
    return 0;
}

ModuleModuleTreeItem *ModuleModuleTreeItem::parent() const
{
    return parent_;
}

int ModuleModuleTreeItem::type() const
{
    return type_;
}

const ModuleInfo *ModuleModuleTreeItem::data() const
{
    return data_;
}

void ModuleModuleTreeItem::deleteChild(int row)
{
    if (row < 0 || row >= children_.size())
        return;

    delete children_.takeAt(row);
}

/* ModuleModel implementation */

ModuleModel::ModuleModel(QObject *parent, QSocModuleManager *moduleManager)
    : QAbstractItemModel(parent)
    , rootItem_(new ModuleModuleTreeItem(Root, nullptr))
    , m_moduleManager(moduleManager)
{
    createModel();
}

ModuleModel::~ModuleModel()
{
    delete rootItem_;
}

const QSchematic::Items::Item *ModuleModel::itemFromIndex(const QModelIndex &index) const
{
    if (!index.isValid())
        return nullptr;

    auto *item = static_cast<ModuleModuleTreeItem *>(index.internalPointer());
    if (!item)
        return nullptr;

    const ModuleInfo *info = item->data();
    if (!info)
        return nullptr;

    return info->item;
}

QModelIndex ModuleModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent))
        return {};

    ModuleModuleTreeItem *parentItem;

    if (!parent.isValid())
        parentItem = rootItem_;
    else
        parentItem = static_cast<ModuleModuleTreeItem *>(parent.internalPointer());

    ModuleModuleTreeItem *childItem = parentItem->child(row);
    if (childItem)
        return createIndex(row, column, childItem);

    return {};
}

QModelIndex ModuleModel::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return {};

    auto                 *childItem  = static_cast<ModuleModuleTreeItem *>(child.internalPointer());
    ModuleModuleTreeItem *parentItem = childItem->parent();

    if (parentItem == rootItem_)
        return {};

    return createIndex(parentItem->row(), 0, parentItem);
}

int ModuleModel::rowCount(const QModelIndex &parent) const
{
    if (parent.column() > 0)
        return 0;

    ModuleModuleTreeItem *parentItem;

    if (!parent.isValid())
        parentItem = rootItem_;
    else
        parentItem = static_cast<ModuleModuleTreeItem *>(parent.internalPointer());

    return parentItem->childCount();
}

int ModuleModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 1;
}

QVariant ModuleModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};

    auto             *item = static_cast<ModuleModuleTreeItem *>(index.internalPointer());
    const ModuleInfo *info = item->data();

    switch (item->type()) {
    case Root:
        return {};

    case CategoryLogic:
        if (role == Qt::DisplayRole) {
            // For dynamically created categories, use the library name
            // We need to track which library this category represents
            // For now, use generic names
            return tr("Logic Gates");
        }
        if (role == Qt::DecorationRole)
            return QIcon::fromTheme("folder");
        break;

    case CategoryMemory:
        if (role == Qt::DisplayRole)
            return tr("Memory");
        if (role == Qt::DecorationRole)
            return QIcon::fromTheme("folder");
        break;

    case CategoryIO:
        if (role == Qt::DisplayRole)
            return tr("I/O Ports");
        if (role == Qt::DecorationRole)
            return QIcon::fromTheme("folder");
        break;

    case CategoryLibrary:
        if (!info)
            return {};
        if (role == Qt::DisplayRole)
            return info->name;
        if (role == Qt::DecorationRole)
            return info->icon.isNull() ? QIcon::fromTheme("folder") : info->icon;
        break;

    case Module:
        if (!info)
            return {};

        if (role == Qt::DisplayRole)
            return info->name;
        if (role == Qt::DecorationRole)
            return info->icon.isNull() ? QIcon::fromTheme("application-x-object") : info->icon;
        break;

    default:
        break;
    }

    return {};
}

Qt::ItemFlags ModuleModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;

    auto *item = static_cast<ModuleModuleTreeItem *>(index.internalPointer());

    // Only modules can be dragged
    if (item->type() == Module)
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;

    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QStringList ModuleModel::mimeTypes() const
{
    return QStringList() << QStringLiteral("application/x-qschematicitem");
}

QMimeData *ModuleModel::mimeData(const QModelIndexList &indexes) const
{
    if (indexes.isEmpty())
        return nullptr;

    const QModelIndex             &index = indexes.first();
    const QSchematic::Items::Item *item  = itemFromIndex(index);

    if (!item)
        return nullptr;

    // Create a shared pointer to a copy of the item
    const std::shared_ptr<QSchematic::Items::Item> itemCopy = item->deepCopy();
    if (!itemCopy)
        return nullptr;

    // Create mime data
    auto *mimeData = new QSchematic::Items::MimeData(itemCopy);
    return mimeData;
}

void ModuleModel::setModuleManager(QSocModuleManager *moduleManager)
{
    m_moduleManager = moduleManager;
    reloadModules();
}

void ModuleModel::reloadModules()
{
    createModel();
}

void ModuleModel::refresh()
{
    reloadModules();
}

void ModuleModel::createModel()
{
    // Clear existing model
    while (rootItem_->childCount() > 0) {
        beginRemoveRows(QModelIndex(), 0, 0);
        rootItem_->deleteChild(0);
        endRemoveRows();
    }

    if (!m_moduleManager) {
        return;
    }

    // Load all modules from the module manager
    if (!m_moduleManager->load()) {
        return;
    }

    // Get all available modules
    QStringList moduleNames = m_moduleManager->listModule();

    if (moduleNames.isEmpty()) {
        return;
    }

    // Group modules by library
    QMap<QString, QStringList> modulesByLibrary;

    for (const QString &moduleName : moduleNames) {
        QString libraryName = m_moduleManager->getModuleLibrary(moduleName);
        if (libraryName.isEmpty()) {
            libraryName = tr("Unknown");
        }
        modulesByLibrary[libraryName].append(moduleName);
    }

    // Create categories for each library
    for (auto it = modulesByLibrary.constBegin(); it != modulesByLibrary.constEnd(); ++it) {
        const QString     &libraryName = it.key();
        const QStringList &modules     = it.value();

        // Create library category with library name info
        auto *libraryInfo
            = new ModuleInfo(libraryName, QIcon::fromTheme("folder"), nullptr, libraryName);
        auto *libraryCategory = new ModuleModuleTreeItem(CategoryLibrary, libraryInfo, rootItem_);
        beginInsertRows(QModelIndex(), rootItem_->childCount(), rootItem_->childCount());
        rootItem_->appendChild(libraryCategory);
        endInsertRows();

        // Add modules to this library category
        for (const QString &moduleName : modules) {
            YAML::Node moduleYaml = m_moduleManager->getModuleYaml(moduleName);
            if (moduleYaml.IsNull()) {
                qDebug() << "Failed to get YAML data for module:" << moduleName;
                continue;
            }

            // Create SOC module item
            auto *socModuleItem = new SocModuleItem(moduleName, moduleYaml);

            // Add to tree
            addTreeItem(moduleName, QIcon::fromTheme("cpu"), socModuleItem, libraryCategory);
        }
    }
}

void ModuleModel::addTreeItem(
    const QString                 &name,
    const QIcon                   &icon,
    const QSchematic::Items::Item *item,
    ModuleModuleTreeItem          *parent)
{
    auto *itemInfo = new ModuleInfo(name, icon, item);
    auto *newItem  = new ModuleModuleTreeItem(Module, itemInfo, parent);

    beginInsertRows(createIndex(parent->row(), 0, parent), parent->childCount(), parent->childCount());
    parent->appendChild(newItem);
    endInsertRows();
}
