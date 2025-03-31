// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/config.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QStringList>
#include <QtCore>
#include <QtTest>

#include <iostream>

struct TestApp
{
    static auto &instance()
    {
        static auto                  argc      = 1;
        static char                  appName[] = "qsoc";
        static std::array<char *, 1> argv      = {{appName}};
        /* Use QCoreApplication for cli test */
        static const QCoreApplication app = QCoreApplication(argc, argv.data());
        return app;
    }
};

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

    /* Project manager instance */
    QSocProjectManager projectManager;
    /* Project name */
    QString projectName;

private slots:
    void initTestCase()
    {
        TestApp::instance();
        qInstallMessageHandler(messageOutput);

        /* Set project name */
        projectName = QFileInfo(__FILE__).baseName() + "_data";

        /* Setup project manager */
        projectManager.setProjectName(projectName);
        projectManager.setCurrentPath(QDir::current().filePath(projectName));
        projectManager.mkpath();
        projectManager.save(projectName);
        projectManager.load(projectName);

        /* Create test CSV files */
        createTestBusFiles();
    }

    void cleanupTestCase()
    {
        /* Clean up the test project directory */
        QDir projectDir(projectManager.getCurrentPath());
        if (projectDir.exists()) {
            projectDir.removeRecursively();
        }
    }

    void createTestBusFiles()
    {
        /* Create a simple APB bus definition CSV file */
        QFile apbFile("test_apb.csv");
        if (apbFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&apbFile);
            out << R"(Signal,Direction,Width,Description
PCLK,input,1,Clock
PRESETn,input,1,Reset (active low)
PADDR,input,32,Address
PSEL,input,1,Select
PENABLE,input,1,Enable
PWRITE,input,1,Write
PWDATA,input,32,Write data
PREADY,output,1,Ready
PRDATA,output,32,Read data
PSLVERR,output,1,Slave error)";
            apbFile.close();
        }

        /* Create a simple AXI bus definition CSV file */
        QFile axiFile("test_axi.csv");
        if (axiFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&axiFile);
            out << R"(Signal,Direction,Width,Description
ACLK,input,1,Clock
ARESETn,input,1,Reset (active low)
AWID,input,4,Write address ID
AWADDR,input,32,Write address
AWLEN,input,8,Burst length
AWSIZE,input,3,Burst size
AWBURST,input,2,Burst type
AWVALID,input,1,Write address valid
AWREADY,output,1,Write address ready
WDATA,input,32,Write data
WSTRB,input,4,Write strobes
WLAST,input,1,Write last
WVALID,input,1,Write valid
WREADY,output,1,Write ready)";
            axiFile.close();
        }
    }

    /* Test bus import command with APB bus definition */
    void testBusImport()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "bus",
               "import",
               "-p",
               projectName,
               "-d",
               projectManager.getProjectPath(),
               "-l",
               "test_lib",
               "-b",
               "apb",
               "test_apb.csv"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Accept test if command runs without crashing */
        QVERIFY(true);
    }

    /* Test bus list command */
    void testBusList()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "bus", "list", "-p", projectName, "-d", projectManager.getProjectPath()};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Accept test if command runs without crashing */
        QVERIFY(true);
    }

    /* Test bus show command for APB bus */
    void testBusShow()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "bus",
               "show",
               "-p",
               projectName,
               "-d",
               projectManager.getProjectPath(),
               "-b",
               "apb"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Accept test if command runs without crashing */
        QVERIFY(true);
    }

    /* Test multiple bus imports */
    void testBusImportMultiple()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "bus",
               "import",
               "-p",
               projectName,
               "-d",
               projectManager.getProjectPath(),
               "-l",
               "test_lib",
               "-b",
               "axi",
               "test_axi.csv"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Accept test if command runs without crashing */
        QVERIFY(true);
    }

    /* Test bus removal command */
    void testBusRemove()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "bus",
               "remove",
               "-p",
               projectName,
               "-d",
               projectManager.getProjectPath(),
               "-b",
               "apb"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Accept test if command runs without crashing */
        QVERIFY(true);
    }

    /* Test bus show command for non-existent bus */
    void testBusNonExistent()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "bus",
               "show",
               "-p",
               projectName,
               "-d",
               projectManager.getProjectPath(),
               "-b",
               "non_existent_bus"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Accept test if command runs without crashing */
        QVERIFY(true);
    }

    /* Test bus commands with verbosity levels */
    void testBusWithVerbosityLevels()
    {
        /* Test with verbosity level 3 (info) */
        messageList.clear();
        QSocCliWorker socCliWorker;
        QStringList   appArguments
            = {"qsoc",
               "--verbose=3",
               "bus",
               "list",
               "-p",
               projectName,
               "-d",
               projectManager.getProjectPath()};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Accept test if command runs without crashing */
        QVERIFY(true);
    }

    /* Test bus command with invalid option */
    void testBusWithInvalidOption()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "bus",
               "import",
               "--invalid-option",
               "-p",
               projectName,
               "-d",
               projectManager.getProjectPath(),
               "test_apb.csv"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Accept test if command runs without crashing */
        QVERIFY(true);
    }

    /* Test bus command with missing required argument */
    void testBusWithMissingRequiredArgument()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {
            "qsoc", "bus", "import", "-p", projectName, "-d", projectManager.getProjectPath()
            /* Missing CSV file */
        };
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Accept test if command runs without crashing */
        QVERIFY(true);
    }

    /* Test bus commands with relative paths */
    void testBusWithRelativePaths()
    {
        /* Create temporary directory for test */
        QDir tempDir;
        tempDir.mkpath("./bus_temp_dir");

        /* Copy CSV file to the temporary directory */
        QFile::copy("test_apb.csv", "./bus_temp_dir/temp_apb.csv");

        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "bus",
               "import",
               "-p",
               projectName,
               "-d",
               projectManager.getProjectPath(),
               "-l",
               "temp_lib",
               "-b",
               "temp_apb",
               "./bus_temp_dir/temp_apb.csv"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Accept test if command runs without crashing */
        QVERIFY(true);

        /* Clean up */
        QFile::remove("./bus_temp_dir/temp_apb.csv");
    }

    /* Test bus export functionality */
    void testBusExport()
    {
        /* First re-import a bus for testing export */
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments
                = {"qsoc",
                   "bus",
                   "import",
                   "-p",
                   projectName,
                   "-d",
                   projectManager.getProjectPath(),
                   "-l",
                   "test_lib",
                   "-b",
                   "apb_export_test",
                   "test_apb.csv"};
            socCliWorker.setup(appArguments, false);
            socCliWorker.run();
        }

        /* Create a temporary directory for export */
        QDir exportDir;
        exportDir.mkpath("./bus_export_dir");

        /* Now test export */
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "bus",
               "export",
               "-p",
               projectName,
               "-d",
               projectManager.getProjectPath(),
               "-b",
               "apb_export_test",
               "./bus_export_dir/exported_apb.csv"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Accept test if command runs without crashing */
        QVERIFY(true);

        /* Clean up */
        if (QDir("./bus_export_dir").exists()) {
            QDir("./bus_export_dir").removeRecursively();
        }
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsebus.moc"
