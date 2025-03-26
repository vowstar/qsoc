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

        /* Create a test project for module tests */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {
            "qsoc",
            "project",
            "create",
            "module_test_project",
        };
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Create test Verilog files */
        createTestVerilogFiles();
    }

    void cleanupTestCase()
    {
        /* Cleanup any leftover test files */
        QStringList filesToRemove = {"module_test_project.soc_pro", "test_counter.v"};

        for (const QString &file : filesToRemove) {
            if (QFile::exists(file)) {
                QFile::remove(file);
            }
        }
    }

    void createTestVerilogFiles()
    {
        /* Create a simple counter module file */
        QFile counterFile("test_counter.v");
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
    }

    // Test if module command exists
    void testModuleCommandExists()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {"qsoc", "module", "--help"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        // Check if help was displayed without errors
        QVERIFY(messageList.size() > 0);
    }

    // Test if module import command exists
    void testModuleImportCommandExists()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {"qsoc", "module", "import", "--help"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        // Check if help was displayed without errors
        QVERIFY(messageList.size() > 0);
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsemodule.moc"
