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
                                break;
                            }
                        }
                    }
                }
            }
        }

        /* If not found from logs, check the project output directory */
        if (templateContent.isEmpty()) {
            const QString projectOutputPath = projectManager.getOutputPath();
            if (!projectOutputPath.isNull()) {
                filePath = QDir(projectOutputPath).filePath(baseFileName);
                if (!filePath.isNull() && QFile::exists(filePath)) {
                    QFile file(filePath);
                    if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
                        templateContent = file.readAll();
                        file.close();
                    }
                }
            }
        }

        /* Empty content check */
        if (templateContent.isEmpty()) {
            return false;
        }

        return templateContent.contains(contentToVerify);
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

    void cleanupTestCase()
    {
#ifdef ENABLE_TEST_CLEANUP
        /* Clean up the test project directory */
        QDir projectDir(projectManager.getCurrentPath());
        if (projectDir.exists()) {
            projectDir.removeRecursively();
        }
#endif
    }

    void testGenerateTemplateHelp()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {"qsoc", "generate", "template", "--help"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        QVERIFY(messageList.filter(QRegularExpression("--help")).count() > 0);
        QVERIFY(messageList.filter(QRegularExpression("--csv")).count() > 0);
        QVERIFY(messageList.filter(QRegularExpression("--yaml")).count() > 0);
        QVERIFY(messageList.filter(QRegularExpression("--json")).count() > 0);
        QVERIFY(messageList.filter(QRegularExpression("templates")).count() > 0);
    }

    void testGenerateTemplateWithMissingTemplateFile()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "generate",
               "template",
               "-d",
               projectManager.getCurrentPath(),
               "non_existent_template.j2"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        QVERIFY(
            !messageList.filter(QRegularExpression("Error:.*Template file does not exist")).empty());
    }

    void testGenerateTemplateWithInvalidTemplate()
    {
        messageList.clear();

        /* Create invalid template file */
        const QDir    projectDir(projectManager.getCurrentPath());
        const QString invalidTemplatePath = projectDir.filePath("invalid_syntax_template.j2");
        QFile         invalidFile(invalidTemplatePath);
        QVERIFY(invalidFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream invalidStream(&invalidFile);
        invalidStream << "{{ invalid syntax }";
        invalidFile.close();

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "generate",
               "template",
               "-d",
               projectManager.getCurrentPath(),
               invalidTemplatePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        QVERIFY(
            !messageList.filter(QRegularExpression("Error:.*failed to render template")).empty());

        /* Clean up test file */
        QFile::remove(invalidTemplatePath);
    }

    void testGenerateTemplateWithCsvData()
    {
        messageList.clear();
        const QDir projectDir(projectManager.getCurrentPath());

        /* Create CSV data file */
        const QString csvContent  = R"(name,value,type
input1,10,input
output1,20,output
param1,string value,param)";
        const QString csvFilePath = projectDir.filePath("csv_only_data.csv");
        QFile         csvFile(csvFilePath);
        QVERIFY(csvFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream csvStream(&csvFile);
        csvStream << csvContent;
        csvFile.close();

        /* Create template file */
        const QString templateContent  = R"(// CSV Data Test
{% for item in csv_only_data %}
// - {{ item.name }}: {{ item.value }} ({{ item.type }})
{% endfor %}
)";
        const QString templateFilePath = projectDir.filePath("csv_test_template.j2");
        QFile         templateFile(templateFilePath);
        QVERIFY(templateFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream templateStream(&templateFile);
        templateStream << templateContent;
        templateFile.close();

        /* Run the command */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "generate",
               "template",
               "-d",
               projectManager.getCurrentPath(),
               "--csv",
               csvFilePath,
               templateFilePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        std::cout << messageList.join("\n").toStdString() << '\n';

        /* Verify results */
        QVERIFY(verifyTemplateOutputExistence("csv_test_template"));
        QVERIFY(verifyTemplateContent("csv_test_template", "input1: 10 (input)"));
        QVERIFY(verifyTemplateContent("csv_test_template", "output1: 20 (output)"));
        QVERIFY(verifyTemplateContent("csv_test_template", "param1: string value (param)"));
    }

    void testGenerateTemplateWithYamlData()
    {
        messageList.clear();
        const QDir projectDir(projectManager.getCurrentPath());

        /* Create YAML data file */
        const QString yamlContent  = R"(settings:
  project: test_project
  version: 1.0.0
options:
  debug: true
  optimization: high)";
        const QString yamlFilePath = projectDir.filePath("yaml_only_config.yaml");
        QFile         yamlFile(yamlFilePath);
        QVERIFY(yamlFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream yamlStream(&yamlFile);
        yamlStream << yamlContent;
        yamlFile.close();

        /* Create template file */
        const QString templateContent  = R"(// YAML Data Test
// Project: {{ settings.project }}
// Version: {{ settings.version }}
// Debug: {{ options.debug }}
// Optimization: {{ options.optimization }})";
        const QString templateFilePath = projectDir.filePath("yaml_test_template.j2");
        QFile         templateFile(templateFilePath);
        QVERIFY(templateFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream templateStream(&templateFile);
        templateStream << templateContent;
        templateFile.close();

        /* Run the command */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "generate",
               "template",
               "-d",
               projectManager.getCurrentPath(),
               "--yaml",
               yamlFilePath,
               templateFilePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify results */
        QVERIFY(verifyTemplateOutputExistence("yaml_test_template"));
        QVERIFY(verifyTemplateContent("yaml_test_template", "Project: test_project"));
        QVERIFY(verifyTemplateContent("yaml_test_template", "Version: 1.0.0"));
        QVERIFY(verifyTemplateContent("yaml_test_template", "Debug: true"));
        QVERIFY(verifyTemplateContent("yaml_test_template", "Optimization: high"));
    }

    void testGenerateTemplateWithJsonData()
    {
        messageList.clear();
        const QDir projectDir(projectManager.getCurrentPath());

        /* Create JSON data file */
        const QString jsonContent  = R"({
  "metadata": {
    "author": "Test User",
    "date": "2025-04-06"
  },
  "settings": {
    "advanced": {
      "feature1": true
    }
  }
})";
        const QString jsonFilePath = projectDir.filePath("json_only_metadata.json");
        QFile         jsonFile(jsonFilePath);
        QVERIFY(jsonFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream jsonStream(&jsonFile);
        jsonStream << jsonContent;
        jsonFile.close();

        /* Create template file */
        const QString templateContent  = R"(// JSON Data Test
// Author: {{ metadata.author }}
// Date: {{ metadata.date }}
// Feature1: {{ settings.advanced.feature1 }})";
        const QString templateFilePath = projectDir.filePath("json_test_template.j2");
        QFile         templateFile(templateFilePath);
        QVERIFY(templateFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream templateStream(&templateFile);
        templateStream << templateContent;
        templateFile.close();

        /* Run the command */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "generate",
               "template",
               "-d",
               projectManager.getCurrentPath(),
               "--json",
               jsonFilePath,
               templateFilePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify results */
        QVERIFY(verifyTemplateOutputExistence("json_test_template"));
        QVERIFY(verifyTemplateContent("json_test_template", "Author: Test User"));
        QVERIFY(verifyTemplateContent("json_test_template", "Date: 2025-04-06"));
        QVERIFY(verifyTemplateContent("json_test_template", "Feature1: true"));
    }

    void testGenerateTemplateWithMultipleDataSources()
    {
        messageList.clear();
        const QDir projectDir(projectManager.getCurrentPath());

        /* Create CSV data file */
        const QString csvContent  = R"(name,value,type
input1,10,input
output1,20,output)";
        const QString csvFilePath = projectDir.filePath("multi_data_entries.csv");
        QFile         csvFile(csvFilePath);
        QVERIFY(csvFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream csvStream(&csvFile);
        csvStream << csvContent;
        csvFile.close();

        /* Create YAML data file */
        const QString yamlContent  = R"(settings:
  project: multi_test_project
  version: 2.0.0)";
        const QString yamlFilePath = projectDir.filePath("multi_data_config.yaml");
        QFile         yamlFile(yamlFilePath);
        QVERIFY(yamlFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream yamlStream(&yamlFile);
        yamlStream << yamlContent;
        yamlFile.close();

        /* Create JSON data file */
        const QString jsonContent  = R"({
  "metadata": {
    "author": "Multi Data Test",
    "department": "Testing"
  }
})";
        const QString jsonFilePath = projectDir.filePath("multi_data_info.json");
        QFile         jsonFile(jsonFilePath);
        QVERIFY(jsonFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream jsonStream(&jsonFile);
        jsonStream << jsonContent;
        jsonFile.close();

        /* Create template file */
        const QString templateContent  = R"(// Multiple Data Sources Test
// Project: {{ settings.project }}
// Version: {{ settings.version }}
// Author: {{ metadata.author }}
// Department: {{ metadata.department }}

// Data Items:
{% for item in data %}
// - {{ item.name }}: {{ item.value }} ({{ item.type }})
{% endfor %}
)";
        const QString templateFilePath = projectDir.filePath("multi_data_template.j2");
        QFile         templateFile(templateFilePath);
        QVERIFY(templateFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream templateStream(&templateFile);
        templateStream << templateContent;
        templateFile.close();

        /* Run the command */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "generate",
               "template",
               "-d",
               projectManager.getCurrentPath(),
               "--csv",
               csvFilePath,
               "--yaml",
               yamlFilePath,
               "--json",
               jsonFilePath,
               templateFilePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify results */
        QVERIFY(verifyTemplateOutputExistence("multi_data_template"));

        /* Verify CSV data is present */
        QVERIFY(verifyTemplateContent("multi_data_template", "input1: 10 (input)"));
        QVERIFY(verifyTemplateContent("multi_data_template", "output1: 20 (output)"));

        /* Verify YAML data is present */
        QVERIFY(verifyTemplateContent("multi_data_template", "Project: multi_test_project"));
        QVERIFY(verifyTemplateContent("multi_data_template", "Version: 2.0.0"));

        /* Verify JSON data is present */
        QVERIFY(verifyTemplateContent("multi_data_template", "Author: Multi Data Test"));
        QVERIFY(verifyTemplateContent("multi_data_template", "Department: Testing"));
    }

    void testGenerateTemplateWithMultipleTemplateFiles()
    {
        messageList.clear();
        const QDir projectDir(projectManager.getCurrentPath());

        /* Create YAML data file with config */
        const QString yamlContent  = R"(module:
  name: cpu_wrapper
  manufacturer: ACME
  id: 12345)";
        const QString yamlFilePath = projectDir.filePath("module_config.yaml");
        QFile         yamlFile(yamlFilePath);
        QVERIFY(yamlFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream yamlStream(&yamlFile);
        yamlStream << yamlContent;
        yamlFile.close();

        /* Create first template file for module header */
        const QString template1Content = R"(// Module Header: {{ module.name }}
// Manufacturer: {{ module.manufacturer }}
// ID: {{ module.id }}

module {{ module.name }} (
  input  wire clk,
  input  wire rst_n,
  output wire ready
);)";
        const QString template1Path    = projectDir.filePath("module_header.j2");
        QFile         template1File(template1Path);
        QVERIFY(template1File.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream template1Stream(&template1File);
        template1Stream << template1Content;
        template1File.close();

        /* Create second template file for module implementation */
        const QString template2Content = R"(// Module Implementation: {{ module.name }}

  // Internal signals
  reg ready_reg;

  always @(posedge clk or negedge rst_n) begin
    if (!rst_n)
      ready_reg <= 1'b0;
    else
      ready_reg <= 1'b1;
  end

  assign ready = ready_reg;

endmodule // {{ module.name }})";
        const QString template2Path    = projectDir.filePath("module_implementation.j2");
        QFile         template2File(template2Path);
        QVERIFY(template2File.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream template2Stream(&template2File);
        template2Stream << template2Content;
        template2File.close();

        /* Run the command */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "generate",
               "template",
               "-d",
               projectManager.getCurrentPath(),
               "--yaml",
               yamlFilePath,
               template1Path,
               template2Path};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify first template output */
        QVERIFY(verifyTemplateOutputExistence("module_header"));
        QVERIFY(verifyTemplateContent("module_header", "Module Header: cpu_wrapper"));
        QVERIFY(verifyTemplateContent("module_header", "module cpu_wrapper"));

        /* Verify second template output */
        QVERIFY(verifyTemplateOutputExistence("module_implementation"));
        QVERIFY(
            verifyTemplateContent("module_implementation", "Module Implementation: cpu_wrapper"));
        QVERIFY(verifyTemplateContent("module_implementation", "endmodule // cpu_wrapper"));
    }

    void testGenerateTemplateWithFormatFilter()
    {
        messageList.clear();
        const QDir projectDir(projectManager.getCurrentPath());

        /* Create JSON data file with various data types */
        const QString jsonContent  = R"({
    "name": "Alice",
    "age": 30,
    "price": 123.456,
    "isActive": true,
    "description": null,
    "hexValue": 255,
    "octalValue": 64,
    "binaryValue": 15
})";
        const QString jsonFilePath = projectDir.filePath("format_test_data.json");
        QFile         jsonFile(jsonFilePath);
        QVERIFY(jsonFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream jsonStream(&jsonFile);
        jsonStream << jsonContent;
        jsonFile.close();

        /* Create template file with format filter tests using fmt library syntax */
        const QString templateContent  = R"(// Format Filter Tests (fmt library - direct)
// String formatting
{{ "Name: {}"|format(name) }}

// Float formatting with precision
{{ "Price: ${:.2f}"|format(price) }}

// Integer formatting
{{ "Age: {:d}"|format(age) }}

// Boolean formatting
{{ "Active: {}"|format(isActive) }}

// Hexadecimal formatting (fmt style: uppercase)
{{ "Hex: 0x{:X}"|format(hexValue) }}

// Octal formatting (fmt style: with # prefix)
{{ "Octal: 0o{:o}"|format(octalValue) }}

// Binary formatting (fmt style: with # prefix)
{{ "Binary: 0b{:b}"|format(binaryValue) }}
)";
        const QString templateFilePath = projectDir.filePath("format_test_template.j2");
        QFile         templateFile(templateFilePath);
        QVERIFY(templateFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream templateStream(&templateFile);
        templateStream << templateContent;
        templateFile.close();

        /* Run the command */
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "generate",
               "template",
               "-d",
               projectManager.getCurrentPath(),
               "--json",
               jsonFilePath,
               templateFilePath};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        std::cout << messageList.join("\n").toStdString() << '\n';

        /* Verify results */
        QVERIFY(verifyTemplateOutputExistence("format_test_template"));

        /* Test string formatting */
        QVERIFY(verifyTemplateContent("format_test_template", "Name: Alice"));

        /* Test float formatting with precision */
        QVERIFY(verifyTemplateContent("format_test_template", "Price: $123.46"));

        /* Test integer formatting */
        QVERIFY(verifyTemplateContent("format_test_template", "Age: 30"));

        /* Test boolean formatting */
        QVERIFY(verifyTemplateContent("format_test_template", "Active: true"));

        /* Test hexadecimal formatting */
        QVERIFY(verifyTemplateContent("format_test_template", "Hex: 0xFF"));

        /* Test octal formatting */
        QVERIFY(verifyTemplateContent("format_test_template", "Octal: 0o100"));

        /* Test binary formatting */
        QVERIFY(verifyTemplateContent("format_test_template", "Binary: 0b1111"));
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegeneratetemplate.moc"
