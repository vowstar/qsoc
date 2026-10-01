// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/qsoccellbinding.h"
#include "common/qsoccellformal.h"
#include "common/qsoccelllibrary.h"
#include "common/qsocconsole.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

namespace {

QStringList messages;

void collect(QtMsgType, const QMessageLogContext &, const QString &message)
{
    messages << message;
}

/* A library entry: 1-bit ports, then the declaration keys. */
QString entry(const QString &name, const QString &inputs, const QString &outputs, const QString &keys)
{
    QString text = name + ":\n  port:\n";
    for (const QString &pin : inputs.split(' ', Qt::SkipEmptyParts))
        text += QString("    %1: {direction: input, type: logic}\n").arg(pin);
    for (const QString &pin : outputs.split(' ', Qt::SkipEmptyParts))
        text += QString("    %1: {direction: output, type: logic}\n").arg(pin);
    for (const QString &key : keys.split('\n', Qt::SkipEmptyParts))
        text += "  " + key + "\n";
    return text;
}

const QString nand2 = entry(
    "CKND2D2", "A1 A2", "ZN", "function: [{A1: 0, ZN: 1}, {A2: 0, ZN: 1}, {A1: 1, A2: 1, ZN: 0}]");
const QString buf  = entry("CKBD4", "I", "Z", "function: {Z: I}");
const QString inv  = entry("CKND4", "I", "ZN", "function: {ZN: \"!I\"}");
const QString or2  = entry("CKOR2D2", "A1 A2", "Z", "function: {Z: \"A1 | A2\"}");
const QString xor2 = entry("CKXOR2D2", "A1 A2", "Z", "function: {Z: \"A1 ^ A2\"}");
const QString mux2 = entry("CKMUX2D2", "I0 I1 S", "Z", "function: [{S: 0, Z: I0}, {S: 1, Z: I1}]");
const QString nor2 = entry("CKNR2D2", "A1 A2", "ZN", "function: {ZN: \"!(A1 | A2)\"}");
const QString and2 = entry("CKAN2D2", "A1 A2", "Z", "function: {Z: \"A1 & A2\"}");
const QString icg  = entry(
    "CKLNQD4",
    "CP E TE",
    "Q",
    "sequential: {type: icg_pos, clock: CP, enable: E, test: TE, output: Q}");
const QString sync2 = entry(
    "SDFSYNC2",
    "CP D CDN SI SE",
    "Q",
    "sequential: {type: sync, stages: 2, clock: CP, data: D, output: Q, reset: CDN}\n"
    "tie: {SI: 0, SE: 0}");

QString fullLibrary()
{
    return nand2 + buf + inv + or2 + xor2 + mux2 + icg + sync2;
}

QSocCellBinding bindYaml(const QString &modules, const QString &target = "asic")
{
    const YAML::Node project = YAML::Load(QString("cell: {target: %1}").arg(target).toStdString());
    return QSocCellBinding::resolve(project, YAML::Load(modules.toStdString()));
}

QString roleText(const QSocCellBinding &binding, const QString &role)
{
    for (const QSocCellLibrary::Cell &cell : QSocCellLibrary::roles(binding)) {
        if (cell.file == role + ".v")
            return cell.text;
    }
    return {};
}

bool save(const QString &path, const QString &text)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile      file(path);
    const auto bytes = text.toUtf8();
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QString load(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? QString::fromUtf8(file.readAll()) : QString();
}

bool run(
    const QString     &tool,
    const QStringList &args,
    const QString     &dir,
    QString           *log,
    int                timeout = 600000)
{
    QProcess process;
    process.setWorkingDirectory(dir);
    process.setProcessChannelMode(QProcess::MergedChannels);
    process.start(tool, args);
    const bool finished = process.waitForFinished(timeout);
    *log                = QString::fromUtf8(process.readAll());
    return finished && process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0;
}

/* Lines of a file list, as paths relative to output. */
QStringList listed(const QString &output, const QString &list)
{
    QStringList lines = load(QDir(output).filePath(list)).split('\n', Qt::SkipEmptyParts);
    for (QString &line : lines)
        line = QDir(output).filePath(line);
    return lines;
}

/* A project in root with the given library, cell target and synthesis budget. */
bool makeProject(
    const QString &root, const QString &library, const QString &target, const QString &rlimit = {})
{
    QSocProjectManager project;
    project.setProjectName("cells");
    project.setCurrentPath(root);
    if (!project.mkpath() || !project.save("cells"))
        return false;
    const QString file = QDir(root).filePath("cells.soc_pro");
    YAML::Node    node = YAML::LoadFile(file.toStdString());
    if (!target.isEmpty())
        node["cell"]["target"] = target.toStdString();
    if (!rlimit.isEmpty())
        node["cell"]["synth_rlimit"] = rlimit.toStdString();
    std::ofstream(file.toStdString()) << node;
    return library.isEmpty() || save(QDir(root).filePath("module/cells.soc_mod"), library);
}

void generate(const QString &root, const QString &netlist, bool formal = false)
{
    QSocCliWorker worker;
    QStringList   args = {"qsoc", "generate", "verilog", "-d", root, netlist};
    if (formal)
        args.append("--with-formal");
    worker.setup(args, false);
    worker.run();
}

/* Clock gates of both polarities, a glitch-free mux, a divider, an inverter,
 * a role STA guide, and a reset synchronizer. */
const char *const clockNetlist = R"(
port:
  clk_a: {direction: input, type: logic}
  clk_b: {direction: input, type: logic}
  sel: {direction: input, type: logic}
  en: {direction: input, type: logic}
  rst_n: {direction: input, type: logic}
  por_n: {direction: input, type: logic}
  test_en: {direction: input, type: logic}
  clk_out: {direction: output, type: logic}
  clk_neg: {direction: output, type: logic}
  sys_rst_n: {direction: output, type: logic}
instance: {}
net: {}
clock:
  - name: cell_clk
    test_enable: test_en
    input:
      clk_a: {freq: 100MHz}
      clk_b: {freq: 50MHz}
    target:
      clk_out:
        freq: 25MHz
        link:
          clk_a:
          clk_b:
        select: sel
        reset: rst_n
        icg:
          enable: en
          sta_guide: {instance: u_icg_guide}
        div: {default: 2, width: 2, reset: rst_n}
      clk_neg:
        freq: 100MHz
        link:
          clk_a:
        icg: {enable: en, polarity: low}
        inv:
reset:
  - name: cell_rst
    source:
      por_n: {active: low}
    target:
      sys_rst_n:
        active: low
        link:
          por_n:
            async: {clock: clk_a, stage: 3}
)";

/* Declared combinational bases for composition, each with a clock gate and a
 * synchronizer so a whole clock controller elaborates. */
QMap<QString, QString> compositionBases()
{
    return {
        {"nand2-inv", nand2 + inv},
        {"nor2-inv", nor2 + inv},
        {"and2-or2-inv", and2 + or2 + inv},
        {"xor2-and2", xor2 + and2},
        {"nand2", nand2},
    };
}

const QStringList synthRoles
    = {"qsoc_ck_buf", "qsoc_ck_inv", "qsoc_ck_or2", "qsoc_ck_xor2", "qsoc_ck_mux2"};

QString composedLibrary(const QString &base)
{
    return icg + sync2 + compositionBases().value(base);
}

/* Run one task of the contract job; returns the status sby recorded. */
QString sbyStatus(const QString &dir, const QString &task, QString *log)
{
    run("sby", {"-f", "contract.sby", task}, dir, log, 1800000);
    QFile status(QDir(dir).filePath("contract_" + task + "/status"));
    if (!status.open(QIODevice::ReadOnly))
        return {};
    return QString::fromUtf8(status.readAll()).section(' ', 0, 0).trimmed();
}

} // namespace

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        qInstallMessageHandler(collect);
        QSocConsole::setTeeToMessageHandler(true);
    }

    void init() { messages.clear(); }

    void bindsEveryRoleOfAFullLibrary()
    {
        const QSocCellBinding binding = bindYaml(fullLibrary());
        QVERIFY2(binding.isValid(), qPrintable(binding.errors().join('\n')));
        QCOMPARE(binding.cells().size(), 8);
        QVERIFY(binding.unresolved().isEmpty());
        const auto pins = [&](const QString &role) { return binding.roles().value(role).pin; };
        QCOMPARE(
            pins("qsoc_ck_mux2"),
            (QMap<QString, QString>{
                {"I0", "clk_in0"}, {"I1", "clk_in1"}, {"S", "clk_sel"}, {"Z", "clk_out"}}));
        QCOMPARE(pins("qsoc_ck_inv"), (QMap<QString, QString>{{"I", "clk_in"}, {"ZN", "clk_out"}}));
        QCOMPARE(
            pins("qsoc_ck_icg_pos"),
            (QMap<QString, QString>{{"CP", "clk"}, {"E", "en"}, {"TE", "test_en"}, {"Q", "clk_out"}}));
        QCOMPARE(
            binding.roles().value("qsoc_ck_icg_neg").via,
            (QStringList{"qsoc_ck_inv", "qsoc_ck_icg_pos", "qsoc_ck_inv"}));
        QVERIFY(binding.warnings().join('\n').contains("two extra inverter delays"));

        const QString mux = roleText(binding, "qsoc_ck_mux2");
        QVERIFY(mux.contains("(* keep_hierarchy = \"yes\" *)\nmodule qsoc_ck_mux2 ("));
        QVERIFY(mux.contains(
            "    (* dont_touch = \"true\" *)\n    CKMUX2D2 u_cell (\n"
            "        .I0(clk_in0),\n        .I1(clk_in1),\n"
            "        .S(clk_sel),\n        .Z(clk_out)\n    );\n"));
        const QString neg = roleText(binding, "qsoc_ck_icg_neg");
        QVERIFY(neg.contains("qsoc_ck_inv u_inv_in ("));
        QVERIFY(neg.contains("qsoc_ck_icg_pos u_icg ("));
        QVERIFY(neg.contains("qsoc_ck_inv u_inv_out ("));
        const QString sync = roleText(binding, "qsoc_sync");
        QVERIFY(sync.contains("SDFSYNC2 u_cell ("));
        QVERIFY(sync.contains(".SE(1'b0)"));
        QVERIFY(sync.contains(".SI(1'b0)"));
    }

    void rejects_data()
    {
        QTest::addColumn<QString>("library");
        QTest::addColumn<QString>("error");
        QTest::newRow("unknown-pin")
            << entry("C", "I", "Z", "function: {Z: J}") << "'J' is not an input pin";
        QTest::newRow("wide-pin")
            << "C:\n  port:\n    I: {direction: input, type: \"logic[1:0]\"}\n"
               "    Z: {direction: output, type: logic}\n"
               "  function: {Z: 1}\n"
            << "2 bits wide";
        QTest::newRow("rows-disagree")
            << entry("C", "A B", "Z", "function: [{A: 0, Z: 0}, {B: 0, Z: 1}, {A: 1, B: 1, Z: 1}]")
            << "disagree on Z";
        QTest::newRow("uncovered")
            << entry("C", "A B", "Z", "function: [{A: 0, Z: 0}, {A: 1, B: 1, Z: 1}]")
            << "Z is undefined when A=1 B=0";
        QTest::newRow("unused-input")
            << entry("C", "A B", "Z", "function: {Z: A}") << "input B changes no output, tie it";
        QTest::newRow("output-without-function")
            << entry("C", "A", "Y Z", "function: {Z: A}") << "output Y has no function";
        QTest::newRow("inout-pin") << "C:\n  port:\n    P: {direction: inout, type: logic}\n"
                                      "    Z: {direction: output, type: logic}\n"
                                      "  function: {Z: 1}\n"
                                   << "has no inout pin";
        QTest::newRow("both") << entry("C", "I", "Z", "function: {Z: I}\nsequential: {type: sync}")
                              << "not both";
        QTest::newRow("tie-alone")
            << entry("C", "I", "Z", "tie: {I: 0}") << "a tie needs a function";
        QTest::newRow("tie-output")
            << entry("C", "I", "Z", "function: {Z: I}\ntie: {Z: 0}") << "tie.Z: not an input pin";
        QTest::newRow("tie-level")
            << entry("C", "I T", "Z", "function: {Z: I}\ntie: {T: x}") << "takes 0 or 1";
        QTest::newRow("reserved") << entry("qsoc_buf", "I", "Z", "function: {Z: I}")
                                  << "reserved for QSoC";
        QTest::newRow("bad-type") << entry("C", "CP E", "Q", "sequential: {type: latch}")
                                  << "must be icg_pos, icg_neg or sync";
        QTest::newRow("gate-missing-enable")
            << entry("C", "CP", "Q", "sequential: {type: icg_pos, clock: CP, output: Q}")
            << "type icg_pos needs enable";
        QTest::newRow("gate-wrong-direction")
            << entry("C", "CP E", "Q", "sequential: {type: icg_pos, clock: CP, enable: Q, output: E}")
            << "'Q' is not an input pin";
        QTest::newRow("gate-stray-input") << entry(
            "C", "CP E X", "Q", "sequential: {type: icg_pos, clock: CP, enable: E, output: Q}")
                                          << "input X is not in the template, tie it";
        QTest::newRow("gate-stray-output") << entry(
            "C", "CP E", "Q ECK", "sequential: {type: icg_pos, clock: CP, enable: E, output: Q}")
                                           << "output ECK is not the template output";
        QTest::newRow("gate-unknown-key") << entry(
            "C",
            "CP E",
            "Q",
            "sequential: {type: icg_neg, clock: CP, enable: E, output: Q, stages: 2}")
                                          << "stages: not a key of a icg_neg template";
        QTest::newRow("sync-without-reset") << entry(
            "C", "CP D", "Q", "sequential: {type: sync, stages: 2, clock: CP, data: D, output: Q}")
                                            << "type sync needs reset";
        QTest::newRow("sync-without-stages") << entry(
            "C", "CP D R", "Q", "sequential: {type: sync, clock: CP, data: D, output: Q, reset: R}")
                                             << "a sync cell needs its flop count";
        QTest::newRow("pin-twice")
            << entry("C", "CP", "Q", "sequential: {type: icg_pos, clock: CP, enable: CP, output: Q}")
            << "pin CP has another use";
        QTest::newRow("ambiguous") << or2 + entry("CKOR2D8", "A B", "Y", "function: {Y: \"A | B\"}")
                                   << "CKOR2D2 and CKOR2D8 all implement it";
        QTest::newRow("no-gate-in-asic") << or2 << "no declared cell implements a clock gate";
    }

    void rejects()
    {
        QFETCH(QString, library);
        QFETCH(QString, error);
        const QSocCellBinding binding = bindYaml(library);
        QVERIFY(!binding.isValid());
        QVERIFY2(binding.errors().join('\n').contains(error), qPrintable(binding.errors().join('\n')));
    }

    void targetIsGenericOrAsic()
    {
        QVERIFY(!bindYaml(icg, "fast").isValid());
        QVERIFY(!QSocCellBinding::resolve(YAML::Load("cell: {mode: asic}"), YAML::Node()).isValid());
        QVERIFY(!QSocCellBinding::resolve(YAML::Load("cell: asic"), YAML::Node()).isValid());
        QVERIFY(QSocCellBinding::resolve(YAML::Load("bus: bus"), YAML::Node()).isValid());
        QVERIFY(!QSocCellBinding::resolve(YAML::Load("bus: bus"), YAML::Node()).isAsic());
        QVERIFY(bindYaml(icg).isAsic());
    }

    /* Generic mode ignores the declarations: the role files are PR6's bytes. */
    void genericTargetKeepsBehavioralRoles()
    {
        const QSocCellBinding binding = bindYaml(fullLibrary(), "generic");
        QVERIFY(binding.isValid());
        QVERIFY(binding.warnings().isEmpty());
        const auto generic = QSocCellLibrary::roles();
        const auto roles   = QSocCellLibrary::roles(binding);
        QCOMPARE(roles.size(), generic.size());
        for (qsizetype i = 0; i < roles.size(); ++i)
            QCOMPARE(roles.at(i).text, generic.at(i).text);
        QVERIFY(!QSocCellLibrary::roles().first().text.contains("keep_hierarchy"));
        QCOMPARE(QSocCellLibrary::models(binding).size(), 8);
    }

    void tiesNarrowAFunction()
    {
        const QSocCellBinding binding = bindYaml(
            icg + entry("CKOR3D2", "A B C", "Z", "function: {Z: \"A | B | C\"}\ntie: {C: 0}"));
        QVERIFY2(binding.isValid(), qPrintable(binding.errors().join('\n')));
        QCOMPARE(
            binding.cells().at(binding.roles().value("qsoc_ck_or2").cell).name, QString("CKOR3D2"));
        QVERIFY(roleText(binding, "qsoc_ck_or2").contains(".C(1'b0)"));
        /* Tied high the same cell is a constant, no role. */
        const QSocCellBinding high = bindYaml(
            icg + entry("CKOR3D2", "A B C", "Z", "function: {Z: \"A | B | C\"}\ntie: {C: 1}"));
        QVERIFY(!high.roles().contains("qsoc_ck_or2"));
    }

    void expressionOutputsBind()
    {
        const QSocCellBinding binding = bindYaml(
            icg + entry("CKAOI", "A B", "ZN", "function: {ZN: \"!(A & B)\"}")
            + entry("CKINVX", "A", "Y", "function: [{A: 0, Y: 1}, {A: 1, Y: 0}]")
            + entry("CKMX", "D0 D1 SEL", "Y", "function: {Y: \"SEL ? D1 : D0\"}"));
        QVERIFY2(binding.isValid(), qPrintable(binding.errors().join('\n')));
        QVERIFY(binding.roles().contains("qsoc_ck_inv"));
        QCOMPARE(
            binding.roles().value("qsoc_ck_mux2").pin,
            (QMap<QString, QString>{
                {"D0", "clk_in0"}, {"D1", "clk_in1"}, {"SEL", "clk_sel"}, {"Y", "clk_out"}}));
        /* No OR cell: the role is composed, not bound. */
        QCOMPARE(binding.roles().value("qsoc_ck_or2").cell, -1);
        QVERIFY(!binding.roles().value("qsoc_ck_or2").network.gate.isEmpty());
    }

    void gateWithoutTestPinOrsTestEnable()
    {
        const QSocCellBinding binding = bindYaml(entry(
            "CKLN", "CP E", "Q", "sequential: {type: icg_neg, clock: CP, enable: E, output: Q}"));
        QVERIFY2(binding.isValid(), qPrintable(binding.errors().join('\n')));
        QVERIFY(roleText(binding, "qsoc_ck_icg_neg").contains(".E(en | test_en)"));
        /* No inverter: the other polarity stays unresolved. */
        QVERIFY(binding.unresolved().contains("qsoc_ck_icg_pos"));
        QVERIFY(QSocCellBinding::model(binding.cells().first()).contains("if (CP) enabled = E;"));
    }

    void positiveGateComposesFromNegative()
    {
        const QSocCellBinding binding = bindYaml(
            inv
            + entry(
                "CKLN",
                "CP E TE",
                "Q",
                "sequential: {type: icg_neg, clock: CP, enable: E, test: TE, output: Q}"));
        QVERIFY2(binding.isValid(), qPrintable(binding.errors().join('\n')));
        QCOMPARE(
            binding.roles().value("qsoc_ck_icg_pos").via,
            (QStringList{"qsoc_ck_inv", "qsoc_ck_icg_neg", "qsoc_ck_inv"}));
        QVERIFY(roleText(binding, "qsoc_ck_icg_pos").contains("qsoc_ck_icg_neg u_icg ("));
    }

    void unresolvedRoleInstantiatesAMissingModule()
    {
        const QSocCellBinding binding = bindYaml(icg + or2);
        QVERIFY(binding.isValid());
        /* An OR with one pin tied low is a buffer, nothing monotone inverts. */
        QVERIFY(binding.roles().value("qsoc_ck_buf").network.gate.size() == 1);
        QVERIFY(binding.warnings().join('\n').contains(
            "qsoc_ck_inv, qsoc_ck_xor2, qsoc_ck_mux2, qsoc_ck_icg_neg, qsoc_sync"));
        QVERIFY(
            roleText(binding, "qsoc_ck_xor2")
                .contains("    qsoc_role_unresolved_ck_xor2 u_cell (\n        .clk_in0(clk_in0),"));
        QVERIFY(binding.report().contains(
            "qsoc_ck_xor2:\n    unresolved: qsoc_role_unresolved_ck_xor2"));
    }

    void composesRolesFromDeclaredCells_data()
    {
        QTest::addColumn<QString>("base");
        for (const QString &base : compositionBases().keys())
            QTest::newRow(qPrintable(base)) << base;
    }

    /* Every combinational role is direct or composed, and composed ones
     * compute the role function with declared cells only. */
    void composesRolesFromDeclaredCells()
    {
        QFETCH(QString, base);
        QElapsedTimer timer;
        timer.start();
        const QSocCellBinding binding = bindYaml(composedLibrary(base));
        const qint64          spent   = timer.elapsed();
        QVERIFY2(binding.isValid(), qPrintable(binding.errors().join('\n')));
        QStringList names;
        for (const QSocCellBinding::Cell &cell : binding.cells())
            names.append(cell.name);
        for (const QString &role : synthRoles) {
            QVERIFY2(binding.roles().contains(role), qPrintable(role));
            const QSocCellBinding::Binding bound = binding.roles().value(role);
            if (bound.cell >= 0)
                continue;
            QVERIFY(!bound.network.gate.isEmpty());
            const QString text = roleText(binding, role);
            for (int i = 0; i < bound.network.gate.size(); ++i) {
                const QString cell = bound.network.gate.at(i).cell;
                QVERIFY(names.contains(cell));
                QVERIFY(
                    text.contains(QString("    (* dont_touch = \"true\" *)\n    %1 u_cell_g%2 (\n")
                                      .arg(cell)
                                      .arg(i)));
            }
            QVERIFY(text.contains("Composed from declared cells as u_cell_g0"));
            std::printf(
                "%s %s: depth %d, %d cells\n",
                qPrintable(base),
                qPrintable(role),
                bound.network.depth,
                int(bound.network.gate.size()));
        }
        /* A declared inverter beats NANDs tied as inverters, whatever the names. */
        if (base == "nand2-inv") {
            const QSocCellSynthNetlist buffer = binding.roles().value("qsoc_ck_buf").network;
            QCOMPARE(buffer.gate.size(), 2);
            for (const QSocCellSynthGate &gate : buffer.gate)
                QCOMPARE(gate.cell, QString("CKND4"));
        }
        std::printf(
            "%s: resolve with synthesis %lld ms\n", qPrintable(base), static_cast<long long>(spent));
        const QString report = binding.report();
        QVERIFY(report.contains("    synthesized:\n      depth: "));
        QVERIFY(report.contains("      hazard: free for one input change at a time"));
        QVERIFY(
            report.contains("      hazard: free for clk_in0 and clk_in1 changes while clk_sel holds")
            || binding.roles().value("qsoc_ck_mux2").cell >= 0);
        /* Generic mode never synthesizes. */
        const QSocCellBinding generic = bindYaml(composedLibrary(base), "generic");
        for (const QSocCellBinding::Binding &bound : generic.roles())
            QVERIFY(bound.network.gate.isEmpty());
    }

    void synthesisTimePerRole_data() { composesRolesFromDeclaredCells_data(); }

    /* Solver time for each role of each base, for the record. */
    void synthesisTimePerRole()
    {
        QFETCH(QString, base);
        const QSocCellBinding                  binding = bindYaml(composedLibrary(base));
        const QMap<QString, QSocCellSynthRole> roles{
            {"qsoc_ck_buf", QSocCellSynthRole::Buf},
            {"qsoc_ck_inv", QSocCellSynthRole::Inv},
            {"qsoc_ck_or2", QSocCellSynthRole::Or2},
            {"qsoc_ck_xor2", QSocCellSynthRole::Xor2},
            {"qsoc_ck_mux2", QSocCellSynthRole::Mux2},
        };
        for (auto it = roles.constBegin(); it != roles.constEnd(); ++it) {
            QSocCellSynthRequest request;
            request.role  = it.value();
            request.basis = binding.basis();
            QElapsedTimer timer;
            timer.start();
            const QSocCellSynthResult result = QSocCellSynth::synthesize(request);
            std::printf(
                "%s\n",
                qPrintable(QString("%1 %2: %3 ms, rlimit %4, %5")
                               .arg(base, it.key())
                               .arg(timer.elapsed())
                               .arg(result.resourceUsed)
                               .arg(
                                   result.status == QSocCellSynthStatus::Found
                                       ? QSocCellSynth::report(it.value(), result.netlist)
                                       : result.reason)));
            QCOMPARE(result.status, QSocCellSynthStatus::Found);
        }
    }

    /* Same declarations in any order give the same bytes. */
    void compositionIgnoresDeclarationOrder()
    {
        QStringList parts = {nand2, nor2, inv, icg, and2};
        std::sort(parts.begin(), parts.end());
        QString first;
        int     orders = 0;
        do {
            const QSocCellBinding binding = bindYaml(parts.join(QString()));
            QString               all     = binding.report();
            for (const QSocCellLibrary::Cell &cell : QSocCellLibrary::roles(binding))
                all += cell.text;
            if (first.isEmpty())
                first = all;
            QCOMPARE(all, first);
            ++orders;
        } while (std::next_permutation(parts.begin(), parts.end()) && orders < 24);
        QCOMPARE(orders, 24);
        QVERIFY(first.contains("u_cell_g"));
    }

    void impossibleRoleStaysUnresolved()
    {
        QTemporaryDir root(QDir::tempPath() + "/test_qsoc_cellbinding-XXXXXX");
        QVERIFY(root.isValid());
        QVERIFY(makeProject(root.path(), icg + sync2 + or2 + and2, "asic"));
        const QString netlist = root.filePath("cell_top.soc_net");
        QVERIFY(save(netlist, clockNetlist));
        generate(root.path(), netlist);
        const QString warning = messages.join('\n');
        QVERIFY2(
            warning.contains(
                "cell role qsoc_ck_xor2 not composed: proven impossible, no "
                "hazard-free network of at most 8 declared cells exists"),
            qPrintable(warning));
        QVERIFY(warning.contains("cell role qsoc_ck_inv not composed: proven impossible"));
        const QString output = root.filePath("output");
        const QString report = load(QDir(output).filePath("qsoc_cell/qsoc_cell_role.rpt"));
        QVERIFY(report.contains(
            "qsoc_ck_xor2:\n    unresolved: qsoc_role_unresolved_ck_xor2\n    synthesis: proven "
            "impossible, no hazard-free network"));
        const QString role = load(QDir(output).filePath("qsoc_cell/rtl/role/qsoc_ck_xor2.v"));
        QVERIFY(role.contains("qsoc_role_unresolved_ck_xor2 u_cell ("));
        QVERIFY(role.contains("and it was not composed:\n *          proven impossible"));
    }

    void budgetExhaustionStaysUnresolved()
    {
        QTemporaryDir root(QDir::tempPath() + "/test_qsoc_cellbinding-XXXXXX");
        QVERIFY(root.isValid());
        QVERIFY(makeProject(root.path(), icg + sync2 + nand2, "asic", "1000"));
        const QString netlist = root.filePath("cell_top.soc_net");
        QVERIFY(save(netlist, clockNetlist));
        generate(root.path(), netlist);
        QVERIFY2(
            messages.join('\n').contains(
                "cell role qsoc_ck_mux2 not composed: the solver "
                "budget of 1000 ran out, raise cell.synth_rlimit"),
            qPrintable(messages.join('\n')));
        const QString report = load(root.filePath("output/qsoc_cell/qsoc_cell_role.rpt"));
        QVERIFY(report.contains("synth_rlimit: 1000\n"));
        QVERIFY(report.contains(
            "qsoc_ck_mux2:\n    unresolved: qsoc_role_unresolved_ck_mux2\n"
            "    synthesis: the solver budget of 1000 ran out"));
        /* The default budget composes it. */
        const QSocCellBinding binding = bindYaml(icg + nand2);
        QVERIFY(!binding.roles().value("qsoc_ck_mux2").network.gate.isEmpty());
    }

    void budgetKeyIsChecked()
    {
        const auto load = [](const QString &value) {
            return QSocCellBinding::resolve(
                YAML::Load(
                    QString("cell: {target: asic, synth_rlimit: %1}").arg(value).toStdString()),
                YAML::Load((icg + nand2).toStdString()));
        };
        QVERIFY(load("0").isValid());
        QVERIFY(load("4294967295").isValid());
        for (const QString &bad : {"-1", "many", "4294967296", "1.5"}) {
            const QSocCellBinding binding = load(bad);
            QVERIFY2(!binding.isValid(), qPrintable(bad));
            QVERIFY(binding.errors().join('\n').contains("cell.synth_rlimit: must be an integer"));
        }
    }

    void composedGenerationElaborates_data() { composesRolesFromDeclaredCells_data(); }

    /* The whole clock controller elaborates on composed roles and the
     * contract of every composed role proves; a swapped wire fails it. */
    void composedGenerationElaborates()
    {
        QFETCH(QString, base);
        QTemporaryDir root(QDir::tempPath() + "/test_qsoc_cellbinding-XXXXXX");
        QVERIFY(root.isValid());
        QVERIFY(makeProject(root.path(), composedLibrary(base), "asic"));
        const QString netlist = root.filePath("cell_top.soc_net");
        QVERIFY(save(netlist, clockNetlist));
        generate(root.path(), netlist, true);
        const QString output = root.filePath("output");
        QVERIFY2(
            QFile::exists(QDir(output).filePath("cell_top/rtl/cell_top.v")),
            qPrintable(messages.join('\n')));
        if (QStandardPaths::findExecutable("iverilog").isEmpty())
            QSOC_TEST_MISSING_DEPENDENCY("iverilog");
        QString log;
        QVERIFY2(
            run("iverilog",
                QStringList{"-g2005", "-s", "cell_top", "-o", root.filePath("a.out")}
                    + listed(output, "qsoc.fl")
                    + listed(output, "qsoc_cell/model/qsoc_cell_model.fl"),
                output,
                &log),
            qPrintable(log));

        for (const char *tool : {"sby", "yosys", "yosys-abc", "z3"}) {
            if (QStandardPaths::findExecutable(tool).isEmpty())
                QSOC_TEST_MISSING_DEPENDENCY(tool);
        }
        const QSocCellBinding binding = bindYaml(composedLibrary(base));
        const QString         job     = QDir(output).filePath("qsoc_cell/formal/contract");
        const QString         tasks   = load(QDir(job).filePath("contract.sby"));
        QString               mutated;
        for (const QString &role : synthRoles) {
            if (binding.roles().value(role).network.gate.isEmpty())
                continue;
            QVERIFY2(tasks.contains("\n" + role + "\n"), qPrintable(role));
            QCOMPARE(sbyStatus(job, role, &log), QString("PASS"));
            if (role == "qsoc_ck_xor2" || !mutated.isEmpty())
                continue;
            /* Swap the first two role inputs of the network. */
            const QString file = QDir(output).filePath("qsoc_cell/rtl/role/" + role + ".v");
            QString       text = load(file);
            const QString from = role == "qsoc_ck_mux2" ? "(clk_sel)" : "(clk_in";
            const QString to   = role == "qsoc_ck_mux2" ? "(clk_in0)" : "(1'b1 ^ clk_in";
            if (!text.contains(from))
                continue;
            QVERIFY(save(file, text.replace(from, to)));
            mutated = role;
        }
        QVERIFY(!mutated.isEmpty());
        QDir(QDir(job).filePath("contract_" + mutated)).removeRecursively();
        QCOMPARE(sbyStatus(job, mutated, &log), QString("FAIL"));
    }

    void unresolvedRoleFailsOnlyWhereUsed_data()
    {
        QTest::addColumn<QString>("role");
        QTest::addColumn<bool>("elaborates");
        QTest::newRow("bound") << "qsoc_ck_or2" << true;
        QTest::newRow("unresolved") << "qsoc_ck_xor2" << false;
    }

    void unresolvedRoleFailsOnlyWhereUsed()
    {
        QFETCH(QString, role);
        QFETCH(bool, elaborates);
        if (QStandardPaths::findExecutable("iverilog").isEmpty())
            QSOC_TEST_MISSING_DEPENDENCY("iverilog");
        const QSocCellBinding binding = bindYaml(icg + or2);
        QTemporaryDir         dir(QDir::tempPath() + "/test_qsoc_cellbinding-XXXXXX");
        QVERIFY(dir.isValid());
        QStringList files = {"top.v"};
        for (const QSocCellLibrary::Cell &cell :
             QSocCellLibrary::roles(binding) + QSocCellLibrary::models(binding)) {
            QVERIFY(save(dir.filePath(cell.file), cell.text));
            files.append(cell.file);
        }
        QVERIFY(save(
            dir.filePath("top.v"),
            QString(
                "module top (input wire a, input wire b, output wire y);\n"
                "    %1 u_role (.clk_in0(a), .clk_in1(b), .clk_out(y));\nendmodule\n")
                .arg(role)));
        QString log;
        QCOMPARE(
            run("iverilog",
                QStringList{"-g2005", "-s", "top", "-o", "a.out"} + files,
                dir.path(),
                &log),
            elaborates);
        QCOMPARE(log.contains("qsoc_role_unresolved_ck_xor2"), !elaborates);
    }

    /* Latency is STAGES for any depth, padded with plain flops. */
    void syncDepthIsPadded_data()
    {
        QTest::addColumn<int>("cellStages");
        QTest::addColumn<int>("stages");
        QTest::addColumn<int>("resetValue");
        for (const int cellStages : {2, 3}) {
            for (int stages = 1; stages <= 5; ++stages) {
                for (const int resetValue : {0, 1})
                    QTest::newRow(qPrintable(QString("cell%1-stages%2-reset%3")
                                                 .arg(cellStages)
                                                 .arg(stages)
                                                 .arg(resetValue)))
                        << cellStages << stages << resetValue;
            }
        }
    }

    void syncDepthIsPadded()
    {
        QFETCH(int, cellStages);
        QFETCH(int, stages);
        QFETCH(int, resetValue);
        for (const char *tool : {"iverilog", "vvp"}) {
            if (QStandardPaths::findExecutable(tool).isEmpty())
                QSOC_TEST_MISSING_DEPENDENCY(tool);
        }
        const QSocCellBinding binding = bindYaml(
            icg
            + entry(
                "SYNCN",
                "CP D CDN",
                "Q",
                QString(
                    "sequential: {type: sync, stages: %1, clock: CP, data: D, output: Q, "
                    "reset: CDN}")
                    .arg(cellStages)));
        QVERIFY2(binding.isValid(), qPrintable(binding.errors().join('\n')));
        QTemporaryDir dir(QDir::tempPath() + "/test_qsoc_cellbinding-XXXXXX");
        QVERIFY(dir.isValid());
        const QString generic = roleText(QSocCellBinding(), "qsoc_sync")
                                    .replace("module qsoc_sync ", "module qsoc_sync_ref ");
        QVERIFY(save(dir.filePath("ref.v"), generic));
        QVERIFY(save(dir.filePath("role.v"), roleText(binding, "qsoc_sync")));
        QVERIFY(save(dir.filePath("model.v"), QSocCellBinding::model(binding.cells().last())));
        QVERIFY(save(
            dir.filePath("tb.v"),
            QString(R"(`timescale 1ns / 1ps
module tb;
    reg clk = 1'b0, rst_n = 1'b0, d = 1'b0;
    wire got, want;
    integer step, errors = 0;
    always #5 clk = ~clk;
    qsoc_sync #(.STAGES(%1), .RESET_VALUE(1'b%2)) u_dut (.clk(clk), .rst_n(rst_n), .d(d), .q(got));
    qsoc_sync_ref #(.STAGES(%1), .RESET_VALUE(1'b%2)) u_ref (.clk(clk), .rst_n(rst_n), .d(d), .q(want));
    initial begin
        #1 if (got !== 1'b%2 || want !== 1'b%2) errors = errors + 1;
        #16 rst_n = 1'b1;
        for (step = 0; step < 64; step = step + 1) begin
            @(negedge clk) d = $random;
            if (step == 40) rst_n = 1'b0;
            if (step == 42) rst_n = 1'b1;
            #1 if (got !== want) errors = errors + 1;
        end
        if (errors == 0) $display("TEST_PASS");
        $finish;
    end
endmodule
)")
                .arg(stages)
                .arg(resetValue)));
        QString log;
        QVERIFY2(
            run("iverilog",
                {"-g2005", "-s", "tb", "-o", "sim", "tb.v", "role.v", "ref.v", "model.v"},
                dir.path(),
                &log),
            qPrintable(log));
        QVERIFY2(run("vvp", {"-n", "sim"}, dir.path(), &log), qPrintable(log));
        QVERIFY2(log.contains("TEST_PASS"), qPrintable(log));
    }

    void asicGenerationElaborates()
    {
        QTemporaryDir root(QDir::tempPath() + "/test_qsoc_cellbinding-XXXXXX");
        QVERIFY(root.isValid());
        QVERIFY(makeProject(root.path(), fullLibrary(), "asic"));
        const QString netlist = root.filePath("cell_top.soc_net");
        QVERIFY(save(netlist, clockNetlist));
        generate(root.path(), netlist);
        const QString output = root.filePath("output");
        const QString top    = load(QDir(output).filePath("cell_top/rtl/cell_top.v"));
        QVERIFY2(!top.isEmpty(), qPrintable(messages.join('\n')));
        QVERIFY(top.contains("qsoc_ck_buf u_icg_guide (\n        .clk_in("));

        const QString mux = load(QDir(output).filePath("qsoc_cell/rtl/role/qsoc_ck_mux2.v"));
        QVERIFY(mux.contains("CKMUX2D2 u_cell ("));
        const QString report = load(QDir(output).filePath("qsoc_cell/qsoc_cell_role.rpt"));
        QVERIFY(report.contains(
            "qsoc_ck_icg_neg:\n    composed: [qsoc_ck_inv, qsoc_ck_icg_pos, qsoc_ck_inv]"));
        QVERIFY(messages.join('\n').contains("two extra inverter delays"));

        const QStringList models = listed(output, "qsoc_cell/model/qsoc_cell_model.fl");
        QCOMPARE(models.size(), 8);
        QVERIFY(QFile::exists(QDir(output).filePath("qsoc_cell/model/CKND2D2.v")));
        const QString qsocList = load(QDir(output).filePath("qsoc.fl"));
        const QString cellList = load(QDir(output).filePath("qsoc_cell/rtl/qsoc_cell.fl"));
        QVERIFY(!qsocList.contains("model") && !cellList.contains("model"));

        if (QStandardPaths::findExecutable("iverilog").isEmpty())
            QSOC_TEST_MISSING_DEPENDENCY("iverilog");
        QString log;
        QVERIFY2(
            run("iverilog",
                QStringList{"-g2005", "-s", "cell_top", "-o", root.filePath("a.out")}
                    + listed(output, "qsoc.fl") + models,
                output,
                &log),
            qPrintable(log));
    }

    void missingGateStopsAsicGeneration()
    {
        QTemporaryDir root(QDir::tempPath() + "/test_qsoc_cellbinding-XXXXXX");
        QVERIFY(root.isValid());
        QVERIFY(makeProject(root.path(), or2, "asic"));
        const QString netlist = root.filePath("cell_top.soc_net");
        QVERIFY(save(netlist, clockNetlist));
        generate(root.path(), netlist);
        QVERIFY(!QFile::exists(root.filePath("output/cell_top/rtl/cell_top.v")));
        QVERIFY(messages.join('\n').contains("no declared cell implements a clock gate"));
    }

    /* Generic projects and projects without the key write PR6 output. */
    void genericGenerationIgnoresDeclarations()
    {
        QString outputs[2];
        for (int i = 0; i < 2; ++i) {
            QTemporaryDir root(QDir::tempPath() + "/test_qsoc_cellbinding-XXXXXX");
            QVERIFY(root.isValid());
            QVERIFY(makeProject(root.path(), i ? fullLibrary() : QString(), i ? "generic" : ""));
            const QString netlist = root.filePath("cell_top.soc_net");
            QVERIFY(save(netlist, clockNetlist));
            generate(root.path(), netlist);
            const QString output = root.filePath("output");
            QString       all;
            for (const QString &file : listed(output, "qsoc.fl"))
                all += load(file);
            QVERIFY(!all.isEmpty());
            outputs[i] = all;
            QVERIFY(!QFile::exists(QDir(output).filePath("qsoc_cell/qsoc_cell_role.rpt")));
            QCOMPARE(
                QFile::exists(QDir(output).filePath("qsoc_cell/model/qsoc_cell_model.fl")), i == 1);
        }
        QCOMPARE(outputs[1], outputs[0]);
    }

    void contracts_data()
    {
        QTest::addColumn<QString>("task");
        const QStringList roles = QSocCellBinding::roleNames();
        for (const QString &task : QSocCellFormal::contractTasks(roles))
            QTest::newRow(qPrintable(task)) << task;
    }

    /* Every bound role meets its contract, the swapped witness breaks it. */
    void contracts()
    {
        QFETCH(QString, task);
        for (const char *tool : {"sby", "yosys", "yosys-abc", "z3"}) {
            if (QStandardPaths::findExecutable(tool).isEmpty())
                QSOC_TEST_MISSING_DEPENDENCY(tool);
        }
        QTemporaryDir root(QDir::tempPath() + "/test_qsoc_cellbinding-XXXXXX");
        QVERIFY(root.isValid());
        QVERIFY(makeProject(root.path(), fullLibrary(), "asic"));
        const QString netlist = root.filePath("cell_top.soc_net");
        QVERIFY(save(netlist, clockNetlist));
        generate(root.path(), netlist, true);
        const QString job = root.filePath("output/qsoc_cell/formal/contract");
        QVERIFY2(QFile::exists(QDir(job).filePath("contract.sby")), qPrintable(messages.join('\n')));
        QString log;
        QCOMPARE(sbyStatus(job, task, &log), QString("PASS"));
    }

    void contractCatchesFaults_data()
    {
        QTest::addColumn<QString>("task");
        QTest::addColumn<QString>("file");
        QTest::addColumn<QString>("from");
        QTest::addColumn<QString>("to");
        QTest::newRow("mux-swapped-inputs")
            << "qsoc_ck_mux2" << "qsoc_ck_mux2.v" << ".I0(clk_in0),\n        .I1(clk_in1),"
            << ".I0(clk_in1),\n        .I1(clk_in0),";
        QTest::newRow("gate-swapped-clock-and-enable")
            << "qsoc_ck_icg_pos" << "qsoc_ck_icg_pos.v" << ".CP(clk),\n        .E(en),"
            << ".CP(en),\n        .E(clk),";
        QTest::newRow("sync-reset-value-dropped")
            << "qsoc_sync_2_1" << "qsoc_sync.v" << "assign q = link[CELLS] ^ RESET_VALUE;"
            << "assign q = link[CELLS];";
    }

    void contractCatchesFaults()
    {
        QFETCH(QString, task);
        QFETCH(QString, file);
        QFETCH(QString, from);
        QFETCH(QString, to);
        for (const char *tool : {"sby", "yosys", "yosys-abc", "z3"}) {
            if (QStandardPaths::findExecutable(tool).isEmpty())
                QSOC_TEST_MISSING_DEPENDENCY(tool);
        }
        QTemporaryDir root(QDir::tempPath() + "/test_qsoc_cellbinding-XXXXXX");
        QVERIFY(root.isValid());
        QVERIFY(makeProject(root.path(), fullLibrary(), "asic"));
        const QString netlist = root.filePath("cell_top.soc_net");
        QVERIFY(save(netlist, clockNetlist));
        generate(root.path(), netlist, true);
        const QString role = root.filePath("output/qsoc_cell/rtl/role/" + file);
        QString       text = load(role);
        QVERIFY2(text.contains(from), qPrintable(text));
        QVERIFY(save(role, text.replace(from, to)));
        QString log;
        QCOMPARE(
            sbyStatus(root.filePath("output/qsoc_cell/formal/contract"), task, &log),
            QString("FAIL"));
    }

    /* In asic mode the cell job reads generic role copies and still proves. */
    void asicCellJobReadsGenericRoles()
    {
        for (const char *tool : {"sby", "yosys", "yosys-abc", "z3"}) {
            if (QStandardPaths::findExecutable(tool).isEmpty())
                QSOC_TEST_MISSING_DEPENDENCY(tool);
        }
        QTemporaryDir root(QDir::tempPath() + "/test_qsoc_cellbinding-XXXXXX");
        QVERIFY(root.isValid());
        QVERIFY(makeProject(root.path(), icg + or2, "asic"));
        const QString netlist = root.filePath("cell_top.soc_net");
        QVERIFY(save(netlist, clockNetlist));
        generate(root.path(), netlist, true);
        const QString job = root.filePath("output/qsoc_cell/formal");
        QVERIFY(load(QDir(job).filePath("check.sby")).contains("\nrole/qsoc_ck_xor2.v\n"));
        QVERIFY(load(QDir(job).filePath("qsoc_cell_formal.fl"))
                    .contains("qsoc_cell/formal/role/qsoc_sync.v\n"));
        QString log;
        run("sby", {"-f", "check.sby", "clk_role"}, job, &log, 1800000);
        QCOMPARE(
            load(QDir(job).filePath("check_clk_role/status")).section(' ', 0, 0).trimmed(),
            QString("PASS"));
    }

    /* Hand added keys survive a second module import of the same library. */
    void reimportKeepsDeclarations()
    {
        QTemporaryDir root(QDir::tempPath() + "/test_qsoc_cellbinding-XXXXXX");
        QVERIFY(root.isValid());
        QVERIFY(makeProject(root.path(), QString(), ""));
        const QString source = root.filePath("ckbd.v");
        QVERIFY(save(source, "module CKBD4 (input I, output Z);\n  assign Z = I;\nendmodule\n"));
        const auto import = [&]() {
            QSocCliWorker worker;
            worker
                .setup({"qsoc", "module", "import", "-d", root.path(), "-l", "cells", source}, false);
            worker.run();
        };
        import();
        const QString library = root.filePath("module/cells.soc_mod");
        YAML::Node    node    = YAML::LoadFile(library.toStdString());
        QVERIFY(node["CKBD4"]["port"]);
        node["CKBD4"]["function"]["Z"] = "I";
        std::ofstream(library.toStdString()) << node;
        import();
        const YAML::Node again = YAML::LoadFile(library.toStdString());
        QCOMPARE(
            QString::fromStdString(again["CKBD4"]["function"]["Z"].as<std::string>()), QString("I"));
        QVERIFY(again["CKBD4"]["port"]["I"]);
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsoccellbinding.moc"
