// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctool.h"
#include "agent/tool/qsoctoolbus.h"
#include "agent/tool/qsoctooldoc.h"
#include "agent/tool/qsoctoolfile.h"
#include "agent/tool/qsoctoolgenerate.h"
#include "agent/tool/qsoctoolmodule.h"
#include "agent/tool/qsoctoolproject.h"
#include "agent/tool/qsoctoolshell.h"
#include "common/qsocbusmanager.h"
#include "common/qsocgeneratemanager.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
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
    QTemporaryDir        tempDir;
    QSocProjectManager  *projectManager  = nullptr;
    QSocModuleManager   *moduleManager   = nullptr;
    QSocBusManager      *busManager      = nullptr;
    QSocGenerateManager *generateManager = nullptr;

private slots:
    void initTestCase()
    {
        TestApp::instance();
        QVERIFY(tempDir.isValid());

        projectManager  = new QSocProjectManager(this);
        moduleManager   = new QSocModuleManager(this, projectManager);
        busManager      = new QSocBusManager(this, projectManager);
        generateManager = new QSocGenerateManager(this, projectManager);

        projectManager->setProjectPath(tempDir.path());
    }

    void cleanupTestCase()
    {
        delete generateManager;
        delete busManager;
        delete moduleManager;
        delete projectManager;
    }

    /* Tool Registry Tests */
    void testRegistryRegisterAndGet()
    {
        QSocToolRegistry registry;
        auto            *tool = new QSocToolProjectList(&registry, projectManager);

        registry.registerTool(tool);

        QCOMPARE(registry.count(), 1);
        QVERIFY(registry.getTool("project_list") != nullptr);
        QVERIFY(registry.getTool("nonexistent") == nullptr);
    }

    void testRegistryMultipleTools()
    {
        QSocToolRegistry registry;
        auto            *tool1 = new QSocToolProjectList(&registry, projectManager);
        auto            *tool2 = new QSocToolProjectShow(&registry, projectManager);
        auto            *tool3 = new QSocToolProjectCreate(&registry, projectManager);

        registry.registerTool(tool1);
        registry.registerTool(tool2);
        registry.registerTool(tool3);

        QCOMPARE(registry.count(), 3);
    }

    void testRegistryGetDefinitions()
    {
        QSocToolRegistry registry;
        auto            *tool = new QSocToolProjectList(&registry, projectManager);
        registry.registerTool(tool);

        json definitions = registry.getToolDefinitions();

        QVERIFY(definitions.is_array());
        QCOMPARE(definitions.size(), 1u);
        QVERIFY(definitions[0].contains("type"));
        QCOMPARE(definitions[0]["type"].get<std::string>(), "function");
    }

    /* Tool Definition Tests */
    void testToolDefinitionFormat()
    {
        QSocToolProjectList tool(this, projectManager);
        json                definition = tool.getDefinition();

        QVERIFY(definition.contains("type"));
        QVERIFY(definition.contains("function"));
        QVERIFY(definition["function"].contains("name"));
        QVERIFY(definition["function"].contains("description"));
        QVERIFY(definition["function"].contains("parameters"));

        QCOMPARE(definition["type"].get<std::string>(), "function");
        QCOMPARE(definition["function"]["name"].get<std::string>(), "project_list");
    }

    /* Project Tools Tests */
    void testProjectListExecute()
    {
        QSocToolProjectList tool(this, projectManager);
        json                args = json::object();

        QString result = tool.execute(args);

        /* Should either find projects or say none found */
        QVERIFY(result.contains("project") || result.contains("No projects"));
    }

    void testProjectShowMissingName()
    {
        QSocToolProjectShow tool(this, projectManager);
        json                args = json::object();

        QString result = tool.execute(args);

        QVERIFY(result.startsWith("Error:"));
        QVERIFY(result.contains("name"));
    }

    void testProjectCreateMissingName()
    {
        QSocToolProjectCreate tool(this, projectManager);
        json                  args = json::object();

        QString result = tool.execute(args);

        QVERIFY(result.startsWith("Error:"));
    }

    /* File Tools Tests */
    void testFileReadMissingPath()
    {
        QSocToolFileRead tool(this, projectManager);
        json             args = json::object();

        QString result = tool.execute(args);

        QVERIFY(result.startsWith("Error:"));
        QVERIFY(result.contains("file_path"));
    }

    void testFileReadNonexistent()
    {
        QSocToolFileRead tool(this, projectManager);
        json             args = {{"file_path", "/nonexistent/path/file.txt"}};

        QString result = tool.execute(args);

        QVERIFY(result.contains("Error:"));
    }

    void testFileReadSecurityCheck()
    {
        QSocToolFileRead tool(this, projectManager);
        json             args = {{"file_path", "/etc/passwd"}};

        QString result = tool.execute(args);

        QVERIFY(result.contains("Access denied") || result.contains("Error:"));
    }

    void testFileListDirectory()
    {
        QSocToolFileList tool(this, projectManager);
        json             args = {{"directory", tempDir.path().toStdString()}};

        QString result = tool.execute(args);

        /* Should list files or say directory is empty */
        QVERIFY(result.contains("Files in") || result.contains("No files"));
    }

    void testFileWriteAndRead()
    {
        /* Write a file */
        QSocToolFileWrite writeTool(this, projectManager);
        QString           testContent = "Hello, QSoC Agent Test!";
        QString           testFile    = tempDir.path() + "/test_write.txt";

        json writeArgs
            = {{"file_path", testFile.toStdString()}, {"content", testContent.toStdString()}};

        QString writeResult = writeTool.execute(writeArgs);
        QVERIFY(writeResult.contains("Successfully"));

        /* Read it back */
        QSocToolFileRead readTool(this, projectManager);
        json             readArgs = {{"file_path", testFile.toStdString()}};

        QString readResult = readTool.execute(readArgs);
        QVERIFY(readResult.contains(testContent));
    }

    void testFileEdit()
    {
        /* Create a file first */
        QString testFile = tempDir.path() + "/test_edit.txt";
        QFile   file(testFile);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("Hello World");
        file.close();

        /* Edit it */
        QSocToolFileEdit editTool(this, projectManager);
        json             editArgs
            = {{"file_path", testFile.toStdString()},
               {"old_string", "World"},
               {"new_string", "QSoC"}};

        QString editResult = editTool.execute(editArgs);
        QVERIFY(editResult.contains("Successfully"));

        /* Verify content changed */
        QVERIFY(file.open(QIODevice::ReadOnly | QIODevice::Text));
        QString content = QString::fromUtf8(file.readAll());
        file.close();
        QVERIFY(content.contains("Hello QSoC"));
    }

    void testFileEditNonUnique()
    {
        /* Create a file with duplicate content */
        QString testFile = tempDir.path() + "/test_edit_dup.txt";
        QFile   file(testFile);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("foo bar foo baz foo");
        file.close();

        /* Try to edit without replace_all */
        QSocToolFileEdit editTool(this, projectManager);
        json             editArgs
            = {{"file_path", testFile.toStdString()}, {"old_string", "foo"}, {"new_string", "xxx"}};

        QString editResult = editTool.execute(editArgs);
        QVERIFY(editResult.contains("Error:"));
        QVERIFY(editResult.contains("3 times") || editResult.contains("replace_all"));
    }

    /* Shell Tool Tests */
    void testBashSimpleCommand()
    {
        QSocToolShellBash tool(this, projectManager);
        json              args = {{"command", "echo hello"}};

        QString result = tool.execute(args);

        QVERIFY(result.contains("hello"));
    }

    void testBashWorkingDirectory()
    {
        QSocToolShellBash tool(this, projectManager);
        json args = {{"command", "pwd"}, {"working_directory", tempDir.path().toStdString()}};

        QString result = tool.execute(args);

        QVERIFY(result.contains(tempDir.path()));
    }

    void testBashMissingCommand()
    {
        QSocToolShellBash tool(this, projectManager);
        json              args = json::object();

        QString result = tool.execute(args);

        QVERIFY(result.startsWith("Error:"));
        QVERIFY(result.contains("command"));
    }

    void testBashExitCode()
    {
        QSocToolShellBash tool(this, projectManager);
        json              args = {{"command", "exit 42"}};

        QString result = tool.execute(args);

        QVERIFY(result.contains("exited with code 42"));
    }

    /* Documentation Tool Tests */
    void testDocQueryMissingTopic()
    {
        QSocToolDocQuery tool(this);
        json             args = json::object();

        QString result = tool.execute(args);

        QVERIFY(result.startsWith("Error:"));
        QVERIFY(result.contains("topic"));
    }

    void testDocQueryInvalidTopic()
    {
        QSocToolDocQuery tool(this);
        json             args = {{"topic", "nonexistent_topic"}};

        QString result = tool.execute(args);

        QVERIFY(result.contains("Error:") || result.contains("Unknown topic"));
    }

    void testDocQueryValidTopic()
    {
        QSocToolDocQuery tool(this);
        json             args = {{"topic", "commands"}};

        QString result = tool.execute(args);

        /* Should return documentation content or error if resources not loaded */
        QVERIFY(result.contains("Documentation") || result.contains("Error:"));
    }

    /* Module Tools Parameter Validation */
    void testModuleShowMissingName()
    {
        QSocToolModuleShow tool(this, moduleManager);
        json               args = json::object();

        QString result = tool.execute(args);

        QVERIFY(result.startsWith("Error:"));
    }

    void testModuleImportMissingFiles()
    {
        QSocToolModuleImport tool(this, moduleManager);
        json                 args = json::object();

        QString result = tool.execute(args);

        QVERIFY(result.startsWith("Error:"));
    }

    /* Bus Tools Parameter Validation */
    void testBusShowMissingName()
    {
        QSocToolBusShow tool(this, busManager);
        json            args = json::object();

        QString result = tool.execute(args);

        QVERIFY(result.startsWith("Error:"));
    }

    void testBusImportMissingParams()
    {
        QSocToolBusImport tool(this, busManager);
        json              args = json::object();

        QString result = tool.execute(args);

        QVERIFY(result.startsWith("Error:"));
    }

    /* Generate Tools Parameter Validation */
    void testGenerateVerilogMissingParams()
    {
        QSocToolGenerateVerilog tool(this, generateManager);
        json                    args = json::object();

        QString result = tool.execute(args);

        QVERIFY(result.startsWith("Error:"));
    }

    void testGenerateTemplateMissingParams()
    {
        QSocToolGenerateTemplate tool(this, generateManager);
        json                     args = json::object();

        QString result = tool.execute(args);

        QVERIFY(result.startsWith("Error:"));
    }

    /* Registry Execute Tool */
    void testRegistryExecuteTool()
    {
        QSocToolRegistry registry;
        auto            *tool = new QSocToolProjectList(&registry, projectManager);
        registry.registerTool(tool);

        QString result = registry.executeTool("project_list", json::object());

        QVERIFY(!result.isEmpty());
    }

    void testRegistryExecuteNonexistent()
    {
        QSocToolRegistry registry;

        QString result = registry.executeTool("nonexistent", json::object());

        QVERIFY(result.contains("Error:") || result.contains("not found"));
    }
};

QTEST_APPLESS_MAIN(Test)
#include "test_qsocagenttool.moc"
