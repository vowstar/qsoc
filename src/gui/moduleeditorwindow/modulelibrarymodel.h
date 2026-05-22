// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef MODULELIBRARYMODEL_H
#define MODULELIBRARYMODEL_H

#include "common/qsocmodulemanager.h"

#include <QStandardItemModel>

class ModuleLibraryModel : public QStandardItemModel
{
    Q_OBJECT

public:
    enum NodeType { RootNode, LibraryNode, ModuleNode };

    enum Column {
        LibraryColumn,
        EnabledColumn,
        PathColumn,
        ModuleCountColumn,
        StatusColumn,
        ColumnCount
    };

    enum Role {
        NodeTypeRole = Qt::UserRole + 1,
        LibraryNameRole,
        ModuleNameRole,
        EnabledRole,
        PathRole,
        StatusRole,
        ActiveLibraryRole,
        ShadowedLibrariesRole
    };

    explicit ModuleLibraryModel(QObject *parent = nullptr);

    void        setModuleManager(QSocModuleManager *manager);
    QModelIndex indexForModule(const QString &libraryName, const QString &moduleName) const;
    bool        setLibraryEnabled(const QString &libraryName, bool enabled);

    static NodeType nodeType(const QModelIndex &index);
    static QString  libraryName(const QModelIndex &index);
    static QString  moduleName(const QModelIndex &index);
    static bool     isEnabled(const QModelIndex &index);
    static QString  status(const QModelIndex &index);

    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

private:
    QList<QStandardItem *> makeRow(
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
        bool               checkable) const;
    static QModelIndex roleIndex(const QModelIndex &index);
    void               setRowEnabled(const QModelIndex &index, bool enabled);
};

#endif // MODULELIBRARYMODEL_H
