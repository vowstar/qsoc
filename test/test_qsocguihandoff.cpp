// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private:
    static QProcessEnvironment withBinDir(const QString &dir)
    {
        auto env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("QSOC_BIN_DIR"), dir);
        return env;
    }

private slots:
    void guiReplacesTheProcessAndKeepsTheOtherArguments()
    {
#ifdef Q_OS_WIN
        QSKIP("Windows starts qsoc-gui detached instead of replacing the process");
#else
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QFile fake(QDir(dir.path()).filePath(QStringLiteral("qsoc-gui")));
        QVERIFY(fake.open(QIODevice::WriteOnly));
        fake.write("#!/bin/sh\necho \"args=$*\"\nexit 7\n");
        fake.close();
        QVERIFY(fake.setPermissions(fake.permissions() | QFileDevice::ExeOwner));

        QProcess qsoc;
        qsoc.setProcessEnvironment(withBinDir(dir.path()));
        qsoc.setWorkingDirectory(dir.path());
        qsoc.start(QStringLiteral(QSOC_BINARY_PATH), {"-d", "project", "gui", "--flag"});
        QVERIFY(qsoc.waitForFinished(10000));
        QCOMPARE(qsoc.exitCode(), 7);
        QCOMPARE(qsoc.readAllStandardOutput().trimmed(), QByteArray("args=-d project --flag"));
#endif
    }

    void missingGuiIsAnError()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QProcess qsoc;
        qsoc.setProcessEnvironment(withBinDir(dir.path()));
        qsoc.setWorkingDirectory(dir.path());
        qsoc.start(QStringLiteral(QSOC_BINARY_PATH), {"gui"});
        QVERIFY(qsoc.waitForFinished(10000));
        QCOMPARE(qsoc.exitCode(), 127);
        QVERIFY(qsoc.readAllStandardError().contains("qsoc-gui was not found"));
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocguihandoff.moc"
