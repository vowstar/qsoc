// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "modulemodel.h"

#include <qschematic/items/item.hpp>
#include <qschematic/items/itemmimedata.hpp>
#include <qschematic/items/label.hpp>
#include <qschematic/items/node.hpp>

#include <QDebug>
#include <QIcon>
#include <QMimeData>

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

ModuleModel::ModuleModel(QObject *parent)
    : QAbstractItemModel(parent)
    , rootItem_(new ModuleModuleTreeItem(Root, nullptr))
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
        if (role == Qt::DisplayRole)
            return tr("Logic Gates");
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

void ModuleModel::createModel()
{
    // Clear existing model
    while (rootItem_->childCount() > 0) {
        beginRemoveRows(QModelIndex(), 0, 0);
        rootItem_->deleteChild(0);
        endRemoveRows();
    }

    // Create Logic Gates category
    auto *logicCategory = new ModuleModuleTreeItem(CategoryLogic, nullptr, rootItem_);
    beginInsertRows(QModelIndex(), rootItem_->childCount(), rootItem_->childCount());
    rootItem_->appendChild(logicCategory);
    endInsertRows();

    // Create Memory category
    auto *memoryCategory = new ModuleModuleTreeItem(CategoryMemory, nullptr, rootItem_);
    beginInsertRows(QModelIndex(), rootItem_->childCount(), rootItem_->childCount());
    rootItem_->appendChild(memoryCategory);
    endInsertRows();

    // Create I/O Ports category
    auto *ioCategory = new ModuleModuleTreeItem(CategoryIO, nullptr, rootItem_);
    beginInsertRows(QModelIndex(), rootItem_->childCount(), rootItem_->childCount());
    rootItem_->appendChild(ioCategory);
    endInsertRows();

    // Add some example modules

    // Logic Gates
    auto *andGate = new QSchematic::Items::Node(0); // We should use proper items here
    andGate->setSize(QSizeF(50, 50));
    addTreeItem(tr("AND Gate"), QIcon::fromTheme("applications-utilities"), andGate, logicCategory);

    auto *orGate = new QSchematic::Items::Node(0);
    orGate->setSize(QSizeF(50, 50));
    addTreeItem(tr("OR Gate"), QIcon::fromTheme("applications-utilities"), orGate, logicCategory);

    auto *notGate = new QSchematic::Items::Node(0);
    notGate->setSize(QSizeF(50, 30));
    addTreeItem(tr("NOT Gate"), QIcon::fromTheme("applications-utilities"), notGate, logicCategory);

    // Memory
    auto *register8bit = new QSchematic::Items::Node(0);
    register8bit->setSize(QSizeF(80, 60));
    addTreeItem(
        tr("8-bit Register"), QIcon::fromTheme("applications-system"), register8bit, memoryCategory);

    auto *ram = new QSchematic::Items::Node(0);
    ram->setSize(QSizeF(100, 80));
    addTreeItem(tr("RAM"), QIcon::fromTheme("applications-system"), ram, memoryCategory);

    // I/O Ports
    auto *input = new QSchematic::Items::Node(0);
    input->setSize(QSizeF(30, 30));
    addTreeItem(tr("Input Port"), QIcon::fromTheme("go-previous"), input, ioCategory);

    auto *output = new QSchematic::Items::Node(0);
    output->setSize(QSizeF(30, 30));
    addTreeItem(tr("Output Port"), QIcon::fromTheme("go-next"), output, ioCategory);

    // Add a text label as an example
    auto *label = new QSchematic::Items::Label(0);
    label->setText(tr("Text Label"));
    label->setHasConnectionPoint(false);
    addTreeItem(tr("Text Label"), QIcon::fromTheme("accessories-text-editor"), label, ioCategory);
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
