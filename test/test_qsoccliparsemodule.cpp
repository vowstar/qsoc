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

    /* Project and file paths */
    QString projectPath;
    QString counterFilePath;
    QString secondModuleFilePath;

private slots:
    void initTestCase()
    {
        TestApp::instance();
        qInstallMessageHandler(messageOutput);

        /* Set project path */
        projectPath          = "module_test_project";
        counterFilePath      = "test_counter.v";
        secondModuleFilePath = "test_adder.v";

        /* Create a test project for module tests */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {
            "qsoc",
            "project",
            "create",
            projectPath,
        };
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Create test Verilog files */
        createTestVerilogFiles();
    }

    void cleanupTestCase()
    {
        /* Cleanup any leftover test files */
        QStringList filesToRemove
            = {projectPath + ".soc_pro", counterFilePath, secondModuleFilePath};

        for (const QString &file : filesToRemove) {
            if (QFile::exists(file)) {
                QFile::remove(file);
            }
        }

        /* Remove module directory if it exists */
        QString moduleDir = QDir(projectPath).filePath("module");
        if (QDir(moduleDir).exists()) {
            QDir(moduleDir).removeRecursively();
        }
    }

    void createTestVerilogFiles()
    {
        /* Create a simple counter module file */
        QFile counterFile(counterFilePath);
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

        /* Create a simple adder module file */
        QFile adderFile(secondModuleFilePath);
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
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {"qsoc", "module", "import", counterFilePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Should complete without raising test failure */
        QVERIFY(messageList.size() > 0);
    }

    /* Test module import with valid project and file */
    void testModuleImportValid()
    {
        /* First verify the test file exists */
        QVERIFY(QFile::exists(counterFilePath));

        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "module", "import", counterFilePath, "--project", projectPath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Should show success message */
        QVERIFY(messageList.size() > 0);

        /* Check if there's no error message */
        bool hasError = false;
        for (const QString &msg : messageList) {
            if (msg.toLower().contains("error") || msg.toLower().contains("failed")) {
                hasError = true;
                break;
            }
        }
        QVERIFY(!hasError);
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
        /* First import a module to have something to list */
        {
            QSocCliWorker     socCliWorker;
            const QStringList appArguments
                = {"qsoc", "module", "import", secondModuleFilePath, "--project", projectPath};
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
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "module", "remove", "test_adder", "--project", projectPath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Should delete the module */
        QString moduleDir   = QDir(projectPath).filePath("module");
        QString adderModule = QDir(moduleDir).filePath("test_adder");

        QVERIFY(!QDir(adderModule).exists());

        /* Confirm with module list */
        messageList.clear();
        QSocCliWorker     listWorker;
        const QStringList listArgs = {"qsoc", "module", "list", "--project", projectPath};
        listWorker.setup(listArgs, false);
        listWorker.run();

        bool adderInList = false;
        for (const QString &msg : messageList) {
            if (msg.contains("test_adder")) {
                adderInList = true;
                break;
            }
        }

        QVERIFY(!adderInList);
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsemodule.moc"
