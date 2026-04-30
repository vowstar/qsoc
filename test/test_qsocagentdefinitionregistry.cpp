// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagentdefinition.h"
#include "agent/qsocagentdefinitionregistry.h"

#include <QDir>
#include <QFile>
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

    void testRegisterBuiltinsAddsExploreReadOnly()
    {
        QSocAgentDefinitionRegistry reg;
        reg.registerBuiltins();
        const QSocAgentDefinition *def = reg.find(QStringLiteral("explore"));
        QVERIFY(def != nullptr);
        QCOMPARE(def->scope, QStringLiteral("builtin"));
        QVERIFY(!def->toolsAllow.isEmpty());
        QVERIFY(def->toolsAllow.contains(QStringLiteral("read_file")));
        QVERIFY(def->toolsAllow.contains(QStringLiteral("lsp")));
        /* Must NOT include any write or shell tool. */
        QVERIFY(!def->toolsAllow.contains(QStringLiteral("write_file")));
        QVERIFY(!def->toolsAllow.contains(QStringLiteral("edit_file")));
        QVERIFY(!def->toolsAllow.contains(QStringLiteral("bash")));
        QVERIFY(!def->toolsAllow.contains(QStringLiteral("memory_write")));
        QVERIFY(!def->toolsAllow.contains(QStringLiteral("module_import")));
        QVERIFY(!def->toolsAllow.contains(QStringLiteral("generate_verilog")));
        /* Recursion guard belt-and-braces: definition itself should not
         * list the spawn tool either. */
        QVERIFY(!def->toolsAllow.contains(QStringLiteral("agent")));
    }

    void testRegisterBuiltinsAddsVerificationNoWriteSrc()
    {
        QSocAgentDefinitionRegistry reg;
        reg.registerBuiltins();
        const QSocAgentDefinition *def = reg.find(QStringLiteral("verification"));
        QVERIFY(def != nullptr);
        QVERIFY(def->toolsAllow.contains(QStringLiteral("bash")));
        QVERIFY(def->toolsAllow.contains(QStringLiteral("read_file")));
        QVERIFY(def->toolsAllow.contains(QStringLiteral("lsp")));
        QVERIFY(!def->toolsAllow.contains(QStringLiteral("write_file")));
        QVERIFY(!def->toolsAllow.contains(QStringLiteral("edit_file")));
        QVERIFY(!def->toolsAllow.contains(QStringLiteral("memory_write")));
        QVERIFY(!def->toolsAllow.contains(QStringLiteral("agent")));
    }

    void testBuiltinSetCount()
    {
        QSocAgentDefinitionRegistry reg;
        reg.registerBuiltins();
        QCOMPARE(reg.count(), 3);
        QStringList names = reg.availableNames();
        QVERIFY(names.contains(QStringLiteral("general-purpose")));
        QVERIFY(names.contains(QStringLiteral("explore")));
        QVERIFY(names.contains(QStringLiteral("verification")));
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

    /* === scanFromDisk: markdown loading =================================== */

    static QString writeFile(QTemporaryDir &dir, const QString &name, const QByteArray &content)
    {
        const QString path = dir.filePath(name);
        QDir().mkpath(QFileInfo(path).absolutePath());
        QFile out(path);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            return {};
        }
        out.write(content);
        out.close();
        return path;
    }

    void testScanFromDiskParsesUserMarkdown()
    {
        QTemporaryDir userDir;
        QVERIFY(userDir.isValid());
        writeFile(
            userDir,
            "rtl-explorer.md",
            "---\n"
            "name: rtl-explorer\n"
            "description: Investigate RTL modules and report a structured map.\n"
            "tools: read_file, list_files, path_context\n"
            "model: \n"
            "inject_memory: false\n"
            "inject_skills: true\n"
            "---\n"
            "You are an RTL explorer.\n"
            "\n"
            "Stay read-only.\n");
        QSocAgentDefinitionRegistry reg;
        reg.scanFromDisk(userDir.path(), QString());

        const QSocAgentDefinition *def = reg.find(QStringLiteral("rtl-explorer"));
        QVERIFY(def != nullptr);
        QCOMPARE(def->scope, QStringLiteral("user"));
        QCOMPARE(
            def->description,
            QStringLiteral("Investigate RTL modules and report a structured map."));
        QCOMPARE(def->toolsAllow.size(), 3);
        QVERIFY(def->toolsAllow.contains(QStringLiteral("read_file")));
        QVERIFY(def->toolsAllow.contains(QStringLiteral("path_context")));
        QVERIFY(!def->injectMemory);
        QVERIFY(def->injectSkills);
        QVERIFY(def->injectProjectMd); /* default true */
        QVERIFY(def->promptBody.contains(QStringLiteral("RTL explorer")));
        QVERIFY(def->parseError.isEmpty());
    }

    void testScanFromDiskAcceptsYamlListTools()
    {
        QTemporaryDir userDir;
        writeFile(
            userDir,
            "blocky.md",
            "---\n"
            "name: blocky\n"
            "description: Test list-form tools field.\n"
            "tools:\n"
            "  - read_file\n"
            "  - list_files\n"
            "---\n"
            "Body.\n");
        QSocAgentDefinitionRegistry reg;
        reg.scanFromDisk(userDir.path(), QString());
        const QSocAgentDefinition *def = reg.find(QStringLiteral("blocky"));
        QVERIFY(def != nullptr);
        QCOMPARE(def->toolsAllow.size(), 2);
        QVERIFY(def->toolsAllow.contains(QStringLiteral("read_file")));
        QVERIFY(def->toolsAllow.contains(QStringLiteral("list_files")));
    }

    void testScanFromDiskProjectShadowsUserShadowsBuiltin()
    {
        QTemporaryDir userDir;
        QTemporaryDir projDir;
        writeFile(
            userDir,
            "general-purpose.md",
            "---\n"
            "name: general-purpose\n"
            "description: USER override.\n"
            "---\n"
            "user body\n");
        writeFile(
            projDir,
            "general-purpose.md",
            "---\n"
            "name: general-purpose\n"
            "description: PROJECT override.\n"
            "---\n"
            "project body\n");

        QSocAgentDefinitionRegistry reg;
        reg.registerBuiltins();
        reg.scanFromDisk(userDir.path(), projDir.path());
        const QSocAgentDefinition *def = reg.find(QStringLiteral("general-purpose"));
        QVERIFY(def != nullptr);
        QCOMPARE(def->scope, QStringLiteral("project"));
        QCOMPARE(def->description, QStringLiteral("PROJECT override."));
    }

    void testScanFromDiskCapturesParseErrors()
    {
        QTemporaryDir userDir;
        writeFile(userDir, "no-frontmatter.md", "Just plain content, no leading dashes.\n");
        writeFile(
            userDir,
            "no-name.md",
            "---\n"
            "description: missing name field.\n"
            "---\n"
            "Body.\n");
        QSocAgentDefinitionRegistry reg;
        reg.scanFromDisk(userDir.path(), QString());

        const auto broken = reg.brokenDefinitions();
        QVERIFY(broken.size() >= 1);
        bool sawNoFrontmatter = false;
        for (const QSocAgentDefinition &def : broken) {
            if (def.sourcePath.endsWith(QStringLiteral("no-frontmatter.md"))) {
                QVERIFY(def.parseError.contains(QStringLiteral("frontmatter")));
                sawNoFrontmatter = true;
            }
        }
        QVERIFY(sawNoFrontmatter);
    }

    void testScanFromDiskNameDefaultsToFileStem()
    {
        QTemporaryDir userDir;
        writeFile(
            userDir,
            "stem-default.md",
            "---\n"
            "description: Has no name field; default to file stem.\n"
            "---\n"
            "Body.\n");
        QSocAgentDefinitionRegistry reg;
        reg.scanFromDisk(userDir.path(), QString());
        QVERIFY(reg.find(QStringLiteral("stem-default")) != nullptr);
    }

    void testScanFromDiskMissingDirsAreSilent()
    {
        QSocAgentDefinitionRegistry reg;
        reg.registerBuiltins();
        const int before = reg.count();
        /* Both paths point at non-existent locations. */
        reg.scanFromDisk(
            QStringLiteral("/nonexistent/qsoc/agents"),
            QStringLiteral("/also/nonexistent/.qsoc/agents"));
        QCOMPARE(reg.count(), before);
        QVERIFY(reg.brokenDefinitions().isEmpty());
    }

    void testScanFromDiskParsesDisallowedTools()
    {
        QTemporaryDir userDir;
        writeFile(
            userDir,
            "deny.md",
            "---\n"
            "name: deny\n"
            "description: tools + disallowed_tools combined.\n"
            "tools: read_file, list_files, bash\n"
            "disallowed_tools: bash, write_file\n"
            "---\n"
            "Body.\n");
        QSocAgentDefinitionRegistry reg;
        reg.scanFromDisk(userDir.path(), QString());
        const QSocAgentDefinition *def = reg.find(QStringLiteral("deny"));
        QVERIFY(def != nullptr);
        QCOMPARE(def->toolsAllow.size(), 3);
        QCOMPARE(def->toolsDeny.size(), 2);
        QVERIFY(def->toolsDeny.contains(QStringLiteral("bash")));
        QVERIFY(def->toolsDeny.contains(QStringLiteral("write_file")));
    }

    /* The camelCase claude-code spelling is also accepted as a
     * convenience. */
    void testScanFromDiskAcceptsCamelDisallowedTools()
    {
        QTemporaryDir userDir;
        writeFile(
            userDir,
            "camel.md",
            "---\n"
            "name: camel\n"
            "disallowedTools:\n"
            "  - lsp\n"
            "  - memory_write\n"
            "---\n"
            "Body.\n");
        QSocAgentDefinitionRegistry reg;
        reg.scanFromDisk(userDir.path(), QString());
        const QSocAgentDefinition *def = reg.find(QStringLiteral("camel"));
        QVERIFY(def != nullptr);
        QCOMPARE(def->toolsDeny.size(), 2);
        QVERIFY(def->toolsDeny.contains(QStringLiteral("lsp")));
    }

    void testScanFromDiskInjectFlagsDefaultsAndOverrides()
    {
        QTemporaryDir userDir;
        writeFile(
            userDir,
            "all-flags.md",
            "---\n"
            "name: all-flags\n"
            "description: flags test.\n"
            "inject_memory: yes\n"
            "inject_skills: TRUE\n"
            "inject_project_md: false\n"
            "---\n"
            "Body.\n");
        QSocAgentDefinitionRegistry reg;
        reg.scanFromDisk(userDir.path(), QString());
        const QSocAgentDefinition *def = reg.find(QStringLiteral("all-flags"));
        QVERIFY(def != nullptr);
        QVERIFY(def->injectMemory);
        QVERIFY(def->injectSkills);
        QVERIFY(!def->injectProjectMd);
    }
};

} // namespace

QTEST_MAIN(Test)
#include "test_qsocagentdefinitionregistry.moc"
