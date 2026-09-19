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
struct Sample
{
    Target   target;
    unsigned bits;
    bool     failure;
};

QList<Sample> input(bool service)
{
    QList<Sample> result;
    for (auto target : {Target::Off, Target::Reset, Target::Run}) {
        for (unsigned bits = 0; bits < 16; ++bits) {
            result.append({target, bits, false});
            if (service)
                result.append({target, bits, true});
        }
    }
    return result;
}

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

QMap<Phase, QList<Sample>> pathToPhase(bool service)
{
    QMap<Phase, QList<Sample>> path{{Phase::Init, {}}};
    QList<Phase>               pending{Phase::Init};
    const auto                 sample = input(service);
    for (qsizetype i = 0; i < pending.size(); ++i) {
        const auto phase = pending[i];
        for (const auto &value : sample) {
            const auto next = QSocPrcmSequence::step(
                {phase, value.target}, value.target, feedback(value.bits), value.failure);
            if (!path.contains(next.state.phase)) {
                auto trace = path[phase];
                trace.append(value);
                path.insert(next.state.phase, trace);
                pending.append(next.state.phase);
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
    void handshake()
    {
        const auto tool = QStandardPaths::findExecutable("verilator");
        if (tool.isEmpty())
            QSOC_TEST_MISSING_DEPENDENCY("verilator");
        QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_prcm_handshake-XXXXXX");
        QVERIFY(directory.isValid());
        const auto rtl = QSocPrcmSequenceRtl::generateHandshake();
        QVERIFY(!rtl.isEmpty());
        QVERIFY(save(directory.filePath("dut.v"), rtl));
        const QString bench = R"(
module tb;
reg clk_i=0, rst_ni=1;
reg need_i=0, grant_i=0, fault_i=0, release_i=0;
wire request_o, hold_o;
qsoc_prcm_service dut(.*);
task tick(input [3:0] value, input [1:0] expected);
    begin
        clk_i=0;
        {need_i,grant_i,fault_i,release_i}=value;
        #5; clk_i=1; #5;
        if ({request_o,hold_o} !== expected)
            $fatal(1,"SERVICE_STEP phase=%0d input=%0h expected=%0b actual=%0b",
                phase,value,expected,{request_o,hold_o});
    end
endtask
task clear;
    begin
        clk_i=0; rst_ni=0; #5;
        if ({request_o,hold_o} !== 2'b00) $fatal(1,"SERVICE_RESET");
        rst_ni=1;
    end
endtask
integer phase, value, next_phase;
reg [1:0] expected;
initial begin
    for (phase=0; phase<4; phase=phase+1) begin
        for (value=0; value<16; value=value+1) begin
            clear;
            if (phase>0) tick(4'b1000,2'b10);
            if (phase>1) tick(4'b1100,2'b11);
            if (phase>2) tick(4'b0101,2'b00);
            case (phase)
                0: next_phase=value[3] ? 1 : 0;
                1: next_phase=value[1] ? 3 : (value[2] ? 2 : 1);
                2: next_phase=(!value[3] || value[1]) && value[0] ? 3 : 2;
                3: next_phase=value[2] ? 3 : 0;
            endcase
            case (next_phase)
                1: expected=2'b10;
                2: expected=2'b11;
                default: expected=2'b00;
            endcase
            tick(value[3:0],expected);
            if (next_phase==0) tick(4'b1100,2'b10);
            if (next_phase==3) tick(4'b1100,2'b00);
        end
    end
    clear;
    $display("SERVICE_STEP_PASS 64");
    $finish;
end
endmodule
)";
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
        QVERIFY(run.contains("SERVICE_STEP_PASS 64"));
    }

    void modelStep_data()
    {
        QTest::addColumn<bool>("service");
        QTest::newRow("domain") << false;
        QTest::newRow("service") << true;
    }

    void modelStep()
    {
        QFETCH(bool, service);
        const auto tool = QStandardPaths::findExecutable("verilator");
        if (tool.isEmpty())
            QSOC_TEST_MISSING_DEPENDENCY("verilator");
        QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_prcm_rtl-XXXXXX");
        QVERIFY(directory.isValid());
        const auto rtl = service ? QSocPrcmSequenceRtl::generateService()
                                 : QSocPrcmSequenceRtl::generate();
        QVERIFY(!rtl.isEmpty());
        QVERIFY(save(directory.filePath("dut.v"), rtl));
        const auto path = pathToPhase(service);
        QCOMPARE(path.size(), int(service ? Phase::FaultPower : Phase::FaultOff) + 1);
        QStringList trace;
        auto        append =
            [&trace](bool reset, const Sample &sample, const QSocPrcmSequenceState &state) {
                const unsigned row = (unsigned(sample.failure) << 18) | (unsigned(!reset) << 17)
                                     | (1U << (14 + unsigned(sample.target))) | (sample.bits << 10)
                                     | output(state);
                trace.append(QString::number(row, 16));
            };
        const auto sample = input(service);
        for (auto reached = path.cbegin(); reached != path.cend(); ++reached) {
            for (const auto &value : sample) {
                QSocPrcmSequenceState state;
                append(true, {Target::Off, 7, false}, state);
                for (const auto &prior : reached.value()) {
                    state = QSocPrcmSequence::step(
                                state, prior.target, feedback(prior.bits), prior.failure)
                                .state;
                    append(false, prior, state);
                }
                QCOMPARE(state.phase, reached.key());
                state
                    = QSocPrcmSequence::step(state, value.target, feedback(value.bits), value.failure)
                          .state;
                append(false, value, state);
            }
        }
        QVERIFY(save(directory.filePath("trace.hex"), trace.join('\n') + '\n'));
        QString bench = R"(
module tb;
reg clk_i = 0;
reg rst_ni = 1;
reg target_off_i, target_reset_i, target_run_i, power_i, reset_i, isolation_i, idle_i;
reg service_fault_i;
wire power_o, clock_o, reset_o, isolation_o, quiesce_o;
wire state_off_o, state_reset_o, state_run_o, fault_o, power_watch_o;
@MODULE@ dut(.*);
reg [18:0] trace [0:@LAST@];
integer i;
initial begin
    $readmemh("trace.hex", trace);
    for (i = 0; i <= @LAST@; i = i + 1) begin
        clk_i = 0;
        rst_ni = trace[i][17];
        service_fault_i = trace[i][18];
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
        bench.replace("@MODULE@", service ? "qsoc_prcm_domain_service" : "qsoc_prcm_domain");
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
