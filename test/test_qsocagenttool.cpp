// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "agent/qsoctool.h"
#include "agent/tool/qsoctoolbus.h"
#include "agent/tool/qsoctooldoc.h"
#include "agent/tool/qsoctoolfile.h"
#include "agent/tool/qsoctoolgenerate.h"
#include "agent/tool/qsoctoolmodule.h"
#include "agent/tool/qsoctoolpath.h"
#include "agent/tool/qsoctoolproject.h"
#include "agent/tool/qsoctoolshell.h"
#include "common/qsocbusmanager.h"
#include "common/qsocgeneratemanager.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

namespace {

QByteArray agentResetNetlist()
{
    return R"(
reset:
  - name: agent_reset
    clock: clk_sys
    source:
      por_rst_n:
        active: low
    target:
      cpu_rst_n:
        active: low
        link:
          por_rst_n:
)";
}

bool writeFileBytes(
    const QString      &filePath,
    const QByteArray   &bytes,
    QIODevice::OpenMode mode = QIODevice::WriteOnly)
{
    QFile file(filePath);
    return file.open(mode) && file.write(bytes) == bytes.size();
}

QByteArray readFileBytes(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return file.readAll();
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir        tempDir;
    QSocProjectManager  *projectManager  = nullptr;
    QSocModuleManager   *moduleManager   = nullptr;
    QSocBusManager      *busManager      = nullptr;
    QSocGenerateManager *generateManager = nullptr;
    QSocPathContext     *pathContext     = nullptr;

private slots:
    void initTestCase()
    {
        QVERIFY(tempDir.isValid());

        projectManager  = new QSocProjectManager(this);
        moduleManager   = new QSocModuleManager(this, projectManager);
        busManager      = new QSocBusManager(this, projectManager);
        generateManager = new QSocGenerateManager(this, projectManager);

        projectManager->setProjectPath(tempDir.path());
        pathContext = new QSocPathContext(this, projectManager);
    }

    void cleanupTestCase()
    {
        delete pathContext;
        delete generateManager;
        delete busManager;
        delete moduleManager;
        delete projectManager;
        QVERIFY(QDir(tempDir.path()).removeRecursively());
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

    void testProjectCreateExistingPreservesFile()
    {
        QSocToolProjectCreate tool(this, projectManager);
        const QString         projectName = "existing_agent_project";
        const QString         projectFile = QDir(tempDir.path()).filePath(projectName + ".soc_pro");

        const json createArgs{
            {"name", projectName.toStdString()},
            {"directory", tempDir.path().toStdString()},
            {"bus_path", QDir(tempDir.path()).filePath("original_bus").toStdString()},
            {"module_path", QDir(tempDir.path()).filePath("module").toStdString()},
            {"schematic_path", QDir(tempDir.path()).filePath("schematic").toStdString()},
            {"output_path", QDir(tempDir.path()).filePath("output").toStdString()}};
        QVERIFY(tool.execute(createArgs).contains("successfully"));

        QFile file(projectFile);
        QVERIFY(file.open(QIODevice::ReadOnly));
        const QByteArray originalBytes = file.readAll();
        file.close();
        QVERIFY(!originalBytes.isEmpty());

        json replacementArgs = createArgs;
        replacementArgs["bus_path"] = QDir(tempDir.path()).filePath("replacement_bus").toStdString();
        const QString result = tool.execute(replacementArgs);

        QVERIFY(result.contains("already exists", Qt::CaseInsensitive));
        QVERIFY(file.open(QIODevice::ReadOnly));
        QCOMPARE(file.readAll(), originalBytes);
        file.close();
    }

    void testProjectCreateRejectsEscapingName()
    {
        QSocToolProjectCreate tool(this, projectManager);
        const QString         projectPath = QDir(tempDir.path()).filePath("contained");
        const json            arguments{
            {"name", "../escaped_agent"},
            {"directory", projectPath.toStdString()},
            {"bus_path", QDir(projectPath).filePath("bus").toStdString()},
            {"module_path", QDir(projectPath).filePath("module").toStdString()},
            {"schematic_path", QDir(projectPath).filePath("schematic").toStdString()},
            {"output_path", QDir(projectPath).filePath("output").toStdString()}};
        const auto previousState  = projectManager->captureState();
        const auto restoreProject = qScopeGuard(
            [&]() { projectManager->restoreState(previousState); });

        QVERIFY(tool.execute(arguments).startsWith("Error:"));
        QCOMPARE(projectManager->getProjectName(), previousState.projectName);
        QCOMPARE(projectManager->getProjectPath(), previousState.projectPath);
        QCOMPARE(projectManager->getBusPath(), previousState.busPath);
        QCOMPARE(projectManager->getModulePath(), previousState.modulePath);
        QCOMPARE(projectManager->getSchematicPath(), previousState.schematicPath);
        QCOMPARE(projectManager->getOutputPath(), previousState.outputPath);
        QCOMPARE(projectManager->getCurrentPath(), previousState.currentPath);
        QCOMPARE(projectManager->getEnv(), previousState.env);
        const auto restoredState = projectManager->captureState();
        QCOMPARE(
            QString::fromStdString(YAML::Dump(restoredState.projectNode)),
            QString::fromStdString(YAML::Dump(previousState.projectNode)));
        QVERIFY(!QFile::exists(QDir(tempDir.path()).filePath("escaped_agent.soc_pro")));
        QVERIFY(!QDir(projectPath).exists());
    }

    void testProjectCreateDirectoryContainsDefaults()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString projectPath    = QDir(directory.path()).filePath("project");
        const auto    previousState  = projectManager->captureState();
        const auto    restoreProject = qScopeGuard(
            [&]() { projectManager->restoreState(previousState); });

        QSocToolProjectCreate tool(this, projectManager);
        const json            arguments{
            {"name", "contained_agent_project"},
            {"directory", projectPath.toStdString()},
        };
        QVERIFY(tool.execute(arguments).contains("successfully"));

        QCOMPARE(projectManager->getProjectPath(), projectPath);
        QCOMPARE(projectManager->getBusPath(), QDir(projectPath).filePath("bus"));
        QCOMPARE(projectManager->getModulePath(), QDir(projectPath).filePath("module"));
        QCOMPARE(projectManager->getSchematicPath(), QDir(projectPath).filePath("schematic"));
        QCOMPARE(projectManager->getOutputPath(), QDir(projectPath).filePath("output"));
        const QStringList directories{"bus", "module", "schematic", "output"};
        for (const QString &name : directories) {
            QVERIFY(QFileInfo(QDir(projectPath).filePath(name)).isDir());
        }
    }

    /* File Tools Tests */
    void testFileReadMissingPath()
    {
        QSocToolFileRead tool(this, pathContext);
        json             args = json::object();

        QString result = tool.execute(args);

        QVERIFY(result.startsWith("Error:"));
        QVERIFY(result.contains("file_path"));
    }

    void testFileReadNonexistent()
    {
        QSocToolFileRead tool(this, pathContext);
        json             args = {{"file_path", "/nonexistent/path/file.txt"}};

        QString result = tool.execute(args);

        QVERIFY(result.contains("Error:"));
    }

    void testFileReadSecurityCheck()
    {
        QTemporaryDir outsideDirectory(QDir::home().filePath(".qsoc_test_outside_read_XXXXXX"));
        if (!outsideDirectory.isValid()) {
            QSKIP("No writable directory outside the allowed roots.");
        }
        const QString fixturePath = QDir(outsideDirectory.path()).filePath("read_fixture.txt");
        QFile         fixtureFile(fixturePath);
        QVERIFY(fixtureFile.open(QIODevice::WriteOnly));
        QCOMPARE(fixtureFile.write("runtime read fixture"), 20);
        fixtureFile.close();
        if (pathContext->isWriteAllowed(fixturePath)) {
            QSKIP("The generated directory is inside an allowed root.");
        }

        QSocToolFileRead tool(this, pathContext);
        json             args = {{"file_path", fixturePath.toStdString()}};

        const QString result = tool.execute(args);

        QVERIFY(result.contains("runtime read fixture"));
    }

    void testFileWriteSecurityCheck()
    {
        QTemporaryDir outsideDirectory(QDir::home().filePath(".qsoc_test_outside_write_XXXXXX"));
        if (!outsideDirectory.isValid()) {
            QSKIP("No writable directory outside the allowed roots.");
        }
        const QString deniedPath = QDir(outsideDirectory.path()).filePath("denied.txt");
        if (pathContext->isWriteAllowed(deniedPath)) {
            QSKIP("The generated directory is inside an allowed root.");
        }

        QSocToolFileWrite tool(this, pathContext);
        json              args = {{"file_path", deniedPath.toStdString()}, {"content", "test"}};

        const QString result = tool.execute(args);

        QVERIFY(result.contains("Access denied"));
        QVERIFY(!QFile::exists(deniedPath));
    }

    void testPathWriteBoundary()
    {
        QTemporaryDir boundaryRoot(QDir::home().filePath(".qsoc_test_path_boundary_XXXXXX"));
        if (!boundaryRoot.isValid()) {
            QSKIP("No writable directory outside the allowed roots.");
        }
        const QString allowedDirectory = QDir(boundaryRoot.path()).filePath("project");
        const QString siblingDirectory = QDir(boundaryRoot.path()).filePath("project_evil");
        QVERIFY(QDir().mkpath(allowedDirectory));
        QVERIFY(QDir().mkpath(siblingDirectory));
        const QString allowedFile = QDir(allowedDirectory).filePath("allowed.txt");
        const QString siblingFile = QDir(siblingDirectory).filePath("denied.txt");
        if (pathContext->isWriteAllowed(siblingFile)) {
            QSKIP("The generated directory is inside an allowed root.");
        }

        pathContext->addUserDir(allowedDirectory);
        const auto removeAllowedDirectory = qScopeGuard(
            [&]() { pathContext->removeUserDir(allowedDirectory); });

        QVERIFY(pathContext->isWriteAllowed(allowedFile));
        QVERIFY(!pathContext->isWriteAllowed(siblingFile));
    }

    void testWritableRootRetargetIsRefused_data()
    {
        QTest::addColumn<bool>("workingRoot");
        QTest::newRow("user root") << false;
        QTest::newRow("working root") << true;
    }

    void testWritableRootRetargetIsRefused()
    {
#ifndef Q_OS_UNIX
        QSKIP("This platform does not provide Unix symbolic-link semantics.");
#else
        QFETCH(bool, workingRoot);
        QTemporaryDir boundaryRoot(QDir::home().filePath(".qsoc_test_root_anchor_XXXXXX"));
        if (!boundaryRoot.isValid()) {
            QSKIP("No writable directory outside the implicit roots.");
        }
        const QString first  = QDir(boundaryRoot.path()).filePath("first");
        const QString second = QDir(boundaryRoot.path()).filePath("second");
        const QString root   = QDir(boundaryRoot.path()).filePath("selected");
        QVERIFY(QDir().mkpath(first));
        QVERIFY(QDir().mkpath(second));
        QVERIFY(QFile::link(first, root));

        QSocPathContext context;
        if (workingRoot) {
            context.setWorkingDir(root);
        } else {
            context.addUserDir(root);
        }
        QString       resolved;
        const QString before = QDir(root).filePath("before.txt");
        QVERIFY(context.resolveWritablePath(before, &resolved));
        QCOMPARE(resolved, QDir(first).filePath("before.txt"));

        QVERIFY(QFile::remove(root));
        QVERIFY(QFile::link(second, root));
        const QString victim = QDir(root).filePath("victim.txt");
        QVERIFY(!context.resolveWritablePath(victim, &resolved));
        QSocToolFileWrite tool(nullptr, &context);
        const QString     result = tool.execute(
            json{{"file_path", victim.toStdString()}, {"content", "blocked\n"}});
        QVERIFY2(result.contains(QStringLiteral("Access denied")), qPrintable(result));
        QVERIFY(!QFileInfo::exists(QDir(second).filePath("victim.txt")));

        if (workingRoot) {
            context.setWorkingDir(root);
        } else {
            context.addUserDir(root);
        }
        QVERIFY(context.resolveWritablePath(victim, &resolved));
        QCOMPARE(resolved, QDir(second).filePath("victim.txt"));
#endif
    }

    void testDanglingLinkCannotEscapeWriteBoundary()
    {
#ifndef Q_OS_UNIX
        QSKIP("This platform does not provide Unix symbolic-link semantics.");
#else
        QTemporaryDir boundaryRoot(QDir::home().filePath(".qsoc_test_dangling_link_XXXXXX"));
        if (!boundaryRoot.isValid()) {
            QSKIP("No writable directory outside the allowed roots.");
        }
        const QString allowedDirectory = QDir(boundaryRoot.path()).filePath("allowed");
        const QString outsideDirectory = QDir(boundaryRoot.path()).filePath("outside");
        QVERIFY(QDir().mkpath(allowedDirectory));
        QVERIFY(QDir().mkpath(outsideDirectory));

        const QString outsidePath = QDir(outsideDirectory).filePath("target.txt");
        const QString linkPath    = QDir(allowedDirectory).filePath("link.txt");
        QVERIFY(QFile::link(outsidePath, linkPath));
        QVERIFY(QFileInfo(linkPath).isSymLink());
        QVERIFY(!QFileInfo::exists(outsidePath));

        pathContext->addUserDir(allowedDirectory);
        const auto removeAllowedDirectory = qScopeGuard(
            [&]() { pathContext->removeUserDir(allowedDirectory); });

        QVERIFY(!pathContext->isWriteAllowed(linkPath));
        QSocToolFileWrite tool(this, pathContext);
        const json        arguments{{"file_path", linkPath.toStdString()}, {"content", "blocked"}};
        QVERIFY(tool.execute(arguments).contains("Access denied"));
        QVERIFY(!QFileInfo::exists(outsidePath));
        QVERIFY(QFileInfo(linkPath).isSymLink());
#endif
    }

    void testFileListDirectory()
    {
        QSocToolFileList tool(this, pathContext);
        json             args = {{"directory", tempDir.path().toStdString()}};

        QString result = tool.execute(args);

        /* Should list files or say directory is empty */
        QVERIFY(result.contains("Files in") || result.contains("No files"));
    }

    /* P25 regression: a file with EXACTLY maxLines (default 500) lines
     * must not produce a truncation marker, while 501 lines must. */
    void testFileReadTruncationBoundary()
    {
        QSocToolFileRead readTool(this, pathContext);

        QString exact500 = tempDir.path() + "/exact500.txt";
        QFile   f1(exact500);
        QVERIFY(f1.open(QIODevice::WriteOnly | QIODevice::Text));
        for (int i = 0; i < 500; ++i) {
            f1.write("line\n");
        }
        f1.close();

        QString result500 = readTool.execute(json{{"file_path", exact500.toStdString()}});
        QVERIFY(!result500.contains("[truncated"));

        QString plus1 = tempDir.path() + "/plus1.txt";
        QFile   f2(plus1);
        QVERIFY(f2.open(QIODevice::WriteOnly | QIODevice::Text));
        for (int i = 0; i < 501; ++i) {
            f2.write("line\n");
        }
        f2.close();

        QString result501 = readTool.execute(json{{"file_path", plus1.toStdString()}});
        QVERIFY(result501.contains("[truncated"));
        QVERIFY(result501.contains("offset=500"));

        /* Without trailing newline: 500 lines exactly should still NOT mark. */
        QString noTrail = tempDir.path() + "/no_trail.txt";
        QFile   f3(noTrail);
        QVERIFY(f3.open(QIODevice::WriteOnly | QIODevice::Text));
        for (int i = 0; i < 499; ++i) {
            f3.write("line\n");
        }
        f3.write("line"); /* no newline on last line */
        f3.close();

        QString resultNoTrail = readTool.execute(json{{"file_path", noTrail.toStdString()}});
        QVERIFY(!resultNoTrail.contains("[truncated"));
    }

    void testFileWriteAndRead()
    {
        /* Write a file */
        QSocToolFileWrite writeTool(this, pathContext);
        QString           testContent = "Hello, QSoC Agent Test!";
        QString           testFile    = tempDir.path() + "/test_write.txt";

        json writeArgs
            = {{"file_path", testFile.toStdString()}, {"content", testContent.toStdString()}};

        QString writeResult = writeTool.execute(writeArgs);
        QVERIFY(writeResult.contains("Successfully"));

        /* Read it back */
        QSocToolFileRead readTool(this, pathContext);
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

        /* Read first to satisfy the read-before-edit guard. */
        QSocToolFileRead readTool(this, pathContext);
        readTool.execute({{"file_path", testFile.toStdString()}});

        /* Edit it */
        QSocToolFileEdit editTool(this, pathContext);
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

        /* Read first to satisfy the read-before-edit guard. */
        QSocToolFileRead readTool(this, pathContext);
        readTool.execute({{"file_path", testFile.toStdString()}});

        /* Try to edit without replace_all */
        QSocToolFileEdit editTool(this, pathContext);
        json             editArgs
            = {{"file_path", testFile.toStdString()}, {"old_string", "foo"}, {"new_string", "xxx"}};

        QString editResult = editTool.execute(editArgs);
        QVERIFY(editResult.contains("Error:"));
        QVERIFY(editResult.contains("3 times") || editResult.contains("replace_all"));
    }

    void testFileEditRequiresPriorRead()
    {
        /* Editing a file the agent never read is rejected. */
        QString testFile = tempDir.path() + "/test_unread.txt";
        QFile   file(testFile);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("alpha beta");
        file.close();

        QSocToolFileEdit editTool(this, pathContext);
        QString          result = editTool.execute(
            {{"file_path", testFile.toStdString()},
             {"old_string", "beta"},
             {"new_string", "gamma"}});
        QVERIFY(result.contains("Error:"));
        QVERIFY(result.contains("not read yet"));
    }

    void testFileEditPathSpellingCanonicalized()
    {
        /* Reading via one spelling (./name) and editing via another (name)
         * must share the read-state key, so the edit is not falsely rejected
         * as "not read yet". */
        QString testFile = tempDir.path() + "/canon.txt";
        QFile   file(testFile);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("uno dos tres");
        file.close();

        QSocToolFileRead readTool(this, pathContext);
        readTool.execute({{"file_path", "./canon.txt"}});

        QSocToolFileEdit editTool(this, pathContext);
        QString          result = editTool.execute(
            {{"file_path", "canon.txt"}, {"old_string", "dos"}, {"new_string", "DOS"}});
        QVERIFY2(result.contains("Successfully"), qPrintable(result));
    }

    void testFileEditRejectsStaleOnDisk()
    {
        /* A file changed on disk after the read is rejected until re-read. */
        QString testFile = tempDir.path() + "/test_stale.txt";
        QFile   file(testFile);
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("one two three");
        file.close();

        QSocToolFileRead readTool(this, pathContext);
        readTool.execute({{"file_path", testFile.toStdString()}});

        /* External modification after the read. */
        QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Text));
        file.write("one two THREE changed");
        file.close();

        QSocToolFileEdit editTool(this, pathContext);
        QString          stale = editTool.execute(
            {{"file_path", testFile.toStdString()}, {"old_string", "two"}, {"new_string", "II"}});
        QVERIFY(stale.contains("Error:"));
        QVERIFY(stale.contains("changed on disk"));

        /* Re-reading clears the staleness; the edit then succeeds. */
        readTool.execute({{"file_path", testFile.toStdString()}});
        QString ok = editTool.execute(
            {{"file_path", testFile.toStdString()}, {"old_string", "two"}, {"new_string", "II"}});
        QVERIFY(ok.contains("Successfully"));
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

    void testGenerateVerilogForceIsScopedAfterSuccess()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString projectPath = QDir(directory.path()).filePath("project");

        QSocProjectManager  manager;
        QSocGenerateManager generator(nullptr, &manager);
        manager.setCurrentPath(projectPath);
        QVERIFY(manager.mkpath());

        const QString netlistPath = QDir(projectPath).filePath("reset.soc_net");
        QVERIFY(writeFileBytes(netlistPath, agentResetNetlist()));

        QSocToolGenerateVerilog tool(this, &generator);
        const json              forcedArguments{
            {"netlist_file", netlistPath.toStdString()},
            {"output_name", "forced_reset"},
            {"force", true}};
        QVERIFY(tool.execute(forcedArguments).startsWith("Successfully"));

        const QString    cellPath = QDir(manager.getOutputPath()).filePath("reset_cell.v");
        const QByteArray marker   = "// user reset cell\n";
        QVERIFY(writeFileBytes(cellPath, marker, QIODevice::Append));
        const QByteArray expectedCell = readFileBytes(cellPath);
        QVERIFY(expectedCell.endsWith(marker));

        const json defaultArguments{
            {"netlist_file", netlistPath.toStdString()}, {"output_name", "default_reset"}};
        QVERIFY(tool.execute(defaultArguments).startsWith("Successfully"));
        QCOMPARE(readFileBytes(cellPath), expectedCell);

        const json falseArguments{
            {"netlist_file", netlistPath.toStdString()},
            {"output_name", "false_reset"},
            {"force", false}};
        QVERIFY(tool.execute(falseArguments).startsWith("Successfully"));
        QCOMPARE(readFileBytes(cellPath), expectedCell);
    }

    void testGenerateVerilogForceIsScopedAfterFailure_data()
    {
        QTest::addColumn<QByteArray>("failedNetlist");
        QTest::addColumn<QString>("failedOutput");
        QTest::addColumn<QString>("expectedError");

        QTest::newRow("load") << QByteArray("reset: [\n") << QString("load_failure")
                              << QString("Failed to load netlist");
        QTest::newRow("process") << QByteArray("comb: malformed\n") << QString("process_failure")
                                 << QString("Failed to process netlist");
        QTest::newRow("generate") << agentResetNetlist() << QString("../blocked")
                                  << QString("Failed to generate Verilog");
    }

    void testGenerateVerilogForceIsScopedAfterFailure()
    {
        QFETCH(QByteArray, failedNetlist);
        QFETCH(QString, failedOutput);
        QFETCH(QString, expectedError);

        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString projectPath = QDir(directory.path()).filePath("project");

        QSocProjectManager  manager;
        QSocGenerateManager generator(nullptr, &manager);
        manager.setCurrentPath(projectPath);
        QVERIFY(manager.mkpath());

        const QString validNetlistPath = QDir(projectPath).filePath("valid_reset.soc_net");
        QVERIFY(writeFileBytes(validNetlistPath, agentResetNetlist()));

        QSocToolGenerateVerilog tool(this, &generator);
        const json              seedArguments{
            {"netlist_file", validNetlistPath.toStdString()}, {"output_name", "seed_reset"}};
        QVERIFY(tool.execute(seedArguments).startsWith("Successfully"));

        const QString    cellPath = QDir(manager.getOutputPath()).filePath("reset_cell.v");
        const QByteArray marker   = "// preserved after failed force call\n";
        QVERIFY(writeFileBytes(cellPath, marker, QIODevice::Append));
        const QByteArray expectedCell = readFileBytes(cellPath);
        QVERIFY(expectedCell.endsWith(marker));

        const QString failedNetlistPath = QDir(projectPath).filePath("failed_reset.soc_net");
        QVERIFY(writeFileBytes(failedNetlistPath, failedNetlist));
        const json failedArguments{
            {"netlist_file", failedNetlistPath.toStdString()},
            {"output_name", failedOutput.toStdString()},
            {"force", true}};
        const QString failedResult = tool.execute(failedArguments);
        QVERIFY2(failedResult.contains(expectedError), qPrintable(failedResult));

        const json retryArguments{
            {"netlist_file", validNetlistPath.toStdString()}, {"output_name", "retry_reset"}};
        QVERIFY(tool.execute(retryArguments).startsWith("Successfully"));
        QCOMPARE(readFileBytes(cellPath), expectedCell);
    }

    void testGenerateVerilogRejectsNonBooleanForce()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString projectPath = QDir(directory.path()).filePath("project");

        QSocProjectManager  manager;
        QSocGenerateManager generator(nullptr, &manager);
        manager.setCurrentPath(projectPath);
        QVERIFY(manager.mkpath());

        const QString netlistPath = QDir(projectPath).filePath("reset.soc_net");
        QVERIFY(writeFileBytes(netlistPath, agentResetNetlist()));

        QSocToolGenerateVerilog tool(this, &generator);
        const QString           result = tool.execute(
            {{"netlist_file", netlistPath.toStdString()},
             {"output_name", "invalid_force"},
             {"force", "true"}});

        QVERIFY(result.startsWith("Error:"));
        QVERIFY(result.contains("force"));
        QVERIFY(result.contains("boolean"));
        QVERIFY(!QFileInfo::exists(QDir(manager.getOutputPath()).filePath("invalid_force.v")));
        QVERIFY(!QFileInfo::exists(QDir(manager.getOutputPath()).filePath("reset_cell.v")));
    }

    void testGenerateTemplateMissingParams()
    {
        QSocToolGenerateTemplate tool(this, generateManager);
        json                     args = json::object();

        QString result = tool.execute(args);

        QVERIFY(result.startsWith("Error:"));
    }

    /* Bash Timeout Tests */
    void testBashTimeout()
    {
        QSocToolShellBash tool(this, projectManager);
        json              args = {{"command", "sleep 10"}, {"timeout", 1000}};

        QString result = tool.execute(args);

        QVERIFY(result.contains("timed out"));
        QVERIFY(result.contains("Process ID:"));
        QVERIFY(result.contains("STILL RUNNING"));
        QVERIFY(result.contains("bash_manage"));
    }

    void testBashManageStatus()
    {
        QSocToolShellBash tool(this, projectManager);
        json              bashArgs = {{"command", "sleep 10"}, {"timeout", 1000}};

        QString bashResult = tool.execute(bashArgs);
        QVERIFY(bashResult.contains("Process ID:"));

        /* Extract process ID */
        QRegularExpression      regex("Process ID: (\\d+)");
        QRegularExpressionMatch match = regex.match(bashResult);
        QVERIFY(match.hasMatch());
        int processId = match.captured(1).toInt();

        /* Check status */
        QSocToolBashManage manageTool(this);
        json               statusArgs = {{"process_id", processId}, {"action", "status"}};

        QString statusResult = manageTool.execute(statusArgs);
        QVERIFY(statusResult.contains("RUNNING") || statusResult.contains("FINISHED"));
        QVERIFY(statusResult.contains("sleep 10"));

        /* Kill it */
        json    killArgs   = {{"process_id", processId}, {"action", "kill"}};
        QString killResult = manageTool.execute(killArgs);
        QVERIFY(killResult.contains("killed") || killResult.contains("exit code"));
    }

    void testBashManageKill()
    {
        QSocToolShellBash tool(this, projectManager);
        json              bashArgs = {{"command", "sleep 30"}, {"timeout", 500}};

        QString bashResult = tool.execute(bashArgs);
        QVERIFY(bashResult.contains("Process ID:"));

        QRegularExpression      regex("Process ID: (\\d+)");
        QRegularExpressionMatch match = regex.match(bashResult);
        QVERIFY(match.hasMatch());
        int processId = match.captured(1).toInt();

        /* Kill the process */
        QSocToolBashManage manageTool(this);
        json               killArgs = {{"process_id", processId}, {"action", "kill"}};

        QString killResult = manageTool.execute(killArgs);
        QVERIFY(killResult.contains("killed"));

        /* Verify it's cleaned up */
        json    statusArgs   = {{"process_id", processId}, {"action", "status"}};
        QString statusResult = manageTool.execute(statusArgs);
        QVERIFY(statusResult.contains("Error:"));
        QVERIFY(statusResult.contains("No active process"));
    }

    void testBashManageWait()
    {
        QSocToolShellBash tool(this, projectManager);
        /* Short command that finishes quickly, but with short timeout */
        json bashArgs = {{"command", "echo done && sleep 1"}, {"timeout", 200}};

        QString bashResult = tool.execute(bashArgs);
        QVERIFY(bashResult.contains("Process ID:"));

        QRegularExpression      regex("Process ID: (\\d+)");
        QRegularExpressionMatch match = regex.match(bashResult);
        QVERIFY(match.hasMatch());
        int processId = match.captured(1).toInt();

        /* Wait for it to finish */
        QSocToolBashManage manageTool(this);
        json waitArgs = {{"process_id", processId}, {"action", "wait"}, {"timeout", 5000}};

        QString waitResult = manageTool.execute(waitArgs);
        QVERIFY(
            waitResult.contains("completed") || waitResult.contains("finished")
            || waitResult.contains("done"));
    }

    void testBashManageInvalidProcess()
    {
        QSocToolBashManage manageTool(this);
        json               args = {{"process_id", 99999}, {"action", "status"}};

        QString result = manageTool.execute(args);
        QVERIFY(result.contains("Error:"));
        QVERIFY(result.contains("No active process"));
    }

    void testBashManageMissingParams()
    {
        QSocToolBashManage manageTool(this);

        /* Missing process_id */
        json args1 = {{"action", "status"}};
        QVERIFY(manageTool.execute(args1).contains("Error:"));

        /* Missing action */
        json args2 = {{"process_id", 1}};
        QVERIFY(manageTool.execute(args2).contains("Error:"));
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

QSOC_TEST_MAIN(Test)
#include "test_qsocagenttool.moc"
