// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolskill.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtCore>
#include <QtTest>

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

private:
    QTemporaryDir       tempDir;
    QSocProjectManager *projectManager = nullptr;

    void createSkillFile(
        const QString &basePath,
        const QString &name,
        const QString &description,
        const QString &body)
    {
        QString skillDir = QDir(basePath).filePath("skills/" + name);
        QDir().mkpath(skillDir);

        QFile file(QDir(skillDir).filePath("SKILL.md"));
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream out(&file);
        out << "---\n";
        out << "name: " << name << "\n";
        out << "description: " << description << "\n";
        out << "---\n\n";
        out << body << "\n";
        file.close();
    }

private slots:
    void initTestCase()
    {
        TestApp::instance();
        QVERIFY(tempDir.isValid());

        projectManager = new QSocProjectManager(this);
        projectManager->setProjectPath(tempDir.path());
    }

    void cleanupTestCase() { delete projectManager; }

    /* skill_find: list empty directory */
    void testFindListEmpty()
    {
        QSocToolSkillFind tool(this, projectManager);
        json              args = {{"action", "list"}, {"scope", "project"}};

        QString result = tool.execute(args);
        QVERIFY(result.contains("No skills found"));
    }

    /* skill_find: list with skills */
    void testFindListWithSkills()
    {
        QString projectQsoc = QDir(tempDir.path()).filePath(".qsoc");
        createSkillFile(projectQsoc, "test-skill", "A test skill", "Do something useful.");

        QSocToolSkillFind tool(this, projectManager);
        json              args = {{"action", "list"}, {"scope", "project"}};

        QString result = tool.execute(args);
        QVERIFY(result.contains("1 skill(s)"));
        QVERIFY(result.contains("test-skill"));
        QVERIFY(result.contains("A test skill"));
        QVERIFY(result.contains("project"));
    }

    /* skill_find: search by name */
    void testFindSearchByName()
    {
        QSocToolSkillFind tool(this, projectManager);
        json              args = {{"action", "search"}, {"query", "test"}, {"scope", "project"}};

        QString result = tool.execute(args);
        QVERIFY(result.contains("test-skill"));
    }

    /* skill_find: search by description */
    void testFindSearchByDescription()
    {
        QSocToolSkillFind tool(this, projectManager);
        json              args = {{"action", "search"}, {"query", "A test"}, {"scope", "project"}};

        QString result = tool.execute(args);
        QVERIFY(result.contains("test-skill"));
    }

    /* skill_find: search no results */
    void testFindSearchNoResults()
    {
        QSocToolSkillFind tool(this, projectManager);
        json args = {{"action", "search"}, {"query", "nonexistent-xyz"}, {"scope", "project"}};

        QString result = tool.execute(args);
        QVERIFY(result.contains("No matching skills"));
    }

    /* skill_find: read full content */
    void testFindReadContent()
    {
        QSocToolSkillFind tool(this, projectManager);
        json args = {{"action", "read"}, {"query", "test-skill"}, {"scope", "project"}};

        QString result = tool.execute(args);
        QVERIFY(result.contains("test-skill"));
        QVERIFY(result.contains("Do something useful."));
        QVERIFY(result.contains("SKILL.md"));
    }

    /* skill_find: read nonexistent skill */
    void testFindReadNotFound()
    {
        QSocToolSkillFind tool(this, projectManager);
        json args = {{"action", "read"}, {"query", "no-such-skill"}, {"scope", "project"}};

        QString result = tool.execute(args);
        QVERIFY(result.contains("Error:"));
        QVERIFY(result.contains("not found"));
    }

    /* skill_find: missing action */
    void testFindMissingAction()
    {
        QSocToolSkillFind tool(this, projectManager);
        json              args = json::object();

        QString result = tool.execute(args);
        QVERIFY(result.contains("Error:"));
        QVERIFY(result.contains("action"));
    }

    /* skill_find: search missing query */
    void testFindSearchMissingQuery()
    {
        QSocToolSkillFind tool(this, projectManager);
        json              args = {{"action", "search"}};

        QString result = tool.execute(args);
        QVERIFY(result.contains("Error:"));
        QVERIFY(result.contains("query"));
    }

    /* skill_create: normal creation */
    void testCreateNormal()
    {
        QSocToolSkillCreate tool(this, projectManager);
        json                args
            = {{"name", "new-skill"},
               {"description", "A brand new skill"},
               {"instructions", "# New Skill\n\nFollow these steps:\n1. Do X\n2. Do Y\n"},
               {"scope", "project"}};

        QString result = tool.execute(args);
        QVERIFY(result.contains("Successfully"));
        QVERIFY(result.contains("new-skill"));

        /* Verify file content */
        QString skillFile = QDir(tempDir.path()).filePath(".qsoc/skills/new-skill/SKILL.md");
        QFile   file(skillFile);
        QVERIFY(file.exists());
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        QString content = QTextStream(&file).readAll();
        file.close();

        QVERIFY(content.contains("name: new-skill"));
        QVERIFY(content.contains("description: A brand new skill"));
        QVERIFY(content.contains("# New Skill"));
    }

    /* skill_create: invalid name (uppercase) */
    void testCreateInvalidNameUppercase()
    {
        QSocToolSkillCreate tool(this, projectManager);
        json                args
            = {{"name", "Bad-Name"},
               {"description", "test"},
               {"instructions", "test"},
               {"scope", "project"}};

        QString result = tool.execute(args);
        QVERIFY(result.contains("Error:"));
        QVERIFY(result.contains("Invalid skill name"));
    }

    /* skill_create: invalid name (consecutive hyphens) */
    void testCreateInvalidNameConsecutiveHyphens()
    {
        QSocToolSkillCreate tool(this, projectManager);
        json                args
            = {{"name", "bad--name"},
               {"description", "test"},
               {"instructions", "test"},
               {"scope", "project"}};

        QString result = tool.execute(args);
        QVERIFY(result.contains("Error:"));
    }

    /* skill_create: invalid name (leading hyphen) */
    void testCreateInvalidNameLeadingHyphen()
    {
        QSocToolSkillCreate tool(this, projectManager);
        json                args
            = {{"name", "-bad"},
               {"description", "test"},
               {"instructions", "test"},
               {"scope", "project"}};

        QString result = tool.execute(args);
        QVERIFY(result.contains("Error:"));
    }

    /* skill_create: invalid name (trailing hyphen) */
    void testCreateInvalidNameTrailingHyphen()
    {
        QSocToolSkillCreate tool(this, projectManager);
        json                args
            = {{"name", "bad-"},
               {"description", "test"},
               {"instructions", "test"},
               {"scope", "project"}};

        QString result = tool.execute(args);
        QVERIFY(result.contains("Error:"));
    }

    /* skill_create: name too long */
    void testCreateNameTooLong()
    {
        QSocToolSkillCreate tool(this, projectManager);
        QString             longName = QString("a").repeated(65);
        json                args
            = {{"name", longName.toStdString()},
               {"description", "test"},
               {"instructions", "test"},
               {"scope", "project"}};

        QString result = tool.execute(args);
        QVERIFY(result.contains("Error:"));
    }

    /* skill_create: already exists */
    void testCreateAlreadyExists()
    {
        QSocToolSkillCreate tool(this, projectManager);
        json                args
            = {{"name", "test-skill"},
               {"description", "duplicate"},
               {"instructions", "test"},
               {"scope", "project"}};

        QString result = tool.execute(args);
        QVERIFY(result.contains("Error:"));
        QVERIFY(result.contains("already exists"));
    }

    /* skill_create: missing required params */
    void testCreateMissingParams()
    {
        QSocToolSkillCreate tool(this, projectManager);

        /* Missing name */
        json args1 = {{"description", "test"}, {"instructions", "test"}, {"scope", "project"}};
        QVERIFY(tool.execute(args1).contains("Error:"));

        /* Missing description */
        json args2 = {{"name", "x"}, {"instructions", "test"}, {"scope", "project"}};
        QVERIFY(tool.execute(args2).contains("Error:"));

        /* Missing instructions */
        json args3 = {{"name", "x"}, {"description", "test"}, {"scope", "project"}};
        QVERIFY(tool.execute(args3).contains("Error:"));

        /* Missing scope */
        json args4 = {{"name", "x"}, {"description", "test"}, {"instructions", "test"}};
        QVERIFY(tool.execute(args4).contains("Error:"));
    }

    /* skill_create: single char name is valid */
    void testCreateSingleCharName()
    {
        QSocToolSkillCreate tool(this, projectManager);
        json                args
            = {{"name", "x"},
               {"description", "single char skill"},
               {"instructions", "just a test"},
               {"scope", "project"}};

        QString result = tool.execute(args);
        QVERIFY(result.contains("Successfully"));
    }

    /* Tool definition format */
    void testSkillFindDefinition()
    {
        QSocToolSkillFind tool(this, projectManager);
        QCOMPARE(tool.getName(), "skill_find");

        json definition = tool.getDefinition();
        QVERIFY(definition.contains("type"));
        QCOMPARE(definition["type"].get<std::string>(), "function");
        QCOMPARE(definition["function"]["name"].get<std::string>(), "skill_find");
    }

    void testSkillCreateDefinition()
    {
        QSocToolSkillCreate tool(this, projectManager);
        QCOMPARE(tool.getName(), "skill_create");

        json definition = tool.getDefinition();
        QVERIFY(definition.contains("type"));
        QCOMPARE(definition["type"].get<std::string>(), "function");
        QCOMPARE(definition["function"]["name"].get<std::string>(), "skill_create");
    }
};

QTEST_APPLESS_MAIN(Test)
#include "test_qsocagenttoolskill.moc"
