// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
// SPDX-License-Identifier: Apache-2.0

#ifndef BUSLIBRARYMODEL_H
#define BUSLIBRARYMODEL_H

#include "common/qsocbusmanager.h"

#include <QStandardItemModel>

class BusLibraryModel : public QStandardItemModel
{
    Q_OBJECT

public:
    enum NodeType { RootNode, LibraryNode, BusNode };

    enum Column {
        LibraryColumn,
        EnabledColumn,
        PathColumn,
        BusCountColumn,
        StatusColumn,
        ColumnCount
    };

    enum Role {
        NodeTypeRole = Qt::UserRole + 1,
        LibraryNameRole,
        BusNameRole,
        EnabledRole,
        PathRole,
        StatusRole
    };

    explicit BusLibraryModel(QObject *parent = nullptr);

    void        setBusManager(QSocBusManager *manager);
    QModelIndex indexForBus(const QString &libraryName, const QString &busName) const;
    bool        setLibraryEnabled(const QString &libraryName, bool enabled);

    static NodeType nodeType(const QModelIndex &index);
    static QString  libraryName(const QModelIndex &index);
    static QString  busName(const QModelIndex &index);
    static bool     isEnabled(const QModelIndex &index);

    bool setData(const QModelIndex &index, const QVariant &value, int role = Qt::EditRole) override;

private:
    QList<QStandardItem *> makeRow(
        const QString &text,
        NodeType       type,
        const QString &libraryName,
        const QString &busName,
        const QString &path,
        const QString &busCount,
        const QString &status,
        bool           enabled,
        bool           checkable) const;
    static QModelIndex roleIndex(const QModelIndex &index);
    void               setRowEnabled(const QModelIndex &index, bool enabled);
};

#endif // BUSLIBRARYMODEL_H
