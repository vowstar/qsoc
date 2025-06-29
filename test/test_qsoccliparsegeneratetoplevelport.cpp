// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/config.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QRegularExpression>
#include <QStringList>
#include <QTemporaryFile>
#include <QTextStream>
#include <QtCore>
#include <QtTest>

struct TestApp
{
    static auto &instance()
    {
        static auto                   argc      = 1;
        static char                   appName[] = "qsoc";
        static std::array<char *, 1>  argv      = {{appName}};
        static const QCoreApplication app       = QCoreApplication(argc, argv.data());
        return app;
    }
};

class Test : public QObject
{
    Q_OBJECT

private:
    static QStringList messageList;
    QString            projectName;
    QSocProjectManager projectManager;

    static void messageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
    {
        Q_UNUSED(type);
        Q_UNUSED(context);
        messageList << msg;
    }

    QString createTempFile(const QString &fileName, const QString &content)
    {
        QString filePath = QDir(projectManager.getCurrentPath()).filePath(fileName);
        QFile   file(filePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&file);
            stream << content;
            file.close();
            return filePath;
        }
        return QString();
    }

    /* Helper function to verify Verilog content with normalized whitespace */
    bool verifyVerilogContentNormalized(const QString &verilogContent, const QString &contentToVerify)
    {
        if (verilogContent.isEmpty() || contentToVerify.isEmpty()) {
            return false;
        }

        /* Helper function to normalize whitespace */
        auto normalizeWhitespace = [](const QString &input) -> QString {
            QString result = input;
            /* Replace all whitespace (including tabs and newlines) with a single space */
            result.replace(QRegularExpression("\\s+"), " ");
            /* Remove whitespace before any symbol/operator/punctuation */
            result.replace(
                QRegularExpression("\\s+([\\[\\]\\(\\)\\{\\}<>\"'`+\\-*/%&|^~!#$,.:;=@_])"), "\\1");
            /* Remove whitespace after any symbol/operator/punctuation */
            result.replace(
                QRegularExpression("([\\[\\]\\(\\)\\{\\}<>\"'`+\\-*/%&|^~!#$,.:;=@_])\\s+"), "\\1");

            return result;
        };

        /* Normalize whitespace in both strings before comparing */
        const QString normalizedContent = normalizeWhitespace(verilogContent);
        const QString normalizedVerify  = normalizeWhitespace(contentToVerify);

        /* Check if the normalized content contains the normalized text we're looking for */
        return normalizedContent.contains(normalizedVerify);
    }

    void createTestFiles()
    {
        /* Create module directory if it doesn't exist */
        QDir moduleDir(projectManager.getModulePath());
        if (!moduleDir.exists()) {
            moduleDir.mkpath(".");
        }

        // Create a simple module with input/output ports
        const QString moduleContent = R"(
test_module:
  port:
    clk:
      type: logic
      direction: in
    rst_n:
      type: logic
      direction: in
    data_out:
      type: logic[7:0]
      direction: out
    enable:
      type: logic
      direction: in
)";

        createTempFile("module/test_module.yaml", moduleContent);

        // Create IO cell module (similar to the original PDDWUWSWCDG_H module)
        const QString ioCellContent = R"(
test_io_cell:
  port:
    I:
      type: logic
      direction: in
    O:
      type: logic
      direction: out
    C:
      type: logic
      direction: out
    OEN:
      type: logic
      direction: in
)";

        createTempFile("module/test_io_cell.yaml", ioCellContent);

        // Create a project that uses this module and has top-level ports
        // This project simulates the original issue: top-level ports that should NOT trigger warnings
        const QString projectContent = R"(
project:
  name: test_toplevel_ports
  description: Test project for top-level port direction checking
  version: 1.0.0
  author: Test

  toplevel:
    name: top_test_chip

    port:
      # Top-level input ports (externally driven, internally consumed)
      test_tck:
        type: logic
        direction: input
      test_tdi:
        type: logic
        direction: input
      # Top-level output ports (internally driven, externally consumed)
      test_tdo:
        type: logic
        direction: output
      test_tdo_oe:
        type: logic
        direction: output

  instances:
    u_test_module:
      module: test_module
      location:
        x: 100
        y: 100
    u_io_cell_tck:
      module: test_io_cell
      location:
        x: 200
        y: 100
    u_io_cell_tdo:
      module: test_io_cell
      location:
        x: 300
        y: 100

  nets:
    # Top-level input driving internal logic - should be valid
    test_tck:
      - { instance: top, port: test_tck }
      - { instance: u_io_cell_tck, port: I }
    test_tdi:
      - { instance: top, port: test_tdi }
      - { instance: u_test_module, port: rst_n }
    # Top-level output driven by internal logic - should be valid
    test_tdo:
      - { instance: top, port: test_tdo }
      - { instance: u_io_cell_tdo, port: O }
    test_tdo_oe:
      - { instance: top, port: test_tdo_oe }
      - { instance: u_test_module, port: data_out }
)";

        createTempFile("project.yaml", projectContent);
    }

private slots:
    void initTestCase()
    {
        TestApp::instance();
        projectName = "test_netlist_toplevel_ports_"
                      + QString::number(QDateTime::currentMSecsSinceEpoch());
    }

    void init()
    {
        messageList.clear();
        qInstallMessageHandler(messageOutput);
    }

    void cleanup() { qInstallMessageHandler(nullptr); }

    void cleanupTestCase()
    {
        if (!projectName.isEmpty()) {
            QDir projectDir(projectManager.getProjectPath());
            if (projectDir.exists()) {
                projectDir.removeRecursively();
            }
        }
    }

    void testToplevelPortDirectionCheck()
    {
        // Create a new project
        projectManager.setProjectName(projectName);
        projectManager.setCurrentPath(QDir::current().filePath(projectName));
        projectManager.mkpath();
        projectManager.save(projectName);
        projectManager.load(projectName);

        // Set up the test files
        createTestFiles();

        // Generate netlist
        QSocCliWorker worker;
        QStringList   arguments;
        arguments << "qsoc" << "generate" << "--project"
                  << projectManager.getProjectPath() + "/project.yaml";

        worker.setup(arguments, true);
        worker.run();

        // Check that no incorrect warnings were generated for proper top-level port connections
        bool hasIncorrectMultidriveWarning = false;
        bool hasIncorrectUndrivenWarning   = false;

        for (const QString &message : messageList) {
            // Top-level input ports should NOT be reported as "multiple drivers"
            // because they are inputs to the chip - only external source should drive them
            if ((message.contains("test_tck") || message.contains("test_tdi"))
                && (message.contains("multiple drivers") || message.contains("Multidrive"))) {
                hasIncorrectMultidriveWarning = true;
                qWarning() << "Incorrect multidrive warning for top-level input:" << message;
            }

            // Top-level output ports should NOT be reported as "undriven"
            // because they should be driven by internal logic
            if ((message.contains("test_tdo") || message.contains("test_tdo_oe"))
                && (message.contains("undriven") || message.contains("Undriven"))) {
                hasIncorrectUndrivenWarning = true;
                qWarning() << "Incorrect undriven warning for top-level output:" << message;
            }
        }

        // These should not have incorrect warnings
        QVERIFY2(
            !hasIncorrectMultidriveWarning,
            "Top-level input ports should not be reported as having multiple drivers");
        QVERIFY2(
            !hasIncorrectUndrivenWarning,
            "Top-level output ports should not be reported as undriven");

        // Print all messages for debugging
        qDebug() << "All messages:";
        for (const QString &message : messageList) {
            qDebug() << message;
        }
    }

    void testToplevelPortCorrectBehavior()
    {
        // Clear previous project
        if (!projectName.isEmpty()) {
            QDir projectDir(projectManager.getProjectPath());
            if (projectDir.exists()) {
                projectDir.removeRecursively();
            }
        }

        // Create new project for this specific test
        projectName = "test_correct_behavior_"
                      + QString::number(QDateTime::currentMSecsSinceEpoch());
        projectManager.setProjectName(projectName);
        projectManager.setCurrentPath(QDir::current().filePath(projectName));
        projectManager.mkpath();
        projectManager.save(projectName);
        projectManager.load(projectName);

        /* Create module directory if it doesn't exist */
        QDir moduleDir(projectManager.getModulePath());
        if (!moduleDir.exists()) {
            moduleDir.mkpath(".");
        }

        // Create module file
        const QString moduleContent = R"(
test_driver:
  port:
    output_port:
      type: logic
      direction: out
)";

        createTempFile("module/test_driver.yaml", moduleContent);

        // Create a project that correctly connects top-level ports
        const QString correctProject = R"(
project:
  name: test_correct_behavior
  description: Test project with correct top-level port connections
  version: 1.0.0
  author: Test

  toplevel:
    name: top_correct_chip

    port:
      external_output:
        type: logic
        direction: output  # Should be driven by internal logic

  instances:
    u_driver:
      module: test_driver
      location:
        x: 100
        y: 100

  nets:
    # Correct: internal driver -> top-level output
    output_net:
      - { instance: u_driver, port: output_port }
      - { instance: top, port: external_output }
)";

        createTempFile("project.yaml", correctProject);

        // Clear message list for this test
        messageList.clear();

        // Generate netlist
        QSocCliWorker worker;
        QStringList   arguments;
        arguments << "qsoc" << "generate" << "--project"
                  << projectManager.getProjectPath() + "/project.yaml";

        worker.setup(arguments, true);
        worker.run();

        // Print all messages for debugging
        qDebug() << "Messages for correct behavior test:";
        for (const QString &message : messageList) {
            qDebug() << message;
        }

        // Should not have incorrect warnings for this correct configuration
        bool hasIncorrectWarning = false;
        for (const QString &message : messageList) {
            if (message.contains("external_output")
                && (message.contains("undriven") || message.contains("multiple drivers"))) {
                hasIncorrectWarning = true;
                qWarning() << "Unexpected warning for correct top-level output:" << message;
            }
        }

        QVERIFY2(
            !hasIncorrectWarning,
            "Should not generate warnings for correctly connected top-level output");
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegeneratetoplevelport.moc"
