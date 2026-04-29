// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagentdefinition.h"
#include "agent/qsocagentdefinitionregistry.h"

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

    void testEmptyRegistry()
    {
        QSocAgentDefinitionRegistry reg;
        QCOMPARE(reg.count(), 0);
        QVERIFY(reg.availableNames().isEmpty());
        QCOMPARE(reg.find(QStringLiteral("nonexistent")), nullptr);
    }

    void testRegisterBuiltinsAddsGeneralPurpose()
    {
        QSocAgentDefinitionRegistry reg;
        reg.registerBuiltins();
        QVERIFY(reg.count() >= 1);
        const QSocAgentDefinition *def = reg.find(QStringLiteral("general-purpose"));
        QVERIFY(def != nullptr);
        QCOMPARE(def->name, QStringLiteral("general-purpose"));
        QCOMPARE(def->scope, QStringLiteral("builtin"));
        QVERIFY(!def->description.isEmpty());
        QVERIFY(!def->promptBody.isEmpty());
        QVERIFY(def->injectProjectMd);
        QVERIFY(!def->injectMemory);
        QVERIFY(!def->injectSkills);
        QVERIFY(def->parseError.isEmpty());
    }

    void testRegisterBuiltinsIsIdempotent()
    {
        QSocAgentDefinitionRegistry reg;
        reg.registerBuiltins();
        const int firstCount = reg.count();
        reg.registerBuiltins();
        QCOMPARE(reg.count(), firstCount);
    }

    void testRegisterReplacesByName()
    {
        QSocAgentDefinitionRegistry reg;
        QSocAgentDefinition         a;
        a.name        = QStringLiteral("custom");
        a.description = QStringLiteral("first");
        a.promptBody  = QStringLiteral("PROMPT A");
        reg.registerDefinition(a);

        QSocAgentDefinition b;
        b.name        = QStringLiteral("custom");
        b.description = QStringLiteral("second");
        b.promptBody  = QStringLiteral("PROMPT B");
        reg.registerDefinition(b);

        QCOMPARE(reg.count(), 1);
        const QSocAgentDefinition *latest = reg.find(QStringLiteral("custom"));
        QVERIFY(latest != nullptr);
        QCOMPARE(latest->description, QStringLiteral("second"));
        QCOMPARE(latest->promptBody, QStringLiteral("PROMPT B"));
    }

    void testAvailableNamesIsSorted()
    {
        QSocAgentDefinitionRegistry reg;
        QSocAgentDefinition         x;
        x.name = QStringLiteral("zeta");
        reg.registerDefinition(x);
        QSocAgentDefinition y;
        y.name = QStringLiteral("alpha");
        reg.registerDefinition(y);
        QSocAgentDefinition z;
        z.name = QStringLiteral("middle");
        reg.registerDefinition(z);

        const QStringList names = reg.availableNames();
        QCOMPARE(
            names,
            QStringList(
                {QStringLiteral("alpha"), QStringLiteral("middle"), QStringLiteral("zeta")}));
    }

    void testDescribeAvailableShape()
    {
        QSocAgentDefinitionRegistry reg;
        reg.registerBuiltins();
        const QString text = reg.describeAvailable();
        QVERIFY(text.contains(QStringLiteral("- general-purpose: ")));
        QVERIFY(!text.endsWith(QLatin1Char('\n')));
    }
};

} // namespace

QTEST_MAIN(Test)
#include "test_qsocagentdefinitionregistry.moc"
