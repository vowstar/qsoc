// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

#include "common/qstaticmarkdown.h"

#include <QtTest>

class TestQStaticMarkdown : public QObject
{
    Q_OBJECT

private slots:
    /* padText */
    void padText_leftAlign();
    void padText_centerAlign();
    void padText_rightAlign();
    void padText_nopadding();
    void padText_emptyString();

    /* renderTable - basic */
    void renderTable_singleRow();
    void renderTable_multipleRows();
    void renderTable_emptyData();
    void renderTable_singleColumn();

    /* renderTable - alignment */
    void renderTable_leftAlignment();
    void renderTable_centerAlignment();
    void renderTable_rightAlignment();

    /* renderTable - edge cases */
    void renderTable_unevenRowLengths();
    void renderTable_withEmptyCells();
    void renderTable_longContent();
};

/* padText */

void TestQStaticMarkdown::padText_leftAlign()
{
    QString result = QStaticMarkdown::padText("test", 10, QStaticMarkdown::Alignment::Left);
    QCOMPARE(result, QString("test      ")); /* 4 chars + 6 spaces */
    QCOMPARE(result.length(), 10);
}

void TestQStaticMarkdown::padText_centerAlign()
{
    QString result = QStaticMarkdown::padText("test", 10, QStaticMarkdown::Alignment::Center);
    /* 3 left + "test" + 3 right = 10 */
    QCOMPARE(result, QString("   test   "));
    QCOMPARE(result.length(), 10);
}

void TestQStaticMarkdown::padText_rightAlign()
{
    QString result = QStaticMarkdown::padText("test", 10, QStaticMarkdown::Alignment::Right);
    QCOMPARE(result, QString("      test")); /* 6 spaces + 4 chars */
    QCOMPARE(result.length(), 10);
}

void TestQStaticMarkdown::padText_nopadding()
{
    /* Text exactly matches width */
    QString result = QStaticMarkdown::padText("test", 4, QStaticMarkdown::Alignment::Left);
    QCOMPARE(result, QString("test"));
    QCOMPARE(result.length(), 4);
}

void TestQStaticMarkdown::padText_emptyString()
{
    QString result = QStaticMarkdown::padText("", 5, QStaticMarkdown::Alignment::Left);
    QCOMPARE(result, QString("     ")); /* 5 spaces */
    QCOMPARE(result.length(), 5);
}

/* renderTable - basic */

void TestQStaticMarkdown::renderTable_singleRow()
{
    QStringList          headers = {"Name", "Value"};
    QVector<QStringList> rows    = {{"test", "123"}};

    QString table = QStaticMarkdown::renderTable(headers, rows);

    /* Should contain header row */
    QVERIFY(table.contains("Name"));
    QVERIFY(table.contains("Value"));

    /* Should contain data row */
    QVERIFY(table.contains("test"));
    QVERIFY(table.contains("123"));

    /* Should have separator line with colons for left alignment */
    QVERIFY(table.contains("|:"));

    /* Should have 3 lines: header, separator, data */
    QCOMPARE(table.count('\n'), 3);
}

void TestQStaticMarkdown::renderTable_multipleRows()
{
    QStringList          headers = {"Signal", "Width", "Type"};
    QVector<QStringList> rows
        = {{"clk", "1", "input"}, {"data", "32", "output"}, {"valid", "1", "output"}};

    QString table = QStaticMarkdown::renderTable(headers, rows);

    /* Should contain all data */
    QVERIFY(table.contains("clk"));
    QVERIFY(table.contains("data"));
    QVERIFY(table.contains("valid"));
    QVERIFY(table.contains("32"));

    /* Should have 5 lines: header, separator, 3 data rows */
    QCOMPARE(table.count('\n'), 5);
}

void TestQStaticMarkdown::renderTable_emptyData()
{
    QStringList          headers = {"Column1", "Column2"};
    QVector<QStringList> rows; /* Empty rows */

    QString table = QStaticMarkdown::renderTable(headers, rows);

    /* Should still have header and separator */
    QVERIFY(table.contains("Column1"));
    QVERIFY(table.contains("Column2"));
    QVERIFY(table.contains("|:"));

    /* Should have 2 lines: header, separator */
    QCOMPARE(table.count('\n'), 2);
}

void TestQStaticMarkdown::renderTable_singleColumn()
{
    QStringList          headers = {"Items"};
    QVector<QStringList> rows    = {{"apple"}, {"banana"}, {"cherry"}};

    QString table = QStaticMarkdown::renderTable(headers, rows);

    QVERIFY(table.contains("Items"));
    QVERIFY(table.contains("apple"));
    QVERIFY(table.contains("banana"));
    QVERIFY(table.contains("cherry"));

    /* Should have 5 lines: header, separator, 3 data rows */
    QCOMPARE(table.count('\n'), 5);
}

/* renderTable - alignment */

void TestQStaticMarkdown::renderTable_leftAlignment()
{
    QStringList          headers = {"A", "B"};
    QVector<QStringList> rows    = {{"1", "2"}};

    QString table = QStaticMarkdown::renderTable(headers, rows, QStaticMarkdown::Alignment::Left);

    /* Left alignment: separator should start with |: */
    QVERIFY(table.contains("|:"));

    /* Right alignment marker should NOT be present */
    QStringList lines = table.split('\n');
    QVERIFY(lines.size() >= 2);
    QString separatorLine = lines[1];
    /* Left aligned columns should not end with : before the pipe */
    QVERIFY(!separatorLine.contains(":|"));
}

void TestQStaticMarkdown::renderTable_centerAlignment()
{
    QStringList          headers = {"A", "B"};
    QVector<QStringList> rows    = {{"1", "2"}};

    QString table = QStaticMarkdown::renderTable(headers, rows, QStaticMarkdown::Alignment::Center);

    /* Center alignment: separator should have |:...: pattern */
    QStringList lines = table.split('\n');
    QVERIFY(lines.size() >= 2);
    QString separatorLine = lines[1];

    /* Center aligned columns have colons on both sides */
    QVERIFY(separatorLine.contains("|:"));
    QVERIFY(separatorLine.contains(":|"));
}

void TestQStaticMarkdown::renderTable_rightAlignment()
{
    QStringList          headers = {"A", "B"};
    QVector<QStringList> rows    = {{"1", "2"}};

    QString table = QStaticMarkdown::renderTable(headers, rows, QStaticMarkdown::Alignment::Right);

    /* Right alignment: separator should end with : before pipes */
    QStringList lines = table.split('\n');
    QVERIFY(lines.size() >= 2);
    QString separatorLine = lines[1];

    /* Right aligned columns should end with : before the pipe */
    QVERIFY(separatorLine.contains(":|"));
}

/* renderTable - edge cases */

void TestQStaticMarkdown::renderTable_unevenRowLengths()
{
    QStringList          headers = {"Col1", "Col2", "Col3"};
    QVector<QStringList> rows    = {
        {"a", "b", "c"},     /* Full row */
        {"x", "y"},          /* Short row */
        {"p", "q", "r", "s"} /* Long row (should be truncated) */
    };

    QString table = QStaticMarkdown::renderTable(headers, rows);

    /* Should handle all rows without crashing */
    QVERIFY(table.contains("a"));
    QVERIFY(table.contains("x"));
    QVERIFY(table.contains("p"));

    /* Should have 5 lines: header, separator, 3 data rows */
    QCOMPARE(table.count('\n'), 5);
}

void TestQStaticMarkdown::renderTable_withEmptyCells()
{
    QStringList          headers = {"Name", "Value"};
    QVector<QStringList> rows
        = {{"test", ""}, /* Empty value */
           {"", "123"},  /* Empty name */
           {"valid", "456"}};

    QString table = QStaticMarkdown::renderTable(headers, rows);

    /* Should handle empty cells gracefully */
    QVERIFY(table.contains("test"));
    QVERIFY(table.contains("123"));
    QVERIFY(table.contains("valid"));

    /* Should have proper number of lines */
    QCOMPARE(table.count('\n'), 5);
}

void TestQStaticMarkdown::renderTable_longContent()
{
    QStringList          headers = {"Short", "Very Long Header Name"};
    QVector<QStringList> rows
        = {{"a", "x"}, {"tiny", "This is a very long content that exceeds header width"}};

    QString table = QStaticMarkdown::renderTable(headers, rows);

    /* Should handle varying content lengths */
    QVERIFY(table.contains("Short"));
    QVERIFY(table.contains("Very Long Header Name"));
    QVERIFY(table.contains("This is a very long content"));

    /* Column width should accommodate the longest content */
    QStringList lines = table.split('\n');
    QVERIFY(lines.size() >= 4);

    /* All rows should have the same structure (same number of | characters) */
    int pipeCount = lines[0].count('|');
    for (int i = 1; i < 4; ++i) {
        QCOMPARE(lines[i].count('|'), pipeCount);
    }
}

QTEST_APPLESS_MAIN(TestQStaticMarkdown)
#include "test_qsoccommonqstaticmarkdown.moc"
