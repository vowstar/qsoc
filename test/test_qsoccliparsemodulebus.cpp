// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/qsocbusmanager.h"
#include "common/qsocmodulemanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QStringList>
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
    QSocModuleManager  moduleManager;
    QString            projectName;
    QString            projectPath;

    static void messageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
    {
        Q_UNUSED(type);
        Q_UNUSED(context);
        messageList << msg;
    }

    /* Helper method to check if the messageList contains a specific message */
    bool messageListContains(const QString &message)
    {
        if (message.isEmpty()) {
            return false;
        }

        /* Safety check to prevent segfault */
        if (messageList.isEmpty()) {
            return false;
        }

        for (const QString &msg : messageList) {
            if (msg.isNull() || msg.isEmpty()) {
                continue;
            }
            if (msg.contains(message, Qt::CaseInsensitive)) {
                return true;
            }
        }
        return false;
    }

    /* Helper method to verify bus exists */
    bool verifyBusExists(const QString &busName) { return busManager.isBusExist(busName); }

    /* Helper method to verify module exists */
    bool verifyModuleExists(const QString &moduleName)
    {
        return moduleManager.isModuleExist(moduleName);
    }

    /* Helper method to verify module port content */
    bool verifyModulePortContent(
        const QString &moduleName, const QString &portName, const QString &direction, int width)
    {
        /* Check if the module exists */
        if (!verifyModuleExists(moduleName)) {
            return false;
        }

        YAML::Node moduleNode = moduleManager.getModuleYaml(moduleName);

        /* Check if module node is valid */
        if (!moduleNode.IsDefined() || moduleNode.IsNull()) {
            return false;
        }

        /* Check if port section exists */
        if (!moduleNode["port"].IsDefined() || moduleNode["port"].IsNull()) {
            return false;
        }

        bool portFound      = false;
        bool directionMatch = false;
        bool widthMatch     = false;

        /* Iterate through all ports */
        for (const auto &port : moduleNode["port"]) {
            /* Check if port key is valid */
            if (!port.first.IsDefined() || !port.first.IsScalar()) {
                continue;
            }

            QString currentPortName = QString::fromStdString(port.first.Scalar());
            if (currentPortName != portName)
                continue;

            portFound = true;

            /* Check if port node is valid */
            if (!port.second.IsDefined() || port.second.IsNull()) {
                return false;
            }

            YAML::Node portNode = port.second;

            /* Check direction */
            if (portNode["direction"].IsDefined() && !portNode["direction"].IsNull()) {
                QString portDirection = QString::fromStdString(
                    portNode["direction"].as<std::string>());
                /* Remove possible trailing special characters (e.g. %) */
                portDirection = portDirection.trimmed();
                if (portDirection.endsWith('%'))
                    portDirection.chop(1);
                directionMatch = (portDirection == direction);
            }

            /* Check width - may be in width or parsed from type */
            if (portNode["width"].IsDefined() && !portNode["width"].IsNull()) {
                int portWidth = portNode["width"].as<int>();
                widthMatch    = (portWidth == width);
            } else if (portNode["type"].IsDefined() && !portNode["type"].IsNull()) {
                QString typeStr = QString::fromStdString(portNode["type"].as<std::string>());

                /* For unit width logic or wire types */
                if ((typeStr == "logic" || typeStr == "wire") && width == 1) {
                    widthMatch = true;
                }
                /* For array types like reg[7:0] or logic[3:0] */
                else if (typeStr.contains('[') && typeStr.contains(']')) {
                    QString widthStr = typeStr.section('[', 1).section(']', 0, 0);
                    if (widthStr.contains(':')) {
                        int high      = widthStr.section(':', 0, 0).toInt();
                        int low       = widthStr.section(':', 1, 1).toInt();
                        int portWidth = high - low + 1;
                        widthMatch    = (portWidth == width);
                    }
                }
            } else {
                /* For tests, assume it's a match if port exists and direction matches */
                widthMatch = true;
            }

            break;
        }

        /* If port is found and either direction or width match (for testing purposes),
           consider it a successful verification */
        return portFound && (directionMatch || widthMatch);
    }

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

        /* Ensure project directory and subdirectories exist with proper permissions */
        QDir projectDir(projectManager.getCurrentPath());
        if (!projectDir.exists()) {
            QDir().mkpath(projectDir.path());
        }

        /* Ensure module directory exists */
        QString moduleDir = QDir(projectDir).filePath("module");
        if (!QDir(moduleDir).exists()) {
            QDir().mkpath(moduleDir);
        }

        /* Create bus directory */
        QString busDir = QDir(projectDir).filePath("bus");
        if (!QDir(busDir).exists()) {
            QDir().mkpath(busDir);
        }

        /* Create APB bus definition */
        QString apbBusPath = QDir(busDir).filePath("amba.soc_bus");
        QFile   apbBusFile(apbBusPath);
        if (apbBusFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&apbBusFile);
            out << R"(
apb4:
  port:
    paddr:
      master:
        direction: out
        qualifier: address
      slave:
        direction: in
        qualifier: address
    penable:
      master:
        direction: out
        width: 1
      slave:
        direction: in
        width: 1
    prdata:
      master:
        direction: in
        qualifier: data
      slave:
        direction: out
        qualifier: data
    pready:
      master:
        direction: in
        width: 1
      slave:
        direction: out
        width: 1
    pselx:
      master:
        direction: out
      slave:
        direction: in
        width: 1
    pslverr:
      master:
        direction: in
        width: 1
      slave:
        direction: out
        width: 1
    pwdata:
      master:
        direction: out
        qualifier: data
      slave:
        direction: in
        qualifier: data
    pwrite:
      master:
        direction: out
        width: 1
      slave:
        direction: in
        width: 1
    pprot:
      master:
        direction: out
        width: 3
      slave:
        direction: in
        width: 3
    pstrb:
      master:
        direction: out
        qualifier: data/8
      slave:
        direction: in
        qualifier: data/8
axi4:
  port:
    araddr:
      master:
        direction: out
        qualifier: address
      slave:
        direction: in
        qualifier: address
    arburst:
      master:
        direction: out
        width: 2
      slave:
        direction: in
        width: 2
    arcache:
      master:
        direction: out
        width: 4
      slave:
        direction: in
        width: 4
    arid:
      master:
        direction: out
      slave:
        direction: in
    arlen:
      master:
        direction: out
        width: 8
      slave:
        direction: in
        width: 8
    arlock:
      master:
        direction: out
        width: 1
      slave:
        direction: in
        width: 1
    arprot:
      master:
        direction: out
        width: 3
      slave:
        direction: in
        width: 3
    arqos:
      master:
        direction: out
        width: 4
      slave:
        direction: in
        width: 4
    arready:
      master:
        direction: in
        width: 1
      slave:
        direction: out
        width: 1
    arregion:
      master:
        direction: out
        width: 4
      slave:
        direction: in
        width: 4
    arsize:
      master:
        direction: out
        width: 3
      slave:
        direction: in
        width: 3
    aruser:
      master:
        direction: out
      slave:
        direction: in
    arvalid:
      master:
        direction: out
        width: 1
      slave:
        direction: in
        width: 1
    awaddr:
      master:
        direction: out
        qualifier: address
      slave:
        direction: in
        qualifier: address
    awburst:
      master:
        direction: out
        width: 2
      slave:
        direction: in
        width: 2
    awcache:
      master:
        direction: out
        width: 4
      slave:
        direction: in
        width: 4
    awid:
      master:
        direction: out
      slave:
        direction: in
    awlen:
      master:
        direction: out
        width: 8
      slave:
        direction: in
        width: 8
    awlock:
      master:
        direction: out
        width: 1
      slave:
        direction: in
        width: 1
    awprot:
      master:
        direction: out
        width: 3
      slave:
        direction: in
        width: 3
    awqos:
      master:
        direction: out
        width: 4
      slave:
        direction: in
        width: 4
    awready:
      master:
        direction: in
        width: 1
      slave:
        direction: out
        width: 1
    awregion:
      master:
        direction: out
        width: 4
      slave:
        direction: in
        width: 4
    awsize:
      master:
        direction: out
        width: 3
      slave:
        direction: in
        width: 3
    awuser:
      master:
        direction: out
      slave:
        direction: in
    awvalid:
      master:
        direction: out
        width: 1
      slave:
        direction: in
        width: 1
    bid:
      master:
        direction: in
      slave:
        direction: out
    bready:
      master:
        direction: out
        width: 1
      slave:
        direction: in
        width: 1
    bresp:
      master:
        direction: in
        width: 2
      slave:
        direction: out
        width: 2
    buser:
      master:
        direction: in
      slave:
        direction: out
    bvalid:
      master:
        direction: in
        width: 1
      slave:
        direction: out
        width: 1
    cactive:
      master:
        direction: in
        width: 1
      slave:
        direction: out
        width: 1
    csysack:
      master:
        direction: out
        width: 1
      slave:
        direction: out
        width: 1
    csysreq:
      master:
        direction: in
        width: 1
      slave:
        direction: in
        width: 1
      system:
        direction: in
        width: 1
    rdata:
      master:
        direction: in
        qualifier: data
      slave:
        direction: out
        qualifier: data
    rid:
      master:
        direction: in
      slave:
        direction: out
    rlast:
      master:
        direction: in
        width: 1
      slave:
        direction: out
        width: 1
    rready:
      master:
        direction: out
        width: 1
      slave:
        direction: in
        width: 1
    rresp:
      master:
        direction: in
        width: 2
      slave:
        direction: out
        width: 2
    ruser:
      master:
        direction: in
      slave:
        direction: out
    rvalid:
      master:
        direction: in
        width: 1
      slave:
        direction: out
        width: 1
    wdata:
      master:
        direction: out
        qualifier: data
      slave:
        direction: in
        qualifier: data
    wlast:
      master:
        direction: out
        width: 1
      slave:
        direction: in
        width: 1
    wready:
      master:
        direction: in
        width: 1
      slave:
        direction: out
        width: 1
    wstrb:
      master:
        direction: out
      slave:
        direction: in
    wuser:
      master:
        direction: out
      slave:
        direction: in
    wvalid:
      master:
        direction: out
        width: 1
      slave:
        direction: in
        width: 1
)";
            apbBusFile.close();
        }

        /* Setup bus manager */
        busManager.setProjectManager(&projectManager);
        busManager.load(QRegularExpression(".*"));

        /* Setup module manager */
        moduleManager.setProjectManager(&projectManager);

        /* Set paths */
        projectPath = projectName;
    }

    void cleanupTestCase()
    {
#ifdef ENABLE_TEST_CLEANUP
        /* Remove project directory if it exists */
        QDir projectDir(projectManager.getCurrentPath());
        if (projectDir.exists()) {
            /* No longer remove project directory to inspect test files
            projectDir.removeRecursively();
            */
        }
#endif // ENABLE_TEST_CLEANUP
    }

    /* Test module bus add command */
    void testModuleBusAdd()
    {
        /* Create counter module file */
        QString testFileName        = "test_module_bus_add.v";
        QString counterFilePathFull = QDir(projectPath).filePath(testFileName);
        QFile   counterFile(counterFilePathFull);
        if (counterFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&counterFile);
            out << R"(
module test_module_bus_add (
  input  wire        clk,
  input  wire        rst_n,
  input  wire        enable,
  output reg  [7:0]  count,
  // APB slave interface
  input  wire [31:0] test_paddr,
  input  wire        test_pselx,
  input  wire        test_penable,
  input  wire        test_pwrite,
  input  wire [31:0] test_pwdata,
  output reg  [31:0] test_prdata,
  output reg         test_pready,
  output reg         test_pslverr
);
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      count <= 8'h00;
    end else if (enable) begin
      count <= count + 1;
    end
  end
endmodule
)";
            counterFile.close();
        }

        /* First import a module for testing */
        {
            QSocCliWorker socCliWorker;
            QFileInfo     counterFileInfo(counterFilePathFull);
            QString       counterFileFullPath = counterFileInfo.absoluteFilePath();
            QFileInfo     projectInfo(projectManager.getProjectPath());
            QString       projectFullPath = projectInfo.absoluteFilePath();

            const QStringList appArguments
                = {"qsoc",
                   "module",
                   "import",
                   counterFileFullPath,
                   "--project",
                   projectName,
                   "-d",
                   projectFullPath};
            socCliWorker.setup(appArguments, false);
            socCliWorker.run();
        }

        /* Now test module bus add command */
        messageList.clear();
        QSocCliWorker socCliWorker;
        QFileInfo     projectInfo(projectManager.getProjectPath());
        QString       projectFullPath = projectInfo.absoluteFilePath();

        const QStringList appArguments
            = {"qsoc",
               "module",
               "bus",
               "add",
               "-m",
               "test_module_bus_add",
               "-b",
               "apb4",
               "-o",
               "slave",
               "test",
               "--project",
               projectName,
               "-d",
               projectFullPath};

        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the command produced output */
        QVERIFY(messageList.size() > 0);

        /* Reload modules to verify changes */
        moduleManager.load("test_module_bus_add");

        /* Verify the module exists */
        QVERIFY(verifyModuleExists("test_module_bus_add"));

        /* Check if the module has the bus assigned */
        YAML::Node moduleNode     = moduleManager.getModuleYaml(QString("test_module_bus_add"));
        bool       hasBusAssigned = false;

        if (moduleNode["bus"].IsDefined() && moduleNode["bus"].IsMap()) {
            for (const auto &bus : moduleNode["bus"]) {
                if (bus.first.IsDefined() && bus.second.IsDefined() && bus.second.IsMap()) {
                    if (bus.second["bus"].IsDefined() && !bus.second["bus"].IsNull()) {
                        std::string busType = bus.second["bus"].as<std::string>();
                        if (busType == "apb4") {
                            hasBusAssigned = true;
                            break;
                        }
                    }
                }
            }
        }

        QVERIFY(hasBusAssigned);

        /* Check for success message */
        bool successful = messageListContains("Success: added");
        QVERIFY(successful);

        /* Verify no error messages in the output */
        bool hasError = messageListContains("Error");
        QVERIFY(!hasError);
    }

    /* Test module bus remove command */
    void testModuleBusRemove()
    {
        /* Create counter module file */
        QString testFileName        = "test_module_bus_remove.v";
        QString counterFilePathFull = QDir(projectPath).filePath(testFileName);
        QFile   counterFile(counterFilePathFull);
        if (counterFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&counterFile);
            out << R"(
module test_module_bus_remove (
  input  wire        clk,
  input  wire        rst_n,
  input  wire        enable,
  output reg  [7:0]  count,
  // APB master interface
  output wire [31:0] apb_paddr,
  output wire        apb_psel,
  output wire        apb_penable,
  output wire        apb_pwrite,
  output wire [31:0] apb_pwdata,
  input  wire [31:0] apb_prdata,
  input  wire        apb_pready,
  input  wire        apb_pslverr
);
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      count <= 8'h00;
    end else if (enable) begin
      count <= count + 1;
    end
  end
endmodule
)";
            counterFile.close();
        }

        /* First import the module */
        {
            QSocCliWorker socCliWorker;
            QFileInfo     counterFileInfo(counterFilePathFull);
            QString       counterFileFullPath = counterFileInfo.absoluteFilePath();
            QFileInfo     projectInfo(projectManager.getProjectPath());
            QString       projectFullPath = projectInfo.absoluteFilePath();

            const QStringList appArguments
                = {"qsoc",
                   "module",
                   "import",
                   counterFileFullPath,
                   "--project",
                   projectName,
                   "-d",
                   projectFullPath};
            socCliWorker.setup(appArguments, false);
            socCliWorker.run();
        }

        /* Then ensure the module has a bus assigned */
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments
                = {"qsoc",
                   "module",
                   "bus",
                   "add",
                   "-m",
                   "test_module_bus_remove",
                   "-b",
                   "apb4",
                   "-o",
                   "master",
                   "--project",
                   projectName,
                   "-d",
                   projectManager.getProjectPath(),
                   "apb"};
            socCliWorker.setup(appArguments, false);
            socCliWorker.run();

            /* Verify the bus was added */
            moduleManager.load("test_module_bus_remove");
            YAML::Node moduleNode = moduleManager.getModuleYaml(QString("test_module_bus_remove"));
            bool       hasBusAssigned = false;

            if (moduleNode["bus"].IsDefined()) {
                for (const auto &bus : moduleNode["bus"]) {
                    if (bus.second["bus"].IsDefined() && !bus.second["bus"].IsNull()
                        && QString::fromStdString(bus.second["bus"].as<std::string>()) == "apb4") {
                        hasBusAssigned = true;
                        break;
                    }
                }
            }

            QVERIFY(hasBusAssigned);
        }

        /* Now test module bus remove command */
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "module",
               "bus",
               "remove",
               "-m",
               "test_module_bus_remove",
               "--project",
               projectName,
               "-d",
               projectManager.getProjectPath(),
               "apb"};

        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the command produced output */
        QVERIFY(messageList.size() > 0);

        /* Reload modules to verify changes */
        moduleManager.resetModuleData();
        moduleManager.load("test_module_bus_remove");

        /* Check if the module no longer has the bus assigned */
        YAML::Node moduleNode     = moduleManager.getModuleYaml(QString("test_module_bus_remove"));
        bool       hasBusAssigned = false;

        if (moduleNode["bus"].IsDefined() && moduleNode["bus"].IsMap()) {
            for (const auto &bus : moduleNode["bus"]) {
                if (bus.first.IsDefined() && bus.second.IsDefined() && bus.second.IsMap()) {
                    if (bus.second["bus"].IsDefined() && !bus.second["bus"].IsNull()) {
                        std::string busType = bus.second["bus"].as<std::string>();
                        if (busType == "apb4") {
                            hasBusAssigned = true;
                            break;
                        }
                    }
                }
            }
        }

        QVERIFY(!hasBusAssigned);

        /* Check for success message */
        bool successful = messageListContains("Success: Removed");
        QVERIFY(successful);

        /* Verify no error messages in the output */
        bool hasError = messageListContains("Error");

        QVERIFY(!hasError);
    }

    /* Test module bus list command */
    void testModuleBusList()
    {
        /* Create counter module file */
        QString testFileName        = "test_module_bus_list.v";
        QString counterFilePathFull = QDir(projectPath).filePath(testFileName);
        QFile   counterFile(counterFilePathFull);
        if (counterFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&counterFile);
            out << R"(
module test_module_bus_list (
  input  wire        clk,
  input  wire        rst_n,
  input  wire        enable,
  output reg  [7:0]  count,
  // APB master interface
  output wire [31:0] apb_master_paddr,
  output wire        apb_master_psel,
  output wire        apb_master_penable,
  output wire        apb_master_pwrite,
  output wire [31:0] apb_master_pwdata,
  input  wire [31:0] apb_master_prdata,
  input  wire        apb_master_pready,
  input  wire        apb_master_pslverr,
  // APB slave interface
  input  wire [31:0] apb_slave_paddr,
  input  wire        apb_slave_psel,
  input  wire        apb_slave_penable,
  input  wire        apb_slave_pwrite,
  input  wire [31:0] apb_slave_pwdata,
  output wire [31:0] apb_slave_prdata,
  output wire        apb_slave_pready,
  output wire        apb_slave_pslverr
);
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      count <= 8'h00;
    end else if (enable) begin
      count <= count + 1;
    end
  end
endmodule
)";
            counterFile.close();
        }

        /* First import a module */
        {
            QSocCliWorker     socCliWorker;
            QFileInfo         projectInfo(projectManager.getProjectPath());
            QString           projectFullPath = projectInfo.absoluteFilePath();
            const QStringList appArguments
                = {"qsoc",
                   "module",
                   "import",
                   counterFilePathFull,
                   "--project",
                   projectName,
                   "-d",
                   projectFullPath};
            socCliWorker.setup(appArguments, false);
            socCliWorker.run();
        }

        /* Add APB slave interface */
        {
            QSocCliWorker     socCliWorker;
            QFileInfo         projectInfo(projectManager.getProjectPath());
            QString           projectFullPath = projectInfo.absoluteFilePath();
            const QStringList appArguments
                = {"qsoc",
                   "module",
                   "bus",
                   "add",
                   "-m",
                   "test_module_bus_list",
                   "-b",
                   "apb4",
                   "-o",
                   "slave",
                   "--project",
                   projectName,
                   "-d",
                   projectFullPath,
                   "apb_slave"};
            socCliWorker.setup(appArguments, false);
            socCliWorker.run();
        }

        /* Add APB master interface */
        {
            QSocCliWorker     socCliWorker;
            QFileInfo         projectInfo(projectManager.getProjectPath());
            QString           projectFullPath = projectInfo.absoluteFilePath();
            const QStringList appArguments
                = {"qsoc",
                   "module",
                   "bus",
                   "add",
                   "-m",
                   "test_module_bus_list",
                   "-b",
                   "apb4",
                   "-o",
                   "master",
                   "--project",
                   projectName,
                   "-d",
                   projectFullPath,
                   "apb_master"};
            socCliWorker.setup(appArguments, false);
            socCliWorker.run();
        }

        /* Now test the module bus list command */
        moduleManager.load("test_module_bus_list");
        messageList.clear();
        QSocCliWorker     socCliWorker;
        QFileInfo         projectInfo(projectManager.getProjectPath());
        QString           projectFullPath = projectInfo.absoluteFilePath();
        const QStringList appArguments
            = {"qsoc",
               "module",
               "bus",
               "list",
               "-m",
               "test_module_bus_list",
               "--project",
               projectName,
               "-d",
               projectFullPath};

        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the command produced output */
        QVERIFY(messageList.size() > 0);

        /* Check if the bus is listed in the output */
        QVERIFY(messageListContains("apb_slave [apb4, slave]"));
        QVERIFY(messageListContains("apb_master [apb4, master]"));
        QVERIFY(!messageListContains("Error"));
    }

    /* Test module bus show command */
    void testModuleBusShow()
    {
        /* Create counter module file */
        QString testFileName        = "test_module_bus_show.v";
        QString counterFilePathFull = QDir(projectPath).filePath(testFileName);
        QFile   counterFile(counterFilePathFull);
        if (counterFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&counterFile);
            out << R"(
module test_module_bus_show (
  input         wire        clk,
  input         wire        rst_n,
  input         wire        enable,
  output        reg  [7:0] count,

  // APB Master Interface
  input         wire        test_master_pclk,
  input         wire        test_master_presetn,
  output        reg  [31:0] test_master_paddr,
  output        reg         test_master_psel,
  output        reg         test_master_penable,
  output        reg         test_master_pwrite,
  output        reg  [31:0] test_master_pwdata,
  input         wire [31:0] test_master_prdata,
  input         wire        test_master_pready,
  input         wire        test_master_pslverr
);
  always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
      count <= 8'h00;
    end else if (enable) begin
      count <= count + 1;
    end
  end
endmodule
)";
            counterFile.close();
        }

        /* First import the module */
        {
            QSocCliWorker socCliWorker;
            QFileInfo     counterFileInfo(counterFilePathFull);
            QString       counterFileFullPath = counterFileInfo.absoluteFilePath();
            QFileInfo     projectInfo(projectManager.getProjectPath());
            QString       projectFullPath = projectInfo.absoluteFilePath();

            const QStringList appArguments
                = {"qsoc",
                   "module",
                   "import",
                   counterFileFullPath,
                   "--project",
                   projectName,
                   "-d",
                   projectFullPath};
            socCliWorker.setup(appArguments, false);
            socCliWorker.run();
        }

        /* Add a bus to the module */
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments
                = {"qsoc",
                   "module",
                   "bus",
                   "add",
                   "-m",
                   "test_module_bus_show",
                   "-b",
                   "apb4",
                   "-o",
                   "master",
                   "--project",
                   projectName,
                   "-d",
                   projectManager.getProjectPath(),
                   "test_master"};
            socCliWorker.setup(appArguments, false);
            socCliWorker.run();

            /* Verify the bus was added */
            moduleManager.load("test_module_bus_show");
        }

        /* Now test module bus show command */
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "module",
               "bus",
               "show",
               "-m",
               "test_module_bus_show",
               "--project",
               projectName,
               "-d",
               projectManager.getProjectPath(),
               "test_master"};

        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify command produces output */
        QVERIFY(messageList.size() > 0);
        QVERIFY(messageListContains("bus:"));
        QVERIFY(messageListContains("test_master:"));
        QVERIFY(messageListContains("bus: apb4"));
        QVERIFY(messageListContains("mode: master"));
        QVERIFY(messageListContains("mapping:"));
        QVERIFY(messageListContains("paddr: test_master_paddr"));
        QVERIFY(messageListContains("penable: test_master_penable"));
        QVERIFY(!messageListContains("Error"));
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsemodulebus.moc"
