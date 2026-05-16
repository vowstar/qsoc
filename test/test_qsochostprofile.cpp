// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsochostprofile.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtTest>

namespace {

QString readAll(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QString::fromUtf8(file.readAll());
}

bool writeAll(const QString &path, const QString &content)
{
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return false;
    }
    file.write(content.toUtf8());
    return true;
}

QString joinPath(const QString &dir, const QString &child)
{
    return QDir(dir).absoluteFilePath(child);
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void testLoadEmpty()
    {
        QTemporaryDir userDir;
        QTemporaryDir projectDir;
        QVERIFY(userDir.isValid());
        QVERIFY(projectDir.isValid());

        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());
        QVERIFY(catalog.allList().isEmpty());
        QVERIFY(catalog.active().isLocal());
    }

    void testUpsertWriteThenReload()
    {
        QTemporaryDir   userDir;
        QTemporaryDir   projectDir;
        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());

        QSocHostProfile entry;
        entry.alias      = QStringLiteral("fpga-build");
        entry.workspace  = QStringLiteral("/home/bob/build");
        entry.capability = QStringLiteral("Vivado synthesis");
        entry.target     = QStringLiteral("bob@fpga.lab");

        QSignalSpy spy(&catalog, &QSocHostCatalog::catalogChanged);
        QString    err;
        QVERIFY2(catalog.upsert(entry, false, &err), qPrintable(err));
        QCOMPARE(spy.count(), 1);

        const QString filePath = catalog.projectFilePath();
        QVERIFY(QFile::exists(filePath));

        QSocHostCatalog reloaded;
        reloaded.load(userDir.path(), projectDir.path());
        const auto list = reloaded.allList();
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.first().alias, entry.alias);
        QCOMPARE(list.first().workspace, entry.workspace);
        QCOMPARE(list.first().capability, entry.capability);
        QCOMPARE(list.first().target, entry.target);
        QCOMPARE(list.first().scope, QStringLiteral("project"));
    }

    void testDuplicateAliasRejected()
    {
        QTemporaryDir   userDir;
        QTemporaryDir   projectDir;
        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());

        QSocHostProfile entry;
        entry.alias     = QStringLiteral("gpu-sim");
        entry.workspace = QStringLiteral("/home/alice/sim");
        QVERIFY(catalog.upsert(entry));

        QString err;
        QVERIFY(!catalog.upsert(entry, false, &err));
        QVERIFY(err.contains(QStringLiteral("already exists")));

        QVERIFY(catalog.upsert(entry, true, &err));
        QCOMPARE(catalog.allList().size(), 1);
    }

    void testProjectShadowsUser()
    {
        QTemporaryDir userDir;
        QTemporaryDir projectDir;
        QDir(projectDir.path()).mkdir(QStringLiteral(".qsoc"));

        QVERIFY(writeAll(
            joinPath(userDir.path(), QStringLiteral("host.yml")),
            QStringLiteral(
                "hostList:\n"
                "  - alias: shared\n"
                "    workspace: /user/path\n"
                "    capability: user-scope text\n")));
        QVERIFY(writeAll(
            joinPath(projectDir.path(), QStringLiteral(".qsoc/host.yml")),
            QStringLiteral(
                "hostList:\n"
                "  - alias: shared\n"
                "    workspace: /project/path\n"
                "    capability: project-scope text\n"
                "  - alias: only-user\n"
                "    workspace: /never-this\n")));

        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());
        const auto *shared = catalog.find(QStringLiteral("shared"));
        QVERIFY(shared);
        QCOMPARE(shared->workspace, QStringLiteral("/project/path"));
        QCOMPARE(shared->scope, QStringLiteral("project"));

        const auto all = catalog.allList();
        QCOMPARE(all.size(), 2);
    }

    void testMalformedEntriesSkipped()
    {
        QTemporaryDir userDir;
        QTemporaryDir projectDir;
        QDir(projectDir.path()).mkdir(QStringLiteral(".qsoc"));

        QVERIFY(writeAll(
            joinPath(projectDir.path(), QStringLiteral(".qsoc/host.yml")),
            QStringLiteral(
                "hostList:\n"
                "  - workspace: /no/alias\n"
                "  - alias: ''\n"
                "    workspace: /empty/alias\n"
                "  - alias: good\n"
                "    workspace: /good\n"
                "  - this is not a map\n")));

        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());
        const auto list = catalog.allList();
        QCOMPARE(list.size(), 1);
        QCOMPARE(list.first().alias, QStringLiteral("good"));
    }

    void testActiveAliasRoundTrip()
    {
        QTemporaryDir   userDir;
        QTemporaryDir   projectDir;
        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());

        QSocHostProfile entry;
        entry.alias     = QStringLiteral("a1");
        entry.workspace = QStringLiteral("/w1");
        QVERIFY(catalog.upsert(entry));

        QVERIFY(catalog.setActiveAlias(QStringLiteral("a1")));
        QVERIFY(catalog.active().isAlias());
        QCOMPARE(catalog.active().alias, QStringLiteral("a1"));

        QSocHostCatalog reloaded;
        reloaded.load(userDir.path(), projectDir.path());
        QVERIFY(reloaded.active().isAlias());
        QCOMPARE(reloaded.active().alias, QStringLiteral("a1"));
    }

    void testActiveAdHocRoundTrip()
    {
        QTemporaryDir   userDir;
        QTemporaryDir   projectDir;
        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());

        QVERIFY(
            catalog.setActiveAdHoc(QStringLiteral("bob@fpga.lab"), QStringLiteral("/home/bob/work")));
        QVERIFY(catalog.active().isAdHoc());

        QSocHostCatalog reloaded;
        reloaded.load(userDir.path(), projectDir.path());
        QVERIFY(reloaded.active().isAdHoc());
        QCOMPARE(reloaded.active().adHocTarget, QStringLiteral("bob@fpga.lab"));
        QCOMPARE(reloaded.active().adHocWorkspace, QStringLiteral("/home/bob/work"));
    }

    void testClearActive()
    {
        QTemporaryDir   userDir;
        QTemporaryDir   projectDir;
        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());
        QVERIFY(catalog.setActiveAdHoc(QStringLiteral("u@h"), QStringLiteral("/w")));
        QVERIFY(catalog.clearActive());
        QVERIFY(catalog.active().isLocal());

        QSocHostCatalog reloaded;
        reloaded.load(userDir.path(), projectDir.path());
        QVERIFY(reloaded.active().isLocal());
    }

    void testApplyOpsCapability()
    {
        QTemporaryDir   userDir;
        QTemporaryDir   projectDir;
        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());

        QSocHostProfile entry;
        entry.alias      = QStringLiteral("ax");
        entry.workspace  = QStringLiteral("/w");
        entry.capability = QStringLiteral("alpha");
        QVERIFY(catalog.upsert(entry));

        QList<QSocHostCatalogOp> ops;
        ops.append(
            {.kind = QSocHostCatalogOp::Kind::CapabilityAppend, .value = QStringLiteral("beta")});
        QString err;
        QVERIFY2(catalog.applyOps(QStringLiteral("ax"), ops, &err), qPrintable(err));
        QVERIFY(catalog.find(QStringLiteral("ax"))->capability.contains(QStringLiteral("alpha")));
        QVERIFY(catalog.find(QStringLiteral("ax"))->capability.contains(QStringLiteral("beta")));

        ops.clear();
        ops.append(
            {.kind = QSocHostCatalogOp::Kind::CapabilityRemove, .value = QStringLiteral("alpha")});
        QVERIFY2(catalog.applyOps(QStringLiteral("ax"), ops, &err), qPrintable(err));
        QVERIFY(!catalog.find(QStringLiteral("ax"))->capability.contains(QStringLiteral("alpha")));

        ops.clear();
        ops.append(
            {.kind = QSocHostCatalogOp::Kind::CapabilityReplace, .value = QStringLiteral("gamma")});
        QVERIFY2(catalog.applyOps(QStringLiteral("ax"), ops, &err), qPrintable(err));
        QCOMPARE(catalog.find(QStringLiteral("ax"))->capability, QStringLiteral("gamma"));
    }

    void testApplyOpsRollbackOnFailure()
    {
        QTemporaryDir   userDir;
        QTemporaryDir   projectDir;
        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());

        QSocHostProfile entry;
        entry.alias      = QStringLiteral("ax");
        entry.workspace  = QStringLiteral("/w");
        entry.capability = QStringLiteral("base");
        QVERIFY(catalog.upsert(entry));

        QList<QSocHostCatalogOp> ops;
        ops.append(
            {.kind = QSocHostCatalogOp::Kind::CapabilityAppend, .value = QStringLiteral("extra")});
        ops.append(
            {.kind  = QSocHostCatalogOp::Kind::CapabilityRemove,
             .value = QStringLiteral("nonexistent")});

        QString err;
        QVERIFY(!catalog.applyOps(QStringLiteral("ax"), ops, &err));
        QVERIFY(!err.isEmpty());

        QCOMPARE(catalog.find(QStringLiteral("ax"))->capability, QStringLiteral("base"));

        QSocHostCatalog reloaded;
        reloaded.load(userDir.path(), projectDir.path());
        QCOMPARE(reloaded.find(QStringLiteral("ax"))->capability, QStringLiteral("base"));
    }

    void testApplyOpsMaterializesUserScope()
    {
        QTemporaryDir userDir;
        QTemporaryDir projectDir;
        QVERIFY(writeAll(
            joinPath(userDir.path(), QStringLiteral("host.yml")),
            QStringLiteral(
                "hostList:\n"
                "  - alias: shared\n"
                "    workspace: /user/path\n"
                "    capability: user text\n")));

        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());

        QList<QSocHostCatalogOp> ops;
        ops.append(
            {.kind  = QSocHostCatalogOp::Kind::CapabilityAppend,
             .value = QStringLiteral("project addition")});
        QString err;
        QVERIFY2(catalog.applyOps(QStringLiteral("shared"), ops, &err), qPrintable(err));

        const auto *shared = catalog.find(QStringLiteral("shared"));
        QVERIFY(shared);
        QCOMPARE(shared->scope, QStringLiteral("project"));
        QVERIFY(shared->capability.contains(QStringLiteral("user text")));
        QVERIFY(shared->capability.contains(QStringLiteral("project addition")));

        const QString userYaml = readAll(joinPath(userDir.path(), QStringLiteral("host.yml")));
        QVERIFY(userYaml.contains(QStringLiteral("user text")));
        QVERIFY(!userYaml.contains(QStringLiteral("project addition")));
    }

    void testRemoveProjectClearsActive()
    {
        QTemporaryDir   userDir;
        QTemporaryDir   projectDir;
        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());

        QSocHostProfile entry;
        entry.alias     = QStringLiteral("ax");
        entry.workspace = QStringLiteral("/w");
        QVERIFY(catalog.upsert(entry));
        QVERIFY(catalog.setActiveAlias(QStringLiteral("ax")));

        QVERIFY(catalog.remove(QStringLiteral("ax")));
        QVERIFY(catalog.find(QStringLiteral("ax")) == nullptr);
        QVERIFY(catalog.active().isLocal());
    }

    void testRemoveUserScopeRefused()
    {
        QTemporaryDir userDir;
        QTemporaryDir projectDir;
        QVERIFY(writeAll(
            joinPath(userDir.path(), QStringLiteral("host.yml")),
            QStringLiteral(
                "hostList:\n"
                "  - alias: only-user\n"
                "    workspace: /w\n")));

        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());

        QString err;
        QVERIFY(!catalog.remove(QStringLiteral("only-user"), &err));
        QVERIFY(err.contains(QStringLiteral("user scope")));
    }

    void testRejectMissingRequiredFields()
    {
        QTemporaryDir   userDir;
        QTemporaryDir   projectDir;
        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());

        QSocHostProfile noAlias;
        noAlias.workspace = QStringLiteral("/w");
        QString err;
        QVERIFY(!catalog.upsert(noAlias, false, &err));
        QVERIFY(err.contains(QStringLiteral("alias")));

        QSocHostProfile noWorkspace;
        noWorkspace.alias = QStringLiteral("ax");
        err.clear();
        QVERIFY(!catalog.upsert(noWorkspace, false, &err));
        QVERIFY(err.contains(QStringLiteral("workspace")));
    }
};

} // namespace

#include "test_qsochostprofile.moc"

QSOC_TEST_MAIN(Test)
