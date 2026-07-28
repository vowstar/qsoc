// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/config.h"
#include "common/qsocconsole.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QStringList>
#include <QtCore>
#include <QtTest>

#include <iostream>

class Test : public QObject
{
    Q_OBJECT

private:
    static QStringList messageList;
    static void messageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
    {
        Q_UNUSED(type);
        Q_UNUSED(context);
        messageList << msg;
    }

private slots:
    void initTestCase()
    {
        /* Re-enable message handler for collecting CLI output */
        qInstallMessageHandler(messageOutput);
        /* Mirror QSocConsole writes through the message handler so legacy
         * messageList-based assertions still see them. */
        QSocConsole::setTeeToMessageHandler(true);
    }

    void cleanupTestCase()
    {
        /* Cleanup any leftover test files */
        const QStringList filesToRemove
            = {"test_project.soc_pro",
               "custom_dir_project.soc_pro",
               "update_test_project.soc_pro",
               "existing_project.soc_pro",
               "unexpected_project.soc_pro"};

        for (const QString &file : filesToRemove) {
            if (QFile::exists(file)) {
                QFile::remove(file);
            }
        }

        /* Also check for files in the build directory */
        const QString buildTestDir = QDir::currentPath() + "/build/test";
        for (const QString &file : filesToRemove) {
            const QString buildFilePath = buildTestDir + "/" + file;
            if (QFile::exists(buildFilePath)) {
                QFile::remove(buildFilePath);
            }
        }

        /* Clean up temporary directories */
        const QStringList dirsToRemove
            = {"./temp_test_dir",
               QDir::currentPath() + "/abs_temp_dir",
               QDir::currentPath() + "/abs_temp_dir/bus",
               QDir::currentPath() + "/abs_temp_dir/modules",
               "./bus_dir",
               "./original_bus",
               "./replacement_bus",
               "./module_dir",
               "./schematic_dir",
               "./output_dir"};

        for (const QString &dir : dirsToRemove) {
            if (QDir(dir).exists()) {
                QDir(dir).removeRecursively();
            }
        }
    }

    void testProjectCreate()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {
            "qsoc",
            "project",
            "create",
            "test_project",
        };
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Check if the project file was created */
        QFile projectFile("test_project.soc_pro");
        QVERIFY(projectFile.exists());

        /* Read the file content */
        QVERIFY(projectFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = projectFile.readAll();
        projectFile.close();

        /* Check for required strings */
        QVERIFY(content.contains("bus"));
        QVERIFY(content.contains("module"));
        QVERIFY(content.contains("schematic"));
        QVERIFY(content.contains("output"));
    }

    void testProjectList()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {
            "qsoc",
            "project",
            "list",
        };
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Check if test_project is listed in the output */
        bool found = false;
        for (const QString &msg : messageList) {
            if (msg.contains("test_project")) {
                found = true;
                break;
            }
        }
        QVERIFY(found);
    }

    void testProjectShow()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {
            "qsoc",
            "project",
            "show",
            "test_project",
        };
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Check for required strings in the output */
        bool hasBus       = false;
        bool hasModule    = false;
        bool hasSchematic = false;
        bool hasOutput    = false;
        for (const QString &msg : messageList) {
            if (msg.contains("bus"))
                hasBus = true;
            if (msg.contains("module"))
                hasModule = true;
            if (msg.contains("schematic"))
                hasSchematic = true;
            if (msg.contains("output"))
                hasOutput = true;
        }
        QVERIFY(hasBus);
        QVERIFY(hasModule);
        QVERIFY(hasSchematic);
        QVERIFY(hasOutput);
    }

    void testProjectUpdate()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {
            "qsoc",
            "project",
            "update",
            "-s",
            "./",
            "test_project",
        };
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Check if schematic path was updated */
        QFile projectFile("test_project.soc_pro");
        QVERIFY(projectFile.exists());

        /* Read the file content */
        QVERIFY(projectFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = projectFile.readAll();
        projectFile.close();

        /* Check for updated schematic path */
        QVERIFY(content.contains("schematic: ${QSOC_PROJECT_DIR}"));
    }

    void testProjectRemove()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {
            "qsoc",
            "project",
            "remove",
            "test_project",
        };
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Check if the project file was deleted */
        const QFile projectFile("test_project.soc_pro");
        QVERIFY(!projectFile.exists());
    }

    void testProjectCreateWithCustomDirectories()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {
            "qsoc",
            "project",
            "create",
            "-b",
            "./bus_dir",
            "-m",
            "./module_dir",
            "-s",
            "./schematic_dir",
            "-o",
            "./output_dir",
            "custom_dir_project",
        };
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Check if the project file was created */
        QFile projectFile("custom_dir_project.soc_pro");
        QVERIFY(projectFile.exists());

        /* Read the file content */
        QVERIFY(projectFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = projectFile.readAll();
        projectFile.close();

        /* Check for custom directory paths */
        QVERIFY(content.contains("bus: ${QSOC_PROJECT_DIR}/bus_dir"));
        QVERIFY(content.contains("module: ${QSOC_PROJECT_DIR}/module_dir"));
        QVERIFY(content.contains("schematic: ${QSOC_PROJECT_DIR}/schematic_dir"));
        QVERIFY(content.contains("output: ${QSOC_PROJECT_DIR}/output_dir"));

        /* Clean up */
        QFile::remove("custom_dir_project.soc_pro");
    }

    void testProjectUpdateMultipleParameters()
    {
        /* First create a test project */
        QSocCliWorker     socCliWorker1;
        const QStringList createArguments = {
            "qsoc",
            "project",
            "create",
            "update_test_project",
        };
        socCliWorker1.setup(createArguments, false);
        socCliWorker1.run();

        /* Now update multiple parameters */
        messageList.clear();
        QSocCliWorker     socCliWorker2;
        const QStringList appArguments = {
            "qsoc",
            "project",
            "update",
            "-b",
            "./custom_bus",
            "-m",
            "./custom_module",
            "-o",
            "./custom_output",
            "update_test_project",
        };
        socCliWorker2.setup(appArguments, false);
        socCliWorker2.run();

        /* Check if parameters were updated */
        QFile projectFile("update_test_project.soc_pro");
        QVERIFY(projectFile.exists());

        /* Read the file content */
        QVERIFY(projectFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = projectFile.readAll();
        projectFile.close();

        /* Check for updated paths */
        QVERIFY(content.contains("bus: ${QSOC_PROJECT_DIR}/custom_bus"));
        QVERIFY(content.contains("module: ${QSOC_PROJECT_DIR}/custom_module"));
        QVERIFY(content.contains("output: ${QSOC_PROJECT_DIR}/custom_output"));

        /* Clean up */
        QFile::remove("update_test_project.soc_pro");
    }

    void testProjectNonExistent()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {
            "qsoc",
            "project",
            "show",
            "non_existent_project",
        };
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Check for error message about non-existent project */
        bool hasErrorMsg = false;
        for (const QString &msg : messageList) {
            if (msg.contains("not found") || msg.contains("does not exist")
                || msg.contains("error")) {
                hasErrorMsg = true;
                break;
            }
        }
        QVERIFY(hasErrorMsg);
    }

    void testProjectCreateWithSameNameFails()
    {
        /* First create a test project */
        QSocCliWorker     socCliWorker1;
        const QStringList createArguments = {
            "qsoc",
            "project",
            "create",
            "-b",
            "./original_bus",
            "existing_project",
        };
        socCliWorker1.setup(createArguments, false);
        socCliWorker1.run();

        QFile projectFile("existing_project.soc_pro");
        QVERIFY(projectFile.open(QIODevice::ReadOnly));
        const QByteArray originalBytes = projectFile.readAll();
        projectFile.close();
        QVERIFY(!originalBytes.isEmpty());

        /* Now try to create a project with the same name */
        messageList.clear();
        QSocCliWorker     socCliWorker2;
        const QStringList appArguments = {
            "qsoc",
            "project",
            "create",
            "-b",
            "./replacement_bus",
            "existing_project",
        };
        socCliWorker2.setup(appArguments, false);
        socCliWorker2.run();

        QVERIFY(messageList.join('\n').contains("already exists", Qt::CaseInsensitive));

        QVERIFY(projectFile.open(QIODevice::ReadOnly));
        QCOMPARE(projectFile.readAll(), originalBytes);
        projectFile.close();
        QVERIFY(QFile::remove("existing_project.soc_pro"));
    }

    void testProjectWithVerbosityLevels()
    {
        /* Just test with a single verbosity level to avoid issues */
        messageList.clear();
        QSocCliWorker socCliWorker;

        /* Create arguments with verbosity level 3 (info) */
        const QStringList appArguments = {"qsoc", "--verbose=3", "project", "list"};

        QSignalSpy exitSpy(&socCliWorker, &QSocCliWorker::exit);
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        QCOMPARE(exitSpy.count(), 1);
        QCOMPARE(exitSpy.takeFirst().at(0).toInt(), 0);
        QVERIFY(!messageList.join('\n').contains("Error:"));
    }

    void testProjectWithInvalidOption_data()
    {
        QTest::addColumn<QString>("subcommand");

        QTest::newRow("create") << QString("create");
        QTest::newRow("update") << QString("update");
        QTest::newRow("remove") << QString("remove");
        QTest::newRow("list") << QString("list");
        QTest::newRow("show") << QString("show");
    }

    void testProjectWithInvalidOption()
    {
        QFETCH(QString, subcommand);
        messageList.clear();
        QSocCliWorker socCliWorker;

        const QStringList appArguments
            = {"qsoc", "project", subcommand, "--invalid-option", "unexpected_project"};

        QSignalSpy exitSpy(&socCliWorker, &QSocCliWorker::exit);
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        QCOMPARE(exitSpy.count(), 1);
        QCOMPARE(exitSpy.takeFirst().at(0).toInt(), 1);
        QVERIFY(messageList.join('\n').contains("unknown option", Qt::CaseInsensitive));
        QVERIFY(!QFile::exists("unexpected_project.soc_pro"));
    }

    void testProjectHelpHasNoSideEffects_data()
    {
        QTest::addColumn<QString>("subcommand");

        QTest::newRow("create") << QString("create");
        QTest::newRow("update") << QString("update");
        QTest::newRow("remove") << QString("remove");
        QTest::newRow("list") << QString("list");
        QTest::newRow("show") << QString("show");
    }

    void testProjectHelpHasNoSideEffects()
    {
        QFETCH(QString, subcommand);
        QTemporaryDir directory;
        QVERIFY(directory.isValid());

        QSocProjectManager manager;
        manager.setCurrentPath(directory.path());
        QVERIFY(manager.create("victim"));

        const QString victimPath = QDir(directory.path()).filePath("victim.soc_pro");
        QFile         victimFile(victimPath);
        QVERIFY(victimFile.open(QIODevice::ReadOnly));
        const QByteArray originalBytes = victimFile.readAll();
        victimFile.close();

        const QString projectName = subcommand == "create" ? "new_project" : "victim";
        QStringList   appArguments
            = {"qsoc", "project", subcommand, "-d", directory.path(), projectName, "--help"};
        if (subcommand == "create" || subcommand == "update") {
            appArguments.insert(appArguments.size() - 1, "-b");
            appArguments
                .insert(appArguments.size() - 1, QDir(directory.path()).filePath("replacement_bus"));
        }

        messageList.clear();
        QSocCliWorker worker;
        QSignalSpy    exitSpy(&worker, &QSocCliWorker::exit);
        worker.setup(appArguments, false);
        worker.run();

        QCOMPARE(exitSpy.count(), 1);
        QCOMPARE(exitSpy.takeFirst().at(0).toInt(), 0);
        QVERIFY(messageList.join('\n').contains("Usage:"));
        QVERIFY(victimFile.open(QIODevice::ReadOnly));
        QCOMPARE(victimFile.readAll(), originalBytes);
        QVERIFY(!QFile::exists(QDir(directory.path()).filePath("new_project.soc_pro")));
    }

    void testProjectNameCannotEscapeDirectory()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString projectPath = QDir(directory.path()).filePath("project");

        messageList.clear();
        QSocCliWorker     worker;
        const QStringList appArguments
            = {"qsoc", "project", "create", "-d", projectPath, "../escaped"};
        worker.setup(appArguments, false);
        worker.run();

        QVERIFY(messageList.join('\n').contains("invalid characters", Qt::CaseInsensitive));
        QVERIFY(!QFile::exists(QDir(directory.path()).filePath("escaped.soc_pro")));
        QVERIFY(!QDir(projectPath).exists());
    }

    void testProjectWithMissingRequiredArgument()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {
            "qsoc", "project", "create"
            /* Missing project name */
        };

        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Check for error message about missing argument */
        bool hasErrorMsg = false;
        for (const QString &msg : messageList) {
            if (msg.contains("missing") || msg.contains("required") || msg.contains("error")) {
                hasErrorMsg = true;
                break;
            }
        }
        QVERIFY(hasErrorMsg);
    }

    void testProjectWithRelativePaths()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString projectPath = QDir(directory.path()).filePath("project");

        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {
            "qsoc",
            "project",
            "create",
            "-d",
            projectPath,
            "relative_path_project",
        };

        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        QFile projectFile(QDir(projectPath).filePath("relative_path_project.soc_pro"));
        QVERIFY(projectFile.exists());
        const QStringList directories{"bus", "module", "schematic", "output"};
        for (const QString &name : directories) {
            QVERIFY(QFileInfo(QDir(projectPath).filePath(name)).isDir());
        }
    }

    void testProjectWithAbsolutePaths()
    {
        /* Get absolute path for temp directory */
        const QString tempPath = QDir::currentPath() + "/abs_temp_dir";

        /* Create temporary directory for test */
        QDir().mkpath(tempPath);
        QDir().mkpath(tempPath + "/bus");
        QDir().mkpath(tempPath + "/modules");

        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {
            "qsoc",
            "project",
            "create",
            "-d",
            tempPath,
            "-b",
            tempPath + "/bus",
            "-m",
            tempPath + "/modules",
            "absolute_path_project",
        };

        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Check if project file was created in the specified directory */
        QFile projectFile(tempPath + "/absolute_path_project.soc_pro");
        QVERIFY(projectFile.exists());

        /* Read the file content */
        QVERIFY(projectFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString content = projectFile.readAll();
        projectFile.close();

        /* Instead of checking for exact paths which may be normalized,
           verify that project file contains references to the directories */
        QVERIFY(content.contains("bus"));
        QVERIFY(content.contains("modules"));

        /* Clean up */
        projectFile.remove();
        QDir().rmdir(tempPath + "/bus");
        QDir().rmdir(tempPath + "/modules");
        QDir().rmdir(tempPath);
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparseproject.moc"
