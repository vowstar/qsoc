// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
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
    void guiHandoffPreservesArgumentsAndPlatformLifetime()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString filename = QStringLiteral("qsoc-gui")
#ifdef Q_OS_WIN
                                 + QStringLiteral(".exe")
#endif
            ;
        QVERIFY(QFile::copy(QStringLiteral(QSOC_GUI_PROBE_PATH), dir.filePath(filename)));
        const QString output      = dir.filePath("arguments.json");
        const QString release     = dir.filePath("release");
        auto          environment = withBinDir(dir.path());
        environment.insert("QSOC_GUI_PROBE_OUTPUT", output);
#ifdef Q_OS_WIN
        environment.insert("QSOC_GUI_PROBE_RELEASE", release);
#endif

        QProcess qsoc;
        qsoc.setProcessEnvironment(environment);
        qsoc.setWorkingDirectory(dir.path());
        qsoc.start(QStringLiteral(QSOC_BINARY_PATH), {"--color", "never", "gui", "--flag", "a b"});
        QVERIFY(qsoc.waitForStarted(5000));
        const qint64 launcherPid = qsoc.processId();
        QVERIFY(qsoc.waitForFinished(10000));
#ifdef Q_OS_WIN
        QCOMPARE(qsoc.exitCode(), 0);
        QTRY_VERIFY(QFile::exists(output));
        QVERIFY(!QFile::exists(output + ".done"));
        QFile releaseFile(release);
        QVERIFY(releaseFile.open(QIODevice::WriteOnly));
        releaseFile.close();
        QTRY_VERIFY(QFile::exists(output + ".done"));
#else
        QCOMPARE(qsoc.exitCode(), 7);
#endif
        QFile arguments(output);
        QVERIFY(arguments.open(QIODevice::ReadOnly));
        const auto result = QJsonDocument::fromJson(arguments.readAll()).object();
        QCOMPARE(
            result.value("arguments").toArray(), QJsonArray({"--color", "never", "--flag", "a b"}));
#ifdef Q_OS_WIN
        QVERIFY(result.value("pid").toInteger() != launcherPid);
#else
        QCOMPARE(result.value("pid").toInteger(), launcherPid);
#endif
    }

    void otherCommandsAndOptionValuesDoNotLaunchTheGui_data()
    {
        QTest::addColumn<QStringList>("arguments");
        QTest::newRow("project-name") << QStringList{"project", "create", "gui", "--help"};
        QTest::newRow("option-value") << QStringList{"--color", "gui", "--help"};
        QTest::newRow("license-name") << QStringList{"--licenses", "gui"};
        QTest::newRow("version") << QStringList{"--version", "gui"};
    }

    void otherCommandsAndOptionValuesDoNotLaunchTheGui()
    {
        QFETCH(QStringList, arguments);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        QProcess qsoc;
        qsoc.setWorkingDirectory(directory.path());
        qsoc.setProcessEnvironment(withBinDir(directory.path()));
        qsoc.start(QStringLiteral(QSOC_BINARY_PATH), arguments);
        QVERIFY(qsoc.waitForFinished(10000));
        QVERIFY(qsoc.exitCode() != 127);
        QVERIFY(!qsoc.readAllStandardError().contains("qsoc-gui was not found"));
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
