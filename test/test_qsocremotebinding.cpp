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

    void testReadAbsentReturnsEmptyEntry()
    {
        QTemporaryDir tmp;
        const auto    entry = QSocRemoteBinding::read(tmp.path());
        QVERIFY(entry.target.isEmpty());
        QVERIFY(entry.workspace.isEmpty());
    }

    void testWriteThenReadBothFields()
    {
        QTemporaryDir            tmp;
        QSocRemoteBinding::Entry entry;
        entry.target    = QStringLiteral("circuitnuggets@10.34.0.186:22");
        entry.workspace = QStringLiteral("/home/circuitnuggets/work");

        QString err;
        QVERIFY(QSocRemoteBinding::write(tmp.path(), entry, &err));
        QVERIFY(err.isEmpty());

        const auto got = QSocRemoteBinding::read(tmp.path());
        QCOMPARE(got.target, entry.target);
        QCOMPARE(got.workspace, entry.workspace);
        QVERIFY(QFileInfo::exists(tmp.path() + QStringLiteral("/.qsoc/remote.yml")));
    }

    void testUpdatePreservesSiblingKeys()
    {
        QTemporaryDir tmp;
        QDir(tmp.path()).mkpath(QStringLiteral(".qsoc"));
        const QString path = tmp.path() + QStringLiteral("/.qsoc/remote.yml");
        QFile         fp(path);
        QVERIFY(fp.open(QIODevice::WriteOnly | QIODevice::Truncate));
        fp.write("note: keep-me\ntarget: old\nworkspace: /old/path\n");
        fp.close();

        QSocRemoteBinding::Entry entry;
        entry.target    = QStringLiteral("new");
        entry.workspace = QStringLiteral("/new/path");
        QVERIFY(QSocRemoteBinding::write(tmp.path(), entry, nullptr));

        const auto got = QSocRemoteBinding::read(tmp.path());
        QCOMPARE(got.target, entry.target);
        QCOMPARE(got.workspace, entry.workspace);

        QFile reread(path);
        QVERIFY(reread.open(QIODevice::ReadOnly));
        const QByteArray body = reread.readAll();
        QVERIFY(body.contains("note: keep-me"));
        QVERIFY(body.contains("target: new"));
        QVERIFY(body.contains("workspace: /new/path"));
    }

    void testWriteEmptyFieldDropsIt()
    {
        QTemporaryDir            tmp;
        QSocRemoteBinding::Entry entry;
        entry.target    = QStringLiteral("t");
        entry.workspace = QStringLiteral("/r");
        QVERIFY(QSocRemoteBinding::write(tmp.path(), entry, nullptr));

        QSocRemoteBinding::Entry rootOnly;
        rootOnly.workspace = QStringLiteral("/r2");
        QVERIFY(QSocRemoteBinding::write(tmp.path(), rootOnly, nullptr));

        const auto got = QSocRemoteBinding::read(tmp.path());
        QVERIFY(got.target.isEmpty());
        QCOMPARE(got.workspace, QStringLiteral("/r2"));
    }

    void testClearDeletesLonelyFile()
    {
        QTemporaryDir            tmp;
        QSocRemoteBinding::Entry entry;
        entry.target    = QStringLiteral("t");
        entry.workspace = QStringLiteral("/r");
        QVERIFY(QSocRemoteBinding::write(tmp.path(), entry, nullptr));
        QVERIFY(QSocRemoteBinding::clear(tmp.path(), nullptr));
        QVERIFY(!QFileInfo::exists(tmp.path() + QStringLiteral("/.qsoc/remote.yml")));
    }

    void testClearKeepsFileWithSiblings()
    {
        QTemporaryDir tmp;
        QDir(tmp.path()).mkpath(QStringLiteral(".qsoc"));
        const QString path = tmp.path() + QStringLiteral("/.qsoc/remote.yml");
        QFile         fp(path);
        QVERIFY(fp.open(QIODevice::WriteOnly | QIODevice::Truncate));
        fp.write("note: still here\ntarget: lab\nworkspace: /r\n");
        fp.close();

        QVERIFY(QSocRemoteBinding::clear(tmp.path(), nullptr));
        QVERIFY(QFileInfo::exists(path));
        const auto got = QSocRemoteBinding::read(tmp.path());
        QVERIFY(got.target.isEmpty());
        QVERIFY(got.workspace.isEmpty());
    }

    void testClearAbsentIsNoop()
    {
        QTemporaryDir tmp;
        QVERIFY(QSocRemoteBinding::clear(tmp.path(), nullptr));
    }

    void testMalformedFileReadsAsEmpty()
    {
        QTemporaryDir tmp;
        QDir(tmp.path()).mkpath(QStringLiteral(".qsoc"));
        const QString path = tmp.path() + QStringLiteral("/.qsoc/remote.yml");
        QFile         fp(path);
        QVERIFY(fp.open(QIODevice::WriteOnly | QIODevice::Truncate));
        fp.write("{{{ invalid yaml");
        fp.close();
        const auto got = QSocRemoteBinding::read(tmp.path());
        QVERIFY(got.target.isEmpty());
        QVERIFY(got.workspace.isEmpty());
    }
};

} // namespace

QTEST_GUILESS_MAIN(Test)
#include "test_qsocremotebinding.moc"
