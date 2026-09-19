// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmformal.h"
#include "qsoc_prcm_fixture.h"
#include "qsoc_test.h"

#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

namespace {

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
    void circuit_data()
    {
        QTest::addColumn<QString>("bus");
        QTest::addColumn<int>("width");
        QTest::addColumn<int>("stage");
        QTest::addColumn<bool>("run");
        QTest::newRow("apb8") << "apb4" << 8 << 2 << true;
        QTest::newRow("axi32") << "axi4_lite" << 32 << 2 << true;
        QTest::newRow("axi64-stage3") << "axi4_lite" << 64 << 3 << true;
        QTest::newRow("off-only") << "apb4" << 8 << 2 << false;
    }

    void circuit()
    {
        QFETCH(QString, bus);
        QFETCH(int, width);
        QFETCH(int, stage);
        QFETCH(bool, run);
        for (const auto &tool : {"sby", "yosys", "z3"}) {
            if (QStandardPaths::findExecutable(tool).isEmpty())
                QSOC_TEST_MISSING_DEPENDENCY(tool);
        }
        auto node                                               = YAML::Load(qsocPrcmDeclaration());
        node["prcm"]["mmio"]["bus"]                             = bus.toStdString();
        node["prcm"]["mmio"]["data_width"]                      = width;
        node["prcm"]["mmio"]["address_width"]                   = 6;
        node["prcm"]["domain"]["periph"]["mode"]["RUN"]["code"] = quint64(1) << (width - 4);
        if (width == 32) {
            auto held         = YAML::Clone(node["prcm"]["domain"]["periph"]["mode"]["RUN"]);
            held["code"]      = 1;
            held["reset"]     = "asserted";
            held["isolation"] = "enabled";
            node["prcm"]["domain"]["periph"]["mode"]["RESET"] = held;
            for (const auto &name : {"OFF", "RUN"}) {
                node["prcm"]["domain"]["periph"]["transition"].push_back(
                    YAML::Load(QString("{from: %1, to: RESET}").arg(name).toStdString()));
                node["prcm"]["domain"]["periph"]["transition"].push_back(
                    YAML::Load(QString("{from: RESET, to: %1}").arg(name).toStdString()));
            }
        }
        node["prcm"]["controller"]["reset"]["target"]  = "management";
        node["reset"][0]["source"]["warm_n"]["active"] = "low";
        node["reset"][0]["target"]["management"]       = YAML::Load(
            "{active: low, async: {clock: aon_clk, stage: 2}, link: {por_n: {}, warm_n: {}}}");
        if (stage == 3) {
            node["prcm"]["supply"]["periph"]["request"]        = "CLOCK";
            node["reset"][0]["target"]["management"]["active"] = "high";
            node["reset"][0]["target"]["periph_n"]["active"]   = "high";
        }
        if (!run) {
            node["prcm"]["domain"]["periph"]["mode"].remove("RUN");
            node["prcm"]["domain"]["periph"]["transition"] = YAML::Node(YAML::NodeType::Sequence);
            node["prcm"]["controller"]["reset"].remove("target");
            node["reset"][0]["target"].remove("management");
            node["reset"][0]["source"].remove("warm_n");
        }
        const auto binding = QSocPrcmBinding::resolve(node, "control.soc_net");
        QVERIFY(binding.plan);
        const auto generated = QSocPrcmGenerator::generate(*binding.plan, "control", stage);
        QVERIFY(generated.circuit);
        const auto formal
            = QSocPrcmFormal::generate(*binding.plan, *generated.circuit, "control", stage);
        QVERIFY(!formal.systemVerilog.contains(QRegularExpression("@[A-Za-z_]+@")));
        QTemporaryDir directory(QDir::tempPath() + "/test_qsoc_prcm_formal-XXXXXX");
        QVERIFY(directory.isValid());
        for (auto file = generated.circuit->rtl.cbegin(); file != generated.circuit->rtl.cend();
             ++file)
            QVERIFY(save(directory.filePath(file.key()), file.value()));
        QVERIFY(save(directory.filePath("control_formal.sv"), formal.systemVerilog));
        QVERIFY(save(directory.filePath("control.sby"), formal.sby));
        QProcess process;
        process.setWorkingDirectory(directory.path());
        process.setProcessChannelMode(QProcess::MergedChannels);
        process.start(QStandardPaths::findExecutable("sby"), {"-f", "control.sby"});
        QVERIFY(process.waitForStarted());
        QVERIFY(process.waitForFinished(180000));
        const auto output = process.readAll();
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QVERIFY2(process.exitCode() == 0, output.right(10000).constData());
        for (const auto &task : {"prove", "cover"}) {
            QFile status(directory.filePath(QString("control_%1/status").arg(task)));
            QVERIFY(status.open(QIODevice::ReadOnly));
            QCOMPARE(status.readAll().simplified().split(' ').first(), QByteArray("PASS"));
        }
        if (run)
            return;
        const auto    cell       = generated.circuit->rtl["clock_cell.v"];
        const QString assignment = "assign clk_out = iq & clk;";
        QCOMPARE(cell.count(assignment), 1);
        auto fault = cell;
        fault.replace(
            assignment,
            "`ifdef SYNTHESIS\n`ifdef FORMAL\n" + assignment
                + "\n`else\nassign clk_out = clk;\n`endif\n`else\n" + assignment + "\n`endif");
        for (const bool corrupt : {true, false}) {
            QVERIFY(save(directory.filePath("clock_cell.v"), corrupt ? fault : cell));
            process.start(QStandardPaths::findExecutable("sby"), {"-f", "control.sby", "prove"});
            QVERIFY(process.waitForStarted());
            QVERIFY(process.waitForFinished(180000));
            const auto log = process.readAll();
            QCOMPARE(process.exitStatus(), QProcess::NormalExit);
            QFile status(directory.filePath("control_prove/status"));
            QVERIFY(status.open(QIODevice::ReadOnly));
            QCOMPARE(
                status.readAll().simplified().split(' ').first(),
                QByteArray(corrupt ? "FAIL" : "PASS"));
            if (corrupt)
                QVERIFY2(
                    log.contains("Assert failed in control_formal: off_control"), log.constData());
            else
                QVERIFY2(process.exitCode() == 0, log.constData());
        }
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocprcmformal.moc"
