// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoccellformal.h"
#include "common/qsoccelllibrary.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QMap>
#include <QProcess>
#include <QSet>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {

/* Every cell and role file of the unit, by file name. */
QMap<QString, QString> library()
{
    QMap<QString, QString> files;
    for (const QSocCellLibrary::Cell &cell : QSocCellLibrary::cells())
        files.insert(cell.file, cell.text);
    return files;
}

QString cellText(const QString &name)
{
    return library().value(name);
}

QStringList roleFiles()
{
    QStringList files;
    for (const QSocCellLibrary::Cell &role : QSocCellLibrary::roles())
        files.append(role.file);
    return files;
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

/* Files in root and their formal collateral in root/cell/formal. */
bool writeFormal(const QString &root, const QMap<QString, QString> &files)
{
    if (!QDir(root).mkpath("cell/formal"))
        return false;
    QStringList sources;
    for (auto file = files.cbegin(); file != files.cend(); ++file) {
        if (!save(QDir(root).filePath(file.key()), file.value()))
            return false;
        sources.append("../../" + file.key());
    }
    const auto formal = QSocCellFormal::generate(sources);
    for (auto file = formal.cbegin(); file != formal.cend(); ++file) {
        if (!save(QDir(root).filePath("cell/formal/" + file.key()), file.value()))
            return false;
    }
    return !formal.isEmpty();
}

/* Run one task of the cell job; returns the status sby recorded. */
QString sbyStatus(const QString &root, const QString &task, QString *log)
{
    const QString dir = QDir(root).filePath("cell/formal");
    run("sby", {"-f", "check.sby", task}, dir, log, 1800000);
    QFile status(QDir(dir).filePath("check_" + task + "/status"));
    if (!status.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(status.readAll()).section(' ', 0, 0).trimmed();
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
        for (const QString &cell : library().keys()) {
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
        QStringList files = roleFiles();
        files.removeAll(cell);
        files.append(cell);
        for (const QString &file : files)
            QVERIFY(save(dir.filePath(file), cellText(file)));
        const QStringList args
            = tool == "iverilog"
                  ? QStringList{"-g2005", "-o", "a.out"} + files
                  : QStringList{"-q", "-p", "read_verilog " + files.join(' ') + "; hierarchy; proc"};
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
            {"qsoc_cell_reset.v", "qsoc_rst_sync", "STAGE"},
            {"qsoc_cell_reset.v", "qsoc_rst_pipe", "STAGE"},
            {"qsoc_cell_reset.v", "qsoc_rst_count", "CYCLE"},
            {"qsoc_cell_power.v", "qsoc_power_rst_sync", "STAGE"},
            {"qsoc_cell_clock.v", "qsoc_clk_mux_gf", "NUM_INPUTS"},
            {"qsoc_cell_clock.v", "qsoc_clk_mux_gf", "NUM_SYNC_STAGES"},
            {"qsoc_cell_clock.v", "qsoc_clk_mux_raw", "NUM_INPUTS"},
            {"qsoc_cell_clock.v", "qsoc_clk_or_tree", "INPUT_COUNT"},
            {"qsoc_sync.v", "qsoc_sync", "STAGES"},
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
        QStringList files = roleFiles();
        files.removeAll(cell);
        files.append(cell);
        for (const QString &file : files)
            QVERIFY(save(dir.filePath(file), cellText(file)));
        for (const int value : {0, 1}) {
            QVERIFY(save(
                dir.filePath("top.v"),
                QString("module top;\n    %1 #(.%2(%3)) u_dut ();\nendmodule\n")
                    .arg(module, parameter)
                    .arg(value)));
            const QStringList args = tool == "iverilog"
                                         ? QStringList{"-g2005", "-s", "top", "-o", "a.out", "top.v"}
                                               + files
                                         : QStringList{
                                               "-q",
                                               "-p",
                                               "read_verilog top.v " + files.join(' ')
                                                   + "; hierarchy -check -top top"};
            QString log;
            QVERIFY2(run(tool, args, dir.path(), &log) == (value == 1), qPrintable(log));
            if (value == 0)
                QVERIFY2(log.contains("qsoc_param_error_"), qPrintable(log));
        }
    }

    void formal_data()
    {
        QTest::addColumn<QString>("task");
        for (const QString &task : QSocCellFormal::tasks(library().keys()))
            QTest::newRow(qPrintable(task)) << task;
    }

    /* Every cell job task proves, or reaches all its covers. */
    void formal()
    {
        QFETCH(QString, task);
        for (const char *tool : {"sby", "yosys", "yosys-abc", "z3"}) {
            if (QStandardPaths::findExecutable(tool).isEmpty())
                QSOC_TEST_MISSING_DEPENDENCY(tool);
        }
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeFormal(dir.path(), library()));
        QString log;
        QCOMPARE(sbyStatus(dir.path(), task, &log), QString("PASS"));
    }

    /* Cover mode checks asserts only up to the depth where its covers are
     * reached, so each cover task needs a prove task of the same setup. */
    void coverTaskHasProveTwin()
    {
        const QString job = QSocCellFormal::generate(library().keys()).value("check.sby");
        QVERIFY(!job.isEmpty());
        const QStringList      lines = job.split('\n');
        QMap<QString, QString> modes;
        for (qsizetype i = lines.indexOf("[tasks]") + 1; i > 0 && i < lines.size(); ++i) {
            const QStringList words = lines.at(i).split(' ', Qt::SkipEmptyParts);
            if (words.size() != 2)
                break;
            modes.insert(words.at(0), words.at(1));
        }
        QMap<QString, QStringList> setup;
        for (const QString &line : lines) {
            const QStringList words = line.split(' ', Qt::SkipEmptyParts);
            if (words.size() < 2 || !words.at(0).endsWith(':'))
                continue;
            const QString task = words.at(0).chopped(1);
            if (words.at(1) == "prep" && words.size() == 4)
                setup[task].append("top " + words.at(3));
            for (qsizetype i = 2; words.at(1) == "chparam" && i + 2 < words.size(); i += 3)
                setup[task].append(words.at(i + 1) + " " + words.at(i + 2));
        }
        QVERIFY(modes.values().contains("cover"));
        QMap<QString, QString> keys;
        QSet<QString>          proven;
        for (auto task = modes.cbegin(); task != modes.cend(); ++task) {
            QStringList key = setup.value(task.key());
            QVERIFY2(!key.isEmpty(), qPrintable(task.key()));
            key.sort();
            keys.insert(task.key(), key.join(','));
            if (task.value() == "prove")
                proven.insert(key.join(','));
        }
        for (auto task = modes.cbegin(); task != modes.cend(); ++task) {
            if (task.value() == "cover")
                QVERIFY2(proven.contains(keys.value(task.key())), qPrintable(task.key()));
        }
    }

    void formalCatchesFaults_data()
    {
        QTest::addColumn<QString>("cell");
        QTest::addColumn<QString>("task");
        QTest::addColumn<QString>("from");
        QTest::addColumn<QString>("to");
        QTest::newRow("div-gate-from-registered-enable")
            << "qsoc_cell_clock.v" << "clk_div_explicit"
            << ".en((gate_en_q & en) | reset_open),"
            << ".en(gate_is_open_q | reset_open),";
        QTest::newRow("div-load-while-gate-open")
            << "qsoc_cell_clock.v" << "clk_div_explicit"
            << "if ((gate_is_open_q == 1'b0) || clk_div_bypass_en_q) begin"
            << "if (1'b1) begin";
        QTest::newRow("div-no-falling-edge-flop") << "qsoc_cell_clock.v" << "clk_div_explicit"
                                                  << "t_ff2_q = !t_ff2_q;"
                                                  << "t_ff2_q = t_ff2_q;";
        QTest::newRow("div-reset-async") << "qsoc_cell_clock.v" << "clk_div_reset_clock"
                                         << "        .rst_n(1'b1),\n"
                                            "        .d    (rst_n),\n"
                                            "        .q    (rst_q_n)"
                                         << "        .rst_n(rst_n),\n"
                                            "        .d    (1'b1),\n"
                                            "        .q    (rst_q_n)";
        QTest::newRow("div-update-as-pulse") << "qsoc_cell_clock.v" << "clk_div_auto_live"
                                             << "load_req_q <= (div_stable_normalized != div_q);"
                                             << "load_req_q <= (div_stable != div_last);";
        QTest::newRow("div-auto-no-filter") << "qsoc_cell_clock.v" << "clk_div_skew"
                                            << "if (div_sync == div_last) div_stable <= div_sync;"
                                            << "div_stable <= div_sync;";
        QTest::newRow("div-auto-loads-unfiltered") << "qsoc_cell_clock.v" << "clk_div_skew"
                                                   << "assign div_src  = div_stable;"
                                                   << "assign div_src  = div_sync;";
        QTest::newRow("mux-gf-no-exclusion") << "qsoc_cell_clock.v" << "clk_mux_gf"
                                             << "= sel_onehot[i] & &(clock_disabled_q | ONEHOT_I);"
                                             << "= sel_onehot[i];";
        QTest::newRow("mux-gf-reset-one-stage") << "qsoc_cell_clock.v" << "clk_mux_gf"
                                                << "            .STAGES(2),\n"
                                                   "            .RESET_VALUE(1'b0)\n"
                                                   "        ) u_reset_sync ("
                                                << "            .STAGES(1),\n"
                                                   "            .RESET_VALUE(1'b0)\n"
                                                   "        ) u_reset_sync (";
        QTest::newRow("mux-gf-reset-opens-all") << "qsoc_cell_clock.v" << "clk_mux_gf_reset_clock"
                                                << "(reset_hold & sel_sync)"
                                                << "(reset_hold)";
        QTest::newRow("mux-raw-half-split") << "qsoc_cell_clock.v" << "clk_mux_raw_3"
                                            << "localparam integer HALF = 1 << (WIDTH - 1);"
                                            << "localparam integer HALF = NUM_INPUTS / 2;";
        QTest::newRow("or-tree-drops-input") << "qsoc_cell_clock.v" << "clk_or_tree_5"
                                             << ".clk_in1(clk_in[1]),"
                                             << ".clk_in1(1'b0),";
        QTest::newRow("gate-latch-open-high") << "qsoc_ck_icg_pos.v" << "clk_gate_pos"
                                              << "if (!clk) iq = (test_en | en);"
                                              << "if (clk) iq = (test_en | en);";
        QTest::newRow("gate-reset-open-missing")
            << "qsoc_cell_clock.v" << "clk_gate_pos_reset_clock"
            << ".en     (en | reset_open),"
            << ".en     (en),";
        QTest::newRow("gate-reset-open-async") << "qsoc_cell_clock.v" << "clk_gate_pos_reset_clock"
                                               << ".en     (en | reset_open),"
                                               << ".en     (en | reset_open | ~rst_n),";
        QTest::newRow("gate-reset-open-one-stage")
            << "qsoc_cell_clock.v" << "clk_gate_pos_reset_clock"
            << "                .STAGES(2),\n"
               "                .RESET_VALUE(1'b0)\n"
               "            ) u_reset_sync ("
            << "                .STAGES(1),\n"
               "                .RESET_VALUE(1'b0)\n"
               "            ) u_reset_sync (";
        QTest::newRow("xor-as-or") << "qsoc_ck_xor2.v" << "clk_role"
                                   << "assign clk_out = clk_in0 ^ clk_in1;"
                                   << "assign clk_out = clk_in0 | clk_in1;";
        QTest::newRow("rst-sync-short") << "qsoc_cell_reset.v" << "rst_sync_3"
                                        << ".STAGES(STAGE),"
                                        << ".STAGES(STAGE - 1),";
        QTest::newRow("rst-pipe-short") << "qsoc_cell_reset.v" << "rst_pipe_3"
                                        << "assign core_rst_n = pipe_reg[STAGE-1];"
                                        << "assign core_rst_n = pipe_reg[STAGE-2];";
        QTest::newRow("rst-count-short") << "qsoc_cell_reset.v" << "rst_count_5"
                                         << "C_M1 = CYCLE - 3;"
                                         << "C_M1 = CYCLE - 4;";
        QTest::newRow("rst-count-one-flop") << "qsoc_cell_reset.v" << "rst_count_1"
                                            << "        .STAGES(2),\n"
                                               "        .RESET_VALUE(1'b0)\n"
                                               "    ) u_sync ("
                                            << "        .STAGES(1),\n"
                                               "        .RESET_VALUE(1'b0)\n"
                                               "    ) u_sync (";
        QTest::newRow("power-stale-off-timer") << "qsoc_cell_power.v" << "power_fsm"
                                               << "            state_n = S_TURN_OFF;\n"
                                                  "            ld_off  = 1'b1;"
                                               << "            state_n = S_TURN_OFF;";
        QTest::newRow("power-pgood-one-stage") << "qsoc_cell_power.v" << "power_fsm"
                                               << "        .STAGES(2),\n"
                                                  "        .RESET_VALUE(1'b0)\n"
                                                  "    ) u_pgood_sync ("
                                               << "        .STAGES(1),\n"
                                                  "        .RESET_VALUE(1'b0)\n"
                                                  "    ) u_pgood_sync (";
        QTest::newRow("power-fault-clear-raw") << "qsoc_cell_power.v" << "power_fsm_any"
                                               << "else if (fault_clear_s) fault <= 1'b0;"
                                               << "else if (fault_clear) fault <= 1'b0;";
        QTest::newRow("power-fault-clear-in-fault-only")
            << "qsoc_cell_power.v" << "power_fsm_race"
            << "else if (fault_clear_s) fault <= 1'b0;"
            << "else if (fault_clear_s && state == S_FAULT) fault <= 1'b0;";
        QTest::newRow("power-icg-out-of-reset")
            << "qsoc_cell_power.v" << "power_fsm"
            << "            clk_enable_d = 1'b1;\n"
               "            valid_d = pgood_s;\n"
               "            rst_gate_n_d = 1'b0;  /* assert reset before clock disable */"
            << "            clk_enable_d = 1'b0;\n"
               "            valid_d = pgood_s;\n"
               "            rst_gate_n_d = 1'b0;  /* assert reset before clock disable */";
        QTest::newRow("power-ready-combinational") << "qsoc_cell_power.v" << "power_fsm_any"
                                                   << "assign ready      = ready_q | test_en;"
                                                   << "assign ready      = ready_d | test_en;";
        QTest::newRow("power-rst-sync-short") << "qsoc_cell_power.v" << "power_rst_sync_3"
                                              << ".STAGES(STAGE),"
                                              << ".STAGES(STAGE - 1),";
    }

    /* Each fault, several of them bugs these cells once had, fails its task. */
    void formalCatchesFaults()
    {
        QFETCH(QString, cell);
        QFETCH(QString, task);
        QFETCH(QString, from);
        QFETCH(QString, to);
        for (const char *tool : {"sby", "yosys", "yosys-abc", "z3"}) {
            if (QStandardPaths::findExecutable(tool).isEmpty())
                QSOC_TEST_MISSING_DEPENDENCY(tool);
        }
        QMap<QString, QString> files = library();
        QVERIFY2(files.value(cell).contains(from), qPrintable(from));
        files[cell].replace(from, to);
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeFormal(dir.path(), files));
        QString log;
        QCOMPARE(sbyStatus(dir.path(), task, &log), QString("FAIL"));
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
        const QStringList files = roleFiles() + QStringList{"qsoc_cell_clock.v"};
        for (const QString &file : files)
            QVERIFY(save(dir.filePath(file), cellText(file)));
        QVERIFY(save(dir.filePath("tb.v"), backToBackBench));
        QString log;
        QVERIFY2(
            run("iverilog", QStringList{"-g2005", "-o", "sim", "tb.v"} + files, dir.path(), &log),
            qPrintable(log));
        QVERIFY2(run("vvp", {"-n", "sim"}, dir.path(), &log), qPrintable(log));
        QVERIFY2(log.contains("div_q=3"), qPrintable(log));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocprimitivecell.moc"
