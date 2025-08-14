// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "common/qsocgeneratereportunconnected.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTemporaryDir>
#include <QTextStream>
#include <QtCore>
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

    static void messageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
    {
        Q_UNUSED(type);
        Q_UNUSED(context);
        messageList << msg;
    }

    bool verifyYamlContent(const QString &filePath, const QString &content)
    {
        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return false;
        }

        QTextStream   in(&file);
        const QString fileContent = in.readAll();
        file.close();

        return fileContent.contains(content);
    }

private slots:
    void initTestCase()
    {
        /* Make sure our app instance is initialized */
        TestApp::instance();

        /* Install message handler to capture debug output */
        qInstallMessageHandler(messageOutput);
    }

    void cleanupTestCase()
    {
        /* Restore default message handler */
        qInstallMessageHandler(nullptr);
    }

    void init()
    {
        /* Clear message list before each test */
        messageList.clear();
    }

    void testUnconnectedPortReporter()
    {
        QSocGenerateReportUnconnected reporter;

        /* Test initial state */
        QCOMPARE(reporter.getUnconnectedPortCount(), 0);
        QCOMPARE(reporter.getInstanceCount(), 0);

        /* Add some unconnected ports */
        QSocGenerateReportUnconnected::UnconnectedPortInfo port1;
        port1.instanceName = "u_axi4_interconnect";
        port1.moduleName   = "axi4_interconnect";
        port1.portName     = "araddr";
        port1.direction    = "input";
        port1.type         = "logic[39:0]";

        QSocGenerateReportUnconnected::UnconnectedPortInfo port2;
        port2.instanceName = "u_axi4_interconnect";
        port2.moduleName   = "axi4_interconnect";
        port2.portName     = "arburst";
        port2.direction    = "input";
        port2.type         = "logic[1:0]";

        QSocGenerateReportUnconnected::UnconnectedPortInfo port3;
        port3.instanceName = "u_uart_controller";
        port3.moduleName   = "uart_controller";
        port3.portName     = "test_mode";
        port3.direction    = "input";
        port3.type         = "logic";

        reporter.addUnconnectedPort(port1);
        reporter.addUnconnectedPort(port2);
        reporter.addUnconnectedPort(port3);

        /* Verify counts */
        QCOMPARE(reporter.getUnconnectedPortCount(), 3);
        QCOMPARE(reporter.getInstanceCount(), 2);

        /* Test report generation */
        QTemporaryDir tempDir;
        QVERIFY2(tempDir.isValid(), "Failed to create temporary directory");

        const QString outputPath    = tempDir.path();
        const QString topModuleName = "soc_top";

        QVERIFY2(
            reporter.generateReport(outputPath, topModuleName),
            "Failed to generate unconnected port report");

        /* Verify report file exists */
        const QString reportFilePath = QDir(outputPath).filePath(topModuleName + ".nc.rpt");
        QVERIFY2(QFile::exists(reportFilePath), "Report file was not created");

        /* Verify report content - header */
        QVERIFY2(
            verifyYamlContent(reportFilePath, "# Unconnected port report - soc_top"),
            "Report header is incorrect");

        QVERIFY2(
            verifyYamlContent(reportFilePath, "# Tool: qsoc"), "Tool information missing in report");

        /* Verify report content - summary */
        QVERIFY2(verifyYamlContent(reportFilePath, "summary:"), "Summary section missing");

        QVERIFY2(
            verifyYamlContent(reportFilePath, "total_instance: 2"),
            "Incorrect instance count in summary");

        QVERIFY2(
            verifyYamlContent(reportFilePath, "total_port: 3"), "Incorrect port count in summary");

        /* Verify report content - instances */
        QVERIFY2(verifyYamlContent(reportFilePath, "instance:"), "Instance section missing");

        QVERIFY2(verifyYamlContent(reportFilePath, "u_axi4_interconnect:"), "First instance missing");

        QVERIFY2(
            verifyYamlContent(reportFilePath, "module: axi4_interconnect"),
            "Module name missing for first instance");

        QVERIFY2(verifyYamlContent(reportFilePath, "araddr:"), "First port missing");

        QVERIFY2(
            verifyYamlContent(reportFilePath, "type: logic[39:0]"),
            "Port type missing or incorrect");

        QVERIFY2(
            verifyYamlContent(reportFilePath, "direction: input"),
            "Port direction missing or incorrect");

        QVERIFY2(verifyYamlContent(reportFilePath, "u_uart_controller:"), "Second instance missing");

        QVERIFY2(
            verifyYamlContent(reportFilePath, "module: uart_controller"),
            "Module name missing for second instance");

        QVERIFY2(verifyYamlContent(reportFilePath, "test_mode:"), "Third port missing");

        QVERIFY2(
            verifyYamlContent(reportFilePath, "type: logic"),
            "Single-bit port type missing or incorrect");
    }

    void testEmptyReporter()
    {
        QSocGenerateReportUnconnected reporter;

        /* Test empty reporter generates report successfully */
        QTemporaryDir tempDir;
        QVERIFY2(tempDir.isValid(), "Failed to create temporary directory");

        const QString outputPath    = tempDir.path();
        const QString topModuleName = "empty_top";

        /* Empty reporter should still succeed (returns true when no ports to report) */
        QVERIFY2(reporter.generateReport(outputPath, topModuleName), "Empty reporter should succeed");

        /* Verify no report file is created for empty reporter */
        const QString reportFilePath = QDir(outputPath).filePath(topModuleName + ".nc.rpt");
        QVERIFY2(
            !QFile::exists(reportFilePath), "Report file should not be created for empty reporter");
    }

    void testClearFunction()
    {
        QSocGenerateReportUnconnected reporter;

        /* Add a port */
        QSocGenerateReportUnconnected::UnconnectedPortInfo port;
        port.instanceName = "u_test";
        port.moduleName   = "test_module";
        port.portName     = "test_port";
        port.direction    = "input";
        port.type         = "logic";

        reporter.addUnconnectedPort(port);

        /* Verify port was added */
        QCOMPARE(reporter.getUnconnectedPortCount(), 1);
        QCOMPARE(reporter.getInstanceCount(), 1);

        /* Clear and verify empty state */
        reporter.clear();
        QCOMPARE(reporter.getUnconnectedPortCount(), 0);
        QCOMPARE(reporter.getInstanceCount(), 0);
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegeneratereportunconnected.moc"
