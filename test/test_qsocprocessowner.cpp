// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprocessowner.h"
#include "qsoc_test.h"

#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <unistd.h>
#endif

namespace {

bool processIsRunning(qint64 pid)
{
#ifdef Q_OS_WIN
    const HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (process == nullptr)
        return false;
    const bool running = ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    ::CloseHandle(process);
    return running;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void rejectsInvalidOwner()
    {
        QSocProcessOwner owner;
        QVERIFY(!owner.watch(-1));
        QVERIFY(!owner.watch(0));
        QVERIFY(!owner.watch(1));
    }

    void ownerDeathStopsAChildWithoutAnEventLoop()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString marker = directory.filePath("child.pid");
        QProcess      owner;
        owner.setWorkingDirectory(directory.path());
        owner.start(QStringLiteral(QSOC_OWNER_PROBE_PATH), {"owner", marker, "0"});
        QVERIFY(owner.waitForStarted(5000));
        QTRY_VERIFY(QFile::exists(marker));
        QFile ready(marker);
        QVERIFY(ready.open(QIODevice::ReadOnly));
        const qint64 childPid = ready.readAll().toLongLong();
        QVERIFY(childPid > 1);
        QVERIFY(processIsRunning(childPid));
        owner.kill();
        QVERIFY(owner.waitForFinished(5000));
        QTRY_VERIFY_WITH_TIMEOUT(!processIsRunning(childPid), 5000);
    }

    void disarmingDoesNotWaitForTheOwnerToExit()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QProcess child;
        child.setWorkingDirectory(directory.path());
        child.start(
            QStringLiteral(QSOC_OWNER_PROBE_PATH),
            {"disarm", "unused", QString::number(QCoreApplication::applicationPid())});
        QVERIFY(child.waitForFinished(5000));
        QCOMPARE(child.exitStatus(), QProcess::NormalExit);
        QCOMPARE(child.exitCode(), 0);
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocprocessowner.moc"
