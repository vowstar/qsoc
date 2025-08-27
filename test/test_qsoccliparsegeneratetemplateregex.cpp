// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/config.h"
#include "common/qsocbusmanager.h"
#include "common/qsocgeneratemanager.h"
#include "common/qsocmodulemanager.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTextStream>
#include <QtCore>
#include <QtTest>

class TestTemplateRegex : public QObject
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

    QString getTemplateOutput(const QString &fileName)
    {
        const QString filePath = QDir(projectManager.getOutputPath()).filePath(fileName);
        QFile         file(filePath);
        if (file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return file.readAll();
        }
        return QString();
    }

private slots:
    void initTestCase()
    {
        projectName = QString("test_qsoc_")
                      + QDateTime::currentDateTime().toString("yyyyMMdd_hhmmss_zzz");
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

    void init()
    {
        messageList.clear();
        qInstallMessageHandler(TestTemplateRegex::messageOutput);
    }

    void cleanup() { qInstallMessageHandler(nullptr); }

    /* Test regex_search filter - returns first match */
    void testRegexSearch()
    {
        const QString templateContent
            = "Test regex_search basic:\n"
              "{{ \"ID:123 NAME:John ID:456\" | regex_search(\"ID:(\\\\d+)\", 1) }}\n"
              "\n"
              "Test regex_search with default value:\n"
              "{{ \"no numbers here\" | regex_search(\"\\\\d+\", 0, \"NOT_FOUND\") }}\n"
              "\n"
              "Test regex_search whole match:\n"
              "{{ \"test@example.com\" | regex_search(\"\\\\w+@\\\\w+\\\\.\\\\w+\") }}\n"
              "\n"
              "Test regex_search case insensitive:\n"
              "{{ \"Hello World\" | regex_search(\"(?i)hello\") }}\n";

        const QString templatePath = createTempFile("test_regex_search.j2", templateContent);
        const QString outputFile   = "test_regex_search.txt";

        QVERIFY(generateManager.renderTemplate(templatePath, {}, {}, {}, {}, {}, outputFile));

        const QString output = getTemplateOutput(outputFile);
        QVERIFY(output.contains("123"));
        QVERIFY(!output.contains("456"));
        QVERIFY(output.contains("NOT_FOUND"));
        QVERIFY(output.contains("test@example.com"));
        QVERIFY(output.contains("Hello"));
    }

    /* Test regex_findall filter - returns all matches */
    void testRegexFindall()
    {
        const QString templateContent
            = "Test regex_findall basic:\n"
              "{% for id in \"ID:123 NAME:John ID:456 ID:789\" | regex_findall(\"ID:(\\\\d+)\", 1) "
              "%}\n"
              "- {{ id }}\n"
              "{% endfor %}\n"
              "\n"
              "Test regex_findall whole matches:\n"
              "{% for word in \"test@example.com\" | regex_findall(\"\\\\w+\") %}\n"
              "- {{ word }}\n"
              "{% endfor %}\n";

        const QString templatePath = createTempFile("test_regex_findall.j2", templateContent);
        const QString outputFile   = "test_regex_findall.txt";

        QVERIFY(generateManager.renderTemplate(templatePath, {}, {}, {}, {}, {}, outputFile));

        const QString output = getTemplateOutput(outputFile);
        QVERIFY(output.contains("123"));
        QVERIFY(output.contains("456"));
        QVERIFY(output.contains("789"));
        QVERIFY(output.contains("test"));
        QVERIFY(output.contains("example"));
        QVERIFY(output.contains("com"));
    }

    /* Test regex_replace filter - replaces all matches */
    void testRegexReplace()
    {
        const QString templateContent
            = "Test regex_replace basic:\n"
              "{{ \"hello world\" | regex_replace(\"\\\\s+\", \"_\") }}\n"
              "\n"
              "Test regex_replace with backreferences:\n"
              "{{ \"ABC123DEF456\" | regex_replace(\"([A-Z]+)(\\\\d+)\", \"\\\\2-\\\\1\") }}\n"
              "\n"
              "Test regex_replace case insensitive:\n"
              "{{ \"Error ERROR error\" | regex_replace(\"(?i)error\", \"WARNING\") }}\n";

        const QString templatePath = createTempFile("test_regex_replace.j2", templateContent);
        const QString outputFile   = "test_regex_replace.txt";

        QVERIFY(generateManager.renderTemplate(templatePath, {}, {}, {}, {}, {}, outputFile));

        const QString output = getTemplateOutput(outputFile);
        QVERIFY(output.contains("hello_world"));
        QVERIFY(output.contains("123-ABC"));
        QVERIFY(output.contains("456-DEF"));
        QVERIFY(output.contains("WARNING WARNING WARNING"));
    }

    /* Test advanced regex features from documentation */
    void testRegexAdvancedFeatures()
    {
        const QString templateContent
            = "Test email domain extraction:\n"
              "{{ \"user@example.com\" | regex_search(\"@([^.]+)\", 1, \"unknown\") }}\n"
              "\n"
              "Test price number extraction:\n"
              "{% for num in \"Price: $123, Tax: $45, Total: $168\" | "
              "regex_findall(\"\\\\$(\\\\d+)\", 1) %}\n"
              "- {{ num }}\n"
              "{% endfor %}\n"
              "\n"
              "Test multiple inline modifiers:\n"
              "{{ \"line1\\nERROR: test\\nline3\" | regex_search(\"(?im)^error.*$\") }}\n"
              "\n"
              "Test local scope modifier:\n"
              "{{ \"name:JOHN\" | regex_search(\"name:(?i:[a-z]+)\") }}\n";

        const QString templatePath = createTempFile("test_regex_advanced.j2", templateContent);
        const QString outputFile   = "test_regex_advanced.txt";

        QVERIFY(generateManager.renderTemplate(templatePath, {}, {}, {}, {}, {}, outputFile));

        const QString output = getTemplateOutput(outputFile);
        QVERIFY(output.contains("example"));     /* Email domain */
        QVERIFY(output.contains("123"));         /* First price */
        QVERIFY(output.contains("45"));          /* Second price */
        QVERIFY(output.contains("168"));         /* Third price */
        QVERIFY(output.contains("ERROR: test")); /* Multiline + case insensitive */
        QVERIFY(output.contains("name:JOHN"));   /* Local scope modifier */
    }

    /* Test basic error handling */
    void testRegexErrorHandling()
    {
        const QString templateContent
            = "Test regex_search with no match returns default:\n"
              "{{ \"test\" | regex_search(\"\\\\d+\", 0, \"DEFAULT\") }}\n";

        const QString templatePath = createTempFile("test_regex_errors.j2", templateContent);
        const QString outputFile   = "test_regex_errors.txt";

        QVERIFY(generateManager.renderTemplate(templatePath, {}, {}, {}, {}, {}, outputFile));

        const QString output = getTemplateOutput(outputFile);
        QVERIFY(output.contains("DEFAULT"));
    }
};

QStringList TestTemplateRegex::messageList;

#include "test_qsoccliparsegeneratetemplateregex.moc"
QSOC_TEST_MAIN(TestTemplateRegex)
