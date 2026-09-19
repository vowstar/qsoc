// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmsequence.h"
#include "common/qsocprcmsequencertl.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QMap>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {

using Phase  = QSocPrcmPhase;
using Target = QSocPrcmTarget;
using Sample = QPair<Target, unsigned>;

QSocPrcmObservation feedback(unsigned bits)
{
    return {(bits & 8U) != 0, (bits & 4U) != 0, (bits & 2U) != 0, (bits & 1U) != 0};
}

unsigned output(const QSocPrcmSequenceState &state)
{
    const auto value = QSocPrcmSequence::control(state);
    unsigned   bits  = 0;
    for (bool bit :
         {value.power,
          value.clock,
          value.reset,
          value.isolation,
          value.quiesce,
          state.phase == Phase::Off,
          state.phase == Phase::Reset,
          state.phase == Phase::Run,
          QSocPrcmSequence::fault(state),
          QSocPrcmSequence::step(state, Target::Off, {}).powerLost})
        bits = bits * 2 + unsigned(bit);
    return bits;
}

QMap<Phase, QList<Sample>> pathToPhase()
{
    QMap<Phase, QList<Sample>> path{{Phase::Init, {}}};
    QList<Phase>               pending{Phase::Init};
    for (qsizetype i = 0; i < pending.size(); ++i) {
        const auto phase = pending[i];
        for (auto target : {Target::Off, Target::Reset, Target::Run}) {
            for (unsigned bits = 0; bits < 16; ++bits) {
                const auto next = QSocPrcmSequence::step({phase, target}, target, feedback(bits));
                if (!path.contains(next.state.phase)) {
                    auto trace = path[phase];
                    trace.append(Sample{target, bits});
                    path.insert(next.state.phase, trace);
                    pending.append(next.state.phase);
                }
            }
        }
    }
    return path;
}

bool save(const QString &path, const QString &text)
{
    QFile      file(path);
    const auto bytes = text.toUtf8();
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void modelStep()
    {
        const auto tool = QStandardPaths::findExecutable("verilator");
        if (tool.isEmpty())
            QSOC_TEST_MISSING_DEPENDENCY("verilator");
        QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_prcm_rtl-XXXXXX");
        QVERIFY(directory.isValid());
        const auto rtl = QSocPrcmSequenceRtl::generate();
        QVERIFY(!rtl.isEmpty());
        QVERIFY(save(directory.filePath("dut.v"), rtl));
        const auto path = pathToPhase();
        QCOMPARE(path.size(), int(Phase::FaultOff) + 1);
        QStringList trace;
        auto        append =
            [&trace](bool reset, Target target, unsigned bits, const QSocPrcmSequenceState &state) {
                const unsigned row = (unsigned(!reset) << 17) | (1U << (14 + unsigned(target)))
                                     | (bits << 10) | output(state);
                trace.append(QString::number(row, 16));
            };
        for (auto reached = path.cbegin(); reached != path.cend(); ++reached) {
            for (auto target : {Target::Off, Target::Reset, Target::Run}) {
                for (unsigned bits = 0; bits < 16; ++bits) {
                    QSocPrcmSequenceState state;
                    append(true, Target::Off, 7, state);
                    for (const auto &sample : reached.value()) {
                        state = QSocPrcmSequence::step(state, sample.first, feedback(sample.second))
                                    .state;
                        append(false, sample.first, sample.second, state);
                    }
                    QCOMPARE(state.phase, reached.key());
                    state = QSocPrcmSequence::step(state, target, feedback(bits)).state;
                    append(false, target, bits, state);
                }
            }
        }
        QVERIFY(save(directory.filePath("trace.hex"), trace.join('\n') + '\n'));
        QString bench = R"(
module tb;
reg clk_i = 0;
reg rst_ni = 1;
reg target_off_i, target_reset_i, target_run_i, power_i, reset_i, isolation_i, idle_i;
wire power_o, clock_o, reset_o, isolation_o, quiesce_o;
wire state_off_o, state_reset_o, state_run_o, fault_o, power_watch_o;
qsoc_prcm_domain dut(.*);
reg [17:0] trace [0:@LAST@];
integer i;
initial begin
    $readmemh("trace.hex", trace);
    for (i = 0; i <= @LAST@; i = i + 1) begin
        clk_i = 0;
        rst_ni = trace[i][17];
        {target_run_i, target_reset_i, target_off_i} = trace[i][16:14];
        {power_i, reset_i, isolation_i, idle_i} = trace[i][13:10];
        #5;
        clk_i = 1;
        #5;
        if ({power_o, clock_o, reset_o, isolation_o, quiesce_o,
             state_off_o, state_reset_o, state_run_o, fault_o, power_watch_o} !== trace[i][9:0])
            $fatal(1, "ACTION_STEP row=%0d", i);
    end
    $display("ACTION_STEP_PASS @COUNT@");
    $finish;
end
endmodule
)";
        bench.replace("@LAST@", QString::number(trace.size() - 1));
        bench.replace("@COUNT@", QString::number(trace.size()));
        QVERIFY(save(directory.filePath("tb.sv"), bench));
        QProcess process;
        process.setWorkingDirectory(directory.path());
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(
            tool,
            {"--binary", "--timing", "--top-module", "tb", "-Wno-fatal", "-j", "16", "tb.sv", "dut.v"});
        QVERIFY(process.waitForStarted());
        QVERIFY(process.waitForFinished(180000));
        const auto build = process.readAll();
        QVERIFY2(process.exitCode() == 0, build.constData());
        process.start(directory.filePath("obj_dir/Vtb"), QStringList{});
        QVERIFY(process.waitForStarted());
        QVERIFY(process.waitForFinished(30000));
        const auto run = process.readAll();
        QVERIFY2(process.exitCode() == 0, run.constData());
        QVERIFY(run.contains("ACTION_STEP_PASS"));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocprcmsequencertl.moc"
