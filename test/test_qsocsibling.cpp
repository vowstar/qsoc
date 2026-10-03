// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocsibling.h"
#include "qsoc_test.h"

#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private slots:
    void findsAnExecutableInTheOverrideDirectory()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
#ifdef Q_OS_WIN
        const QString file = dir.filePath(QStringLiteral("qsoc-probe.exe"));
#else
        const QString file = dir.filePath(QStringLiteral("qsoc-probe"));
#endif
        QFile probe(file);
        QVERIFY(probe.open(QIODevice::WriteOnly));
        probe.write("#!/bin/sh\n");
        probe.close();
        QVERIFY(probe.setPermissions(probe.permissions() | QFileDevice::ExeOwner));
        qputenv("QSOC_BIN_DIR", dir.path().toLocal8Bit());
        QCOMPARE(QFileInfo(QSocSibling::path(QStringLiteral("qsoc-probe"))), QFileInfo(file));
        qunsetenv("QSOC_BIN_DIR");
    }

    void reportsAMissingProgram()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        qputenv("QSOC_BIN_DIR", dir.path().toLocal8Bit());
        QVERIFY(QSocSibling::path(QStringLiteral("qsoc-absent")).isEmpty());
        const QString message = QSocSibling::missingMessage(QStringLiteral("qsoc-absent"));
        QVERIFY(message.contains(QStringLiteral("qsoc-absent")));
        QVERIFY(message.contains(QStringLiteral("QSOC_BIN_DIR")));
        qunsetenv("QSOC_BIN_DIR");
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocsibling.moc"
