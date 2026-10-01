// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocgenerateprimitiveclock.h"
#include "common/qsocgenerateprimitivepower.h"
#include "common/qsocgenerateprimitivereset.h"
#include "qsoc_test.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {

QString cellText(const QString &name)
{
    if (name == "clock_cell.v")
        return QSocClockPrimitive().generateCellVerilog();
    if (name == "reset_cell.v")
        return QSocResetPrimitive().generateCellVerilog();
    return QSocPowerPrimitive().generateCellVerilog();
}

bool save(const QString &path, const QString &text)
{
    QFile      file(path);
    const auto bytes = text.toUtf8();
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

/* Run a tool to completion in dir, with its merged output in log. */
bool run(
    const QString     &tool,
    const QStringList &args,
    const QString     &dir,
    QString           *log,
    int                timeout = 120000)
{
    QProcess process;
    process.setWorkingDirectory(dir);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(tool, args);
    const bool finished = process.waitForFinished(timeout);
    *log                = QString::fromUtf8(process.readAll());
    return finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

/* Two div changes 12 ns apart: the second lands while the first is loading. */
const char *const backToBackBench = R"(`timescale 1ns / 1ps
module tb;
    reg        clk   = 1'b0;
    reg        rst_n = 1'b0;
    reg  [3:0] div   = 4'd4;
    wire       clk_out;
    always #1 clk = ~clk;
    qsoc_clk_div #(.WIDTH(4), .DEFAULT_VAL(4), .AUTO_UPDATE(1'b1)) dut (
        .clk(clk), .rst_n(rst_n), .en(1'b1), .test_en(1'b0), .div(div),
        .div_valid(1'b0), .div_ready(), .clk_out(clk_out), .count());
    initial begin
        #10 rst_n = 1'b1;
        #40 div   = 4'd8;
        #12 div   = 4'd3;
        #400 $display("div_q=%0d", dut.div_q);
        $finish;
    end
endmodule
)";

class Test : public QObject
{
    Q_OBJECT

private slots:
    void verilog2005_data()
    {
        QTest::addColumn<QString>("cell");
        QTest::addColumn<QString>("tool");
        for (const char *cell : {"clock_cell.v", "reset_cell.v", "power_cell.v"}) {
            for (const char *tool : {"iverilog", "yosys"})
                QTest::newRow(qPrintable(QString("%1-%2").arg(cell, tool))) << cell << tool;
        }
    }

    /* Cells are plain Verilog-2005: no SystemVerilog-only construct may slip in. */
    void verilog2005()
    {
        QFETCH(QString, cell);
        QFETCH(QString, tool);
        if (QStandardPaths::findExecutable(tool).isEmpty())
            QSOC_TEST_MISSING_DEPENDENCY(tool);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(save(dir.filePath(cell), cellText(cell)));
        const QStringList args
            = tool == "iverilog"
                  ? QStringList{"-g2005", "-o", "a.out", cell}
                  : QStringList{"-q", "-p", "read_verilog " + cell + "; hierarchy; proc"};
        QString log;
        QVERIFY2(run(tool, args, dir.path(), &log), qPrintable(log));
    }

    void countBelowOneFailsElaboration_data()
    {
        QTest::addColumn<QString>("tool");
        QTest::addColumn<QString>("cell");
        QTest::addColumn<QString>("module");
        QTest::addColumn<QString>("parameter");
        const struct
        {
            const char *cell;
            const char *module;
            const char *parameter;
        } counts[] = {
            {"reset_cell.v", "qsoc_rst_sync", "STAGE"},
            {"reset_cell.v", "qsoc_rst_pipe", "STAGE"},
            {"reset_cell.v", "qsoc_rst_count", "CYCLE"},
            {"power_cell.v", "qsoc_power_rst_sync", "STAGE"},
            {"clock_cell.v", "qsoc_clk_mux_gf", "NUM_INPUTS"},
            {"clock_cell.v", "qsoc_clk_mux_gf", "NUM_SYNC_STAGES"},
            {"clock_cell.v", "qsoc_clk_mux_raw", "NUM_INPUTS"},
            {"clock_cell.v", "qsoc_clk_or_tree", "INPUT_COUNT"},
        };
        for (const char *tool : {"iverilog", "yosys"}) {
            for (const auto &count : counts) {
                QTest::newRow(qPrintable(QString("%1-%2-%3").arg(tool, count.module, count.parameter)))
                    << tool << count.cell << count.module << count.parameter;
            }
        }
    }

    /* A stage or cycle count below one is an elaboration error, never clamped to one. */
    void countBelowOneFailsElaboration()
    {
        QFETCH(QString, tool);
        QFETCH(QString, cell);
        QFETCH(QString, module);
        QFETCH(QString, parameter);
        if (QStandardPaths::findExecutable(tool).isEmpty())
            QSOC_TEST_MISSING_DEPENDENCY(tool);

        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(save(dir.filePath(cell), cellText(cell)));
        for (const int value : {0, 1}) {
            QVERIFY(save(
                dir.filePath("top.v"),
                QString("module top;\n    %1 #(.%2(%3)) u_dut ();\nendmodule\n")
                    .arg(module, parameter)
                    .arg(value)));
            const QStringList args
                = tool == "iverilog"
                      ? QStringList{"-g2005", "-s", "top", "-o", "a.out", "top.v", cell}
                      : QStringList{
                            "-q",
                            "-p",
                            "read_verilog top.v " + cell + "; hierarchy -check -top top"};
            QString log;
            QVERIFY2(run(tool, args, dir.path(), &log) == (value == 1), qPrintable(log));
            if (value == 0)
                QVERIFY2(log.contains("qsoc_param_error_"), qPrintable(log));
        }
    }

    /* The last div written is the one loaded, even when it arrives mid-load. */
    void autoUpdateKeepsLastChange()
    {
        for (const char *tool : {"iverilog", "vvp"}) {
            if (QStandardPaths::findExecutable(tool).isEmpty())
                QSOC_TEST_MISSING_DEPENDENCY(tool);
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(save(dir.filePath("clock_cell.v"), cellText("clock_cell.v")));
        QVERIFY(save(dir.filePath("tb.v"), backToBackBench));
        QString log;
        QVERIFY2(
            run("iverilog", {"-g2005", "-o", "sim", "tb.v", "clock_cell.v"}, dir.path(), &log),
            qPrintable(log));
        QVERIFY2(run("vvp", {"-n", "sim"}, dir.path(), &log), qPrintable(log));
        QVERIFY2(log.contains("div_q=3"), qPrintable(log));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocprimitivecell.moc"
