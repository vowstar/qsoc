// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOC_RESOURCE_PROCESS_TEST_H
#define QSOC_RESOURCE_PROCESS_TEST_H

#include <QDebug>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonObject>
#include <QSet>

#ifdef Q_OS_WIN
#include <windows.h>

#include <tlhelp32.h>
#endif

namespace QSocTest {

inline QString resourceProcessError(
    const QJsonArray &processes,
    qint64            rootPid,
    const QString    &rootProgram,
    const QString    &excludedProgram)
{
    if (processes.isEmpty() || processes.first().toObject().value("pid").toInteger() != rootPid)
        return QStringLiteral("Resource snapshot does not begin with its root process");
    QSet<qint64> pids;
    for (const auto &value : processes) {
        const auto pid = value.toObject().value("pid").toInteger();
        if (pid <= 0 || pids.contains(pid))
            return QStringLiteral("Resource snapshot contains an invalid or duplicate PID");
        pids.insert(pid);
    }
#ifdef Q_OS_WIN
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return QStringLiteral("Could not enumerate native processes");
    QHash<qint64, QString> images;
    PROCESSENTRY32W        entry{};
    entry.dwSize          = sizeof(entry);
    const bool enumerated = ::Process32FirstW(snapshot, &entry);
    if (enumerated) {
        do {
            images.insert(entry.th32ProcessID, QString::fromWCharArray(entry.szExeFile));
        } while (::Process32NextW(snapshot, &entry));
    }
    const DWORD error = ::GetLastError();
    ::CloseHandle(snapshot);
    if (!enumerated || error != ERROR_NO_MORE_FILES)
        return QStringLiteral("Native process enumeration was incomplete");
    if (images.value(rootPid).compare(QFileInfo(rootProgram).fileName(), Qt::CaseInsensitive) != 0)
        return QStringLiteral("Resource root does not match its native process image");
    for (const auto pid : pids) {
        if (pid == rootPid)
            continue;
        const auto image = images.value(pid);
        qInfo().noquote() << "Resource descendant" << pid
                          << (image.isEmpty() ? QStringLiteral("already exited") : image);
        if (image.compare(QFileInfo(excludedProgram).fileName(), Qt::CaseInsensitive) == 0)
            return QStringLiteral("Resource snapshot contains an extra live daemon process");
    }
#else
    Q_UNUSED(rootProgram)
    Q_UNUSED(excludedProgram)
    if (processes.size() != 1)
        return QStringLiteral("Resource snapshot retained a child process");
#endif
    return {};
}

} // namespace QSocTest

#endif
