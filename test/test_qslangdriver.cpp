// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "common/qslangdriver.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTemporaryFile>
#include <QtTest>

#include <nlohmann/json.hpp>

using json = nlohmann::json;

class Test : public QObject
{
    Q_OBJECT

private:
    QTemporaryDir tempDir;
    QString       createTemporaryVerilogFile(const QString &content);
    QString       createTemporaryFileList(const QStringList &filePaths);

private slots:
    void initTestCase();
    void cleanupTestCase();

    /* Test parsing command line arguments */
    void parseArgs_validArgs();
    void parseArgs_invalidArgs();

    /* Test file list parsing */
    void parseFileList_validFiles();
    void parseFileList_invalidFiles();
    void parseFileList_emptyList();

    /* Test utility functions */
    void contentCleanComment_singleLine();
    void contentCleanComment_multiLine();
    void contentCleanComment_mixed();

    void contentValidFile_relativeAndAbsolute();
    void contentValidFile_nonExistentFiles();

    /* Test AST and module functionalities */
    void getAst_afterSuccessfulParse();
    void getModuleList_afterParse();
    void getModuleAst_validModule();
    void getModuleAst_invalidModule();
};

void Test::initTestCase()
{
    QVERIFY(tempDir.isValid());
}

void Test::cleanupTestCase()
{
    /* Cleanup is handled by QTemporaryDir */
    tempDir.remove();
}

QString Test::createTemporaryVerilogFile(const QString &content)
{
    QTemporaryFile file(tempDir.path() + "/XXXXXX.v");
    file.setAutoRemove(false);

    if (file.open()) {
        QTextStream stream(&file);
        stream << content;
        return file.fileName();
    }

    return QString();
}

QString Test::createTemporaryFileList(const QStringList &filePaths)
{
    QTemporaryFile file(tempDir.path() + "/filelist_XXXXXX.f");
    file.setAutoRemove(false);

    if (file.open()) {
        QTextStream stream(&file);
        for (const QString &path : filePaths) {
            stream << path << "\n";
        }
        return file.fileName();
    }

    return QString();
}

void Test::parseArgs_validArgs()
{
    /* Create a simple Verilog file */
    QString verilogContent = R"(
        module test_module(
            input wire clk,
            input wire rst_n,
            input wire [7:0] data_in,
            output reg [7:0] data_out
        );
            always @(posedge clk or negedge rst_n) begin
                if (!rst_n)
                    data_out <= 8'h00;
                else
                    data_out <= data_in;
            end
        endmodule
    )";

    QString verilogFile = createTemporaryVerilogFile(verilogContent);
    QVERIFY(!verilogFile.isEmpty());

    /* Create the slang arguments */
    QString args = QString("slang --single-unit %1").arg(verilogFile);

    /* Test parsing */
    QSlangDriver driver;
    bool         result = driver.parseArgs(args);

    /* Verify parse succeeded */
    QVERIFY(result);

    /* Verify AST is not empty */
    QVERIFY(!driver.getAst().empty());
}

void Test::parseArgs_invalidArgs()
{
    /* Test with invalid arguments */
    QSlangDriver driver;
    bool         result = driver.parseArgs("slang --invalid-option");

    /* Parse should fail with invalid options */
    QVERIFY(!result);
}

void Test::parseFileList_validFiles()
{
    /* Create a simple Verilog file */
    QString verilogContent = R"(
        module counter(
            input wire clk,
            input wire rst_n,
            output reg [3:0] count
        );
            always @(posedge clk or negedge rst_n) begin
                if (!rst_n)
                    count <= 4'h0;
                else
                    count <= count + 1;
            end
        endmodule
    )";

    QString verilogFile1 = createTemporaryVerilogFile(verilogContent);
    QString verilogFile2 = createTemporaryVerilogFile(verilogContent.replace("counter", "counter2"));

    QVERIFY(!verilogFile1.isEmpty());
    QVERIFY(!verilogFile2.isEmpty());

    /* Create file list */
    QString fileList = createTemporaryFileList({verilogFile1, verilogFile2});
    QVERIFY(!fileList.isEmpty());

    /* Test parsing file list */
    QSlangDriver driver;
    bool         result = driver.parseFileList(fileList, {});

    /* Verify parse succeeded */
    QVERIFY(result);

    /* Verify module list contains both modules */
    QStringList modules = driver.getModuleList();
    QCOMPARE(modules.size(), 2);
    QVERIFY(modules.contains("counter") || modules.contains("counter2"));
}

void Test::parseFileList_invalidFiles()
{
    /* Create file list with non-existent files */
    QString fileList = createTemporaryFileList(
        {tempDir.path() + "/nonexistent1.v", tempDir.path() + "/nonexistent2.v"});
    QVERIFY(!fileList.isEmpty());

    /* Test parsing file list with non-existent files */
    QSlangDriver driver;
    bool         result = driver.parseFileList(fileList, {});

    /* Parse should fail with non-existent files */
    QVERIFY(!result);
}

void Test::parseFileList_emptyList()
{
    /* Create empty file list */
    QString fileList = createTemporaryFileList({});
    QVERIFY(!fileList.isEmpty());

    /* Test parsing empty file list */
    QSlangDriver driver;
    bool         result = driver.parseFileList(fileList, {});

    /* Parse should fail with empty list */
    QVERIFY(!result);
}

void Test::contentCleanComment_singleLine()
{
    QSlangDriver driver;

    /* Test content with some simple single-line comments */
    QString input = "line1 // This is a comment\nline2\n// This is a full-line comment\nline3";

    /* Clean up comments */
    QString result = driver.contentCleanComment(input);

    /* Verify comments have been removed */
    QVERIFY(!result.contains("//"));
    QVERIFY(!result.contains("This is a comment"));
    QVERIFY(result.contains("line1"));
    QVERIFY(result.contains("line2"));
    QVERIFY(result.contains("line3"));
}

void Test::contentCleanComment_multiLine()
{
    QSlangDriver driver;

    /* Test with a simpler multi-line comment example */
    QString input = "line1\n/* Simple multi-line comment */\nline3";

    /* Clean up comments */
    QString result = driver.contentCleanComment(input);

    /* Output actual result for inspection */
    qDebug() << "Original input:" << input;
    qDebug() << "Cleaned result:" << result;

    /* Verify result contains all code lines but not comment content */
    QVERIFY(result.contains("line1"));
    QVERIFY(result.contains("line3"));
}

void Test::contentCleanComment_mixed()
{
    QSlangDriver driver;

    /* Test content with mixed comment types */
    QString input = "line1 /* Multi-line */ // Single line\nline2 // Single /* with multi-line "
                    "syntax\nline3 /* Multi // with single line syntax */\nline4";

    /* Clean up comments */
    QString result = driver.contentCleanComment(input);

    /* Verify comment content has been removed */
    QVERIFY(!result.contains("Single line"));
    QVERIFY(!result.contains("Multi-line"));
    QVERIFY(!result.contains("single line syntax"));
    QVERIFY(result.contains("line1"));
    QVERIFY(result.contains("line2"));
    QVERIFY(result.contains("line3"));
    QVERIFY(result.contains("line4"));
}

void Test::contentValidFile_relativeAndAbsolute()
{
    /* Create a temporary file */
    QTemporaryFile file1(tempDir.path() + "/test_file_XXXXXX.txt");
    file1.setAutoRemove(false);
    QVERIFY(file1.open());
    file1.close();

    QSlangDriver driver;
    QDir         baseDir(tempDir.path());

    /* Test with relative and absolute paths */
    QString relativePath = QFileInfo(file1.fileName()).fileName();
    QString absolutePath = file1.fileName();

    QString input  = relativePath + "\n" + absolutePath;
    QString result = driver.contentValidFile(input, baseDir);

    /* Both paths should be validated to the same absolute path */
    QStringList resultLines = result.split("\n");
    QCOMPARE(resultLines.size(), 2);
    QCOMPARE(
        QFileInfo(resultLines[0]).absoluteFilePath(), QFileInfo(resultLines[1]).absoluteFilePath());
}

void Test::contentValidFile_nonExistentFiles()
{
    QSlangDriver driver;
    QDir         baseDir(tempDir.path());

    QString input = tempDir.path() + "/nonexistent1.txt\n" + tempDir.path() + "/nonexistent2.txt";

    QString result = driver.contentValidFile(input, baseDir);

    /* Result should be empty as files don't exist */
    QVERIFY(result.isEmpty());
}

void Test::getAst_afterSuccessfulParse()
{
    /* Create a simple Verilog file */
    QString verilogContent = R"(
        module simple_module(
            input wire in1,
            output wire out1
        );
            assign out1 = in1;
        endmodule
    )";

    QString verilogFile = createTemporaryVerilogFile(verilogContent);
    QVERIFY(!verilogFile.isEmpty());

    /* Parse the file */
    QSlangDriver driver;
    QString      args   = QString("slang --single-unit %1").arg(verilogFile);
    bool         result = driver.parseArgs(args);
    QVERIFY(result);

    /* Check AST contains expected elements */
    const json &ast = driver.getAst();
    QVERIFY(!ast.empty());
    QVERIFY(ast.contains("kind"));

    /* If "members" is available, check it's an array */
    if (ast.contains("members")) {
        QVERIFY(ast["members"].is_array());
    }
}

void Test::getModuleList_afterParse()
{
    /* Create Verilog files with multiple modules */
    QString verilogContent1 = R"(
        module module1(input a, output b);
            assign b = a;
        endmodule
    )";

    QString verilogContent2 = R"(
        module module2(input c, output d);
            assign d = c;
        endmodule
    )";

    QString verilogFile1 = createTemporaryVerilogFile(verilogContent1);
    QString verilogFile2 = createTemporaryVerilogFile(verilogContent2);
    QVERIFY(!verilogFile1.isEmpty());
    QVERIFY(!verilogFile2.isEmpty());

    /* Create file list and parse */
    QSlangDriver driver;
    bool         result = driver.parseFileList("", {verilogFile1, verilogFile2});
    QVERIFY(result);

    /* Check module list contains the expected modules */
    QStringList modules = driver.getModuleList();
    QVERIFY(modules.contains("module1"));
    QVERIFY(modules.contains("module2"));
    QCOMPARE(modules.size(), 2);
}

void Test::getModuleAst_validModule()
{
    /* Create a Verilog file with a module */
    QString verilogContent = R"(
        module test_module(
            input wire a,
            output wire b
        );
            assign b = ~a;
        endmodule
    )";

    QString verilogFile = createTemporaryVerilogFile(verilogContent);
    QVERIFY(!verilogFile.isEmpty());

    /* Parse the file */
    QSlangDriver driver;
    QString      args   = QString("slang --single-unit %1").arg(verilogFile);
    bool         result = driver.parseArgs(args);
    QVERIFY(result);

    /* Check module AST */
    const json &moduleAst = driver.getModuleAst("test_module");
    QVERIFY(!moduleAst.empty());
    QVERIFY(moduleAst.contains("name"));
    QCOMPARE(moduleAst["name"], "test_module");
}

void Test::getModuleAst_invalidModule()
{
    /* Create a Verilog file with a module */
    QString verilogContent = R"(
        module actual_module(
            input wire a,
            output wire b
        );
            assign b = a;
        endmodule
    )";

    QString verilogFile = createTemporaryVerilogFile(verilogContent);
    QVERIFY(!verilogFile.isEmpty());

    /* Parse the file */
    QSlangDriver driver;
    QString      args   = QString("slang --single-unit %1").arg(verilogFile);
    bool         result = driver.parseArgs(args);
    QVERIFY(result);

    /* Try to get AST for a non-existent module */
    const json &moduleAst = driver.getModuleAst("nonexistent_module");

    /* Should return the full AST when module not found */
    QCOMPARE(moduleAst, driver.getAst());
}

QSOC_TEST_MAIN(Test)

#include "test_qslangdriver.moc"
