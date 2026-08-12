// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocshellpath.h"

#include <QtCore>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void cleanup()
    {
        qunsetenv("QSOC_GIT_BASH_PATH");
        QSocShellPath::resetCache();
    }

    /* toPosixPath: pure string transform, testable on every platform */
    void posixPathDriveLetter()
    {
        QCOMPARE(
            QSocShellPath::toPosixPath(QStringLiteral(R"(C:\Users\foo)")),
            QStringLiteral("/c/Users/foo"));
        QCOMPARE(
            QSocShellPath::toPosixPath(QStringLiteral("D:/Work/proj")),
            QStringLiteral("/d/Work/proj"));
    }

    void posixPathUnc()
    {
        QCOMPARE(
            QSocShellPath::toPosixPath(QStringLiteral(R"(\\server\share\dir)")),
            QStringLiteral("//server/share/dir"));
    }

    void posixPathPassthrough()
    {
        QCOMPARE(
            QSocShellPath::toPosixPath(QStringLiteral("/home/user/x")),
            QStringLiteral("/home/user/x"));
        QCOMPARE(
            QSocShellPath::toPosixPath(QStringLiteral(R"(rel\sub\file)")),
            QStringLiteral("rel/sub/file"));
        QCOMPARE(QSocShellPath::toPosixPath(QString()), QString());
    }

    void posixPathDriveRootOnly()
    {
        QCOMPARE(QSocShellPath::toPosixPath(QStringLiteral(R"(C:\)")), QStringLiteral("/c/"));
    }

    /* gitBashCandidates: layout derivation without filesystem access */
    void gitBashCandidatesFromCmdDir()
    {
        const QStringList out = QSocShellPath::gitBashCandidates(
            QStringLiteral("/opt/Git/cmd/git.exe"));
        QVERIFY(out.contains(QStringLiteral("/opt/Git/bin/bash.exe")));
        QVERIFY(out.contains(QStringLiteral("/opt/Git/usr/bin/bash.exe")));
    }

    void gitBashCandidatesFromMingwDir()
    {
        const QStringList out = QSocShellPath::gitBashCandidates(
            QStringLiteral("/opt/Git/mingw64/bin/git.exe"));
        QVERIFY(out.contains(QStringLiteral("/opt/Git/bin/bash.exe")));
    }

    void gitBashCandidatesEmpty()
    {
        QVERIFY(QSocShellPath::gitBashCandidates(QString()).isEmpty());
    }

    /* bashPath: discovery behavior */
    void bashPathFindsSystemShell()
    {
#ifdef Q_OS_WIN
        QSKIP("POSIX discovery path");
#endif
        const QString shell = QSocShellPath::bashPath();
        QVERIFY(!shell.isEmpty());
        QVERIFY(QFileInfo(shell).isAbsolute());
        QVERIFY(QFileInfo::exists(shell));
    }

    void bashPathCached()
    {
        const QString first = QSocShellPath::bashPath();
        /* Env changes after first resolution must not change the result
         * until resetCache(). */
        qputenv("QSOC_GIT_BASH_PATH", "/nonexistent/bash");
        QCOMPARE(QSocShellPath::bashPath(), first);
    }

    void overrideHonored()
    {
#ifdef Q_OS_WIN
        QSKIP("POSIX-shaped override fixture");
#endif
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString fake = dir.filePath(QStringLiteral("mybash"));
        {
            QFile file(fake);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("#!/bin/sh\n");
        }
        QVERIFY(QFile::setPermissions(fake, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner));
        qputenv("QSOC_GIT_BASH_PATH", fake.toUtf8());
        QSocShellPath::resetCache();
        QCOMPARE(QSocShellPath::bashPath(), fake);
    }

    void overrideFailClosed()
    {
        /* A pinned interpreter that is missing must yield empty, not a
         * silent fallback to a different shell. */
        qputenv("QSOC_GIT_BASH_PATH", "/nonexistent/dir/bash");
        QSocShellPath::resetCache();
        QVERIFY(QSocShellPath::bashPath().isEmpty());
    }

    void overrideRejectsNonExecutable()
    {
#ifdef Q_OS_WIN
        QSKIP("POSIX permission fixture");
#endif
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString plain = dir.filePath(QStringLiteral("notexec"));
        {
            QFile file(plain);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("data\n");
        }
        QVERIFY(QFile::setPermissions(plain, QFile::ReadOwner | QFile::WriteOwner));
        qputenv("QSOC_GIT_BASH_PATH", plain.toUtf8());
        QSocShellPath::resetCache();
        QVERIFY(QSocShellPath::bashPath().isEmpty());
    }

    void toShellPathIdentityOnUnix()
    {
#ifdef Q_OS_WIN
        QCOMPARE(QSocShellPath::toShellPath(QStringLiteral(R"(C:\x\y)")), QStringLiteral("/c/x/y"));
#else
        QCOMPARE(QSocShellPath::toShellPath(QStringLiteral("/home/u/x")), QStringLiteral("/home/u/x"));
#endif
    }
};

QTEST_APPLESS_MAIN(Test)
#include "test_qsocshellpath.moc"
