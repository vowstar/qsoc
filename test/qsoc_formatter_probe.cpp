// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include <QCoreApplication>
#include <QFile>

#include <cstdio>

int main(int argc, char *argv[])
{
    QCoreApplication  application(argc, argv);
    const QStringList arguments = application.arguments();
    if (arguments.size() < 2) {
        return 2;
    }

    bool      hasRequestedExit = false;
    const int requestedExit
        = qEnvironmentVariableIntValue("QSOC_FORMATTER_PROBE_EXIT_CODE", &hasRequestedExit);
    if (hasRequestedExit) {
        fputs("formatter probe failure\n", stderr);
        return requestedExit == 0 ? 8 : requestedExit;
    }

    QFile outputFile(arguments.constLast());
    if (!outputFile.open(QIODevice::WriteOnly | QIODevice::Append)) {
        return 3;
    }
    if (outputFile.write("\n// formatter probe\n") < 0) {
        return 4;
    }
    outputFile.close();

    const QString sentinelPath = qEnvironmentVariable("QSOC_FORMATTER_PROBE_SENTINEL");
    if (sentinelPath.isEmpty()) {
        return 5;
    }
    QFile sentinelFile(sentinelPath);
    if (!sentinelFile.open(QIODevice::WriteOnly)) {
        return 6;
    }
    return sentinelFile.write("called\n") == 7 ? 0 : 7;
}
