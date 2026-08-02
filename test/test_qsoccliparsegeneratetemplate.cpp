// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/config.h"
#include "common/qsocbusmanager.h"
#include "common/qsocconsole.h"
#include "common/qsocgeneratemanager.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocnumberinfo.h"
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
            return filePath;
        }
        return {};
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
        /* Mirror QSocConsole writes through the message handler so legacy
         * messageList-based assertions still see them. */
        QSocConsole::setTeeToMessageHandler(true);
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

    /* String values convert to integers only on a complete two-state number;
       sized literals are truncated to their declared width first. */
    void testGenerateTemplateFormatConvertsNumericStrings()
    {
        messageList.clear();
        const QDir projectDir(projectManager.getCurrentPath());

        /* Plain string literal: apostrophes inside a raw string desync the
           moc lexer and it stops seeing Q_OBJECT. */
        const QString jsonContent  = "{\n"
                                     "    \"sizedHex\": \"8'hAB\",\n"
                                     "    \"overWidth\": \"16'hFFFFF\",\n"
                                     "    \"unsizedHex\": \"'h1f\",\n"
                                     "    \"cOctal\": \"0644\",\n"
                                     "    \"prefixedOctal\": \"0o644\",\n"
                                     "    \"badHex\": \"0xZZ\",\n"
                                     "    \"badUnderscore\": \"8'h___\",\n"
                                     "    \"plainDecimal\": \"42\",\n"
                                     "    \"bareZero\": \"0\"\n"
                                     "}";
        const QString jsonFilePath = projectDir.filePath("format_numeric_data.json");
        QFile         jsonFile(jsonFilePath);
        QVERIFY(jsonFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream(&jsonFile) << jsonContent;
        jsonFile.close();

        const QString templateContent  = R"(sized={{ "{:d}"|format(sizedHex) }}
over={{ "{:d}"|format(overWidth) }}
unsized={{ "{:d}"|format(unsizedHex) }}
coctal={{ "{:d}"|format(cOctal) }}
poctal={{ "{:d}"|format(prefixedOctal) }}
bad=[{{ "{:s}"|format(badHex) }}]
badu=[{{ "{:s}"|format(badUnderscore) }}]
plain=[{{ "{:s}"|format(plainDecimal) }}]
zero={{ "{:d}"|format(bareZero) }}
)";
        const QString templateFilePath = projectDir.filePath("format_numeric_template.j2");
        QFile         templateFile(templateFilePath);
        QVERIFY(templateFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream(&templateFile) << templateContent;
        templateFile.close();

        const QString outputPath
            = QDir(projectManager.getOutputPath()).filePath("format_numeric_template");
        QVERIFY(QFile::remove(outputPath) || !QFile::exists(outputPath));

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

        const QString messages = messageList.join('\n');
        QVERIFY2(
            messages.contains("Successfully generated file from template: " + outputPath),
            qPrintable(messages));
        QFile outputFile(outputPath);
        QVERIFY(outputFile.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString output = outputFile.readAll();
        QVERIFY(output.contains("sized=171"));
        QVERIFY(output.contains("over=65535"));
        QVERIFY(output.contains("unsized=31"));
        QVERIFY(output.contains("coctal=420"));
        QVERIFY(output.contains("poctal=420"));
        /* Invalid digits and plain decimals keep string semantics. */
        QVERIFY(output.contains("bad=[0xZZ]"));
        QVERIFY(output.contains("badu=[8'h___]"));
        QVERIFY(output.contains("plain=[42]"));
        /* Bare 0 is the zero of the C octal family, not a plain-decimal
           string; it converts like 00 and 0o0 do. */
        QVERIFY(output.contains("zero=0"));
    }

    /* A converted value outside the signed 64-bit range fails generation
       before the output file is written. */
    void testGenerateTemplateFormatRejectsOverflow()
    {
        messageList.clear();
        const QDir projectDir(projectManager.getCurrentPath());

        const QString huge = "0x" + QString(QSocNumberInfo::MaximumNumericCharacters - 2, 'F');
        QCOMPARE(huge.size(), QSocNumberInfo::MaximumNumericCharacters);
        const QJsonDocument document(QJsonObject{{"huge", huge}});
        const QString       jsonFilePath = projectDir.filePath("format_overflow_data.json");
        QFile               jsonFile(jsonFilePath);
        QVERIFY(jsonFile.open(QIODevice::WriteOnly));
        QCOMPARE(jsonFile.write(document.toJson()), document.toJson().size());
        jsonFile.close();

        const QDir    outputDir(projectManager.getOutputPath());
        const QString outputPath  = outputDir.filePath("format_overflow_template");
        const QString sidecarPath = outputDir.filePath("format_overflow_template.json");
        QFile::remove(outputPath);
        QFile::remove(sidecarPath);

        const QString templateContent  = R"(huge={{ "{:d}"|format(huge) }}
)";
        const QString templateFilePath = projectDir.filePath("format_overflow_template.j2");
        QFile         templateFile(templateFilePath);
        QVERIFY(templateFile.open(QIODevice::WriteOnly | QIODevice::Text));
        QTextStream(&templateFile) << templateContent;
        templateFile.close();

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

        QVERIFY(!verifyTemplateOutputExistence("format_overflow_template"));
        QVERIFY(!QFile::exists(sidecarPath));
        const QString            messages = messageList.join('\n');
        const QRegularExpression rangeRegex(
            "Template format value \"([^\"]+)\" is outside the signed 64-bit range");
        const QRegularExpressionMatch rangeMatch = rangeRegex.match(messages);
        QVERIFY(rangeMatch.hasMatch());
        QCOMPARE(rangeMatch.captured(1).size(), 128);
        QVERIFY(rangeMatch.captured(1).endsWith("..."));
        QVERIFY(!messages.contains(huge));
        QVERIFY(messages.size() < 512);
    }

    void testGenerateTemplateFormatRejectsOversizedNumericText()
    {
        messageList.clear();
        const QDir    projectDir(projectManager.getCurrentPath());
        const QString maximum = "0x1" + QString(QSocNumberInfo::MaximumNumericCharacters - 3, '_');
        QCOMPARE(maximum.size(), QSocNumberInfo::MaximumNumericCharacters);
        const QString oversized = maximum + '_';
        QCOMPARE(oversized.size(), QSocNumberInfo::MaximumNumericCharacters + 1);

        const QString jsonPath       = projectDir.filePath("format_limit_data.json");
        const auto    writeJsonValue = [&jsonPath](const QString &value) {
            QFile file(jsonPath);
            if (!file.open(QIODevice::WriteOnly)) {
                return false;
            }
            const QByteArray bytes = QJsonDocument(QJsonObject{{"huge", value}}).toJson();
            return file.write(bytes) == bytes.size();
        };

        const QString templatePath = projectDir.filePath("format_limit_template.j2");
        QFile         templateFile(templatePath);
        QVERIFY(templateFile.open(QIODevice::WriteOnly));
        const QByteArray templateData = "decimal={{ \"{:d}\"|format(huge) }}\n"
                                        "hex={{ \"{:x}\"|format(huge) }}\n";
        QCOMPARE(templateFile.write(templateData), templateData.size());
        templateFile.close();

        const QDir    outputDir(projectManager.getOutputPath());
        const QString outputPath    = outputDir.filePath("format_limit.out");
        const QString sidecarPath   = outputDir.filePath("format_limit.json");
        const auto    writeSentinel = [](const QString &path, const QByteArray &bytes) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
        };
        const auto readBytes = [](const QString &path) {
            QFile file(path);
            return file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray();
        };

        const QString maximumOutputPath = outputDir.filePath("format_limit_ok.out");
        QVERIFY(writeJsonValue(maximum));
        QVERIFY(
            generateManager
                .renderTemplate(templatePath, {}, {}, {jsonPath}, {}, {}, "format_limit_ok.out"));
        QCOMPARE(readBytes(maximumOutputPath), QByteArray("decimal=1\nhex=1\n"));

        messageList.clear();
        QVERIFY(writeJsonValue(oversized));
        QVERIFY(writeSentinel(outputPath, "old primary\n"));
        QVERIFY(writeSentinel(sidecarPath, "old sidecar\n"));
        QVERIFY(!generateManager
                     .renderTemplate(templatePath, {}, {}, {jsonPath}, {}, {}, "format_limit.out"));
        QCOMPARE(readBytes(outputPath), QByteArray("old primary\n"));
        QCOMPARE(readBytes(sidecarPath), QByteArray("old sidecar\n"));

        const QString            messages = messageList.join('\n');
        const QRegularExpression limitRegex(
            "Template format value \"([^\"]+)\" exceeds the 65536-character limit");
        const QRegularExpressionMatch limitMatch = limitRegex.match(messages);
        QVERIFY(limitMatch.hasMatch());
        QCOMPARE(limitMatch.captured(1).size(), 128);
        QVERIFY(limitMatch.captured(1).endsWith("..."));
        QVERIFY(!messages.contains(oversized));
        QVERIFY(messages.size() < 512);

        QVERIFY(QFile::remove(outputPath));
        QVERIFY(QFile::remove(sidecarPath));
        messageList.clear();
        QVERIFY(!generateManager
                     .renderTemplate(templatePath, {}, {}, {jsonPath}, {}, {}, "format_limit.out"));
        QVERIFY(!QFile::exists(outputPath));
        QVERIFY(!QFile::exists(sidecarPath));
    }

    void testTemplateArtifactCompatibility()
    {
        QTemporaryDir directory;
        QVERIFY(directory.isValid());
        const QString projectPath = QDir(directory.path()).filePath("project");

        QSocProjectManager manager;
        manager.setCurrentPath(projectPath);
        QVERIFY(manager.mkpath());
        QSocGenerateManager generator(nullptr, &manager);

        const QString templatePath = QDir(projectPath).filePath("stable.inja");
        QFile         templateFile(templatePath);
        QVERIFY(templateFile.open(QIODevice::WriteOnly));
        QCOMPARE(templateFile.write("stable output\n"), 14);
        templateFile.close();

        QVERIFY(QDir(manager.getOutputPath()).mkdir("nested"));
        QVERIFY(generator.renderTemplate(templatePath, {}, {}, {}, {}, {}, "nested/rendered.txt"));
        QFile nestedOutput(QDir(manager.getOutputPath()).filePath("nested/rendered.txt"));
        QVERIFY(nestedOutput.open(QIODevice::ReadOnly));
        QCOMPARE(nestedOutput.readAll(), QByteArray("stable output\n"));
        QVERIFY(QFileInfo::exists(QDir(manager.getOutputPath()).filePath("rendered.json")));

        QVERIFY(generator.renderTemplate(templatePath, {}, {}, {}, {}, {}, "/nested/leading.txt"));
        QFile leadingOutput(QDir(manager.getOutputPath()).filePath("nested/leading.txt"));
        QVERIFY(leadingOutput.open(QIODevice::ReadOnly));
        QCOMPARE(leadingOutput.readAll(), QByteArray("stable output\n"));

        QVERIFY(generator.renderTemplate(templatePath, {}, {}, {}, {}, {}, "payload.json"));
        QFile collisionOutput(QDir(manager.getOutputPath()).filePath("payload.json"));
        QVERIFY(collisionOutput.open(QIODevice::ReadOnly));
        QCOMPARE(collisionOutput.readAll(), QByteArray("stable output\n"));

#ifdef Q_OS_LINUX
        QVERIFY(generator.renderTemplate(templatePath, {}, {}, {}, {}, {}, "case.JSON"));
        QVERIFY(QFileInfo::exists(QDir(manager.getOutputPath()).filePath("case.JSON")));
        QVERIFY(QFileInfo::exists(QDir(manager.getOutputPath()).filePath("case.json")));
#endif
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegeneratetemplate.moc"
