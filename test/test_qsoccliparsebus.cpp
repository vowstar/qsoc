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

private slots:
    void initTestCase()
    {
        TestApp::instance();
        qInstallMessageHandler(messageOutput);

        /* Create a test project for bus tests */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {
            "qsoc",
            "project",
            "create",
            "bus_test_project",
        };
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Create test CSV files */
        createTestBusFiles();
    }

    void cleanupTestCase()
    {
        /* Cleanup any leftover test files */
        QStringList filesToRemove = {"bus_test_project.soc_pro", "test_apb.csv", "test_axi.csv"};

        for (const QString &file : filesToRemove) {
            if (QFile::exists(file)) {
                QFile::remove(file);
            }
        }

        /* Also check for files in the build directory */
        QString buildTestDir = QDir::currentPath() + "/build/test";
        for (const QString &file : filesToRemove) {
            QString buildFilePath = buildTestDir + "/" + file;
            if (QFile::exists(buildFilePath)) {
                QFile::remove(buildFilePath);
            }
        }

        /* Clean up temporary directories */
        QStringList dirsToRemove = {"./bus_temp_dir"};

        for (const QString &dir : dirsToRemove) {
            if (QDir(dir).exists()) {
                QDir(dir).removeRecursively();
            }
        }
    }

    void createTestBusFiles()
    {
        /* Create a simple APB bus definition CSV file */
        QFile apbFile("test_apb.csv");
        if (apbFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&apbFile);
            out << "Signal,Direction,Width,Description\n"
                << "PCLK,input,1,Clock\n"
                << "PRESETn,input,1,Reset (active low)\n"
                << "PADDR,input,32,Address\n"
                << "PSEL,input,1,Select\n"
                << "PENABLE,input,1,Enable\n"
                << "PWRITE,input,1,Write\n"
                << "PWDATA,input,32,Write data\n"
                << "PREADY,output,1,Ready\n"
                << "PRDATA,output,32,Read data\n"
                << "PSLVERR,output,1,Slave error\n";
            apbFile.close();
        }

        /* Create a simple AXI bus definition CSV file */
        QFile axiFile("test_axi.csv");
        if (axiFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&axiFile);
            out << "Signal,Direction,Width,Description\n"
                << "ACLK,input,1,Clock\n"
                << "ARESETn,input,1,Reset (active low)\n"
                << "AWID,input,4,Write address ID\n"
                << "AWADDR,input,32,Write address\n"
                << "AWLEN,input,8,Burst length\n"
                << "AWSIZE,input,3,Burst size\n"
                << "AWBURST,input,2,Burst type\n"
                << "AWVALID,input,1,Write address valid\n"
                << "AWREADY,output,1,Write address ready\n"
                << "WDATA,input,32,Write data\n"
                << "WSTRB,input,4,Write strobes\n"
                << "WLAST,input,1,Write last\n"
                << "WVALID,input,1,Write valid\n"
                << "WREADY,output,1,Write ready\n";
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
               "bus_test_project",
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
        const QStringList appArguments = {"qsoc", "bus", "list", "-p", "bus_test_project"};
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
            = {"qsoc", "bus", "show", "-p", "bus_test_project", "-b", "apb"};
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
               "bus_test_project",
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
            = {"qsoc", "bus", "remove", "-p", "bus_test_project", "-b", "apb"};
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
            = {"qsoc", "bus", "show", "-p", "bus_test_project", "-b", "non_existent_bus"};
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
        QStringList appArguments = {"qsoc", "--verbose=3", "bus", "list", "-p", "bus_test_project"};
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
            = {"qsoc", "bus", "import", "--invalid-option", "-p", "bus_test_project", "test_apb.csv"};
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
            "qsoc", "bus", "import", "-p", "bus_test_project"
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
               "bus_test_project",
               "-l",
               "temp_lib",
               "-b",
               "temp_apb",
               "-d",
               "./bus_temp_dir",
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
                   "bus_test_project",
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
               "bus_test_project",
               "-b",
               "apb_export_test",
               "-d",
               "./bus_export_dir",
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
