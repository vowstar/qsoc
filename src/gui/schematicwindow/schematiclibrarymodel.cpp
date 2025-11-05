// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "schematiclibrarymodel.h"
#include "common/qsocmodulemanager.h"
#include "schematicmodule.h"

#include <qschematic/items/item.hpp>
#include <qschematic/items/itemmimedata.hpp>
#include <qschematic/items/label.hpp>
#include <qschematic/items/node.hpp>

#include <QDebug>
#include <QIcon>
#include <QMimeData>
#include <QRegularExpression>

/* SchematicLibraryTreeItem implementation */

SchematicLibraryTreeItem::SchematicLibraryTreeItem(
    int type, const SchematicLibraryInfo *data, SchematicLibraryTreeItem *parent)
    : type_(type)
    , data_(data)
    , parent_(parent)
{}

SchematicLibraryTreeItem::~SchematicLibraryTreeItem()
{
    qDeleteAll(children_);
    delete data_;
}

void SchematicLibraryTreeItem::appendChild(SchematicLibraryTreeItem *child)
{
    children_.append(child);
}

SchematicLibraryTreeItem *SchematicLibraryTreeItem::child(int row) const
{
    if (row < 0 || row >= children_.size())
        return nullptr;
    return children_.at(row);
}

int SchematicLibraryTreeItem::childCount() const
{
    return static_cast<int>(children_.size());
}

int SchematicLibraryTreeItem::row() const
{
    if (parent_)
        return static_cast<int>(
            parent_->children_.indexOf(const_cast<SchematicLibraryTreeItem *>(this)));
    return 0;
}

SchematicLibraryTreeItem *SchematicLibraryTreeItem::parent() const
{
    return parent_;
}

int SchematicLibraryTreeItem::type() const
{
    return type_;
}

const SchematicLibraryInfo *SchematicLibraryTreeItem::data() const
{
    return data_;
}

void SchematicLibraryTreeItem::deleteChild(int row)
{
    if (row < 0 || row >= children_.size())
        return;

    delete children_.takeAt(row);
}

/* SchematicLibraryModel implementation */

SchematicLibraryModel::SchematicLibraryModel(QObject *parent, QSocModuleManager *moduleManager)
    : QAbstractItemModel(parent)
    , rootItem_(new SchematicLibraryTreeItem(Root, nullptr))
    , m_moduleManager(moduleManager)
{
    createModel();
}

SchematicLibraryModel::~SchematicLibraryModel()
{
    delete rootItem_;
}

const QSchematic::Items::Item *SchematicLibraryModel::itemFromIndex(const QModelIndex &index) const
{
    if (!index.isValid())
        return nullptr;

    auto *item = static_cast<SchematicLibraryTreeItem *>(index.internalPointer());
    if (!item)
        return nullptr;

    const SchematicLibraryInfo *info = item->data();
    if (!info)
        return nullptr;

    return info->item;
}

QModelIndex SchematicLibraryModel::index(int row, int column, const QModelIndex &parent) const
{
    if (!hasIndex(row, column, parent))
        return {};

    SchematicLibraryTreeItem *parentItem;

    if (!parent.isValid())
        parentItem = rootItem_;
    else
        parentItem = static_cast<SchematicLibraryTreeItem *>(parent.internalPointer());

    SchematicLibraryTreeItem *childItem = parentItem->child(row);
    if (childItem)
        return createIndex(row, column, childItem);

    return {};
}

QModelIndex SchematicLibraryModel::parent(const QModelIndex &child) const
{
    if (!child.isValid())
        return {};

    auto *childItem = static_cast<SchematicLibraryTreeItem *>(child.internalPointer());
    SchematicLibraryTreeItem *parentItem = childItem->parent();

    if (parentItem == rootItem_)
        return {};

    return createIndex(parentItem->row(), 0, parentItem);
}

int SchematicLibraryModel::rowCount(const QModelIndex &parent) const
{
    if (parent.column() > 0)
        return 0;

    SchematicLibraryTreeItem *parentItem;

    if (!parent.isValid())
        parentItem = rootItem_;
    else
        parentItem = static_cast<SchematicLibraryTreeItem *>(parent.internalPointer());

    return parentItem->childCount();
}

int SchematicLibraryModel::columnCount(const QModelIndex &parent) const
{
    Q_UNUSED(parent);
    return 1;
}

QVariant SchematicLibraryModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid())
        return {};

    auto *item = static_cast<SchematicLibraryTreeItem *>(index.internalPointer());
    const SchematicLibraryInfo *info = item->data();

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

Qt::ItemFlags SchematicLibraryModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;

    auto *item = static_cast<SchematicLibraryTreeItem *>(index.internalPointer());

    // Only modules can be dragged
    if (item->type() == Module)
        return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled;

    return Qt::ItemIsEnabled | Qt::ItemIsSelectable;
}

QStringList SchematicLibraryModel::mimeTypes() const
{
    return QStringList() << QStringLiteral("application/x-qschematicitem");
}

QMimeData *SchematicLibraryModel::mimeData(const QModelIndexList &indexes) const
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

void SchematicLibraryModel::setModuleManager(QSocModuleManager *moduleManager)
{
    m_moduleManager = moduleManager;
    reloadModules();
}

void SchematicLibraryModel::reloadModules()
{
    createModel();
}

void SchematicLibraryModel::refresh()
{
    reloadModules();
}

void SchematicLibraryModel::createModel()
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
            = new SchematicLibraryInfo(libraryName, QIcon::fromTheme("folder"), nullptr, libraryName);
        auto *libraryCategory
            = new SchematicLibraryTreeItem(CategoryLibrary, libraryInfo, rootItem_);
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
            auto *socModuleItem = new SchematicModule(moduleName, moduleYaml);

            // Add to tree
            addTreeItem(moduleName, QIcon::fromTheme("cpu"), socModuleItem, libraryCategory);
        }
    }
}

void SchematicLibraryModel::addTreeItem(
    const QString                 &name,
    const QIcon                   &icon,
    const QSchematic::Items::Item *item,
    SchematicLibraryTreeItem      *parent)
{
    auto *itemInfo = new SchematicLibraryInfo(name, icon, item);
    auto *newItem  = new SchematicLibraryTreeItem(Module, itemInfo, parent);

    beginInsertRows(createIndex(parent->row(), 0, parent), parent->childCount(), parent->childCount());
    parent->appendChild(newItem);
    endInsertRows();
}
