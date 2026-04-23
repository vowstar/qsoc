// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocremotebinding.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

namespace {

struct TestApp
{
    static auto &instance()
    {
        static auto                   argc      = 1;
        static char                   appName[] = "qsoc";
        static std::array<char *, 1>  argv      = {{appName}};
        static const QCoreApplication app       = QCoreApplication(argc, argv.data());
        return app;
    }
};

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase() { TestApp::instance(); }

    void testPathComposition()
    {
        QCOMPARE(
            QSocRemoteBinding::pathFor(QStringLiteral("/tmp/proj")),
            QStringLiteral("/tmp/proj/.qsoc/remote.yml"));
        QCOMPARE(QSocRemoteBinding::pathFor(QString()), QString());
    }

    void testReadAbsentReturnsEmpty()
    {
        QTemporaryDir tmp;
        QCOMPARE(QSocRemoteBinding::readTarget(tmp.path()), QString());
    }

    void testWriteThenRead()
    {
        QTemporaryDir tmp;
        QString       err;
        QVERIFY(QSocRemoteBinding::writeTarget(tmp.path(), QStringLiteral("lab"), &err));
        QVERIFY(err.isEmpty());
        QCOMPARE(QSocRemoteBinding::readTarget(tmp.path()), QStringLiteral("lab"));
        QVERIFY(QFileInfo::exists(tmp.path() + QStringLiteral("/.qsoc/remote.yml")));
    }

    void testWriteUpdatePreservesSiblingKeys()
    {
        QTemporaryDir tmp;
        QDir(tmp.path()).mkpath(QStringLiteral(".qsoc"));
        /* Pre-populate with a sibling field that should survive updates. */
        const QString path = tmp.path() + QStringLiteral("/.qsoc/remote.yml");
        QFile         f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("note: keep-me\ntarget: old\n");
        f.close();

        QVERIFY(QSocRemoteBinding::writeTarget(tmp.path(), QStringLiteral("new"), nullptr));

        /* target updated, note preserved. */
        QCOMPARE(QSocRemoteBinding::readTarget(tmp.path()), QStringLiteral("new"));

        QFile reread(path);
        QVERIFY(reread.open(QIODevice::ReadOnly));
        const QByteArray body = reread.readAll();
        QVERIFY(body.contains("note: keep-me"));
        QVERIFY(body.contains("target: new"));
    }

    void testRemoveDeletesLonelyFile()
    {
        QTemporaryDir tmp;
        QVERIFY(QSocRemoteBinding::writeTarget(tmp.path(), QStringLiteral("lab"), nullptr));
        QVERIFY(QSocRemoteBinding::removeTarget(tmp.path(), nullptr));
        QVERIFY(!QFileInfo::exists(tmp.path() + QStringLiteral("/.qsoc/remote.yml")));
    }

    void testRemoveKeepsFileWithSiblings()
    {
        QTemporaryDir tmp;
        QDir(tmp.path()).mkpath(QStringLiteral(".qsoc"));
        const QString path = tmp.path() + QStringLiteral("/.qsoc/remote.yml");
        QFile         f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("note: still here\ntarget: lab\n");
        f.close();

        QVERIFY(QSocRemoteBinding::removeTarget(tmp.path(), nullptr));
        QVERIFY(QFileInfo::exists(path));
        QCOMPARE(QSocRemoteBinding::readTarget(tmp.path()), QString());
    }

    void testRemoveAbsentIsNoop()
    {
        QTemporaryDir tmp;
        QVERIFY(QSocRemoteBinding::removeTarget(tmp.path(), nullptr));
    }

    void testMalformedFileReadsAsEmpty()
    {
        QTemporaryDir tmp;
        QDir(tmp.path()).mkpath(QStringLiteral(".qsoc"));
        const QString path = tmp.path() + QStringLiteral("/.qsoc/remote.yml");
        QFile         f(path);
        QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
        f.write("{{{ invalid yaml");
        f.close();
        QCOMPARE(QSocRemoteBinding::readTarget(tmp.path()), QString());
    }
};

} // namespace

QTEST_GUILESS_MAIN(Test)
#include "test_qsocremotebinding.moc"
