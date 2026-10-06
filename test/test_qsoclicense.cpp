// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/qsoclicense.h"
#include "qsoc_test.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QSignalSpy>
#include <QtTest>

namespace {

QString runCli(const QStringList &arguments, int *exitCode)
{
    QSocTestCapture capture;
    QSocCliWorker   worker;
    QSignalSpy      exitSpy(&worker, &QSocCliWorker::exit);
    worker.setup(QStringList{"qsoc"} + arguments, true);
    if (!exitSpy.wait(5000) && exitSpy.isEmpty()) {
        *exitCode = -1;
        return {};
    }
    *exitCode = exitSpy.takeFirst().at(0).toInt();
    return capture.text();
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void everyExternalDependencyHasAComponent()
    {
        const QString external = QFileInfo(QStringLiteral(__FILE__)).absolutePath()
                                 + QStringLiteral("/../external");
        QVERIFY2(QFileInfo(external + "/z3").isDir(), qPrintable(external));
        const QStringList dirs = QDir(external).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
        QVERIFY(!dirs.isEmpty());
        for (const QString &dir : dirs) {
            QVERIFY2(QSocLicense::find(dir), qPrintable("no license entry for external/" + dir));
            QVERIFY2(
                QFile::exists(":/license/" + dir + ".txt"),
                qPrintable("missing :/license/" + dir + ".txt"));
        }
    }

    void componentsAreSortedAndUnique()
    {
        QStringList names;
        for (const QSocLicense::Component &component : QSocLicense::components()) {
            names.append(component.name);
        }
        QStringList sorted = names;
        sorted.sort();
        sorted.removeDuplicates();
        QCOMPARE(names, sorted);
    }

    void everyResourceLooksLikeALicense()
    {
        QDirIterator files(":/license", QDir::Files);
        int          count = 0;
        while (files.hasNext()) {
            QFile file(files.next());
            QVERIFY(file.open(QIODevice::ReadOnly));
            const QString text = QString::fromUtf8(file.readAll()).toLower();
            QVERIFY2(
                text.contains("license") || text.contains("copyright")
                    || text.contains("public domain"),
                qPrintable(file.fileName()));
            ++count;
        }
        QVERIFY(count > 0);
    }

    void everyResourceBelongsToAComponent()
    {
        QSet<QString> referenced;
        for (const QSocLicense::Component &component : QSocLicense::components()) {
            QVERIFY(!component.license.isEmpty());
            QVERIFY(!component.url.isEmpty());
            for (const QString &name : component.files) {
                QVERIFY2(QFile::exists(":/license/" + name), qPrintable(name));
                referenced.insert(name);
            }
        }
        QDirIterator files(":/license", QDir::Files);
        while (files.hasNext()) {
            const QString name = QFileInfo(files.next()).fileName();
            QVERIFY2(referenced.contains(name), qPrintable("unreferenced :/license/" + name));
        }
    }

    void cliListsEveryComponent()
    {
        int           exitCode = 0;
        const QString output   = runCli({"--licenses"}, &exitCode);
        QCOMPARE(exitCode, 0);
        for (const QSocLicense::Component &component : QSocLicense::components()) {
            QVERIFY2(output.contains(component.name), qPrintable(component.name));
            QVERIFY2(output.contains(component.url), qPrintable(component.url));
        }
    }

    void cliPrintsFullText()
    {
        int           exitCode = 0;
        const QString output   = runCli({"--licenses", "aws-lc", "z3"}, &exitCode);
        QCOMPARE(exitCode, 0);
        QVERIFY(output.contains("New AWS-LC files are Apache-2.0 OR ISC."));
        QVERIFY(output.contains("Apache License"));
        QVERIFY(output.contains("Microsoft Corporation"));
    }

    void cliRejectsUnknownComponent()
    {
        int           exitCode = 0;
        const QString output   = runCli({"--licenses", "no-such-component"}, &exitCode);
        QCOMPARE(exitCode, 1);
        QVERIFY(output.contains("unknown component: no-such-component"));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)

#include "test_qsoclicense.moc"
