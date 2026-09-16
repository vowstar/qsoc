// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocuvmresources.h"

#include <mutex>
#include <QDirIterator>
#include <QFile>
#include <QResource>

static void initializeUvmResource()
{
    Q_INIT_RESOURCE(uvm_core);
}

QMap<QString, QByteArray> QSocUvmResources::sources()
{
    static std::once_flag registration;
    std::call_once(registration, initializeUvmResource);
    const QString             root = QStringLiteral(":/uvm-core/");
    QMap<QString, QByteArray> result;
    QDirIterator              files(root, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        QFile file(files.next());
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        result.insert(file.fileName().mid(root.size()), file.readAll());
    }
    return result;
}
