// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprocessowner.h"

#include <QCoreApplication>
#include <QProcess>
#include <QSaveFile>
#include <QThread>

int main(int argc, char *argv[])
{
    QCoreApplication app(argc, argv);
    const auto       arguments = app.arguments();
    if (arguments.size() != 4)
        return 2;
    if (arguments.at(1) == "disarm") {
        QSocProcessOwner owner;
        return owner.watch(arguments.at(3).toLongLong()) ? 0 : 3;
    }
    if (arguments.at(1) == "child") {
        QSocProcessOwner owner;
        if (!owner.watch(arguments.at(3).toLongLong()))
            return 3;
        QSaveFile ready(arguments.at(2));
        if (!ready.open(QIODevice::WriteOnly))
            return 4;
        ready.write(QByteArray::number(QCoreApplication::applicationPid()));
        if (!ready.commit())
            return 4;
        QThread::sleep(60);
        return 0;
    }
    QProcess child;
    child.start(
        QCoreApplication::applicationFilePath(),
        {"child", arguments.at(2), QString::number(QCoreApplication::applicationPid())});
    if (!child.waitForStarted(5000))
        return 5;
    return app.exec();
}
