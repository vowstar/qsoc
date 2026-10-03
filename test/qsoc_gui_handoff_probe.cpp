// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>

int main(int argc, char *argv[])
{
    const QCoreApplication app(argc, argv);
    const QString          output = qEnvironmentVariable("QSOC_GUI_PROBE_OUTPUT");
    QFile                  arguments(output);
    if (!arguments.open(QIODevice::WriteOnly))
        return 1;
    arguments.write(QJsonDocument(
                        QJsonObject{
                            {"pid", QCoreApplication::applicationPid()},
                            {"arguments", QJsonArray::fromStringList(app.arguments().mid(1))}})
                        .toJson());
    arguments.close();
    const QString  release = qEnvironmentVariable("QSOC_GUI_PROBE_RELEASE");
    QDeadlineTimer deadline(10000);
    while (!release.isEmpty() && !QFile::exists(release) && !deadline.hasExpired())
        QThread::msleep(10);
    QFile done(output + ".done");
    if (!done.open(QIODevice::WriteOnly))
        return 1;
    done.close();
    return 7;
}
