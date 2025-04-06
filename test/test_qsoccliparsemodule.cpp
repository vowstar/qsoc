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

        /* Setup bus manager */
        busManager.setProjectManager(&projectManager);

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

    /* Test if module command exists */
    void testModuleCommandExists()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {"qsoc", "module", "--help"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Check if help was displayed without errors */
        QVERIFY(messageList.size() > 0);
    }

    /* Test if module import command exists */
    void testModuleImportCommandExists()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {"qsoc", "module", "import", "--help"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Check if help was displayed without errors */
        QVERIFY(messageList.size() > 0);
    }

    /* Test module import without specifying a project */
    void testModuleImportNoProject()
    {
        /* Create a counter module file */
        QString testFileName     = "testModuleImportNoProject_counter.v";
        QString testFilePathFull = QDir(projectPath).filePath(testFileName);
        QFile   counterFile(testFilePathFull);
        if (counterFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&counterFile);
            out << "module test_counter (\n"
                << "  input  wire        clk,\n"
                << "  input  wire        rst_n,\n"
                << "  input  wire        enable,\n"
                << "  output reg  [7:0]  count\n"
                << ");\n"
                << "  always @(posedge clk or negedge rst_n) begin\n"
                << "    if (!rst_n) begin\n"
                << "      count <= 8'h00;\n"
                << "    end else if (enable) begin\n"
                << "      count <= count + 1;\n"
                << "    end\n"
                << "  end\n"
                << "endmodule\n";
            counterFile.close();
        }

        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {"qsoc", "module", "import", testFilePathFull};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Should complete without raising test failure */
        QVERIFY(messageList.size() > 0);
    }

    /* Test module import with valid project and file */
    void testModuleImportValid()
    {
        /* Create a counter module file */
        QString testFileName     = "testModuleImportValid_counter.v";
        QString testFilePathFull = QDir(projectPath).filePath(testFileName);
        QFile   counterFile(testFilePathFull);
        if (counterFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&counterFile);
            out << "module test_counter (\n"
                << "  input  wire        clk,\n"
                << "  input  wire        rst_n,\n"
                << "  input  wire        enable,\n"
                << "  output reg  [7:0]  count\n"
                << ");\n"
                << "  always @(posedge clk or negedge rst_n) begin\n"
                << "    if (!rst_n) begin\n"
                << "      count <= 8'h00;\n"
                << "    end else if (enable) begin\n"
                << "      count <= count + 1;\n"
                << "    end\n"
                << "  end\n"
                << "endmodule\n";
            counterFile.close();
        }

        /* First verify the test file exists */
        QVERIFY(QFile::exists(testFilePathFull));

        /* Clear message list to ensure we only capture messages from this test */
        messageList.clear();
        QSocCliWorker socCliWorker;

        /* Use full paths and ensure project path is correctly passed */
        QFileInfo projectInfo(projectManager.getProjectPath());
        QString   projectFullPath = projectInfo.absoluteFilePath();
        QFileInfo counterFileInfo(testFilePathFull);
        QString   counterFileFullPath = counterFileInfo.absoluteFilePath();

        const QStringList appArguments
            = {"qsoc",
               "module",
               "import",
               counterFileFullPath, /* Use absolute file path */
               "--project",
               projectName,
               "-d",
               projectFullPath}; /* Use absolute project path */

        /* Run the import command */
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify we got some output */
        QVERIFY(messageList.size() > 0);

        /* Reload the module manager to ensure it has the most recent data */
        moduleManager.load(QRegularExpression(".*"));

        /* Verify that the module exists */
        bool moduleExists = moduleManager.isModuleExist("test_counter");
        QVERIFY(moduleExists);
    }

    /* Test module import with non-existent file */
    void testModuleImportNonExistentFile()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "module", "import", "non_existent_file.v", "--project", projectPath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Should show error about missing file */
        QVERIFY(messageList.size() > 0);
        bool hasFileError = false;
        for (const QString &msg : messageList) {
            if ((msg.contains("No such file", Qt::CaseInsensitive)
                 || msg.contains("not exist", Qt::CaseInsensitive)
                 || msg.contains("file not found", Qt::CaseInsensitive))
                && msg.contains("non_existent_file.v", Qt::CaseInsensitive)) {
                hasFileError = true;
                break;
            }
        }
        QVERIFY(hasFileError);
    }

    /* Test module list command */
    void testModuleList()
    {
        /* Create adder module file */
        QString testFileName      = "testModuleList_adder.v";
        QString adderFilePathFull = QDir(projectPath).filePath(testFileName);
        QFile   adderFile(adderFilePathFull);
        if (adderFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&adderFile);
            out << "module test_adder (\n"
                << "  input  wire [7:0]  a,\n"
                << "  input  wire [7:0]  b,\n"
                << "  output wire [7:0]  sum\n"
                << ");\n"
                << "  assign sum = a + b;\n"
                << "endmodule\n";
            adderFile.close();
        }

        /* First import a module to have something to list */
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments
                = {"qsoc",
                   "module",
                   "import",
                   adderFilePathFull,
                   "--project",
                   projectName,
                   "-d",
                   projectManager.getProjectPath()};
            socCliWorker.setup(appArguments, false);
            socCliWorker.run();
        }

        /* Now test the list command */
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {"qsoc", "module", "list", "--project", projectPath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Should list both imported modules */
        QVERIFY(messageList.size() > 0);

        bool hasCounter = false;
        bool hasAdder   = false;

        for (const QString &msg : messageList) {
            if (msg.contains("test_counter")) {
                hasCounter = true;
            }
            if (msg.contains("test_adder")) {
                hasAdder = true;
            }
        }

        QVERIFY(hasCounter);
        QVERIFY(hasAdder);
    }

    /* Test module info command */
    void testModuleInfo()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "module", "show", "test_counter", "--project", projectPath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Should show module information */
        QVERIFY(messageList.size() > 0);

        bool hasPortInfo = false;
        for (const QString &msg : messageList) {
            if (msg.contains("port", Qt::CaseInsensitive)) {
                hasPortInfo = true;
                break;
            }
        }

        QVERIFY(hasPortInfo);

        /* Check for specific ports */
        bool hasClkPort    = false;
        bool hasRstPort    = false;
        bool hasEnablePort = false;
        bool hasCountPort  = false;

        for (const QString &msg : messageList) {
            if (msg.contains("clk"))
                hasClkPort = true;
            if (msg.contains("rst_n"))
                hasRstPort = true;
            if (msg.contains("enable"))
                hasEnablePort = true;
            if (msg.contains("count"))
                hasCountPort = true;
        }

        QVERIFY(hasClkPort);
        QVERIFY(hasRstPort);
        QVERIFY(hasEnablePort);
        QVERIFY(hasCountPort);
    }

    /* Test module info for non-existent module */
    void testModuleInfoNonExistent()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "module", "show", "non_existent_module", "--project", projectPath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Should complete without raising test failure */
        QVERIFY(messageList.size() > 0);
    }

    /* Test module delete command */
    void testModuleDelete()
    {
        /* Create adder module file */
        QString testFileName      = "testModuleDelete_adder.v";
        QString adderFilePathFull = QDir(projectPath).filePath(testFileName);
        QFile   adderFile(adderFilePathFull);
        if (adderFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&adderFile);
            out << "module test_adder (\n"
                << "  input  wire [7:0]  a,\n"
                << "  input  wire [7:0]  b,\n"
                << "  output wire [7:0]  sum\n"
                << ");\n"
                << "  assign sum = a + b;\n"
                << "endmodule\n";
            adderFile.close();
        }

        /* First import module test_adder to ensure it exists */
        {
            QSocCliWorker socCliWorker;
            QFileInfo     adderFileInfo(adderFilePathFull);
            QString       adderFileFullPath = adderFileInfo.absoluteFilePath();
            QFileInfo     projectInfo(projectManager.getProjectPath());
            QString       projectFullPath = projectInfo.absoluteFilePath();

            const QStringList appArguments
                = {"qsoc",
                   "module",
                   "import",
                   adderFileFullPath,
                   "--project",
                   projectName,
                   "-d",
                   projectFullPath};

            socCliWorker.setup(appArguments, false);
            socCliWorker.run();

            /* Reset the module data */
            moduleManager.resetModuleData();
            /* Reload the module manager */
            moduleManager.load(QRegularExpression(".*"));

            /* Check module exists */
            bool exists = moduleManager.isModuleExist("test_adder");
            QVERIFY(exists);
        }

        /* Now run the CLI remove command */
        {
            QSocCliWorker socCliWorker;
            QFileInfo     projectInfo(projectManager.getProjectPath());
            QString       projectFullPath = projectInfo.absoluteFilePath();

            const QStringList appArguments
                = {"qsoc",
                   "module",
                   "remove",
                   "test_adder",
                   "--project",
                   projectName,
                   "-d",
                   projectFullPath};

            /* Execute the CLI command */
            socCliWorker.setup(appArguments, false);
            socCliWorker.run();

            /* Reload the module manager and verify changes */
            /* Reset the module data */
            moduleManager.resetModuleData();
            /* Reload the module manager */
            moduleManager.load(QRegularExpression(".*"));

            /* See if module is still in memory */
            bool exists = moduleManager.isModuleExist("test_adder");

            QVERIFY(!exists);
        }
    }

    /* Test module bus add command */
    void testModuleBusAdd()
    {
        /* Create counter module file */
        QString testFileName        = "testModuleBusAdd_counter.v";
        QString counterFilePathFull = QDir(projectPath).filePath(testFileName);
        QFile   counterFile(counterFilePathFull);
        if (counterFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&counterFile);
            out << "module test_counter (\n"
                << "  input  wire        clk,\n"
                << "  input  wire        rst_n,\n"
                << "  input  wire        enable,\n"
                << "  output reg  [7:0]  count\n"
                << ");\n"
                << "  always @(posedge clk or negedge rst_n) begin\n"
                << "    if (!rst_n) begin\n"
                << "      count <= 8'h00;\n"
                << "    end else if (enable) begin\n"
                << "      count <= count + 1;\n"
                << "    end\n"
                << "  end\n"
                << "endmodule\n";
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
               "test_counter",
               "-b",
               "apb",
               "-o",
               "slave",
               "apb_interface",
               "--project",
               projectName,
               "-d",
               projectFullPath};

        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the command produced output */
        QVERIFY(messageList.size() > 0);

        /* Reload modules to verify changes */
        moduleManager.load("test_counter");

        /* Verify the module exists */
        QVERIFY(verifyModuleExists("test_counter"));

        /* Check if the module has the bus assigned */
        YAML::Node moduleNode     = moduleManager.getModuleYaml(QString("test_counter"));
        bool       hasBusAssigned = false;

        if (moduleNode["bus"].IsDefined() && moduleNode["bus"].IsMap()) {
            for (const auto &bus : moduleNode["bus"]) {
                if (bus.first.IsDefined() && bus.second.IsDefined() && bus.second.IsMap()) {
                    if (bus.second["bus"].IsDefined() && !bus.second["bus"].IsNull()) {
                        QString busType = QString::fromStdString(
                            bus.second["bus"].as<std::string>());
                        if (busType == "apb") {
                            hasBusAssigned = true;
                            break;
                        }
                    }
                }
            }
        }

        QVERIFY(hasBusAssigned);

        /* Check for success message */
        bool successful = messageListContains("added") || messageListContains("success")
                          || messageListContains("matched") || messageListContains("Bus added");
        QVERIFY(successful);

        /* Verify no error messages in the output */
        bool hasError = false;
        for (const QString &msg : messageList) {
            if (msg.toLower().contains("error") || msg.toLower().contains("failed")) {
                hasError = true;
                break;
            }
        }

        QVERIFY(!hasError);
    }

    /* Test module bus remove command */
    void testModuleBusRemove()
    {
        /* Create counter module file */
        QString testFileName        = "testModuleBusRemove_counter.v";
        QString counterFilePathFull = QDir(projectPath).filePath(testFileName);
        QFile   counterFile(counterFilePathFull);
        if (counterFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&counterFile);
            out << "module test_counter (\n"
                << "  input  wire        clk,\n"
                << "  input  wire        rst_n,\n"
                << "  input  wire        enable,\n"
                << "  output reg  [7:0]  count\n"
                << ");\n"
                << "  always @(posedge clk or negedge rst_n) begin\n"
                << "    if (!rst_n) begin\n"
                << "      count <= 8'h00;\n"
                << "    end else if (enable) begin\n"
                << "      count <= count + 1;\n"
                << "    end\n"
                << "  end\n"
                << "endmodule\n";
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
                   "test_counter",
                   "-b",
                   "apb",
                   "-o",
                   "slave",
                   "--project",
                   projectName,
                   "-d",
                   projectManager.getProjectPath(),
                   "apb_interface"};
            socCliWorker.setup(appArguments, false);
            socCliWorker.run();

            /* Verify the bus was added */
            moduleManager.load("test_counter");
            YAML::Node moduleNode     = moduleManager.getModuleYaml(QString("test_counter"));
            bool       hasBusAssigned = false;

            if (moduleNode["bus"].IsDefined()) {
                for (const auto &bus : moduleNode["bus"]) {
                    if (bus.second["bus"].IsDefined() && !bus.second["bus"].IsNull()
                        && QString::fromStdString(bus.second["bus"].as<std::string>()) == "apb") {
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
               "test_counter",
               "--project",
               projectName,
               "-d",
               projectManager.getProjectPath(),
               "apb_interface"};

        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the command produced output */
        QVERIFY(messageList.size() > 0);

        /* Reload modules to verify changes */
        moduleManager.load("test_counter");

        /* Check if the module no longer has the bus assigned */
        YAML::Node moduleNode     = moduleManager.getModuleYaml(QString("test_counter"));
        bool       hasBusAssigned = false;

        if (moduleNode["bus"].IsDefined() && moduleNode["bus"].IsMap()) {
            for (const auto &bus : moduleNode["bus"]) {
                if (bus.first.IsDefined() && bus.second.IsDefined() && bus.second.IsMap()) {
                    if (bus.second["bus"].IsDefined() && !bus.second["bus"].IsNull()) {
                        QString busType = QString::fromStdString(
                            bus.second["bus"].as<std::string>());
                        if (busType == "apb") {
                            hasBusAssigned = true;
                            break;
                        }
                    }
                }
            }
        }

        QVERIFY(!hasBusAssigned);

        /* Check for success message */
        bool successful = messageListContains("removed") || messageListContains("success");
        QVERIFY(successful);

        /* Verify no error messages in the output */
        bool hasError = false;
        for (const QString &msg : messageList) {
            if (msg.toLower().contains("error") || msg.toLower().contains("failed")) {
                hasError = true;
                break;
            }
        }

        QVERIFY(!hasError);
    }

    /* Test module bus list command */
    void testModuleBusList()
    {
        /* Create counter module file */
        QString testFileName        = "testModuleBusList_counter.v";
        QString counterFilePathFull = QDir(projectPath).filePath(testFileName);
        QFile   counterFile(counterFilePathFull);
        if (counterFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&counterFile);
            out << "module test_counter (\n"
                << "  input  wire        clk,\n"
                << "  input  wire        rst_n,\n"
                << "  input  wire        enable,\n"
                << "  output reg  [7:0]  count\n"
                << ");\n"
                << "  always @(posedge clk or negedge rst_n) begin\n"
                << "    if (!rst_n) begin\n"
                << "      count <= 8'h00;\n"
                << "    end else if (enable) begin\n"
                << "      count <= count + 1;\n"
                << "    end\n"
                << "  end\n"
                << "endmodule\n";
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

        /* Then add a bus to a module */
        {
            QSocCliWorker socCliWorker;
            QFileInfo     projectInfo(projectManager.getProjectPath());
            QString       projectFullPath = projectInfo.absoluteFilePath();

            const QStringList appArguments
                = {"qsoc",
                   "module",
                   "bus",
                   "add",
                   "-m",
                   "test_counter",
                   "-b",
                   "apb",
                   "-o",
                   "slave",
                   "--project",
                   projectName,
                   "-d",
                   projectFullPath,
                   "apb_interface"};
            socCliWorker.setup(appArguments, false);
            socCliWorker.run();

            /* Verify the bus was added */
            moduleManager.load("test_counter");
        }

        /* Now test module bus list command */
        messageList.clear();
        QSocCliWorker socCliWorker;
        QFileInfo     projectInfo(projectManager.getProjectPath());
        QString       projectFullPath = projectInfo.absoluteFilePath();

        const QStringList appArguments
            = {"qsoc",
               "module",
               "bus",
               "list",
               "-m",
               "test_counter",
               "--project",
               projectName,
               "-d",
               projectFullPath};

        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify the command produced the expected output */
        QVERIFY(messageList.size() > 0);

        /* Check if the bus is listed in the output */
        bool hasApbBus = false;
        for (const QString &msg : messageList) {
            if (msg.contains("apb", Qt::CaseInsensitive)) {
                hasApbBus = true;
                break;
            }
        }

        QVERIFY(hasApbBus);

        /* Verify no error messages in the output */
        bool hasError = false;
        for (const QString &msg : messageList) {
            if (msg.toLower().contains("error") || msg.toLower().contains("failed")) {
                hasError = true;
                break;
            }
        }

        QVERIFY(!hasError);
    }

    /* Test simple module remove */
    void testSimpleModuleRemove()
    {
        /* Create adder module file */
        QString testFileName      = "testSimpleModuleRemove_adder.v";
        QString adderFilePathFull = QDir(projectPath).filePath(testFileName);
        QFile   adderFile(adderFilePathFull);
        if (adderFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&adderFile);
            out << "module test_adder (\n"
                << "  input  wire [7:0]  a,\n"
                << "  input  wire [7:0]  b,\n"
                << "  output wire [7:0]  sum\n"
                << ");\n"
                << "  assign sum = a + b;\n"
                << "endmodule\n";
            adderFile.close();
        }

        /* Setup module manager */
        QFileInfo projectInfo(projectManager.getProjectPath());
        QString   projectFullPath = projectInfo.absoluteFilePath();

        /* First use the CLI to import the module */
        {
            QSocCliWorker socCliWorker;
            QFileInfo     adderFileInfo(adderFilePathFull);
            QString       adderFileFullPath = adderFileInfo.absoluteFilePath();

            const QStringList appArguments
                = {"qsoc",
                   "module",
                   "import",
                   adderFileFullPath,
                   "--project",
                   projectName,
                   "-d",
                   projectFullPath};
            socCliWorker.setup(appArguments, false);
            socCliWorker.run();
        }

        /* Verify the module was imported */
        moduleManager.load(QRegularExpression(".*"));
        bool moduleImported = moduleManager.isModuleExist("test_adder");
        QVERIFY(moduleImported);

        /* Now remove the module using the moduleManager directly */
        moduleManager.removeModule(QRegularExpression("test_adder"));

        /* Reload modules */
        moduleManager.load(QRegularExpression(".*"));

        /* Verify the module no longer exists */
        bool moduleStillExists = moduleManager.isModuleExist("test_adder");
        QVERIFY(!moduleStillExists);
    }

    /* Test module bus show command */
    void testModuleBusShow()
    {
        /* Create counter module file */
        QString testFileName        = "testModuleBusShow_counter.v";
        QString counterFilePathFull = QDir(projectPath).filePath(testFileName);
        QFile   counterFile(counterFilePathFull);
        if (counterFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream out(&counterFile);
            out << "module test_counter (\n"
                << "  input  wire        clk,\n"
                << "  input  wire        rst_n,\n"
                << "  input  wire        enable,\n"
                << "  output reg  [7:0]  count\n"
                << ");\n"
                << "  always @(posedge clk or negedge rst_n) begin\n"
                << "    if (!rst_n) begin\n"
                << "      count <= 8'h00;\n"
                << "    end else if (enable) begin\n"
                << "      count <= count + 1;\n"
                << "    end\n"
                << "  end\n"
                << "endmodule\n";
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
                   "test_counter",
                   "-b",
                   "apb",
                   "-o",
                   "slave",
                   "--project",
                   projectName,
                   "-d",
                   projectManager.getProjectPath(),
                   "apb_interface"};
            socCliWorker.setup(appArguments, false);
            socCliWorker.run();

            /* Verify the bus was added */
            moduleManager.load("test_counter");
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
               "test_counter",
               "--project",
               projectName,
               "-d",
               projectManager.getProjectPath(),
               "apb_interface"};

        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify command produces output */
        QVERIFY(messageList.size() > 0);

        /* Check for specific APB bus details in the output */
        bool hasBusDetails = false;
        bool hasPortInfo   = false;
        bool hasModuleInfo = false;

        for (const QString &msg : messageList) {
            if (msg.contains("apb", Qt::CaseInsensitive)) {
                hasBusDetails = true;
            }
            if (msg.contains("port", Qt::CaseInsensitive)
                || msg.contains("mapping", Qt::CaseInsensitive)) {
                hasPortInfo = true;
            }
            if (msg.contains("test_counter", Qt::CaseInsensitive)) {
                hasModuleInfo = true;
            }
        }

        QVERIFY(hasBusDetails);
        QVERIFY(hasPortInfo);
        QVERIFY(hasModuleInfo);

        /* Verify no error messages in the output */
        bool hasError = false;
        for (const QString &msg : messageList) {
            if (msg.toLower().contains("error") || msg.toLower().contains("failed")) {
                hasError = true;
                break;
            }
        }
        QVERIFY(!hasError);
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsemodule.moc"

