// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#ifndef MODULELIBRARY_QSOCMODEL_H
#define MODULELIBRARY_QSOCMODEL_H

#include <QAbstractItemModel>
#include <QIcon>
#include <QString>

#include <yaml-cpp/yaml.h>

class QSocModuleManager;

namespace QSchematic::Items {
class Item;
}

namespace ModuleLibrary {

/**
 * @brief Module information structure.
 * @details This structure contains information about a module or category.
 */
struct ModuleInfo
{
    QString                        name;    /**< Name of the module/category */
    QString                        library; /**< Library name (for categories) */
    QIcon                          icon;    /**< Icon of the module */
    const QSchematic::Items::Item *item;    /**< Item associated with the module */

    /**
     * @brief Constructor for ModuleInfo.
     * @details This constructor initializes the module information.
     * @param[in] name Name of the module
     * @param[in] icon Icon of the module
     * @param[in] item Item associated with the module
     * @param[in] library Library name (optional)
     */
    ModuleInfo(
        const QString                 &name,
        const QIcon                   &icon,
        const QSchematic::Items::Item *item,
        const QString                 &library = QString())
        : name(name)
        , library(library)
        , icon(icon)
        , item(item)
    {}
};

/**
 * @brief Module tree item structure.
 * @details This structure represents an item in the module tree.
 */
class ModuleModuleTreeItem
{
public:
    /**
     * @brief Constructor for ModuleModuleTreeItem.
     * @details This constructor initializes the module tree item.
     * @param[in] type Type of the module tree item
     * @param[in] data Data of the module tree item
     * @param[in] parent Parent of the module tree item
     */
    explicit ModuleModuleTreeItem(
        int type, const ModuleInfo *data = nullptr, ModuleModuleTreeItem *parent = nullptr);

    /**
     * @brief Destructor for ModuleModuleTreeItem.
     * @details This destructor will free the module tree item.
     */
    ~ModuleModuleTreeItem();

    /**
     * @brief Add a child to the module tree item.
     * @details This function will add a child to the module tree item.
     * @param[in] child Child to add
     */
    void appendChild(ModuleModuleTreeItem *child);

    /**
     * @brief Get a child of the module tree item.
     * @details This function will get a child of the module tree item.
     * @param[in] row Row of the child
     * @return Child of the module tree item
     */
    ModuleModuleTreeItem *child(int row) const;

    /**
     * @brief Get the number of children of the module tree item.
     * @details This function will get the number of children of the module tree item.
     * @return Number of children of the module tree item
     */
    int childCount() const;

    /**
     * @brief Get the row of the module tree item.
     * @details This function will get the row of the module tree item.
     * @return Row of the module tree item
     */
    int row() const;

    /**
     * @brief Get the parent of the module tree item.
     * @details This function will get the parent of the module tree item.
     * @return Parent of the module tree item
     */
    ModuleModuleTreeItem *parent() const;

    /**
     * @brief Get the type of the module tree item.
     * @details This function will get the type of the module tree item.
     * @return Type of the module tree item
     */
    int type() const;

    /**
     * @brief Get the data of the module tree item.
     * @details This function will get the data of the module tree item.
     * @return Data of the module tree item
     */
    const ModuleInfo *data() const;

    /**
     * @brief Delete a child of the module tree item.
     * @details This function will delete a child of the module tree item.
     * @param[in] row Row of the child to delete
     */
    void deleteChild(int row);

private:
    int                           type_;     /**< Type of the module tree item */
    const ModuleInfo             *data_;     /**< Data of the module tree item */
    ModuleModuleTreeItem         *parent_;   /**< Parent of the module tree item */
    QList<ModuleModuleTreeItem *> children_; /**< Children of the module tree item */
};

/**
 * @brief The ModuleModel class.
 * @details This class is the module library model class for the module
 *          application. It is responsible for providing data to the module
 *          library view.
 */
class ModuleModel : public QAbstractItemModel
{
    Q_OBJECT

public:
    /**
     * @brief Module library item types.
     * @details This enum defines the types of module library items.
     */
    enum ItemTypes {
        Root,            /**< Root item */
        CategoryLogic,   /**< Logic category */
        CategoryMemory,  /**< Memory category */
        CategoryIO,      /**< I/O category */
        CategoryLibrary, /**< Library category */
        Module           /**< Module item */
    };
    Q_ENUM(ItemTypes)

    /**
     * @brief Constructor for ModuleModel.
     * @details This constructor will initialize the module library model.
     * @param[in] parent Parent object
     * @param[in] moduleManager QSocModuleManager instance for loading modules
     */
    explicit ModuleModel(QObject *parent = nullptr, QSocModuleManager *moduleManager = nullptr);

    /**
     * @brief Destructor for ModuleModel.
     * @details This destructor will free the module library model.
     */
    ~ModuleModel() override;

    /**
     * @brief Get the item from the index.
     * @details This function will get the item from the index.
     * @param[in] index Index of the item
     * @return Item from the index
     */
    const QSchematic::Items::Item *itemFromIndex(const QModelIndex &index) const;

    /**
     * @brief Get the index of the item.
     * @details This function will get the index of the item.
     * @param[in] row Row of the item
     * @param[in] column Column of the item
     * @param[in] parent Parent of the item
     * @return Index of the item
     */
    QModelIndex index(int row, int column, const QModelIndex &parent) const override;

    /**
     * @brief Get the parent of the item.
     * @details This function will get the parent of the item.
     * @param[in] child Child of the item
     * @return Parent of the item
     */
    QModelIndex parent(const QModelIndex &child) const override;

    /**
     * @brief Get the number of rows.
     * @details This function will get the number of rows.
     * @param[in] parent Parent of the item
     * @return Number of rows
     */
    int rowCount(const QModelIndex &parent) const override;

    /**
     * @brief Get the number of columns.
     * @details This function will get the number of columns.
     * @param[in] parent Parent of the item
     * @return Number of columns
     */
    int columnCount(const QModelIndex &parent) const override;

    /**
     * @brief Get the data of the item.
     * @details This function will get the data of the item.
     * @param[in] index Index of the item
     * @param[in] role Role of the data
     * @return Data of the item
     */
    QVariant data(const QModelIndex &index, int role) const override;

    /**
     * @brief Get the flags of the item.
     * @details This function will get the flags of the item.
     * @param[in] index Index of the item
     * @return Flags of the item
     */
    Qt::ItemFlags flags(const QModelIndex &index) const override;

    /**
     * @brief Get the MIME types.
     * @details This function will get the MIME types.
     * @return MIME types
     */
    QStringList mimeTypes() const override;

    /**
     * @brief Get the MIME data.
     * @details This function will get the MIME data.
     * @param[in] indexes Indexes of the items
     * @return MIME data
     */
    QMimeData *mimeData(const QModelIndexList &indexes) const override;

    /**
     * @brief Set the module manager.
     * @details This function sets the QSocModuleManager instance and reloads the model.
     * @param[in] moduleManager QSocModuleManager instance
     */
    void setModuleManager(QSocModuleManager *moduleManager);

    /**
     * @brief Reload modules from the module manager.
     * @details This function reloads the module data from QSocModuleManager.
     */
    void reloadModules();

public slots:
    /**
     * @brief Refresh the model.
     * @details This slot refreshes the model by reloading modules from QSocModuleManager.
     */
    void refresh();

private:
    /**
     * @brief Create the model.
     * @details This function will create the model.
     */
    void createModel();

    /**
     * @brief Add a tree item.
     * @details This function will add a tree item.
     * @param[in] name Name of the tree item
     * @param[in] icon Icon of the tree item
     * @param[in] item Item of the tree item
     * @param[in] parent Parent of the tree item
     */
    void addTreeItem(
        const QString                 &name,
        const QIcon                   &icon,
        const QSchematic::Items::Item *item,
        ModuleModuleTreeItem          *parent);

    ModuleModuleTreeItem *rootItem_;       /**< Root item of the model */
    QSocModuleManager    *m_moduleManager; /**< QSocModuleManager instance */
};

} // namespace ModuleLibrary

#endif // MODULELIBRARY_QSOCMODEL_H
