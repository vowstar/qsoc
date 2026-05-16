// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsochostprofile.h"
#include "agent/tool/qsoctoolhostcatalog.h"
#include "qsoc_test.h"

#include <QTemporaryDir>
#include <QtTest>

namespace {

class Test : public QObject
{
    Q_OBJECT

private slots:
    void testRegisterCreatesEntry()
    {
        QTemporaryDir   userDir;
        QTemporaryDir   projectDir;
        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());

        QSocToolHostRegister tool(nullptr, &catalog);
        const json           args
            = {{"alias", "fpga-build"},
               {"workspace", "/home/bob/build"},
               {"capability", "Vivado synthesis"},
               {"target", "bob@fpga.lab"}};
        const QString result = tool.execute(args);
        QVERIFY2(!result.startsWith(QStringLiteral("Error")), qPrintable(result));

        const auto *entry = catalog.find(QStringLiteral("fpga-build"));
        QVERIFY(entry);
        QCOMPARE(entry->workspace, QStringLiteral("/home/bob/build"));
        QCOMPARE(entry->target, QStringLiteral("bob@fpga.lab"));
    }

    void testRegisterRejectsDuplicate()
    {
        QTemporaryDir   userDir;
        QTemporaryDir   projectDir;
        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());

        QSocToolHostRegister tool(nullptr, &catalog);
        const json           args = {{"alias", "ax"}, {"workspace", "/w"}};
        QVERIFY(!tool.execute(args).startsWith(QStringLiteral("Error")));

        const QString second = tool.execute(args);
        QVERIFY(second.startsWith(QStringLiteral("Error")));
        QVERIFY(second.contains(QStringLiteral("already exists")));
    }

    void testRegisterRequiresAliasAndWorkspace()
    {
        QTemporaryDir   userDir;
        QTemporaryDir   projectDir;
        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());

        QSocToolHostRegister tool(nullptr, &catalog);
        QString              result = tool.execute({{"workspace", "/w"}});
        QVERIFY(result.startsWith(QStringLiteral("Error")));
        result = tool.execute({{"alias", "ax"}});
        QVERIFY(result.startsWith(QStringLiteral("Error")));
    }

    void testUpdateAppendCapability()
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

        QSocToolHostUpdate tool(nullptr, &catalog);
        const json         args
            = {{"alias", "ax"},
               {"opList", json::array({{{"op", "capability_append"}, {"value", "added line"}}})}};
        const QString result = tool.execute(args);
        QVERIFY2(!result.startsWith(QStringLiteral("Error")), qPrintable(result));

        const auto *got = catalog.find(QStringLiteral("ax"));
        QVERIFY(got);
        QVERIFY(got->capability.contains(QStringLiteral("base")));
        QVERIFY(got->capability.contains(QStringLiteral("added line")));
    }

    void testUpdateRejectsUnknownOp()
    {
        QTemporaryDir   userDir;
        QTemporaryDir   projectDir;
        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());

        QSocHostProfile entry;
        entry.alias     = QStringLiteral("ax");
        entry.workspace = QStringLiteral("/w");
        QVERIFY(catalog.upsert(entry));

        QSocToolHostUpdate tool(nullptr, &catalog);
        const json         args
            = {{"alias", "ax"}, {"opList", json::array({{{"op", "weird_op"}, {"value", "x"}}})}};
        const QString result = tool.execute(args);
        QVERIFY(result.startsWith(QStringLiteral("Error")));
        QVERIFY(result.contains(QStringLiteral("weird_op")));
    }

    void testUpdateAtomicRollback()
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

        QSocToolHostUpdate tool(nullptr, &catalog);
        const json         args
            = {{"alias", "ax"},
               {"opList",
                json::array(
                    {{{"op", "capability_append"}, {"value", "should not stick"}},
                     {{"op", "capability_remove"}, {"value", "phrase not present"}}})}};
        QVERIFY(tool.execute(args).startsWith(QStringLiteral("Error")));

        const auto *got = catalog.find(QStringLiteral("ax"));
        QVERIFY(got);
        QCOMPARE(got->capability, QStringLiteral("base"));
    }

    void testRemoveDropsEntry()
    {
        QTemporaryDir   userDir;
        QTemporaryDir   projectDir;
        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());

        QSocHostProfile entry;
        entry.alias     = QStringLiteral("ax");
        entry.workspace = QStringLiteral("/w");
        QVERIFY(catalog.upsert(entry));

        QSocToolHostRemove tool(nullptr, &catalog);
        const QString      result = tool.execute({{"alias", "ax"}});
        QVERIFY2(!result.startsWith(QStringLiteral("Error")), qPrintable(result));
        QVERIFY(catalog.find(QStringLiteral("ax")) == nullptr);
    }

    void testRemoveMissingAliasError()
    {
        QTemporaryDir   userDir;
        QTemporaryDir   projectDir;
        QSocHostCatalog catalog;
        catalog.load(userDir.path(), projectDir.path());

        QSocToolHostRemove tool(nullptr, &catalog);
        const QString      result = tool.execute({{"alias", "ghost"}});
        QVERIFY(result.startsWith(QStringLiteral("Error")));
    }

    void testSchemasAreObjects()
    {
        QSocToolHostRegister reg(nullptr, nullptr);
        QSocToolHostUpdate   upd(nullptr, nullptr);
        QSocToolHostRemove   rem(nullptr, nullptr);
        QVERIFY(reg.getParametersSchema().is_object());
        QVERIFY(upd.getParametersSchema().is_object());
        QVERIFY(rem.getParametersSchema().is_object());
        QCOMPARE(reg.getName(), QStringLiteral("host_register"));
        QCOMPARE(upd.getName(), QStringLiteral("host_update"));
        QCOMPARE(rem.getName(), QStringLiteral("host_remove"));
    }
};

} // namespace

#include "test_qsoctoolhostcatalog.moc"

QSOC_TEST_MAIN(Test)
