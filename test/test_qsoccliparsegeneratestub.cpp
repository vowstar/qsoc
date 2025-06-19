// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/config.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTemporaryFile>
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
    QString            projectName;
    QSocProjectManager projectManager;

    static void messageOutput(QtMsgType type, const QMessageLogContext &context, const QString &msg)
    {
        Q_UNUSED(type);
        Q_UNUSED(context);
        messageList << msg;
    }

    void createTestStubFiles()
    {
        /* Create dff_mem_1r_1w module */
        const QString dffMemContent = R"(
dff_mem_1r_1w:
  description: "Two-Port DFF-Based Memory"
  parameter:
    DW:
      type: integer
      value: 8
      description: "Data Width"
    DP:
      type: integer
      value: 4
      description: "Depth of Register File"
    DP_PTR_W:
      type: integer
      value: 2
      description: "Depth Pointer Width"
    REG_OUT:
      type: integer
      value: 1
      description: "Register Output Enable"
    AW:
      type: integer
      value: "(DP <= 1) ? 1 : DP_PTR_W"
      description: "Address Width derived from Depth Pointer Width"
  port:
    wclk:
      type: logic
      direction: in
      description: "Write clock."
    wrst_n:
      type: logic
      direction: in
      description: "Write reset, active low."
    wr:
      type: logic
      direction: in
      description: "Write enable."
    waddr:
      type: logic[AW-1:0]
      direction: in
      description: "Write address."
    wdata:
      type: logic[DW-1:0]
      direction: in
      description: "Write data."
    rclk:
      type: logic
      direction: in
      description: "Read clock."
    rrst_n:
      type: logic
      direction: in
      description: "Read reset, active low."
    rd:
      type: logic
      direction: in
      description: "Read enable."
    raddr:
      type: logic[AW-1:0]
      direction: in
      description: "Read address."
    rdata:
      type: logic[DW-1:0]
      direction: out
      description: "Read data."
)";

        /* Create simple_pll module */
        const QString pllContent = R"(
simple_pll:
  description: "Simple Phase-Locked Loop"
  parameter:
    M:
      type: logic[7:0]
      value: 8'h10
      description: "Multiplier value"
    N:
      type: logic[3:0]
      value: 4'h2
      description: "Divider value"
    OD:
      type: logic[1:0]
      value: 2'b00
      description: "Output divider"
  port:
    XIN:
      type: logic
      direction: input
      description: "Crystal input"
    BP:
      type: logic
      direction: input
      description: "Bypass mode"
    PDRST:
      type: logic
      direction: input
      description: "Power down reset"
    M:
      type: logic[7:0]
      direction: input
      description: "Multiplier input"
    N:
      type: logic[3:0]
      direction: input
      description: "Divider input"
    OD:
      type: logic[1:0]
      direction: input
      description: "Output divider input"
    LKDT:
      type: logic
      direction: output
      description: "Lock detect"
    CLK_OUT:
      type: logic
      direction: output
      description: "Output clock"
)";

        /* Create the module files */
        const QDir moduleDir(projectManager.getModulePath());

        /* Create dff_mem_1r_1w module file */
        const QString dffMemPath = moduleDir.filePath("dff_mem_1r_1w.soc_mod");
        QFile         dffMemFile(dffMemPath);
        if (dffMemFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&dffMemFile);
            stream << dffMemContent;
            dffMemFile.close();
        }

        /* Create simple_pll module file */
        const QString pllPath = moduleDir.filePath("simple_pll.soc_mod");
        QFile         pllFile(pllPath);
        if (pllFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&pllFile);
            stream << pllContent;
            pllFile.close();
        }
    }

    /* Check if files exist */
    bool verifyFileExists(const QString &fileName)
    {
        const QString projectOutputPath = projectManager.getOutputPath();
        const QString filePath          = QDir(projectOutputPath).filePath(fileName);
        return QFile::exists(filePath);
    }

    /* Get file content and check if it contains specific text */
    bool verifyFileContent(const QString &fileName, const QString &contentToVerify)
    {
        if (fileName.isNull() || contentToVerify.isNull()) {
            return false;
        }

        const QString projectOutputPath = projectManager.getOutputPath();
        const QString filePath          = QDir(projectOutputPath).filePath(fileName);

        if (!QFile::exists(filePath)) {
            return false;
        }

        QFile file(filePath);
        if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return false;
        }

        QString fileContent = file.readAll();
        file.close();

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
        const QString normalizedContent = normalizeWhitespace(fileContent);
        const QString normalizedVerify  = normalizeWhitespace(contentToVerify);

        /* Check if the normalized content contains the normalized text we're looking for */
        return normalizedContent.contains(normalizedVerify);
    }

private slots:
    void initTestCase()
    {
        TestApp::instance();
        /* Re-enable message handler for collecting CLI output */
        qInstallMessageHandler(messageOutput);
        /* Set project name */
        projectName = QFileInfo(__FILE__).baseName() + "_data";
        /* Setup project manager */
        projectManager.setProjectName(projectName);
        projectManager.setCurrentPath(QDir::current().filePath(projectName));
        projectManager.mkpath();
        projectManager.save(projectName);
        projectManager.load(projectName);
        /* Create test files */
        createTestStubFiles();
    }

    void cleanupTestCase()
    {
#ifdef ENABLE_TEST_CLEANUP
        /* Clean up the test project directory */
        QDir projectDir(projectManager.getCurrentPath());
        if (projectDir.exists()) {
            projectDir.removeRecursively();
        }
#endif // ENABLE_TEST_CLEANUP
    }

    void testGenerateStubHelp()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments = {"qsoc", "generate", "stub", "--help"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Just verify the command doesn't crash */
        QVERIFY(true);
    }

    void testGenerateStubMissingStubName()
    {
        messageList.clear();
        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "stub", "-d", projectManager.getCurrentPath()};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Just verify the command doesn't crash */
        QVERIFY(true);
    }

    void testGenerateStubBasic()
    {
        /* Clear previous messages */
        messageList.clear();

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc", "generate", "stub", "-d", projectManager.getCurrentPath(), "test_stub"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that stub files were generated */
        QVERIFY(verifyFileExists("test_stub.v"));
        QVERIFY(verifyFileExists("test_stub.lib"));

        /* Verify Verilog stub content */
        QVERIFY(verifyFileContent("test_stub.v", "module dff_mem_1r_1w"));
        QVERIFY(verifyFileContent("test_stub.v", "module simple_pll"));
        QVERIFY(verifyFileContent("test_stub.v", "parameter DW = 8"));
        QVERIFY(verifyFileContent("test_stub.v", "input wclk"));
        QVERIFY(verifyFileContent("test_stub.v", "output [DW-1:0] rdata"));
        QVERIFY(
            verifyFileContent("test_stub.v", "/* It is a stub, not a complete implementation */"));

        /* Verify Liberty stub content */
        QVERIFY(verifyFileContent("test_stub.lib", "library (test_stub)"));
        QVERIFY(verifyFileContent("test_stub.lib", "cell (dff_mem_1r_1w)"));
        QVERIFY(verifyFileContent("test_stub.lib", "cell (simple_pll)"));
        QVERIFY(verifyFileContent("test_stub.lib", "pin(wclk)"));
        QVERIFY(verifyFileContent("test_stub.lib", "direction : input"));
        QVERIFY(verifyFileContent("test_stub.lib", "direction : output"));
    }

    void testGenerateStubWithModuleFilter()
    {
        messageList.clear();

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "generate",
               "stub",
               "-d",
               projectManager.getCurrentPath(),
               "-m",
               "dff_.*",
               "memory_stub"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that stub files were generated */
        QVERIFY(verifyFileExists("memory_stub.v"));
        QVERIFY(verifyFileExists("memory_stub.lib"));

        /* Verify only dff_mem_1r_1w module is included */
        QVERIFY(verifyFileContent("memory_stub.v", "module dff_mem_1r_1w"));
        QVERIFY(!verifyFileContent("memory_stub.v", "module simple_pll"));

        /* Verify Liberty content */
        QVERIFY(verifyFileContent("memory_stub.lib", "cell (dff_mem_1r_1w)"));
        QVERIFY(!verifyFileContent("memory_stub.lib", "cell (simple_pll)"));
    }

    void testGenerateStubWithLibraryFilter()
    {
        messageList.clear();

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "generate",
               "stub",
               "-d",
               projectManager.getCurrentPath(),
               "-l",
               ".*",
               "-m",
               "simple_.*",
               "pll_stub"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that stub files were generated */
        QVERIFY(verifyFileExists("pll_stub.v"));
        QVERIFY(verifyFileExists("pll_stub.lib"));

        /* Verify only simple_pll module is included */
        QVERIFY(verifyFileContent("pll_stub.v", "module simple_pll"));
        QVERIFY(!verifyFileContent("pll_stub.v", "module dff_mem_1r_1w"));

        /* Verify Liberty content */
        QVERIFY(verifyFileContent("pll_stub.lib", "cell (simple_pll)"));
        QVERIFY(!verifyFileContent("pll_stub.lib", "cell (dff_mem_1r_1w)"));
    }

    void testGenerateStubVerilogDetails()
    {
        messageList.clear();

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "generate",
               "stub",
               "-d",
               projectManager.getCurrentPath(),
               "-m",
               "dff_mem_1r_1w",
               "detailed_stub"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that stub files were generated */
        QVERIFY(verifyFileExists("detailed_stub.v"));

        /* Verify detailed Verilog content structure */
        QVERIFY(verifyFileContent("detailed_stub.v", "/**"));
        QVERIFY(verifyFileContent("detailed_stub.v", "@file detailed_stub.v"));
        QVERIFY(verifyFileContent("detailed_stub.v", "@brief Verilog stub file"));
        QVERIFY(verifyFileContent("detailed_stub.v", "Auto-generated stub file"));

        /* Verify parameter documentation */
        QVERIFY(verifyFileContent("detailed_stub.v", "Parameters:"));
        QVERIFY(verifyFileContent("detailed_stub.v", "- DW: Data Width"));
        QVERIFY(verifyFileContent("detailed_stub.v", "- DP: Depth of Register File"));

        /* Verify port documentation */
        QVERIFY(verifyFileContent("detailed_stub.v", "/**< Write clock. */"));
        QVERIFY(verifyFileContent("detailed_stub.v", "/**< Read data. */"));

        /* Verify parameter declarations */
        QVERIFY(verifyFileContent("detailed_stub.v", "parameter DW = 8  /**< DW */"));
        QVERIFY(verifyFileContent("detailed_stub.v", "parameter DP = 4  /**< DP */"));

        /* Verify port declarations with comments */
        QVERIFY(verifyFileContent("detailed_stub.v", "input wclk    /**< Write clock. */"));
        QVERIFY(verifyFileContent("detailed_stub.v", "output [DW-1:0] rdata    /**< Read data. */"));
    }

    void testGenerateStubLibDetails()
    {
        messageList.clear();

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "generate",
               "stub",
               "-d",
               projectManager.getCurrentPath(),
               "-m",
               "simple_pll",
               "pll_lib_stub"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that stub files were generated */
        QVERIFY(verifyFileExists("pll_lib_stub.lib"));

        /* Verify detailed Liberty content structure */
        QVERIFY(verifyFileContent("pll_lib_stub.lib", "library (pll_lib_stub)"));
        QVERIFY(verifyFileContent("pll_lib_stub.lib", "technology (cmos)"));
        QVERIFY(verifyFileContent("pll_lib_stub.lib", "delay_model : table_lookup"));

        /* Verify operating conditions */
        QVERIFY(verifyFileContent("pll_lib_stub.lib", "operating_conditions(\"typical\")"));
        QVERIFY(verifyFileContent("pll_lib_stub.lib", "nom_voltage : 1.100"));

        /* Verify power pins */
        QVERIFY(verifyFileContent("pll_lib_stub.lib", "pg_pin(DVDD)"));
        QVERIFY(verifyFileContent("pll_lib_stub.lib", "pg_pin(DVSS)"));
        QVERIFY(verifyFileContent("pll_lib_stub.lib", "voltage_name : DVDD"));

        /* Verify cell structure */
        QVERIFY(verifyFileContent("pll_lib_stub.lib", "cell (simple_pll)"));
        QVERIFY(verifyFileContent("pll_lib_stub.lib", "area : 100"));
        QVERIFY(verifyFileContent("pll_lib_stub.lib", "dont_touch : true"));

        /* Verify pin declarations */
        QVERIFY(verifyFileContent("pll_lib_stub.lib", "pin(XIN)"));
        QVERIFY(verifyFileContent("pll_lib_stub.lib", "pin(CLK_OUT)"));
        QVERIFY(verifyFileContent("pll_lib_stub.lib", "capacitance : 0.02"));

        /* Verify bus declarations for multi-bit ports */
        QVERIFY(verifyFileContent("pll_lib_stub.lib", "bus(M)"));
        QVERIFY(verifyFileContent("pll_lib_stub.lib", "bus_type : \"DATA8B\""));
        QVERIFY(verifyFileContent("pll_lib_stub.lib", "pin (M[0])"));
        QVERIFY(verifyFileContent("pll_lib_stub.lib", "pin (M[7])"));
    }

    void testGenerateStubNoMatchingModules()
    {
        messageList.clear();

        QSocCliWorker     socCliWorker;
        const QStringList appArguments
            = {"qsoc",
               "generate",
               "stub",
               "-d",
               projectManager.getCurrentPath(),
               "-m",
               "nonexistent_.*",
               "empty_stub"};
        socCliWorker.setup(appArguments, false);
        socCliWorker.run();

        /* Verify that no stub files were generated */
        QVERIFY(!verifyFileExists("empty_stub.v"));
        QVERIFY(!verifyFileExists("empty_stub.lib"));

        /* Check for error message */
        bool foundNoModulesError = false;
        for (const QString &msg : messageList) {
            if (msg.contains("No modules found matching the specified criteria")) {
                foundNoModulesError = true;
                break;
            }
        }
        QVERIFY(foundNoModulesError);
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)

#include "test_qsoccliparsegeneratestub.moc"
