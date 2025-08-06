// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/config.h"
#include "common/qsocbusmanager.h"
#include "common/qsocgeneratemanager.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTextStream>
#include <QtCore>
#include <QtTest>

class Test : public QObject
{
    Q_OBJECT

private:
    static QStringList  messageList;
    QString             projectName;
    QSocProjectManager  projectManager;
    QSocModuleManager   moduleManager;
    QSocBusManager      busManager;
    QSocGenerateManager generateManager;

    static void messageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
    {
        Q_UNUSED(type);
        Q_UNUSED(context);
        messageList << msg;
    }

    QString createTempFile(const QString &fileName, const QString &content)
    {
        QString filePath = QDir(projectManager.getOutputPath()).filePath(fileName);
        QFile   file(filePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&file);
            stream << content;
            file.close();
        }
        return filePath;
    }

    /* Verify template output existence */
    bool verifyTemplateOutputExistence(const QString &baseFileName)
    {
        /* First check the current project's output directory if available */
        for (const QString &msg : messageList) {
            if (msg.contains("Successfully generated file from template:")
                && msg.contains(baseFileName)) {
                const QRegularExpression regex("Successfully generated file from template: (.+)");
                const QRegularExpressionMatch match = regex.match(msg);
                if (match.hasMatch()) {
                    const QString filePath = match.captured(1);
                    if (QFile::exists(filePath)) {
                        return true;
                    }
                }
            }
        }

        /* Check the project output directory */
        const QString projectOutputPath = projectManager.getOutputPath();
        const QString projectFilePath   = QDir(projectOutputPath).filePath(baseFileName);
        return QFile::exists(projectFilePath);
    }

    /* Get rendered template content and check if it contains specific text */
    bool verifyTemplateContent(const QString &baseFileName, const QString &contentToVerify)
    {
        if (baseFileName.isNull() || contentToVerify.isNull()) {
            return false;
        }

        QString templateContent;
        QString filePath;

        /* First try from message logs */
        for (const QString &msg : messageList) {
            if (msg.isNull()) {
                continue;
            }
            if (msg.contains("Successfully generated file from template:")
                && msg.contains(baseFileName)) {
                const QRegularExpression regex("Successfully generated file from template: (.+)");
                const QRegularExpressionMatch match = regex.match(msg);
                if (match.hasMatch()) {
                    filePath = match.captured(1);
                    if (!filePath.isNull() && QFile::exists(filePath)) {
                        QFile file(filePath);
                        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                            templateContent = file.readAll();
                            file.close();
                            if (!templateContent.isNull()) {
                                return templateContent.contains(contentToVerify);
                            }
                        }
                    }
                }
            }
        }

        /* Fallback to project output directory */
        const QString projectOutputPath = projectManager.getOutputPath();
        const QString projectFilePath   = QDir(projectOutputPath).filePath(baseFileName);
        if (QFile::exists(projectFilePath)) {
            QFile file(projectFilePath);
            if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                templateContent = file.readAll();
                file.close();
                if (!templateContent.isNull()) {
                    return templateContent.contains(contentToVerify);
                }
            }
        }

        return false;
    }

private slots:
    void initTestCase()
    {
        qInstallMessageHandler(messageOutput);
        /* Set project name */
        projectName = QFileInfo(__FILE__).baseName() + "_data";
        /* Setup project manager */
        projectManager.setProjectName(projectName);
        projectManager.setCurrentPath(QDir::current().filePath(projectName));
        projectManager.mkpath();
        projectManager.save(projectName);
        projectManager.load(projectName);

        /* Setup other managers */
        moduleManager.setProjectManager(&projectManager);
        busManager.setProjectManager(&projectManager);
        generateManager.setProjectManager(&projectManager);
        generateManager.setModuleManager(&moduleManager);
        generateManager.setBusManager(&busManager);
    }

    void cleanupTestCase() { projectManager.remove(projectName); }

    void testRdlTemplateBasic()
    {
        messageList.clear();

        /* Create a simple SystemRDL file */
        const QString rdlContent = R"(addrmap simple_chip {
    reg {
        field {
            sw = rw;
            hw = r;
        } enable[0:0];
    } ctrl_reg @ 0x0000;
};)";

        const QString rdlFilePath = createTempFile("simple_chip.rdl", rdlContent);

        /* Create a simple Jinja2 template that uses simplified RDL data */
        const QString templateContent = R"(// Generated from SystemRDL (Simplified JSON)
// Chip: {{ simple_chip.addrmap.inst_name }}
#define CHIP_NAME "{{ simple_chip.addrmap.inst_name }}"

{% for reg in simple_chip.registers %}
// Register: {{ reg.inst_name }} @ {{ reg.absolute_address }}
{% for field in reg.fields %}
//   Field: {{ field.inst_name }} [{{ field.msb }}:{{ field.lsb }}]
{% endfor %}
{% endfor %})";

        const QString templateFilePath = createTempFile("chip_header.h.j2", templateContent);

        /* Test command: qsoc generate template --rdl simple_chip.rdl chip_header.h.j2 */
        const QStringList arguments
            = {"qsoc",
               "generate",
               "template",
               "-d",
               projectManager.getCurrentPath(),
               "--rdl",
               rdlFilePath,
               templateFilePath};

        QSocCliWorker worker;
        worker.setup(arguments, false);
        worker.run();
        QVERIFY2(verifyTemplateOutputExistence("chip_header.h"), "Generated header file should exist");
        QVERIFY2(
            verifyTemplateContent("chip_header.h", "CHIP_NAME"),
            "Generated file should contain chip name");
        QVERIFY2(
            verifyTemplateContent("chip_header.h", "simple_chip"),
            "Generated file should contain simple_chip name");
        QVERIFY2(
            verifyTemplateContent("chip_header.h", "Register: ctrl_reg"),
            "Generated file should contain register information from simplified JSON");
        QVERIFY2(
            verifyTemplateContent("chip_header.h", "Field: enable"),
            "Generated file should contain field information from simplified JSON");
    }

    void testRdlTemplateWithMultipleFiles()
    {
        messageList.clear();

        /* Create first SystemRDL file */
        const QString rdlContent1 = R"(addrmap cpu_regs {
    reg {
        field {
            sw = rw;
            hw = r;
        } run[0:0];
    } cpu_ctrl @ 0x0000;
};)";

        /* Create second SystemRDL file */
        const QString rdlContent2 = R"(addrmap memory_regs {
    reg {
        field {
            sw = rw;
            hw = r;
        } init[0:0];
    } mem_ctrl @ 0x1000;
};)";

        const QString rdlFilePath1 = createTempFile("cpu_regs.rdl", rdlContent1);
        const QString rdlFilePath2 = createTempFile("memory_regs.rdl", rdlContent2);

        /* Create template that uses both RDL files with simplified JSON format */
        const QString templateContent = R"(// Multi-chip register definitions (Simplified JSON)
{% if cpu_regs %}
// CPU: {{ cpu_regs.addrmap.inst_name }}
{% for reg in cpu_regs.registers %}
//   CPU Register: {{ reg.inst_name }}
{% endfor %}
{% endif %}
{% if memory_regs %}
// Memory: {{ memory_regs.addrmap.inst_name }}
{% for reg in memory_regs.registers %}
//   Memory Register: {{ reg.inst_name }}
{% endfor %}
{% endif %})";

        const QString templateFilePath = createTempFile("multi_chip.h.j2", templateContent);

        /* Test with multiple RDL files */
        const QStringList arguments
            = {"qsoc",
               "generate",
               "template",
               "-d",
               projectManager.getCurrentPath(),
               "--rdl",
               rdlFilePath1,
               "--rdl",
               rdlFilePath2,
               templateFilePath};

        QSocCliWorker worker;
        worker.setup(arguments, false);
        worker.run();
        QVERIFY2(verifyTemplateOutputExistence("multi_chip.h"), "Generated header file should exist");
        QVERIFY2(
            verifyTemplateContent("multi_chip.h", "CPU: cpu_regs"),
            "Generated file should contain CPU registers");
        QVERIFY2(
            verifyTemplateContent("multi_chip.h", "Memory: memory_regs"),
            "Generated file should contain memory registers");
        QVERIFY2(
            verifyTemplateContent("multi_chip.h", "CPU Register: cpu_ctrl"),
            "Generated file should contain specific CPU register from simplified JSON");
        QVERIFY2(
            verifyTemplateContent("multi_chip.h", "Memory Register: mem_ctrl"),
            "Generated file should contain specific memory register from simplified JSON");
    }

    void testRdlWithOtherDataSources()
    {
        messageList.clear();

        /* Create SystemRDL file */
        const QString rdlContent = R"(addrmap test_chip {
    reg {
        field {
            sw = rw;
            hw = r;
        } test[0:0];
    } test_reg @ 0x0000;
};)";

        /* Create CSV file */
        const QString csvContent = R"(name,value,description
test_param,42,Test parameter value
version,1.0,Version information
)";

        /* Create YAML file */
        const QString yamlContent = R"(
metadata:
  author: "Test Author"
  date: "2025-01-01"
  project: "RDL Test Project"
)";

        const QString rdlFilePath  = createTempFile("test_chip.rdl", rdlContent);
        const QString csvFilePath  = createTempFile("params.csv", csvContent);
        const QString yamlFilePath = createTempFile("metadata.yaml", yamlContent);

        /* Create template that uses all data sources with simplified JSON format */
        const QString templateContent = R"(// Project: {{ metadata.project }}
// Author: {{ metadata.author }}

// Chip: {{ test_chip.addrmap.inst_name }} (Simplified JSON Format)
{% for reg in test_chip.registers %}
// Register: {{ reg.inst_name }} @ {{ reg.absolute_address }}
{% for field in reg.fields %}
//   Field: {{ field.inst_name }} [{{ field.msb }}:{{ field.lsb }}]
{% endfor %}
{% endfor %}

// Parameters from CSV:
{% for item in params %}
// {{ item.name }}: {{ item.value }}
{% endfor %})";

        const QString templateFilePath = createTempFile("combined.h.j2", templateContent);

        /* Test with mixed data sources */
        const QStringList arguments
            = {"qsoc",
               "generate",
               "template",
               "-d",
               projectManager.getCurrentPath(),
               "--csv",
               csvFilePath,
               "--yaml",
               yamlFilePath,
               "--rdl",
               rdlFilePath,
               templateFilePath};

        QSocCliWorker worker;
        worker.setup(arguments, false);
        worker.run();
        QVERIFY2(verifyTemplateOutputExistence("combined.h"), "Generated header file should exist");
        QVERIFY2(
            verifyTemplateContent("combined.h", "RDL Test Project"),
            "Generated file should contain YAML data");
        QVERIFY2(
            verifyTemplateContent("combined.h", "test_param: 42"),
            "Generated file should contain CSV data");
        QVERIFY2(
            verifyTemplateContent("combined.h", "test_chip"),
            "Generated file should contain RDL data");
        QVERIFY2(
            verifyTemplateContent("combined.h", "Register: test_reg"),
            "Generated file should contain register info from simplified JSON");
        QVERIFY2(
            verifyTemplateContent("combined.h", "Field: test"),
            "Generated file should contain field info from simplified JSON");
    }

    void testRdlFileNotFound()
    {
        messageList.clear();

        /* Create template file */
        const QString templateContent  = "// Template content";
        const QString templateFilePath = createTempFile("test.h.j2", templateContent);

        /* Test with non-existent RDL file */
        const QStringList arguments
            = {"qsoc",
               "generate",
               "template",
               "-d",
               projectManager.getCurrentPath(),
               "--rdl",
               "nonexistent.rdl",
               templateFilePath};

        QSocCliWorker worker;
        worker.setup(arguments, false);
        worker.run();

        /* Check error messages */
        bool foundError = false;
        for (const QString &msg : messageList) {
            if (msg.contains("Error: SystemRDL file does not exist")) {
                foundError = true;
                break;
            }
        }
        QVERIFY2(foundError, "Should show error message for missing RDL file");
    }

    void testRdlInvalidSyntax()
    {
        messageList.clear();

        /* Create SystemRDL file with invalid syntax */
        const QString rdlContent = R"(addrmap broken_chip {
    // Missing closing brace and invalid syntax
    reg {
        field invalid_field;
    } broken_reg @ 0x0000
// Missing closing brace for addrmap)";

        const QString rdlFilePath = createTempFile("broken.rdl", rdlContent);

        /* Create template file */
        const QString templateContent  = "// Template content";
        const QString templateFilePath = createTempFile("test.h.j2", templateContent);

        /* Test with invalid RDL file */
        const QStringList arguments
            = {"qsoc",
               "generate",
               "template",
               "-d",
               projectManager.getCurrentPath(),
               "--rdl",
               rdlFilePath,
               templateFilePath};

        QSocCliWorker worker;
        worker.setup(arguments, false);
        worker.run();

        /* Check error messages */
        bool foundError = false;
        for (const QString &msg : messageList) {
            if (msg.contains("Error: Failed to elaborate SystemRDL file")
                || msg.contains("Error: Failed to process SystemRDL file")) {
                foundError = true;
                break;
            }
        }
        QVERIFY2(foundError, "Should show error message for invalid RDL syntax");
    }
};

QStringList Test::messageList = QStringList();

QTEST_MAIN(Test)

#include "test_qsoccliparsegeneratetemplaterdl.moc"
