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

    void testRcsvTemplateBasic()
    {
        messageList.clear();

        /* Create a simple RCSV file based on SystemRDL test example */
        const QString rcsvContent
            = R"(addrmap_offset,addrmap_name,reg_offset,reg_name,reg_width,field_name,field_lsb,field_msb,reset_value,sw_access,hw_access,description
0x0000,DEMO,,,,,,,,,,
,,0x0000,CTRL,32,,,,,,,Control register
,,,,,ENABLE,0,0,0,RW,RW,Enable control bit
,,,,,MODE,1,2,0,RW,RW,Operation mode)";

        const QString rcsvFilePath = createTempFile("demo_chip.csv", rcsvContent);

        /* Create a simple Jinja2 template that uses simplified RCSV data */
        const QString templateContent = R"(// Generated from RCSV (Simplified JSON Format)
// Chip: {{ demo_chip.addrmap.inst_name }}
#define CHIP_NAME "{{ demo_chip.addrmap.inst_name }}"

{% for reg in demo_chip.registers %}
// Register: {{ reg.inst_name }} @ {{ reg.absolute_address }}
{% for field in reg.fields %}
//   Field: {{ field.inst_name }} [{{ field.msb }}:{{ field.lsb }}]
{% endfor %}
{% endfor %})";

        const QString templateFilePath = createTempFile("chip_header.h.j2", templateContent);

        /* Test command: qsoc generate template --rcsv demo_chip.csv chip_header.h.j2 */
        const QStringList arguments
            = {"qsoc",
               "generate",
               "template",
               "-d",
               projectManager.getCurrentPath(),
               "--rcsv",
               rcsvFilePath,
               templateFilePath};

        QSocCliWorker worker;
        worker.setup(arguments, false);
        worker.run();
        QVERIFY2(verifyTemplateOutputExistence("chip_header.h"), "Generated header file should exist");
        QVERIFY2(
            verifyTemplateContent("chip_header.h", "CHIP_NAME"),
            "Generated file should contain chip name");
        QVERIFY2(
            verifyTemplateContent("chip_header.h", "DEMO"),
            "Generated file should contain DEMO name");
        QVERIFY2(
            verifyTemplateContent("chip_header.h", "Register: CTRL"),
            "Generated file should contain register information from simplified JSON");
        QVERIFY2(
            verifyTemplateContent("chip_header.h", "Field: ENABLE"),
            "Generated file should contain field information from simplified JSON");
    }

    void testRcsvTemplateWithMultipleFiles()
    {
        messageList.clear();

        /* Create first RCSV file */
        const QString rcsvContent1
            = R"(addrmap_offset,addrmap_name,reg_offset,reg_name,reg_width,field_name,field_lsb,field_msb,reset_value,sw_access,hw_access,description
0x0000,CPU,,,,,,,,,,
,,0x0000,CPU_CTRL,32,,,,,,,CPU control register
,,,,,RUN,0,0,0,RW,RW,CPU run bit)";

        /* Create second RCSV file */
        const QString rcsvContent2
            = R"(addrmap_offset,addrmap_name,reg_offset,reg_name,reg_width,field_name,field_lsb,field_msb,reset_value,sw_access,hw_access,description
0x1000,MEMORY,,,,,,,,,,
,,0x0000,MEM_CTRL,32,,,,,,,Memory control register
,,,,,INIT,0,0,0,RW,RW,Memory init bit)";

        const QString rcsvFilePath1 = createTempFile("cpu_regs.csv", rcsvContent1);
        const QString rcsvFilePath2 = createTempFile("memory_regs.csv", rcsvContent2);

        /* Create template that uses both RCSV files with simplified JSON format */
        const QString templateContent = R"(// Multi-chip register definitions (Simplified JSON Format)
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

        /* Test with multiple RCSV files */
        const QStringList arguments
            = {"qsoc",
               "generate",
               "template",
               "-d",
               projectManager.getCurrentPath(),
               "--rcsv",
               rcsvFilePath1,
               "--rcsv",
               rcsvFilePath2,
               templateFilePath};

        QSocCliWorker worker;
        worker.setup(arguments, false);
        worker.run();
        QVERIFY2(verifyTemplateOutputExistence("multi_chip.h"), "Generated header file should exist");
        QVERIFY2(
            verifyTemplateContent("multi_chip.h", "CPU: CPU"),
            "Generated file should contain CPU registers");
        QVERIFY2(
            verifyTemplateContent("multi_chip.h", "Memory: MEMORY"),
            "Generated file should contain memory registers");
        QVERIFY2(
            verifyTemplateContent("multi_chip.h", "CPU Register: CPU_CTRL"),
            "Generated file should contain specific CPU register from simplified JSON");
        QVERIFY2(
            verifyTemplateContent("multi_chip.h", "Memory Register: MEM_CTRL"),
            "Generated file should contain specific memory register from simplified JSON");
    }

    void testRcsvWithOtherDataSources()
    {
        messageList.clear();

        /* Create RCSV file */
        const QString rcsvContent
            = R"(addrmap_offset,addrmap_name,reg_offset,reg_name,reg_width,field_name,field_lsb,field_msb,reset_value,sw_access,hw_access,description
0x0000,TEST_CHIP,,,,,,,,,,
,,0x0000,TEST_REG,32,,,,,,,Test register
,,,,,TEST,0,0,0,RW,RW,Test field)";

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
  project: "RCSV Test Project"
)";

        const QString rcsvFilePath = createTempFile("test_chip.csv", rcsvContent);
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
               "--rcsv",
               rcsvFilePath,
               templateFilePath};

        QSocCliWorker worker;
        worker.setup(arguments, false);
        worker.run();
        QVERIFY2(verifyTemplateOutputExistence("combined.h"), "Generated header file should exist");
        QVERIFY2(
            verifyTemplateContent("combined.h", "RCSV Test Project"),
            "Generated file should contain YAML data");
        QVERIFY2(
            verifyTemplateContent("combined.h", "test_param: 42"),
            "Generated file should contain CSV data");
        QVERIFY2(
            verifyTemplateContent("combined.h", "TEST_CHIP"),
            "Generated file should contain RCSV data");
        QVERIFY2(
            verifyTemplateContent("combined.h", "Register: TEST_REG"),
            "Generated file should contain register info from simplified JSON");
        QVERIFY2(
            verifyTemplateContent("combined.h", "Field: TEST"),
            "Generated file should contain field info from simplified JSON");
    }

    void testRcsvFileNotFound()
    {
        messageList.clear();

        /* Create template file */
        const QString templateContent  = "// Template content";
        const QString templateFilePath = createTempFile("test.h.j2", templateContent);

        /* Test with non-existent RCSV file */
        const QStringList arguments
            = {"qsoc",
               "generate",
               "template",
               "-d",
               projectManager.getCurrentPath(),
               "--rcsv",
               "nonexistent.csv",
               templateFilePath};

        QSocCliWorker worker;
        worker.setup(arguments, false);
        worker.run();

        /* Check error messages */
        bool foundError = false;
        for (const QString &msg : messageList) {
            if (msg.contains("Error: RCSV file does not exist")) {
                foundError = true;
                break;
            }
        }
        QVERIFY2(foundError, "Should show error message for missing RCSV file");
    }

    void testRcsvInvalidFormat()
    {
        messageList.clear();

        /* Create RCSV file with invalid format */
        const QString rcsvContent = R"(invalid,header,format
broken,data,row
incomplete,csv,format)";

        const QString rcsvFilePath = createTempFile("broken.csv", rcsvContent);

        /* Create template file */
        const QString templateContent  = "// Template content";
        const QString templateFilePath = createTempFile("test.h.j2", templateContent);

        /* Test with invalid RCSV file */
        const QStringList arguments
            = {"qsoc",
               "generate",
               "template",
               "-d",
               projectManager.getCurrentPath(),
               "--rcsv",
               rcsvFilePath,
               templateFilePath};

        QSocCliWorker worker;
        worker.setup(arguments, false);
        worker.run();

        /* Check error messages */
        bool foundError = false;
        for (const QString &msg : messageList) {
            if (msg.contains("Error: Failed to convert RCSV file")
                || msg.contains("Error: Failed to elaborate RCSV file")
                || msg.contains("Error: Failed to process RCSV file")) {
                foundError = true;
                break;
            }
        }
        QVERIFY2(foundError, "Should show error message for invalid RCSV format");
    }
};

QStringList Test::messageList = QStringList();

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegeneratetemplatercsv.moc"
