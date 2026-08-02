// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "cli/qsoccliworker.h"
#include "common/qsocconsole.h"
#include "common/qsocprojectmanager.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QStringList>
#include <QTextStream>
#include <QtCore>
#include <QtTest>

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

    QString createTempFile(const QString &fileName, const QString &content)
    {
        const QString filePath = QDir(projectManager.getCurrentPath()).filePath(fileName);
        QFile         file(filePath);
        if (file.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&file);
            stream << content;
            file.close();
            return filePath;
        }
        return {};
    }

    void createSubModule()
    {
        QDir moduleDir(projectManager.getModulePath());
        if (!moduleDir.exists()) {
            moduleDir.mkpath(".");
        }
        QFile moduleFile(moduleDir.filePath("sub_mod.soc_mod"));
        if (moduleFile.open(QIODevice::WriteOnly | QIODevice::Text)) {
            QTextStream stream(&moduleFile);
            stream << R"(
sub_mod:
  port:
    i:
      type: logic[7:0]
      direction: in
    o:
      type: logic[7:0]
      direction: out
)";
            moduleFile.close();
        }
    }

    QString createNibbleModule()
    {
        return createTempFile("module/nibble_mod.soc_mod", R"(
nibble_mod:
  port:
    o:
      type: logic[3:0]
      direction: out
)");
    }

    QString createIoModule()
    {
        return createTempFile("module/io_mod.soc_mod", R"(
io_mod:
  port:
    pad:
      type: logic
      direction: inout
    drive:
      type: logic
      direction: output
)");
    }

    /* Generate one netlist and hand back the emitted Verilog. */
    QString generate(const QString &name, const QString &netlistContent)
    {
        const QString netlistPath = createTempFile(name + ".soc_net", netlistContent);
        if (netlistPath.isEmpty()) {
            return {};
        }
        const QString outputPath = QDir(projectManager.getOutputPath()).filePath(name + ".v");
        if (QFileInfo::exists(outputPath) && !QFile::remove(outputPath)) {
            return {};
        }
        {
            QSocCliWorker socCliWorker;
            QStringList   args;
            args << "qsoc" << "generate" << "verilog" << "-d" << projectManager.getCurrentPath()
                 << netlistPath;
            socCliWorker.setup(args, false);
            socCliWorker.run();
        }
        const QString successMessage
            = QString("Successfully generated Verilog code: %1").arg(outputPath);
        if (!messageList.join('\n').contains(successMessage)) {
            return {};
        }
        QFile outputFile(outputPath);
        if (!outputFile.open(QIODevice::ReadOnly | QIODevice::Text)) {
            return {};
        }
        QTextStream stream(&outputFile);
        return stream.readAll();
    }

private slots:
    void initTestCase()
    {
        qInstallMessageHandler(messageOutput);
        QSocConsole::setTeeToMessageHandler(true);
        projectName = QFileInfo(__FILE__).baseName() + "_data";
        projectManager.setProjectName(projectName);
        projectManager.setCurrentPath(QDir::current().filePath(projectName));
        projectManager.mkpath();
        projectManager.save(projectName);
        projectManager.load(projectName);
        createSubModule();
        QVERIFY(!createNibbleModule().isEmpty());
    }

    void cleanupTestCase()
    {
#ifdef ENABLE_TEST_CLEANUP
        QDir projectDir(projectManager.getCurrentPath());
        if (projectDir.exists()) {
            projectDir.removeRecursively();
        }
#endif // ENABLE_TEST_CLEANUP
    }

    void init() { messageList.clear(); }

    /* An explicit `instance: top` binding has to reach the sequential target.
       Pre-fix the net stayed an implicit scalar wire, so the register was one
       bit wide and neither bound output was driven. */
    void testExplicitTopBindingDrivesSequentialTarget()
    {
        const QString verilog = generate("connected_explicit_top", R"(
port:
  clk:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  d:
    direction: input
    type: logic[7:0]
  q_a:
    direction: output
    type: logic[7:0]
  q_b:
    direction: output
    type: logic[7:0]

net:
  shared_q:
    - instance: top
      port: q_a
    - instance: top
      port: q_b

seq:
  - reg: shared_q
    clk: clk
    rst: rst_n
    rst_val: "8'h00"
    next: d
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("reg [7:0] q_a_reg;"));
        QVERIFY(verilog.contains("assign q_a = q_a_reg;"));
        QVERIFY(verilog.contains("assign q_b = q_a;"));
        /* The bare net must not survive as an undeclared identifier. */
        QVERIFY(!verilog.contains("shared_q"));
    }

    /* A sequential target reached through an all-output alias group must be
       sized from the canonical port instead of collapsing to a scalar. */
    void testSequentialTargetThroughAliasGroupKeepsWidth()
    {
        const QString verilog = generate("connected_seq_alias", R"(
port:
  clk:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  d:
    direction: input
    type: logic[7:0]
  q_a:
    direction: output
    type: logic[7:0]
    connect: shared_q
  q_b:
    direction: output
    type: logic[7:0]
    connect: shared_q


seq:
  - reg: shared_q
    clk: clk
    rst: rst_n
    rst_val: "8'h00"
    next: d
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("reg [7:0] q_a_reg;"));
        QVERIFY(verilog.contains("assign q_a = q_a_reg;"));
        QVERIFY(verilog.contains("assign q_b = q_a;"));
        QVERIFY(!verilog.contains("shared_q"));
    }

    /* When the net name is itself a top-level output, that port carries the
       net. Pre-fix the combinational target was rewritten to the aliasing
       port, which left the port the user named undriven. */
    void testNetNamedAfterTopPortKeepsCombinationalTarget()
    {
        const QString verilog = generate("connected_net_named_port", R"(
port:
  d:
    direction: input
    type: logic[7:0]
  q_a:
    direction: output
    type: logic[7:0]
  q_b:
    direction: output
    type: logic[7:0]
    connect: q_a


comb:
  - out: q_a
    expr: d
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign q_a = d;"));
        QVERIFY(verilog.contains("assign q_b = q_a;"));
        QVERIFY(!verilog.contains("assign q_b = d;"));
    }

    /* Instance wiring and alias emission must pick the same canonical member.
       Pre-fix routing took the container-ordered first port while the alias
       took the first declared one, double-driving one output and leaving the
       other undriven. */
    void testInstanceWiringAndAliasAgreeOnCanonicalPort()
    {
        const QString verilog = generate("connected_canonical_agreement", R"(
port:
  d:
    direction: input
    type: logic[7:0]
  z_out:
    direction: output
    type: logic[7:0]
    connect: shared_n
  a_out:
    direction: output
    type: logic[7:0]
    connect: shared_n

instance:
  u_sub:
    module: sub_mod

net:
  shared_n:
    - instance: u_sub
      port: o
  din_n:
    - instance: u_sub
      port: i
    - instance: top
      port: d
)");
        QVERIFY(!verilog.isEmpty());
        /* First declared member wins for both consumers. */
        QVERIFY(verilog.contains(".o(z_out)"));
        QVERIFY(verilog.contains("assign a_out = z_out;"));
        QVERIFY(!verilog.contains(".o(a_out)"));
        /* The explicit top binding reaches the module input as well. */
        QVERIFY(verilog.contains(".i(d)"));
        QVERIFY(!verilog.contains("din_n"));
    }

    /* A process may name any member of an alias group. Writing to a
       non-canonical member drove it twice, once from the process and once
       from the group's own alias assignment. */
    void testProcessTargetOnSecondaryAliasMemberResolves()
    {
        const QString verilog = generate("connected_secondary_member", R"(
port:
  d:
    direction: input
    type: logic[7:0]
  z_out:
    direction: output
    type: logic[7:0]
    connect: shared_n
  a_out:
    direction: output
    type: logic[7:0]
    connect: shared_n

comb:
  - out: a_out
    expr: d
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign z_out = d;"));
        QVERIFY(verilog.contains("assign a_out = z_out;"));
        QVERIFY(!verilog.contains("assign a_out = d;"));
    }

    /* Declaration order decides the canonical member even when the group is
       assembled from both binding spellings. */
    void testMixedBindingSpellingsFollowDeclarationOrder()
    {
        const QString verilog = generate("connected_mixed_spelling", R"(
port:
  d:
    direction: input
    type: logic[7:0]
  z_out:
    direction: output
    type: logic[7:0]
  a_out:
    direction: output
    type: logic[7:0]
    connect: shared_n

instance:
  u_sub:
    module: sub_mod

net:
  shared_n:
    - instance: u_sub
      port: o
    - instance: top
      port: z_out
  din_n:
    - instance: u_sub
      port: i
    - instance: top
      port: d
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains(".o(z_out)"));
        QVERIFY(verilog.contains("assign a_out = z_out;"));
    }

    /* Explicit bindings listed against declaration order must not reorder the
       group; otherwise the canonical member follows how the net was written. */
    void testExplicitBindingsListedOutOfOrderKeepDeclarationOrder()
    {
        const QString verilog = generate("connected_explicit_out_of_order", R"(
port:
  d:
    direction: input
    type: logic[7:0]
  z_out:
    direction: output
    type: logic[7:0]
  a_out:
    direction: output
    type: logic[7:0]

instance:
  u_sub:
    module: sub_mod

net:
  shared_n:
    - instance: u_sub
      port: o
    - instance: top
      port: a_out
    - instance: top
      port: z_out
  din_n:
    - instance: u_sub
      port: i
    - instance: top
      port: d
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains(".o(z_out)"));
        QVERIFY(verilog.contains("assign a_out = z_out;"));
    }

    /* A top binding that carries `bits` binds only that slice. Treating it as
       a whole-port carrier let a sequential target capture the bits the
       netlist never bound. */
    void testSlicedTopBindingDrivesOnlyItsSlice()
    {
        const QString verilog = generate("connected_sliced_binding", R"(
port:
  clk:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  d:
    direction: input
    type: logic[7:0]
  q:
    direction: output
    type: logic[7:0]

net:
  low_n:
    - instance: top
      port: q
      bits: "[3:0]"

seq:
  - reg: low_n
    clk: clk
    rst: rst_n
    rst_val: "4'h0"
    next: d[3:0]
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign q[3:0] = q_reg[3:0];"));
        QVERIFY(!verilog.contains("assign q = q_reg;"));
    }

    /* The sliced-binding rule has to hold on the combinational path too, not
       just the sequential one. */
    void testCombinationalTargetThroughSlicedBinding()
    {
        const QString verilog = generate("connected_sliced_comb", R"(
port:
  d:
    direction: input
    type: logic[7:0]
  q:
    direction: output
    type: logic[7:0]

net:
  low_n:
    - instance: top
      port: q
      bits: "[3:0]"

comb:
  - out: low_n
    expr: d[3:0]
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign q[3:0] = d[3:0];"));
        QVERIFY(!verilog.contains("assign q = d[3:0];"));
    }

    /* Only an all-output group is driven by an alias assignment. Folding the
       members of a mixed-direction group would redirect a legal output target
       onto an input port and leave the output undriven. */
    void testMixedDirectionGroupKeepsMemberIdentity()
    {
        const QString verilog = generate("connected_mixed_direction", R"(
port:
  a_in:
    direction: input
    type: logic[7:0]
    connect: shared_n
  z_out:
    direction: output
    type: logic[7:0]
    connect: shared_n

comb:
  - out: z_out
    expr: a_in
)");
        QVERIFY(!verilog.isEmpty());
        QCOMPARE(verilog.count("assign z_out = a_in;"), 1);
        QVERIFY(!verilog.contains("tried to drive top-level input"));
        /* The bound input and the explicit comb driver are two sources of the
           net, the same diagnosis a bound input slice gets. */
        QVERIFY(verilog.contains(
            "FIXME: net shared_n is already driven but binds a_in - multi-driver conflict"));
    }

    /* One input member sources the net; every output member follows it,
       whichever member is declared first. */
    void testMixedMembersWireTheSinksFromTheSource()
    {
        for (const bool inputFirst : {true, false}) {
            const QString ports   = inputFirst ? R"(
  a_in:
    direction: input
    type: logic[7:0]
    connect: shared_n
  z_out:
    direction: output
    type: logic[7:0]
    connect: shared_n
)"
                                               : R"(
  z_out:
    direction: output
    type: logic[7:0]
    connect: shared_n
  a_in:
    direction: input
    type: logic[7:0]
    connect: shared_n
)";
            const QString verilog = generate(
                QString("connected_mixed_source_%1").arg(inputFirst ? "io" : "oi"),
                QString(R"(
port:%1
  y:
    direction: output
    type: logic
  a:
    direction: input
    type: logic

comb:
  - out: y
    expr: a
)")
                    .arg(ports));
            QVERIFY(!verilog.isEmpty());
            QCOMPARE(verilog.count("assign z_out = a_in;"), 1);
            QVERIFY(!verilog.contains("mixed direction"));
            QVERIFY(!verilog.contains("is undriven"));
        }
    }

    void testDefaultInputDirectionSourcesTheOutput()
    {
        const QString verilog = generate("connected_default_input", R"(
port:
  src:
    type: logic
    connect: shared
  y:
    direction: output
    type: logic
    connect: shared
  a:
    direction: input
    type: logic
  r:
    direction: output
    type: logic

comb:
  - out: r
    expr: a
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("input wire src"));
        QVERIFY(verilog.contains("assign y = src;"));
        QVERIFY(!verilog.contains("whose direction gives no ownership"));
    }

    void testUnownedEndpointStopsComponentAliasing()
    {
        const QString modulePath = createIoModule();
        QVERIFY(!modulePath.isEmpty());

        const QString verilog = generate("connected_inout", R"(
port:
  src:
    direction: input
    type: logic
    connect: shared
  pad:
    direction: inout
    type: logic
    connect: shared
  y:
    direction: output
    type: logic
    connect: shared
  a:
    direction: input
    type: logic
  r:
    direction: output
    type: logic
  alias_head:
    direction: output
    type: logic
    connect: alias_net
  alias_slice:
    direction: output
    type: logic[0:0]

instance:
  u_alias:
    module: io_mod

net:
  alias_net:
    - { instance: u_alias, port: pad }
  alias_bind:
    - { instance: top, port: alias_head }
    - { instance: top, port: alias_slice, bits: "[0]" }

comb:
  - out: r
    expr: a
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("whose direction gives no ownership"));
        QVERIFY(!verilog.contains("assign y = src;"));
        QVERIFY(!verilog.contains("assign y = pad;"));
        QVERIFY(!verilog.contains("assign pad ="));
        QVERIFY(!verilog.contains("assign src ="));
        QVERIFY(verilog.contains(".pad(alias_head)"));
        QVERIFY(!verilog.contains("assign alias_slice[0] = alias_head;"));
    }

    void testKnownInoutUsesItsOnlyTopEndpoint()
    {
        const QString modulePath = createIoModule();
        QVERIFY(!modulePath.isEmpty());

        const QString verilog = generate("connected_single_inout", R"(
port:
  pad:
    direction: inout
    type: logic
    connect: shared
  src:
    direction: input
    type: logic
    connect: src_shared
  bare_src:
    direction: input
    type: logic
  driven_pad:
    direction: inout
    type: logic
    connect: driven_shared

instance:
  u_io:
    module: io_mod
  u_input:
    module: io_mod
  u_bare:
    module: io_mod
  u_driver:
    module: io_mod

net:
  shared:
    - { instance: u_io, port: pad }
  src_shared:
    - { instance: u_input, port: pad }
  bare_src:
    - { instance: u_bare, port: pad }
  driven_shared:
    - { instance: u_driver, port: drive }
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains(".pad(pad)"));
        QVERIFY(verilog.contains(".pad(src)"));
        QVERIFY(verilog.contains(".pad(bare_src)"));
        QVERIFY(verilog.contains(".drive(driven_pad)"));
        QVERIFY(!verilog.contains(".pad(shared)"));
        QVERIFY(!verilog.contains("wire shared;"));
        QVERIFY(!verilog.contains("direction gives no ownership"));
    }

    /* A bound input slice is the one source of its component; a whole
       output member is one of its sinks, not a driver. */
    void testInputSliceSourcesAWholeMember()
    {
        const QString verilog = generate("connected_slice_to_member", R"(
port:
  a_in:
    direction: input
    type: logic[7:0]
  z_out:
    direction: output
    type: logic[7:0]
    connect: n1
  y:
    direction: output
    type: logic
  a:
    direction: input
    type: logic

net:
  n1:
    - { instance: top, port: a_in, bits: "[7:0]" }

comb:
  - out: y
    expr: a
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign z_out = a_in[7:0];"));
        QVERIFY(!verilog.contains("is undriven"));
        QVERIFY(!verilog.contains("ambiguous"));
    }

    /* Two whole input members are two sources; the bound output slice is
       diagnosed and never silently wired from either. */
    void testTwoWholeSourcesAndASliceSinkAreAmbiguous()
    {
        const QString verilog = generate("connected_two_whole_sources", R"(
port:
  a:
    direction: input
    type: logic[3:0]
    connect: n2
  b:
    direction: input
    type: logic[3:0]
    connect: n2
  y:
    direction: output
    type: logic[7:0]
  w:
    direction: output
    type: logic
  c:
    direction: input
    type: logic

net:
  n2:
    - { instance: top, port: y, bits: "[3:0]" }

comb:
  - out: w
    expr: c
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("binds 2 sources - ambiguous driver, not wired"));
        QVERIFY(!verilog.contains("assign y[3:0]"));
    }

    /* A process form on a top-level input is the same illegal driver as an
       expression on one, in comb and in seq alike. */
    void testProcessFormsCannotDriveATopInput()
    {
        const QString verilog = generate("connected_input_process", R"(
port:
  clk:
    direction: input
    type: logic
  a_in:
    direction: input
    type: logic[7:0]
  b_in:
    direction: input
    type: logic[7:0]
  s:
    direction: input
    type: logic

comb:
  - out: a_in
    if:
      - cond: s
        then: "8'h1"
    default: "8'h0"
seq:
  - reg: b_in
    clk: clk
    next: "b_in + 1"
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("FIXME: comb tried to drive top-level input a_in"));
        QVERIFY(verilog.contains("FIXME: seq tried to drive top-level input b_in"));
        QVERIFY(!verilog.contains("a_in_reg"));
        QVERIFY(!verilog.contains("b_in_reg"));
        QVERIFY(!verilog.contains("assign a_in"));
        QVERIFY(!verilog.contains("assign b_in"));
    }

    void testDefaultInputDirectionsCannotBeDriven()
    {
        const QString verilog = generate("connected_default_input_process", R"(
port:
  clk:
    direction: input
    type: logic
  data:
    direction: input
    type: logic
  comb_default:
    type: logic
  comb_invalid:
    direction: sideways
    type: logic
  seq_default:
    type: logic
  seq_invalid:
    direction: sideways
    type: logic

comb:
  - out: comb_default
    expr: data
  - out: comb_invalid
    expr: data

seq:
  - reg: seq_default
    clk: clk
    next: data
  - reg: seq_invalid
    clk: clk
    next: data
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("input wire comb_default"));
        QVERIFY(verilog.contains("input wire comb_invalid"));
        QVERIFY(verilog.contains("input wire seq_default"));
        QVERIFY(verilog.contains("input wire seq_invalid"));
        QVERIFY(verilog.contains("comb tried to drive top-level input comb_default"));
        QVERIFY(verilog.contains("comb tried to drive top-level input comb_invalid"));
        QVERIFY(verilog.contains("seq tried to drive top-level input seq_default"));
        QVERIFY(verilog.contains("seq tried to drive top-level input seq_invalid"));
        QVERIFY(!verilog.contains("assign comb_default"));
        QVERIFY(!verilog.contains("assign comb_invalid"));
        QVERIFY(!verilog.contains("assign seq_default"));
        QVERIFY(!verilog.contains("assign seq_invalid"));
        QVERIFY(!verilog.contains("comb_default_reg"));
        QVERIFY(!verilog.contains("comb_invalid_reg"));
        QVERIFY(!verilog.contains("seq_default_reg"));
        QVERIFY(!verilog.contains("seq_invalid_reg"));
    }

    void testSkippedInputProcessDoesNotClaimTheAlias()
    {
        const QString verilog = generate("owner_skipped_input_process", R"(
port:
  clk:
    direction: input
    type: logic
  a:
    direction: input
    type: logic
  src_c:
    direction: input
    type: logic
    connect: comb_net
  y_c:
    direction: output
    type: logic
    connect: comb_net
  src_s:
    direction: input
    type: logic
    connect: seq_net
  y_s:
    direction: output
    type: logic
    connect: seq_net
  src_default:
    type: logic
    connect: default_net
  y_default:
    direction: output
    type: logic
    connect: default_net
  src_invalid:
    direction: sideways
    type: logic
    connect: invalid_net
  y_invalid:
    direction: output
    type: logic
    connect: invalid_net

comb:
  - out: src_c
    expr: a
  - out: src_default
    expr: a

seq:
  - reg: src_s
    clk: clk
    next: a
  - reg: src_invalid
    clk: clk
    next: a
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("FIXME: comb tried to drive top-level input src_c"));
        QVERIFY(verilog.contains("FIXME: seq tried to drive top-level input src_s"));
        QVERIFY(verilog.contains("FIXME: comb tried to drive top-level input src_default"));
        QVERIFY(verilog.contains("FIXME: seq tried to drive top-level input src_invalid"));
        QVERIFY(verilog.contains("assign y_c = src_c;"));
        QVERIFY(verilog.contains("assign y_s = src_s;"));
        QVERIFY(verilog.contains("assign y_default = src_default;"));
        QVERIFY(verilog.contains("assign y_invalid = src_invalid;"));
        QVERIFY(!verilog.contains("net comb_net is already driven"));
        QVERIFY(!verilog.contains("net seq_net is already driven"));
        QVERIFY(!verilog.contains("net default_net is already driven"));
        QVERIFY(!verilog.contains("net invalid_net is already driven"));
        QVERIFY(!verilog.contains("multi-driver conflict"));
    }

    /* A whole-base comb assign and a seq slice of the same base overlap on
       bit ranges even though their spellings differ. */
    void testCombWholeAndSeqSliceConflict()
    {
        const QString verilog = generate("connected_comb_seq_overlap", R"(
port:
  clk:
    direction: input
    type: logic
  d:
    direction: input
    type: logic[7:0]
  q:
    direction: output
    type: logic[7:0]

comb:
  - out: q
    expr: d
seq:
  - reg: "q[3:0]"
    clk: clk
    next: "d[3:0]"
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign q = d;"));
        QVERIFY(verilog.contains("FIXME: comb also drives q[3:0] - multi-driver conflict"));
    }

    /* Two slices of one port on one net are two distinct sinks; the second
       is not a duplicate of the first. */
    void testTwoSlicesOfOnePortBothCarryTheNet()
    {
        const QString verilog = generate("connected_two_slices_one_port", R"(
port:
  d:
    direction: input
    type: logic[3:0]
  q:
    direction: output
    type: logic[7:0]

net:
  w:
    - { instance: top, port: q, bits: "[3:0]" }
    - { instance: top, port: q, bits: "[7:4]" }

comb:
  - out: w
    expr: d
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign q[3:0] = d;"));
        QVERIFY(verilog.contains("assign q[7:4] = q[3:0];"));
        QVERIFY(!verilog.contains("more than once"));
    }

    /* A sink slice narrower than its source is flagged instead of silently
       truncating. */
    void testSliceSinkWidthMismatchIsFlagged()
    {
        const QString verilog = generate("connected_slice_width_mismatch", R"(
port:
  d:
    direction: input
    type: logic[7:0]
  p_all:
    direction: output
    type: logic[7:0]
    connect: shared
  p_a:
    direction: output
    type: logic[7:0]

net:
  shared:
    - { instance: top, port: p_a, bits: "[1:0]" }

comb:
  - out: shared
    expr: d
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign p_all = d;"));
        QVERIFY(verilog.contains(
            "FIXME: p_a[1:0] is 2 bits but its source p_all is 8 bits - width mismatch"));
        QVERIFY(verilog.contains("assign p_a[1:0] = p_all;"));
    }

    /* A sliced member carries only part of the net, so it cannot be the whole
       net's carrier while a whole-port member exists, even when it is declared
       first. */
    void testWholePortMemberOutranksSlicedMember()
    {
        const QString verilog = generate("connected_sliced_first", R"(
port:
  d:
    direction: input
    type: logic[7:0]
  p_low:
    direction: output
    type: logic[7:0]
  p_all:
    direction: output
    type: logic[7:0]
    connect: shared_n

instance:
  u_sub:
    module: sub_mod

net:
  shared_n:
    - instance: u_sub
      port: o
    - instance: top
      port: p_low
      bits: "[3:0]"
  n_in:
    - instance: u_sub
      port: i
    - instance: top
      port: d
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains(".o(p_all)"));
        QVERIFY(!verilog.contains(".o(p_low)"));
        QVERIFY(verilog.contains(".i(d)"));
    }

    /* `connect:` is transitive: chained spellings name one signal. Grouping
       per binding let a port carry one group while being a member of another,
       and both groups then assigned onto it. */
    void testChainedConnectFormsOneGroup()
    {
        const QString verilog = generate("connected_chain", R"(
port:
  d:
    direction: input
    type: logic[7:0]
  q:
    direction: output
    type: logic[7:0]
  mid:
    direction: output
    type: logic[7:0]
    connect: q
  a_out:
    direction: output
    type: logic[7:0]
    connect: mid

comb:
  - out: a_out
    expr: d
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign q = d;"));
        QVERIFY(verilog.contains("assign mid = q;"));
        QVERIFY(verilog.contains("assign a_out = q;"));
        /* One assignment per member, so no member is driven twice. */
        QCOMPARE(verilog.count("assign mid ="), 1);
        QCOMPARE(verilog.count("assign a_out ="), 1);
        QCOMPARE(verilog.count("assign q ="), 1);
    }

    /* A sliced binding through a secondary alias member names the canonical
       signal without changing the interval. */
    void testAliasSliceProcessResolvesToCanonical()
    {
        const QString verilog = generate("connected_alias_conflict", R"(
port:
  d:
    direction: input
    type: logic[3:0]
  y_p:
    direction: output
    type: logic[7:0]
    connect: shared_n
  m_p:
    direction: output
    type: logic[7:0]
    connect: shared_n

net:
  drv_n:
    - { instance: top, port: m_p, bits: "[3:0]" }

comb:
  - out: drv_n
    expr: d
)");
        QVERIFY(!verilog.isEmpty());
        QCOMPARE(verilog.count("assign y_p[3:0] ="), 1);
        QVERIFY(verilog.contains("assign y_p[3:0] = d;"));
        QCOMPARE(verilog.count("assign m_p = y_p;"), 1);
        QVERIFY(!verilog.contains("assign m_p[3:0] = d;"));
        QVERIFY(!verilog.contains("another driver also reaches"));
        QVERIFY(!verilog.contains("width mismatch"));
    }

    void testAliasSliceInstanceResolvesToCanonical()
    {
        const QString verilog = generate("connected_alias_instance", R"(
port:
  y_p:
    direction: output
    type: logic[7:0]
    connect: shared_n
  m_p:
    direction: output
    type: logic[7:0]
    connect: shared_n

instance:
  u_nibble:
    module: nibble_mod

net:
  drv_n:
    - { instance: top, port: m_p, bits: "[3:0]" }
    - { instance: u_nibble, port: o }
)");
        QVERIFY(!verilog.isEmpty());
        QCOMPARE(verilog.count(".o(y_p[3:0])"), 1);
        QCOMPARE(verilog.count("assign m_p = y_p;"), 1);
        QVERIFY(!verilog.contains("assign y_p[3:0] ="));
        QVERIFY(!verilog.contains(".o(m_p[3:0])"));
        QVERIFY(!verilog.contains("another driver also reaches"));
        QVERIFY(!verilog.contains("width mismatch"));
    }

    /* A process driving the group's primary is the normal shape and must not
       be reported as a conflict. */
    void testAliasDrivenFromItsPrimaryIsNotDiagnosed()
    {
        const QString verilog = generate("connected_alias_no_conflict", R"(
port:
  d:
    direction: input
    type: logic[7:0]
  q_a:
    direction: output
    type: logic[7:0]
  q_b:
    direction: output
    type: logic[7:0]
    connect: q_a

comb:
  - out: q_a
    expr: d
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign q_a = d;"));
        QVERIFY(verilog.contains("assign q_b = q_a;"));
        QVERIFY(!verilog.contains("multi-driver conflict"));
    }

    /* One net, one value: a net binding several slices delivers the driven
       value to every one of them, whatever form drives it. */
    void testSlicedOnlyNetFansOutFromExpression()
    {
        const QString verilog = generate("fanout_expr", R"(
port:
  d:
    direction: input
    type: logic[7:0]
  p_a:
    direction: output
    type: logic[7:0]
  p_b:
    direction: output
    type: logic[7:0]

net:
  parts:
    - { instance: top, port: p_a, bits: "[1:0]" }
    - { instance: top, port: p_b, bits: "[5:4]" }

comb:
  - out: parts
    expr: "d[1:0]"
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign p_a[1:0] = d[1:0];"));
        QVERIFY(verilog.contains("assign p_b[5:4] = p_a[1:0];"));
    }

    void testSlicedOnlyNetFansOutFromProcessForm()
    {
        const QString verilog = generate("fanout_if", R"(
port:
  sel:
    direction: input
    type: logic
  p_a:
    direction: output
    type: logic[7:0]
  p_b:
    direction: output
    type: logic[7:0]

net:
  parts:
    - { instance: top, port: p_a, bits: "[1:0]" }
    - { instance: top, port: p_b, bits: "[5:4]" }

comb:
  - out: parts
    default: "2'h0"
    if:
      - cond: sel
        then: "2'h3"
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign p_a[1:0] = p_a_reg[1:0];"));
        QVERIFY(verilog.contains("assign p_b[5:4] = p_a[1:0];"));
    }

    void testSlicedOnlyNetFansOutFromSequential()
    {
        const QString verilog = generate("fanout_seq", R"(
port:
  clk:
    direction: input
    type: logic
  p_a:
    direction: output
    type: logic[7:0]
  p_b:
    direction: output
    type: logic[7:0]

net:
  parts:
    - { instance: top, port: p_a, bits: "[1:0]" }
    - { instance: top, port: p_b, bits: "[5:4]" }

seq:
  - reg: parts
    clk: clk
    next: "2'h1"
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign p_a[1:0] = p_a_reg[1:0];"));
        QVERIFY(verilog.contains("assign p_b[5:4] = p_a[1:0];"));
    }

    /* Endpoint roles are fixed by direction: a bound top-input slice sources
       the net and every bound top-output slice sinks it, whatever order the
       netlist spells them in. */
    void testInputSliceSourcesTheOutputSinks()
    {
        const char   *shape    = R"(
port:
  o_hi:
    direction: output
    type: logic[7:0]
  i_lo:
    direction: input
    type: logic[7:0]
  r:
    direction: output
    type: logic

net:
  sig:
%1

comb:
  - out: r
    expr: "1'b1"
)";
        const QString srcFirst = QString(shape).arg(
            "    - { instance: top, port: i_lo, bits: \"[7:4]\" }\n"
            "    - { instance: top, port: o_hi, bits: \"[3:0]\" }");
        const QString sinkFirst = QString(shape).arg(
            "    - { instance: top, port: o_hi, bits: \"[3:0]\" }\n"
            "    - { instance: top, port: i_lo, bits: \"[7:4]\" }");
        for (const auto &pair :
             {qMakePair(QStringLiteral("owner_src_a"), srcFirst),
              qMakePair(QStringLiteral("owner_src_b"), sinkFirst)}) {
            const QString verilog = generate(pair.first, pair.second);
            QVERIFY(!verilog.isEmpty());
            QVERIFY(verilog.contains("assign o_hi[3:0] = i_lo[7:4];"));
            QVERIFY(!verilog.contains("assign i_lo"));
            /* The bound input slice is the net's source; the sink must not
               be misreported as undriven. */
            QVERIFY(!verilog.contains("is undriven"));
        }
    }

    /* A process driving the net and a bound input slice are two sources;
       the conflict is reported and the process keeps the sinks. */
    void testProcessAndInputSliceAreTwoSources()
    {
        const QString verilog = generate("owner_two_sources", R"(
port:
  o_hi:
    direction: output
    type: logic[7:0]
  i_lo:
    direction: input
    type: logic[7:0]

net:
  sig:
    - { instance: top, port: o_hi, bits: "[3:0]" }
    - { instance: top, port: i_lo, bits: "[7:4]" }

comb:
  - out: sig
    expr: "4'hA"
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign o_hi[3:0] = 4'hA;"));
        QVERIFY(!verilog.contains("assign i_lo"));
        QVERIFY(verilog.contains("is already driven but binds i_lo[7:4]"));
    }

    void testDisjointCombSliceKeepsTheAliasConnection()
    {
        const QString verilog = generate("owner_disjoint_comb", R"(
port:
  src:
    direction: input
    type: logic[3:0]
  d:
    direction: input
    type: logic[3:0]
  q:
    direction: output
    type: logic[7:0]

net:
  sig:
    - { instance: top, port: src, bits: "[3:0]" }
    - { instance: top, port: q, bits: "[3:0]" }

comb:
  - out: q
    bits: "[7:4]"
    expr: d
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign q[7:4] = d;"));
        QVERIFY(verilog.contains("assign q[3:0] = src[3:0];"));
        QVERIFY(!verilog.contains("multi-driver conflict"));
    }

    void testDrivenSliceBecomesTheAliasCarrier()
    {
        const QString verilog = generate("owner_driven_slice", R"(
port:
  d:
    direction: input
    type: logic[3:0]
  q:
    direction: output
    type: logic[7:0]

net:
  w:
    - { instance: top, port: q, bits: "[3:0]" }
    - { instance: top, port: q, bits: "[7:4]" }

comb:
  - out: q
    bits: "[7:4]"
    expr: d
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign q[7:4] = d;"));
        QVERIFY(verilog.contains("assign q[3:0] = q[7:4];"));
        QVERIFY(!verilog.contains("assign q[7:4] = q[3:0];"));
        QVERIFY(!verilog.contains("multi-driver conflict"));
    }

    void testWholeDriverCoversTheBoundSlice()
    {
        const QString verilog = generate("owner_whole_driver", R"(
port:
  d:
    direction: input
    type: logic[7:0]
  q:
    direction: output
    type: logic[7:0]
  r:
    direction: output
    type: logic[7:0]

net:
  w:
    - { instance: top, port: q, bits: "[3:0]" }
    - { instance: top, port: r, bits: "[7:4]" }

comb:
  - out: r
    expr: d
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign r = d;"));
        QVERIFY(verilog.contains("assign q[3:0] = r[7:4];"));
        QVERIFY(!verilog.contains("assign r[7:4] = q[3:0];"));
        QVERIFY(!verilog.contains("multi-driver conflict"));
    }

    void testTwoDrivenSlicesAreNotAliased()
    {
        const QString verilog = generate("owner_two_driven_slices", R"(
port:
  a:
    direction: input
    type: logic[3:0]
  b:
    direction: input
    type: logic[3:0]
  q:
    direction: output
    type: logic[7:0]

net:
  w:
    - { instance: top, port: q, bits: "[3:0]" }
    - { instance: top, port: q, bits: "[7:4]" }

comb:
  - out: q
    bits: "[3:0]"
    expr: a
  - out: q
    bits: "[7:4]"
    expr: b
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign q[3:0] = a;"));
        QVERIFY(verilog.contains("assign q[7:4] = b;"));
        QVERIFY(!verilog.contains("assign q[3:0] = q[7:4];"));
        QVERIFY(!verilog.contains("assign q[7:4] = q[3:0];"));
        QVERIFY(verilog.contains("multiple driven sinks"));
    }

    /* Two bound input slices are an ambiguous driver; nothing is wired and
       the ambiguity is reported. */
    void testTwoInputSliceSourcesAreAmbiguous()
    {
        const QString verilog = generate("owner_ambiguous", R"(
port:
  o_hi:
    direction: output
    type: logic[7:0]
  i_a:
    direction: input
    type: logic[7:0]
  i_b:
    direction: input
    type: logic[7:0]
  r:
    direction: output
    type: logic

net:
  sig:
    - { instance: top, port: i_a, bits: "[3:0]" }
    - { instance: top, port: i_b, bits: "[7:4]" }
    - { instance: top, port: o_hi, bits: "[3:0]" }

comb:
  - out: r
    expr: "1'b1"
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(!verilog.contains("assign o_hi"));
        QVERIFY(verilog.contains("ambiguous driver"));
    }

    /* An inner select on a bound name lands inside the bound range. */
    void testInnerSelectComposesWithTheBinding()
    {
        const QString verilog = generate("owner_compose", R"(
port:
  d:
    direction: input
    type: logic[1:0]
  q:
    direction: output
    type: logic[7:0]

net:
  w:
    - { instance: top, port: q, bits: "[7:4]" }

comb:
  - out: "w[1:0]"
    expr: d
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign q[5:4] = d;"));
        QVERIFY(!verilog.contains("assign q[1:0]"));
    }

    /* A whole-base assign and a sliced one overlap even though the strings
       differ, so the second driver is skipped with a diagnostic. */
    void testWholeAndSlicedCombTargetsConflict()
    {
        const QString verilog = generate("overlap_conflict", R"(
port:
  o_p:
    direction: output
    type: logic[7:0]
    connect: whole
  o_q:
    direction: output
    type: logic[7:0]

net:
  whole:
    - { instance: top, port: o_q }
  part:
    - { instance: top, port: o_p, bits: "[3:0]" }

comb:
  - out: whole
    expr: "8'h11"
  - out: part
    expr: "4'h2"
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign o_p = 8'h11;"));
        QVERIFY(!verilog.contains("assign o_p[3:0] = 4'h2;"));
        QVERIFY(verilog.contains("duplicate comb driver for o_p[3:0]"));
    }

    /* An instance input just reads the net; only a driving instance port may
       collide with the alias assignment. */
    void testInstanceInputDoesNotTriggerAliasCollision()
    {
        const QString verilog = generate("collision_input", R"(
port:
  o_a:
    direction: output
    type: logic[7:0]
    connect: shared
  o_b:
    direction: output
    type: logic[7:0]

instance:
  u_sub:
    module: sub_mod

net:
  shared:
    - { instance: top, port: o_b }
  o_b:
    - { instance: u_sub, port: i }
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign o_b = o_a;"));
        QVERIFY(!verilog.contains("another driver also reaches"));
    }

    void testSkippedInstancesDoNotClaimAliases()
    {
        const QString verilog = generate("owner_skipped_instances", R"(
port:
  src_a:
    direction: input
    type: logic
    connect: net_a
  y_a:
    direction: output
    type: logic
    connect: net_a
  src_b:
    direction: input
    type: logic
    connect: net_b
  y_b:
    direction: output
    type: logic
    connect: net_b

instance:
  u_guarded:
    module: sub_mod
    ifdef: [FEATURE]
    ifndef: [FEATURE]

net:
  net_a:
    - { instance: u_guarded, port: o }
  net_b:
    - { instance: u_missing, port: o }
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(!verilog.contains("sub_mod u_guarded"));
        QVERIFY(verilog.contains("assign y_a = src_a;"));
        QVERIFY(verilog.contains("assign y_b = src_b;"));
        QVERIFY(!verilog.contains("net net_a is already driven"));
        QVERIFY(!verilog.contains("net net_b is already driven"));
        QVERIFY(!verilog.contains("multiple drivers"));
    }

    void testInstanceOutputNeverUsesATopInputAsItsDestination()
    {
        const QString verilog = generate("owner_instance_top_input", R"(
port:
  src:
    direction: input
    type: logic[7:0]
    connect: shared

instance:
  u_sub:
    module: sub_mod

net:
  shared:
    - { instance: u_sub, port: o }
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(!verilog.contains(".o(src)"));
        QVERIFY(verilog.contains("wire [7:0] shared;"));
        QVERIFY(verilog.contains(".o(shared)"));
        QVERIFY(verilog.contains("multi-driver conflict"));
    }

    void testUnboundInstanceOutputNeverDrivesATopInput()
    {
        const QString verilog = generate("owner_unbound_top_input", R"(
port:
  src:
    direction: input
    type: logic[7:0]

instance:
  u_sub:
    module: sub_mod

net:
  src:
    - { instance: u_sub, port: o }
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(!verilog.contains(".o(src)"));
        QVERIFY(verilog.contains("sub_mod u_sub"));
        QVERIFY(verilog.contains(".o()"));
        QVERIFY(messageList.join('\n').contains("same-named top-level input"));
    }

    void testUnknownInstanceOwnershipStopsComponentAliasing()
    {
        const QString verilog = generate("owner_unknown_instance", R"(
port:
  y:
    direction: output
    type: logic
    connect: shared
  z:
    direction: output
    type: logic
    connect: shared
  solo:
    direction: output
    type: logic
    connect: solo_shared

instance:
  u_external:
    module: external_block
  u_solo:
    module: external_block

net:
  shared:
    - { instance: u_external, port: p }
  solo_shared:
    - { instance: u_solo, port: q }
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains(".p(y)"));
        QVERIFY(verilog.contains(".q(solo)"));
        QVERIFY(verilog.contains("whose direction gives no ownership"));
        QVERIFY(!verilog.contains(
            "net solo_shared has a connection whose direction gives no ownership"));
        QVERIFY(!verilog.contains("assign z = y;"));
        QVERIFY(!verilog.contains("assign y = z;"));
    }

    /* A net spelled with a member's name is that member's signal, so an
       instance driver on it lands on the component's canonical port and the
       alias chain stays single-driven. */
    void testMemberNamedNetResolvesToTheCanonical()
    {
        const QString verilog = generate("collision_output", R"(
port:
  z_out:
    direction: output
    type: logic[7:0]
    connect: shared_n
  a_out:
    direction: output
    type: logic[7:0]
    connect: shared_n

instance:
  u_sub:
    module: sub_mod

net:
  a_out:
    - { instance: u_sub, port: o }
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains(".o(z_out)"));
        QVERIFY(verilog.contains("assign a_out = z_out;"));
        QVERIFY(!verilog.contains(".o(a_out)"));
        QVERIFY(!verilog.contains("another driver also reaches"));
    }

    /* An instance connected to a sliced-only net lands on the head slice, and
       the remaining slices are fanned out from it. */
    void testInstanceOnSlicedOnlyNetConnectsTheSlice()
    {
        const QString verilog = generate("fanout_instance", R"(
port:
  p_a:
    direction: output
    type: logic[7:0]
  p_b:
    direction: output
    type: logic[7:0]

instance:
  u_sub:
    module: sub_mod

net:
  parts:
    - { instance: top, port: p_a, bits: "[1:0]" }
    - { instance: top, port: p_b, bits: "[5:4]" }
    - { instance: u_sub, port: o }
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains(".o(p_a[1:0])"));
        QVERIFY(verilog.contains("assign p_b[5:4] = p_a[1:0];"));
    }

    /* The top binding selects the destination interval. An instance-local
       `bits` field cannot redirect that connection onto another top slice. */
    void testInstanceSelectDoesNotOverrideTopBindingSlice()
    {
        const QString verilog = generate("instance_top_slice", R"(
port:
  q:
    direction: output
    type: logic[7:0]

instance:
  u_nibble:
    module: nibble_mod

net:
  upper:
    - { instance: top, port: q, bits: "[7:4]" }
    - { instance: u_nibble, port: o, bits: "[3:0]" }
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains(".o(q[7:4])"));
        QVERIFY(!verilog.contains(".o(q[3:0])"));
    }

    /* A process form and an expression form on one target are two continuous
       drivers; the expression must stand down with a diagnostic. */
    void testProcessAndExpressionFormsOnOneTargetConflict()
    {
        const QString verilog = generate("process_expr_conflict", R"(
port:
  sel:
    direction: input
    type: logic
  a:
    direction: input
    type: logic[7:0]
  y:
    direction: output
    type: logic[7:0]

comb:
  - out: y
    default: a
    if:
      - cond: sel
        then: "8'hFF"
  - out: y
    expr: a
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains("assign y = y_reg;"));
        QVERIFY(!verilog.contains("assign y = a;"));
        QVERIFY(verilog.contains("duplicate comb driver for y"));
    }

    /* A net that merely shares a name with a top-level port, without any
       binding, stays an ordinary internal wire. Folding it into an alias
       group would suppress the existing multi-driver diagnostics. */
    void testUnboundNetSharingPortNameStaysInternal()
    {
        const QString verilog = generate("connected_unbound_samename", R"(
port:
  d:
    direction: input
    type: logic[7:0]
  q:
    direction: output
    type: logic[7:0]

instance:
  u_sub:
    module: sub_mod

net:
  q:
    - instance: u_sub
      port: o
  d:
    - instance: u_sub
      port: i
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(verilog.contains(".o(q)"));
        QVERIFY(verilog.contains(".i(d)"));
        QVERIFY(!verilog.contains("Top-level port aliases"));
    }

    void testMalformedTopPortBindingDoesNotCrash()
    {
        const QString verilog = generate("connected_malformed_top_port", R"(
port:
  ghost: malformed
instance:
  u_sub:
    module: sub_mod
net:
  shared_n:
    - { instance: u_sub, port: o }
    - { instance: top, port: ghost }
)");
        QVERIFY(!verilog.isEmpty());
        QVERIFY(!verilog.contains("input wire ghost"));
        QVERIFY(!verilog.contains("output wire ghost"));
        QCOMPARE(messageList.filter("Port ghost has invalid format, skipping").size(), 1);
    }
};

QStringList Test::messageList;

QSOC_TEST_MAIN(Test)
#include "test_qsoccliparsegenerateconnectedtarget.moc"
