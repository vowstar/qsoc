// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#pragma once

#include "qsoc_test.h"

#include <QDir>
#include <QElapsedTimer>
#include <QProcess>
#include <QStandardPaths>
#include <QtTest>

#ifdef Q_OS_UNIX
#include <csignal>
#include <unistd.h>
#endif

namespace {

bool waitForIomuxProcess(QProcess &process, int timeout)
{
    if (process.waitForFinished(timeout)) {
        return true;
    }
    const qint64 pid = process.processId();
#ifdef Q_OS_UNIX
    if (pid > 0) {
        ::kill(-pid_t(pid), SIGKILL);
    }
#elif defined(Q_OS_WIN)
    if (pid > 0) {
        QProcess::execute("taskkill", {"/PID", QString::number(pid), "/T", "/F"});
    }
#endif
    process.kill();
    process.waitForFinished();
    return false;
}

void runIomuxSimulation(const QString &directory, const QStringList &sources)
{
    const QString compiler = QStandardPaths::findExecutable("verilator");
    if (compiler.isEmpty()) {
        QSOC_TEST_MISSING_DEPENDENCY("verilator");
    }
    const QString binary = QDir(directory).absoluteFilePath("iomux_sim");
    QStringList   arguments{
        "--binary",
        "--timing",
        "--threads",
        "1",
        "-j",
        "16",
        "-Wno-fatal",
        "-Werror-INITIALDLY",
        "--top-module",
        "tb",
        "--Mdir",
        QDir(directory).absoluteFilePath("obj_dir"),
        "-o",
        binary};
    arguments.append(sources);
    QProcess process;
    process.setWorkingDirectory(directory);
    process.setProcessChannelMode(QProcess::MergedChannels);
#ifdef Q_OS_UNIX
    process.setChildProcessModifier([]() {
        if (::setpgid(0, 0) != 0) {
            ::_exit(127);
        }
    });
#endif
    QElapsedTimer timer;
    timer.start();
    process.start(compiler, arguments);
    QVERIFY2(process.waitForStarted(), qPrintable(process.errorString()));
    const bool       compiled      = waitForIomuxProcess(process, 300000);
    const QByteArray compileOutput = process.readAll();
    qInfo() << "Verilator two-state compile ms:" << timer.elapsed();
    QVERIFY2(compiled, qPrintable("Verilator compile timeout\n" + compileOutput));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QVERIFY2(process.exitCode() == 0, compileOutput.constData());

    timer.restart();
    process.start(binary, QStringList{});
    QVERIFY2(process.waitForStarted(), qPrintable(process.errorString()));
    const bool       simulated = waitForIomuxProcess(process, 120000);
    const QByteArray output    = process.readAll();
    qInfo() << "Verilator two-state simulation ms:" << timer.elapsed();
    QVERIFY2(simulated, qPrintable("Verilator simulation timeout\n" + output));
    QCOMPARE(process.exitStatus(), QProcess::NormalExit);
    QVERIFY2(process.exitCode() == 0, output.constData());
    QVERIFY2(!output.contains("TEST_FAIL"), output.constData());
    QVERIFY2(!output.contains("CHECK_FAIL"), output.constData());
    QVERIFY2(output.contains("TEST_PASS"), output.constData());
}

} // namespace
