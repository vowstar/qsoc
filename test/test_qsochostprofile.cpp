// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsochostprofile.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
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
        QVERIFY(!catalog.projectNamesActive());
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

    /* Counterexample: `active:` in the project file chose where qsoc
     * connected, so a cloned repository could aim it at any host. It is
     * reported, never acted on, and a catalog write leaves it as written. */
    void testProjectActiveIsReportedAndPreserved()
    {
        QTemporaryDir userDir;
        QTemporaryDir projectDir;
        QVERIFY(QDir().mkpath(joinPath(projectDir.path(), QStringLiteral(".qsoc"))));
        const QString path = joinPath(projectDir.path(), QStringLiteral(".qsoc/host.yml"));
        QVERIFY(writeAll(path, QStringLiteral("active:\n  target: t1\n  workspace: /w1\n")));

        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());
        QVERIFY(catalog.projectNamesActive());

        QSocHostProfile entry;
        entry.alias     = QStringLiteral("a1");
        entry.workspace = QStringLiteral("/w1");
        QVERIFY(catalog.upsert(entry));
        const QString written = readAll(path);
        QVERIFY2(written.contains(QStringLiteral("target: t1")), qPrintable(written));
        QVERIFY2(written.contains(QStringLiteral("alias: a1")), qPrintable(written));
    }

    void testUserActiveIsNotReported()
    {
        QTemporaryDir userDir;
        QTemporaryDir projectDir;
        QVERIFY(writeAll(
            joinPath(userDir.path(), QStringLiteral("host.yml")), QStringLiteral("active: a1\n")));
        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());
        QVERIFY(!catalog.projectNamesActive());
    }

    void testBindingRoundTripPerProject()
    {
        QTemporaryDir store;
        QTemporaryDir first;
        QTemporaryDir second;
        QVERIFY(QSocHostBindingStore::load(store.path(), first.path()).isLocal());

        QString error;
        QVERIFY2(
            QSocHostBindingStore::save(
                store.path(), first.path(), {QStringLiteral("t1"), QStringLiteral("/w1")}, &error),
            qPrintable(error));
        const QSocHostBinding loaded = QSocHostBindingStore::load(store.path(), first.path());
        QCOMPARE(loaded.target, QStringLiteral("t1"));
        QCOMPARE(loaded.workspace, QStringLiteral("/w1"));
        QVERIFY(QSocHostBindingStore::load(store.path(), second.path()).isLocal());

        /* The binding lives in the store, never in the project tree. */
        QVERIFY(!QFileInfo::exists(joinPath(first.path(), QStringLiteral(".qsoc"))));
#ifdef Q_OS_UNIX
        const QString file = QSocHostBindingStore::filePath(store.path(), first.path());
        QCOMPARE(
            QFileInfo(file).permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
            QFileDevice::Permissions());
#endif
    }

    void testBindingWithoutProjectIsNotStored()
    {
        QTemporaryDir store;
        QString       error;
        QVERIFY(!QSocHostBindingStore::save(
            store.path(), QString(), {QStringLiteral("t1"), QStringLiteral("/w1")}, &error));
        QVERIFY(QDir(store.path()).isEmpty());
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
