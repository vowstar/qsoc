// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/config.h"
#include "common/qsocbusmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTextStream>
#include <QtTest>

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
    QSocProjectManager projectManager;
    QSocBusManager     busManager;
    QString            projectName;

    static void messageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
    {
        Q_UNUSED(type);
        Q_UNUSED(context);
        messageList << msg;
    }

    /* Create a temporary file with specified content and return its path */
    QString createTempFile(const QString &fileName, const QString &content)
    {
        QString filePath = QDir(projectManager.getCurrentPath()).filePath(fileName);
        QFile   file(filePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&file);
            stream << content;
            file.close();
        }
        return filePath;
    }

    /* Create a simple APB bus CSV file and return its path */
    QString createApbBusCsv(const QString &fileName = "apb_bus.csv")
    {
        const QString csvContent
            = R"(Name;Mode;Presence;Direction;Initiative;Kind;Width;Bus width;Default value;Driver;Qualifier;Port match;System group;Protocol type;Payload name;Payload type;Payload extension;Description;
pclk;slave;;in;;;1;;;;;false;;;;;;;Clock
presetn;slave;;in;;;1;;;;;false;;;;;;;Reset (active low)
paddr;slave;;in;;;32;;;;;address;false;;;;;;;Address
psel;slave;;in;;;1;;;;;false;;;;;;;Select
penable;slave;;in;;;1;;;;;false;;;;;;;Enable
pwrite;slave;;in;;;1;;;;;false;;;;;;;Write
pwdata;slave;;in;;;32;;;;;data;false;;;;;;;Write data
pready;slave;;out;;;1;;;;;false;;;;;;;Ready
prdata;slave;;out;;;32;;;;;data;false;;;;;;;Read data
pslverr;slave;;out;;;1;;;;;false;;;;;;;Slave error)";

        return createTempFile(fileName, csvContent);
    }

    /* Create a simple APB bus with both master and slave modes CSV file and return its path */
    QString createFullApbBusCsv(const QString &fileName = "full_apb_bus.csv")
    {
        const QString csvContent
            = R"(Name;Mode;Presence;Direction;Initiative;Kind;Width;Bus width;Default value;Driver;Qualifier;Port match;System group;Protocol type;Payload name;Payload type;Payload extension;Description;
paddr;master;;out;;;32;;;;;address;false;;;;;;;Address out
paddr;slave;;in;;;32;;;;;address;false;;;;;;;Address in
penable;master;;out;;;1;;;;;false;;;;;;;Enable out
penable;slave;;in;;;1;;;;;false;;;;;;;Enable in
pprot;master;;out;;;3;;;;;false;;;;;;;Protection out
pprot;slave;;in;;;3;;;;;false;;;;;;;Protection in
prdata;master;;in;;;32;;;;;data;false;;;;;;;Read data in
prdata;slave;;out;;;32;;;;;data;false;;;;;;;Read data out
pready;master;;in;;;1;;;;;false;;;;;;;Ready in
pready;slave;;out;;;1;;;;;false;;;;;;;Ready out
pselx;master;;out;;;1;;;;;false;;;;;;;Select out
pselx;slave;;in;;;1;;;;;false;;;;;;;Select in
pslverr;master;;in;;;1;;;;;false;;;;;;;Slave error in
pslverr;slave;;out;;;1;;;;;false;;;;;;;Slave error out
pstrb;master;;out;;;4;;;;;data/8;false;;;;;;;Strobes out
pstrb;slave;;in;;;4;;;;;data/8;false;;;;;;;Strobes in
pwdata;master;;out;;;32;;;;;data;false;;;;;;;Write data out
pwdata;slave;;in;;;32;;;;;data;false;;;;;;;Write data in
pwrite;master;;out;;;1;;;;;false;;;;;;;Write out
pwrite;slave;;in;;;1;;;;;false;;;;;;;Write in)";

        return createTempFile(fileName, csvContent);
    }

    /* Create a simple AXI bus CSV file and return its path */
    QString createAxiBusCsv(const QString &fileName = "axi_bus.csv")
    {
        const QString csvContent
            = R"(Name;Mode;Presence;Direction;Initiative;Kind;Width;Bus width;Default value;Driver;Qualifier;System group;Protocol type;Payload name;Payload type;Payload extension;Description;
aclk;slave;;in;;;1;;;;;;;;;;;Clock
aresetn;slave;;in;;;1;;;;;;;;;;;Reset (active low)
araddr;slave;;in;;;;;;;address;;;;;;;
arburst;slave;;in;;;2;;;;;;;;;;;
arcache;slave;;in;;;4;;;;;;;;;;;
arid;slave;;in;;;;;;;;;;;;;;
arlen;slave;;in;;;8;;;;;;;;;;;
arlock;slave;;in;;;1;;;;;;;;;;;
arprot;slave;;in;;;3;;;;;;;;;;;
arqos;slave;;in;;;4;;;;;;;;;;;
arready;slave;;out;;;1;;;;;;;;;;;
arregion;slave;;in;;;4;;;;;;;;;;;
arsize;slave;;in;;;3;;;;;;;;;;;
aruser;slave;;in;;;;;;;;;;;;;;
arvalid;slave;;in;;;1;;;;;;;;;;;
awaddr;slave;;in;;;;;;;address;;;;;;;
awburst;slave;;in;;;2;;;;;;;;;;;
awcache;slave;;in;;;4;;;;;;;;;;;
awid;slave;;in;;;;;;;;;;;;;;
awlen;slave;;in;;;8;;;;;;;;;;;
awlock;slave;;in;;;1;;;;;;;;;;;
awprot;slave;;in;;;3;;;;;;;;;;;
awqos;slave;;in;;;4;;;;;;;;;;;
awready;slave;;out;;;1;;;;;;;;;;;
awregion;slave;;in;;;4;;;;;;;;;;;
awsize;slave;;in;;;3;;;;;;;;;;;
awuser;slave;;in;;;;;;;;;;;;;;
awvalid;slave;;in;;;1;;;;;;;;;;;
bid;slave;;out;;;;;;;;;;;;;;
bready;slave;;in;;;1;;;;;;;;;;;
bresp;slave;;out;;;2;;;;;;;;;;;
buser;slave;;out;;;;;;;;;;;;;;
bvalid;slave;;out;;;1;;;;;;;;;;;
rdata;slave;;out;;;;;;;data;;;;;;;
rid;slave;;out;;;;;;;;;;;;;;
rlast;slave;;out;;;1;;;;;;;;;;;
rready;slave;;in;;;1;;;;;;;;;;;
rresp;slave;;out;;;2;;;;;;;;;;;
ruser;slave;;out;;;;;;;;;;;;;;
rvalid;slave;;out;;;1;;;;;;;;;;;
wdata;slave;;in;;;;;;;data;;;;;;;
wlast;slave;;in;;;1;;;;;;;;;;;
wready;slave;;out;;;1;;;;;;;;;;;
wstrb;slave;;in;;;;;;;;;;;;;;
wuser;slave;;in;;;;;;;;;;;;;;
wvalid;slave;;in;;;1;;;;;;;;;;;)";

        return createTempFile(fileName, csvContent);
    }

    /* Create a comprehensive AXI bus with both master and slave modes CSV file and return its path */
    QString createFullAxiBusCsv(const QString &fileName = "full_axi_bus.csv")
    {
        const QString csvContent
            = R"(Name;Mode;Presence;Direction;Initiative;Kind;Width;Bus width;Default value;Driver;Qualifier;System group;Protocol type;Payload name;Payload type;Payload extension;Description;
araddr;master;;out;;;;;;;address;;;;;;;
araddr;slave;;in;;;;;;;address;;;;;;;
arburst;master;;out;;;2;;;;;;;;;;;
arburst;slave;;in;;;2;;;;;;;;;;;
arcache;master;;out;;;4;;;;;;;;;;;
arcache;slave;;in;;;4;;;;;;;;;;;
arid;master;;out;;;;;;;;;;;;;;
arid;slave;;in;;;;;;;;;;;;;;
arlen;master;;out;;;8;;;;;;;;;;;
arlen;slave;;in;;;8;;;;;;;;;;;
arlock;master;;out;;;1;;;;;;;;;;;
arlock;slave;;in;;;1;;;;;;;;;;;
arprot;master;;out;;;3;;;;;;;;;;;
arprot;slave;;in;;;3;;;;;;;;;;;
arqos;master;;out;;;4;;;;;;;;;;;
arqos;slave;;in;;;4;;;;;;;;;;;
arready;master;;in;;;1;;;;;;;;;;;
arready;slave;;out;;;1;;;;;;;;;;;
arregion;master;;out;;;4;;;;;;;;;;;
arregion;slave;;in;;;4;;;;;;;;;;;
arsize;master;;out;;;3;;;;;;;;;;;
arsize;slave;;in;;;3;;;;;;;;;;;
aruser;master;;out;;;;;;;;;;;;;;
aruser;slave;;in;;;;;;;;;;;;;;
arvalid;master;;out;;;1;;;;;;;;;;;
arvalid;slave;;in;;;1;;;;;;;;;;;
awaddr;master;;out;;;;;;;address;;;;;;;
awaddr;slave;;in;;;;;;;address;;;;;;;
awburst;master;;out;;;2;;;;;;;;;;;
awburst;slave;;in;;;2;;;;;;;;;;;
awcache;master;;out;;;4;;;;;;;;;;;
awcache;slave;;in;;;4;;;;;;;;;;;
awid;master;;out;;;;;;;;;;;;;;
awid;slave;;in;;;;;;;;;;;;;;
awlen;master;;out;;;8;;;;;;;;;;;
awlen;slave;;in;;;8;;;;;;;;;;;
awlock;master;;out;;;1;;;;;;;;;;;
awlock;slave;;in;;;1;;;;;;;;;;;
awprot;master;;out;;;3;;;;;;;;;;;
awprot;slave;;in;;;3;;;;;;;;;;;
awqos;master;;out;;;4;;;;;;;;;;;
awqos;slave;;in;;;4;;;;;;;;;;;
awready;master;;in;;;1;;;;;;;;;;;
awready;slave;;out;;;1;;;;;;;;;;;
awregion;master;;out;;;4;;;;;;;;;;;
awregion;slave;;in;;;4;;;;;;;;;;;
awsize;master;;out;;;3;;;;;;;;;;;
awsize;slave;;in;;;3;;;;;;;;;;;
awuser;master;;out;;;;;;;;;;;;;;
awuser;slave;;in;;;;;;;;;;;;;;
awvalid;master;;out;;;1;;;;;;;;;;;
awvalid;slave;;in;;;1;;;;;;;;;;;
bid;master;;in;;;;;;;;;;;;;;
bid;slave;;out;;;;;;;;;;;;;;
bready;master;;out;;;1;;;;;;;;;;;
bready;slave;;in;;;1;;;;;;;;;;;
bresp;master;;in;;;2;;;;;;;;;;;
bresp;slave;;out;;;2;;;;;;;;;;;
buser;master;;in;;;;;;;;;;;;;;
buser;slave;;out;;;;;;;;;;;;;;
bvalid;master;;in;;;1;;;;;;;;;;;
bvalid;slave;;out;;;1;;;;;;;;;;;
cactive;master;;in;;;1;;;;;;;;;;;
cactive;slave;;out;;;1;;;;;;;;;;;
cactive;system;;;;;;;;;;axi_lowpwr;;;;;;
csysack;master;;out;;;1;;;;;;;;;;;
csysack;slave;;out;;;1;;;;;;;;;;;
csysack;system;;;;;;;;;;axi_lowpwr;;;;;;
csysreq;master;;in;;;1;;;;;;;;;;;
csysreq;slave;;in;;;1;;;;;;;;;;;
csysreq;system;;in;;;1;;;;;axi_lowpwr;;;;;;
rdata;master;;in;;;;;;;data;;;;;;;
rdata;slave;;out;;;;;;;data;;;;;;;
rid;master;;in;;;;;;;;;;;;;;
rid;slave;;out;;;;;;;;;;;;;;
rlast;master;;in;;;1;;;;;;;;;;;
rlast;slave;;out;;;1;;;;;;;;;;;
rready;master;;out;;;1;;;;;;;;;;;
rready;slave;;in;;;1;;;;;;;;;;;
rresp;master;;in;;;2;;;;;;;;;;;
rresp;slave;;out;;;2;;;;;;;;;;;
ruser;master;;in;;;;;;;;;;;;;;
ruser;slave;;out;;;;;;;;;;;;;;
rvalid;master;;in;;;1;;;;;;;;;;;
rvalid;slave;;out;;;1;;;;;;;;;;;
wdata;master;;out;;;;;;;data;;;;;;;
wdata;slave;;in;;;;;;;data;;;;;;;
wlast;master;;out;;;1;;;;;;;;;;;
wlast;slave;;in;;;1;;;;;;;;;;;
wready;master;;in;;;1;;;;;;;;;;;
wready;slave;;out;;;1;;;;;;;;;;;
wstrb;master;;out;;;;;;;;;;;;;;
wstrb;slave;;in;;;;;;;;;;;;;;
wuser;master;;out;;;;;;;;;;;;;;
wuser;slave;;in;;;;;;;;;;;;;;
wvalid;master;;out;;;1;;;;;;;;;;;
wvalid;slave;;in;;;1;;;;;;;;;;;)";

        return createTempFile(fileName, csvContent);
    }

    /* Verify if a bus exists in the library */
    bool verifyBusExists(const QString &busName) { return busManager.isBusExist(busName); }

    /* Verify specific content in a bus port definition */
    bool verifyBusPortContent(
        const QString &busName, const QString &portName, const QString &direction, int width)
    {
        if (!busManager.isBusExist(busName)) {
            return false;
        }

        YAML::Node busNode = busManager.getBusYaml(busName);
        if (!busNode["port"]) {
            return false;
        }

        if (!busNode["port"][portName.toStdString()]) {
            return false;
        }

        YAML::Node portNode = busNode["port"][portName.toStdString()];

        // Check if the port has mode (slave/master) nodes
        bool directionMatch = false;
        bool widthMatch     = false;

        // Look for direction and width in nested mode structures (slave or master)
        if (portNode["slave"] && direction == "in" || direction == "out") {
            YAML::Node slaveNode = portNode["slave"];
            if (slaveNode["direction"]) {
                directionMatch
                    = (slaveNode["direction"].as<std::string>() == direction.toStdString());
            }
            if (slaveNode["width"]) {
                widthMatch = (slaveNode["width"].as<int>() == width);
            }
        }

        if (portNode["master"] && (direction == "in" || direction == "out")) {
            YAML::Node masterNode = portNode["master"];
            if (masterNode["direction"]) {
                directionMatch = directionMatch
                                 || (masterNode["direction"].as<std::string>()
                                     == direction.toStdString());
            }
            if (masterNode["width"]) {
                widthMatch = widthMatch || (masterNode["width"].as<int>() == width);
            }
        }

        // Also check for direct attributes (old format)
        if (portNode["direction"]) {
            directionMatch = directionMatch
                             || (portNode["direction"].as<std::string>() == direction.toStdString());
        }

        if (portNode["width"]) {
            widthMatch = widthMatch || (portNode["width"].as<int>() == width);
        }

        return directionMatch && widthMatch;
    }

    /* Check if a specific library file exists */
    bool verifyLibraryExists(const QString &libraryName)
    {
        return busManager.isLibraryFileExist(libraryName);
    }

    /* Check if the messageList contains a specific message */
    bool messageListContains(const QString &message)
    {
        for (const QString &msg : messageList) {
            if (msg.contains(message)) {
                return true;
            }
        }
        return false;
    }

private slots:
    void initTestCase()
    {
        TestApp::instance();
        /* Re-enable message handler for collecting CLI output */
        qInstallMessageHandler(messageOutput);

        /* Set project name */
        projectName = QFileInfo(__FILE__).baseName() + "_data";

        /* Setup project manager */
        projectManager.setProjectName(projectName);
        projectManager.setCurrentPath(QDir::current().filePath(projectName));
        projectManager.mkpath();
        projectManager.save(projectName);
        projectManager.load(projectName);

        /* Setup bus manager */
        busManager.setProjectManager(&projectManager);
    }

    void cleanupTestCase()
    {
#ifdef ENABLE_TEST_CLEANUP
        /* Clean up the test project directory */
        QDir projectDir(projectManager.getCurrentPath());
        if (projectDir.exists()) {
            projectDir.removeRecursively();
        }
#endif // ENABLE_TEST_CLEANUP
    }

    /* Test bus import command with APB bus definition */
    void testBusImport()
    {
        /* Create the APB bus CSV file for this test */
        const QString apbFilePath = createApbBusCsv("test_import_apb.csv");

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
               apbFilePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Load the library to verify it was created */
        busManager.load("test_lib");

        /* Verify that the bus was imported correctly */
        QVERIFY(verifyBusExists("apb"));
        QVERIFY(verifyBusPortContent("apb", "pclk", "in", 1));
        QVERIFY(verifyBusPortContent("apb", "paddr", "in", 32));
        QVERIFY(verifyBusPortContent("apb", "prdata", "out", 32));
        QVERIFY(messageListContains("Success: imported"));

        /* Clean up the CSV file */
        QFile::remove(apbFilePath);
    }

    /* Test bus list command */
    void testBusList()
    {
        /* Create an APB bus for this test */
        const QString apbFilePath = createApbBusCsv("test_list_apb.csv");

        /* Import the bus first */
        QSocCliWorker     importWorker;
        const QStringList importArgs
            = {"qsoc",
               "bus",
               "import",
               "-p",
               projectName,
               "-d",
               projectManager.getProjectPath(),
               "-l",
               "list_lib",
               "-b",
               "list_apb",
               apbFilePath};
        importWorker.setup(importArgs, false);
        importWorker.run();

        /* Now test the list command */
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "bus", "list", "-p", projectName, "-d", projectManager.getProjectPath()};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Load the library to check if it exists */
        busManager.load("list_lib");

        /* Verify that the bus exists */
        QVERIFY(busManager.isBusExist("list_apb"));
        /* Verify that the bus name is in the message list */
        QVERIFY(messageListContains("list_apb"));
    }

    /* Test bus show command for APB bus */
    void testBusShow()
    {
        /* Create an APB bus for this test */
        const QString apbFilePath = createApbBusCsv("test_show_apb.csv");

        /* Import the bus first */
        QSocCliWorker     importWorker;
        const QStringList importArgs
            = {"qsoc",
               "bus",
               "import",
               "-p",
               projectName,
               "-d",
               projectManager.getProjectPath(),
               "-l",
               "show_lib",
               "-b",
               "show_apb",
               apbFilePath};
        importWorker.setup(importArgs, false);
        importWorker.run();

        /* Now test the show command */
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
               "show_apb"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that the show command displays the bus details */
        QVERIFY(messageListContains("pclk") || messageListContains("PCLK"));
        QVERIFY(messageListContains("in"));
        QVERIFY(messageListContains("prdata") || messageListContains("PRDATA"));
        QVERIFY(messageListContains("out"));
        QVERIFY(messageListContains("32"));

        /* Clean up the CSV file */
        QFile::remove(apbFilePath);
    }

    /* Test multiple bus imports */
    void testBusImportMultiple()
    {
        /* Create an APB bus and AXI bus for this test */
        const QString apbFilePath = createApbBusCsv("test_multi_apb.csv");
        const QString axiFilePath = createAxiBusCsv("test_multi_axi.csv");

        /* Import the APB bus first */
        QSocCliWorker     importApbWorker;
        const QStringList importApbArgs
            = {"qsoc",
               "bus",
               "import",
               "-p",
               projectName,
               "-d",
               projectManager.getProjectPath(),
               "-l",
               "multi_lib",
               "-b",
               "multi_apb",
               apbFilePath};
        importApbWorker.setup(importApbArgs, false);
        importApbWorker.run();

        /* Now import the AXI bus */
        messageList.clear();
        QSocCliWorker     importAxiWorker;
        const QStringList importAxiArgs
            = {"qsoc",
               "bus",
               "import",
               "-p",
               projectName,
               "-d",
               projectManager.getProjectPath(),
               "-l",
               "multi_lib",
               "-b",
               "multi_axi",
               axiFilePath};
        importAxiWorker.setup(importAxiArgs, false);
        importAxiWorker.run();

        /* Reload the library to verify both buses are there */
        busManager.load("multi_lib");

        /* Verify both buses exist and have correct port content */
        QVERIFY(verifyBusExists("multi_apb"));
        QVERIFY(verifyBusExists("multi_axi"));
        QVERIFY(verifyBusPortContent("multi_axi", "aclk", "in", 1));
        QVERIFY(messageListContains("Success: imported"));

        /* Verify that both buses are in the same library */
        QSocCliWorker     listWorker;
        const QStringList listArgs
            = {"qsoc", "bus", "list", "-p", projectName, "-d", projectManager.getProjectPath()};
        messageList.clear();
        listWorker.setup(listArgs, false);
        listWorker.run();

        QVERIFY(messageListContains("multi_apb"));
        QVERIFY(messageListContains("multi_axi"));

        /* Clean up the CSV files */
        QFile::remove(apbFilePath);
        QFile::remove(axiFilePath);
    }

    /* Test bus removal command */
    void testBusRemove()
    {
        /* Create an APB bus and AXI bus for this test */
        const QString apbFilePath = createApbBusCsv("test_remove_apb.csv");
        const QString axiFilePath = createAxiBusCsv("test_remove_axi.csv");

        /* Import both buses */
        QSocCliWorker     importWorker;
        const QStringList importApbArgs
            = {"qsoc",
               "bus",
               "import",
               "-p",
               projectName,
               "-d",
               projectManager.getProjectPath(),
               "-l",
               "remove_lib",
               "-b",
               "remove_apb",
               apbFilePath};
        importWorker.setup(importApbArgs, false);
        importWorker.run();

        const QStringList importAxiArgs
            = {"qsoc",
               "bus",
               "import",
               "-p",
               projectName,
               "-d",
               projectManager.getProjectPath(),
               "-l",
               "remove_lib",
               "-b",
               "remove_axi",
               axiFilePath};
        importWorker.setup(importAxiArgs, false);
        importWorker.run();

        /* Now test the remove command */
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
               "remove_apb"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Reload the library and verify the bus was removed */
        busManager.load("remove_lib");
        QVERIFY(!verifyBusExists("remove_apb"));
        QVERIFY(verifyBusExists("remove_axi"));
        QVERIFY(messageListContains("Success: removed"));

        /* Clean up the CSV files */
        QFile::remove(apbFilePath);
        QFile::remove(axiFilePath);
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

        QVERIFY(messageListContains("Error: bus not found"));
        QVERIFY(!messageListContains("Success"));
    }

    /* Test bus command with invalid option */
    void testBusWithInvalidOption()
    {
        /* Create a bus file for this test */
        const QString apbFilePath = createApbBusCsv("test_invalid_option.csv");

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
               apbFilePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that an error message about the invalid option is displayed */
        QVERIFY(messageListContains("Error") || messageListContains("Unknown option"));
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

        /* Verify that an error message about the missing CSV file is displayed */
        QVERIFY(messageListContains("Error") || messageListContains("missing"));
    }

    /* Test bus commands with relative paths */
    void testBusWithRelativePaths()
    {
        /* Create temporary directory for test */
        QDir().mkpath("./bus_temp_dir");

        /* Create an APB bus file in the temporary directory */
        const QString apbContent
            = R"(Name;Mode;Presence;Direction;Initiative;Kind;Width;Bus width;Default value;Driver;Qualifier;Port match;System group;Protocol type;Payload name;Payload type;Payload extension;Description;
pclk;slave;;in;;;1;;;;;false;;;;;;;Clock
presetn;slave;;in;;;1;;;;;false;;;;;;;Reset (active low)
paddr;slave;;in;;;32;;;;;address;false;;;;;;;Address
psel;slave;;in;;;1;;;;;false;;;;;;;Select
penable;slave;;in;;;1;;;;;false;;;;;;;Enable
pwrite;slave;;in;;;1;;;;;false;;;;;;;Write
pwdata;slave;;in;;;32;;;;;data;false;;;;;;;Write data
pready;slave;;out;;;1;;;;;false;;;;;;;Ready
prdata;slave;;out;;;32;;;;;data;false;;;;;;;Read data
pslverr;slave;;out;;;1;;;;;false;;;;;;;Slave error)";

        QFile apbFile("./bus_temp_dir/temp_apb.csv");
        if (apbFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&apbFile);
            out << apbContent;
            apbFile.close();
        }

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

        /* Verify the bus was imported with a relative path */
        busManager.load("temp_lib");
        QVERIFY(verifyBusExists("temp_apb"));
        QVERIFY(verifyBusPortContent("temp_apb", "pclk", "in", 1));
        QVERIFY(verifyLibraryExists("temp_lib"));
        QVERIFY(messageListContains("Success: imported"));
    }

    /* Test showing help for bus command */
    void testBusHelp()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {"qsoc", "bus", "--help"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that help shows the available commands */
        QVERIFY(messageListContains("import"));
        QVERIFY(messageListContains("remove"));
        QVERIFY(messageListContains("list"));
        QVERIFY(messageListContains("show"));
    }

    /* Test bus import with both master and slave modes */
    void testBusImportWithMasterSlaveMode()
    {
        /* Create a full APB bus CSV file (with both master and slave modes) for this test */
        const QString apbFilePath = createFullApbBusCsv("test_full_apb.csv");

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
               "full_mode_lib",
               "-b",
               "full_apb",
               apbFilePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Load the library to verify it was created */
        busManager.load("full_mode_lib");

        /* Verify that the bus was imported correctly */
        QVERIFY(verifyBusExists("full_apb"));

        /* Verify both master and slave modes */
        /* Master mode signals (direction out) */
        QVERIFY(verifyBusPortContent("full_apb", "paddr", "out", 32));
        QVERIFY(verifyBusPortContent("full_apb", "penable", "out", 1));
        QVERIFY(verifyBusPortContent("full_apb", "pwrite", "out", 1));

        /* Master mode signals (direction in) */
        QVERIFY(verifyBusPortContent("full_apb", "prdata", "in", 32));
        QVERIFY(verifyBusPortContent("full_apb", "pready", "in", 1));

        /* Safely check YAML node values instead of directly accessing with potentially unsafe operators */
        YAML::Node busNode      = busManager.getBusYaml("full_apb");
        const bool hasPaddrNode = busNode["port"] && busNode["port"]["paddr"];

        // Use try-catch to prevent uncaught exceptions
        try {
            if (hasPaddrNode && busNode["port"]["paddr"]["mode"]) {
                const auto mode = busNode["port"]["paddr"]["mode"].as<std::string>();
                QVERIFY(mode == "master" || mode == "slave");
            }

            if (hasPaddrNode && busNode["port"]["paddr"]["qualifier"]) {
                const auto qualifier = busNode["port"]["paddr"]["qualifier"].as<std::string>();
                QVERIFY(qualifier == "address");
            }
        } catch (const std::exception &e) {
            QFAIL("Exception while accessing YAML");
        }

        /* Check the presence of the bus in the list command */
        QSocCliWorker listWorker;
        messageList.clear();
        const QStringList listArgs
            = {"qsoc", "bus", "list", "-p", projectName, "-d", projectManager.getProjectPath()};
        listWorker.setup(listArgs, false);
        listWorker.run();

        QVERIFY(messageListContains("full_apb"));
    }

    /* Test bus import with full AXI implementation including master, slave and system modes */
    void testBusImportWithFullAxi()
    {
        /* Create a full AXI bus CSV file with both master and slave modes for this test */
        const QString axiFilePath = createFullAxiBusCsv("test_full_axi.csv");

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
               "full_axi_lib",
               "-b",
               "full_axi",
               axiFilePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Load the library to verify it was created */
        busManager.load("full_axi_lib");

        /* Verify that the bus was imported correctly */
        QVERIFY(verifyBusExists("full_axi"));

        /* Check the presence of the bus in the list command */
        QSocCliWorker listWorker;
        messageList.clear();
        const QStringList listArgs
            = {"qsoc", "bus", "list", "-p", projectName, "-d", projectManager.getProjectPath()};
        listWorker.setup(listArgs, false);
        listWorker.run();

        QVERIFY(messageListContains("full_axi"));
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsebus.moc"
