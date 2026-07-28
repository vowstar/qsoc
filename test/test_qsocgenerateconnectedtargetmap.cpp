// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocgeneratemanager.h"
#include "qsoc_test.h"

#include <QStringList>
#include <QtCore>
#include <QtTest>

#include <yaml-cpp/yaml.h>

/**
 * Properties of the connected-target maps. Routing, aliasing, comb, and seq
 * all resolve a net through these two functions, so a violated property here
 * shows up as a double-driven or undriven port in RTL. Asserting the relations
 * directly keeps the checks independent of Verilog formatting.
 */
class Test : public QObject
{
    Q_OBJECT

private:
    using Binding = QSocGenerateManager::TopPortBinding;

    using Group = QSocGenerateManager::NetTopPorts;

    static QMap<QString, Group> groups(const char *yaml)
    {
        return QSocGenerateManager::buildNetToTopPorts(YAML::Load(yaml));
    }

    static QMap<QString, Binding> redirect(const char *yaml)
    {
        return QSocGenerateManager::buildTopPortRedirect(YAML::Load(yaml));
    }

    /* Port names in declaration order. */
    static QStringList declaredPorts(const char *yaml)
    {
        const YAML::Node node = YAML::Load(yaml);
        QStringList      names;
        if (node["port"] && node["port"].IsMap()) {
            for (const auto &entry : node["port"]) {
                if (entry.first.IsScalar()) {
                    names.append(QString::fromStdString(entry.first.as<std::string>()));
                }
            }
        }
        return names;
    }

    static QString canonical(const QMap<QString, Group> &all, const QString &net)
    {
        const QStringList members = all.value(net).members;
        return members.isEmpty() ? QString() : members.first();
    }

private slots:
    /* The carrier is the earliest declared whole-port member. Container order
       must never decide it, or routing and aliasing can pick different ports
       for the same net. */
    void testCanonicalIsFirstDeclaredWholeMember()
    {
        const char   *yaml  = R"(
port:
  z_out: { direction: output, type: "logic[7:0]", connect: shared_n }
  a_out: { direction: output, type: "logic[7:0]", connect: shared_n }
)";
        const auto    all   = groups(yaml);
        const auto    order = declaredPorts(yaml);
        const QString head  = canonical(all, "shared_n");
        QCOMPARE(head, QStringLiteral("z_out"));
        for (const QString &member : all.value("shared_n").members) {
            QVERIFY(order.indexOf(head) <= order.indexOf(member));
        }
    }

    /* Both binding spellings feed one ordering. */
    void testBothSpellingsShareOneOrdering()
    {
        const auto all = groups(R"(
port:
  z_out: { direction: output, type: "logic[7:0]" }
  a_out: { direction: output, type: "logic[7:0]", connect: shared_n }
net:
  shared_n:
    - { instance: top, port: z_out }
)");
        QCOMPARE(canonical(all, "shared_n"), QStringLiteral("z_out"));
        QCOMPARE(all.value("shared_n").members, (QStringList{"z_out", "a_out"}));
    }

    /* Listing explicit bindings against declaration order must not reorder the
       group. */
    void testNetEntryOrderDoesNotDecideCanonical()
    {
        const char *forward  = R"(
port:
  z_out: { direction: output, type: "logic[7:0]" }
  a_out: { direction: output, type: "logic[7:0]" }
net:
  shared_n:
    - { instance: top, port: z_out }
    - { instance: top, port: a_out }
)";
        const char *reversed = R"(
port:
  z_out: { direction: output, type: "logic[7:0]" }
  a_out: { direction: output, type: "logic[7:0]" }
net:
  shared_n:
    - { instance: top, port: a_out }
    - { instance: top, port: z_out }
)";
        QCOMPARE(canonical(groups(forward), "shared_n"), QStringLiteral("z_out"));
        QCOMPARE(canonical(groups(reversed), "shared_n"), QStringLiteral("z_out"));
    }

    /* A sliced binding covers part of the net, so it cannot carry the whole of
       it while a whole-port member exists. */
    void testSlicedMemberNeverHeadsAGroupThatHasAWholeMember()
    {
        const auto  all   = groups(R"(
port:
  p_low: { direction: output, type: "logic[7:0]" }
  p_all: { direction: output, type: "logic[7:0]", connect: shared_n }
net:
  shared_n:
    - { instance: top, port: p_low, bits: "[3:0]" }
)");
        const Group group = all.value("shared_n");
        QCOMPARE(group.members.first(), QStringLiteral("p_all"));
        QCOMPARE(group.slices.size(), 1);
        QCOMPARE(group.slices.first().port, QStringLiteral("p_low"));
        QCOMPARE(group.slices.first().slice, QStringLiteral("[3:0]"));
    }

    /* The bound slice survives into the map; dropping it makes a consumer
       drive bits the netlist never bound. */
    void testSliceIsPreserved()
    {
        const auto  all   = groups(R"(
port:
  q: { direction: output, type: "logic[7:0]" }
net:
  low_n:
    - { instance: top, port: q, bits: "[3:0]" }
)");
        const Group group = all.value("low_n");
        QVERIFY(group.members.isEmpty());
        QCOMPARE(group.slices.size(), 1);
        QCOMPARE(group.slices.first().port, QStringLiteral("q"));
        QCOMPARE(group.slices.first().slice, QStringLiteral("[3:0]"));
    }

    /* `connect:` is transitive. Every net name of one component has to report
       the same carrier, or two groups assign onto the same port. */
    void testChainedConnectSharesOneCarrier()
    {
        const auto all = groups(R"(
port:
  q: { direction: output, type: "logic[7:0]" }
  mid: { direction: output, type: "logic[7:0]", connect: q }
  a_out: { direction: output, type: "logic[7:0]", connect: mid }
)");
        QCOMPARE(canonical(all, "q"), QStringLiteral("q"));
        QCOMPARE(canonical(all, "mid"), QStringLiteral("q"));
        QCOMPARE(all.value("q").members.size(), 3);
        QCOMPARE(all.value("mid").members.size(), 3);
    }

    /* Declared head-last, the chain is two hops deep. A one-step parent lookup
       still passes the head-first fixture, so only this order proves the walk
       is transitive. */
    void testChainedConnectResolvesWhenDeclaredHeadLast()
    {
        const auto all = groups(R"(
port:
  a_out: { direction: output, type: "logic[7:0]", connect: mid }
  mid: { direction: output, type: "logic[7:0]", connect: q }
  q: { direction: output, type: "logic[7:0]" }
)");
        QCOMPARE(canonical(all, "mid"), QStringLiteral("a_out"));
        QCOMPARE(canonical(all, "q"), QStringLiteral("a_out"));
        QCOMPARE(all.value("q").members, (QStringList{"a_out", "mid", "q"}));
    }

    /* A net that only binds slices resolves to its head slice, and the group
       keeps every bound slice so the alias emitter can fan the head out. */
    void testSlicedOnlyNetResolvesToItsHeadSlice()
    {
        const char   *yaml = R"(
port:
  p_a: { direction: output, type: "logic[7:0]" }
  p_b: { direction: output, type: "logic[7:0]" }
net:
  parts:
    - { instance: top, port: p_a, bits: "[1:0]" }
    - { instance: top, port: p_b, bits: "[5:4]" }
)";
        const Binding head = redirect(yaml).value("parts");
        QCOMPARE(head.port, QStringLiteral("p_a"));
        QCOMPARE(head.slice, QStringLiteral("[1:0]"));
        const QList<Binding> slices = groups(yaml).value("parts").slices;
        QCOMPARE(slices.size(), 2);
        QCOMPARE(slices.at(1).port, QStringLiteral("p_b"));
        QCOMPARE(slices.at(1).slice, QStringLiteral("[5:4]"));
    }

    /* An input slice must never head the list while an output slice exists,
       whatever order the netlist spells them in. */
    void testInputSliceNeverHeadsADrivableNet()
    {
        const Binding head = redirect(R"(
port:
  i_lo: { direction: input, type: "logic[7:0]" }
  o_hi: { direction: output, type: "logic[7:0]" }
net:
  sig:
    - { instance: top, port: i_lo, bits: "[7:4]" }
    - { instance: top, port: o_hi, bits: "[3:0]" }
)")
                                 .value("sig");
        QCOMPARE(head.port, QStringLiteral("o_hi"));
        QCOMPARE(head.slice, QStringLiteral("[3:0]"));
    }

    /* A sliced member is a port in its own right; a process may target it
       directly and must not be folded onto the group's carrier. */
    void testSlicedMemberIsNotFoldedOntoTheCarrier()
    {
        const auto map = redirect(R"(
port:
  p_low: { direction: output, type: "logic[7:0]" }
  p_all: { direction: output, type: "logic[7:0]", connect: shared_n }
  p_two: { direction: output, type: "logic[7:0]", connect: shared_n }
net:
  shared_n:
    - { instance: top, port: p_low, bits: "[3:0]" }
)");
        QCOMPARE(map.value("p_two").port, QStringLiteral("p_all"));
        QVERIFY(!map.contains("p_low"));
    }

    /* Resolving an already resolved name must not move again. */
    void testRedirectIsIdempotent()
    {
        const auto map = redirect(R"(
port:
  q: { direction: output, type: "logic[7:0]" }
  mid: { direction: output, type: "logic[7:0]", connect: q }
  a_out: { direction: output, type: "logic[7:0]", connect: mid }
)");
        QVERIFY(!map.isEmpty());
        for (auto it = map.constBegin(); it != map.constEnd(); ++it) {
            const QString once  = it.value().port;
            const QString twice = map.value(once, {once, QString()}).port;
            QCOMPARE(twice, once);
        }
    }

    /* Every non-canonical whole member resolves to the carrier, so a process
       cannot target a member the alias assignment already drives. */
    void testSecondaryMembersResolveToTheCarrier()
    {
        const auto map = redirect(R"(
port:
  z_out: { direction: output, type: "logic[7:0]", connect: shared_n }
  a_out: { direction: output, type: "logic[7:0]", connect: shared_n }
)");
        QCOMPARE(map.value("a_out").port, QStringLiteral("z_out"));
        QCOMPARE(map.value("shared_n").port, QStringLiteral("z_out"));
    }

    /* Only an all-output group is driven by an alias assignment, so only its
       members may fold. Folding a mixed group redirects an output target onto
       an input port. */
    void testMixedDirectionGroupDoesNotFoldMembers()
    {
        const auto map = redirect(R"(
port:
  a_in: { direction: input, type: "logic[7:0]", connect: shared_n }
  z_out: { direction: output, type: "logic[7:0]", connect: shared_n }
)");
        QVERIFY(!map.contains("z_out"));
    }

    /* A net nothing binds stays an ordinary internal wire even when a port
       happens to share its name. */
    void testUnboundNetSharingAPortNameIsAbsent()
    {
        const auto all = groups(R"(
port:
  data_out: { direction: output, type: "logic[7:0]" }
net:
  data_out:
    - { instance: u_drv, port: o }
)");
        QVERIFY(!all.contains("data_out"));
    }

    /* An `instance: top` entry naming a port the netlist never declares is not
       a binding. */
    void testUndeclaredPortIsNotBound()
    {
        const auto all = groups(R"(
port:
  q: { direction: output, type: "logic[7:0]" }
net:
  shared_n:
    - { instance: top, port: ghost }
)");
        QVERIFY(!all.contains("shared_n"));
    }

    /* Empty and absent sections are legal input for the map builders. */
    void testEmptyNetlistYieldsEmptyMaps()
    {
        QVERIFY(groups("port: {}").isEmpty());
        QVERIFY(redirect("port: {}").isEmpty());
        QVERIFY(groups("{}").isEmpty());
        QVERIFY(redirect("{}").isEmpty());
    }
};

QSOC_TEST_MAIN(Test)
#include "test_qsocgenerateconnectedtargetmap.moc"
