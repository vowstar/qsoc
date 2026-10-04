// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocprivatefile.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>

#ifdef Q_OS_UNIX
namespace {

constexpr QFileDevice::Permissions filePermissions = QFileDevice::ReadOwner
                                                     | QFileDevice::WriteOwner;
constexpr QFileDevice::Permissions dirPermissions  = filePermissions | QFileDevice::ExeOwner;

} // namespace
#endif

namespace QSocPrivateFile {

void restrict(QFileDevice &file)
{
#ifdef Q_OS_UNIX
    file.setPermissions(filePermissions);
#else
    Q_UNUSED(file);
#endif
}

void restrict(const QString &path)
{
#ifdef Q_OS_UNIX
    const QFileInfo info(path);
    if (!info.isSymLink() && info.exists()) {
        QFile::setPermissions(path, info.isDir() ? dirPermissions : filePermissions);
    }
#else
    Q_UNUSED(path);
#endif
}

bool makeDir(const QString &path)
{
    if (!QDir().mkpath(path)) {
        return false;
    }
    restrict(path);
    return true;
}

} // namespace QSocPrivateFile
