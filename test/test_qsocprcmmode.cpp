// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmmode.h"
#include "qsoc_test.h"

#include <QtTest>

namespace {

using Status = QSocPrcmCheckStatus;

QSocPrcmInput system()
{
    QSocPrcmInput input;
    input.supply = "aon";
    input.supplyTable.insert("aon", {true, {}, {}});
    for (const auto &name : {"compute", "memory"}) {
        input.supplyTable.insert(name, {});
        QSocPrcmDomain domain;
        domain.supply    = name;
        domain.resetMode = "OFF";
        domain.mode.insert("OFF", {0, false, false, true, true});
        domain.mode.insert("RUN", {1, true, true, false, false});
        domain.transition = {{"OFF", "RUN"}, {"RUN", "OFF"}};
        input.domain.insert(name, domain);
    }
    input.domain["memory"].service.insert("online", "RUN");
    input.domain["compute"].require.insert("memory", {"memory", "online", {"RUN"}});
    return input;
}

const QSocPrcmCheckResult *findCase(const QSocPrcmModeResult &report, const QString &name)
{
    for (const auto &check : report.check) {
        if (check.name == name) {
            return &check.result;
        }
    }
    return nullptr;
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void serviceAndPolicy()
    {
        auto input          = system();
        input.chipResetMode = "SLEEP";
        input.chipMode.insert("SLEEP", {0, {{"compute", {{}, "OFF"}}, {"memory", {{}, "OFF"}}}});
        input.chipMode
            .insert("ACTIVE", {1, {{"compute", {{}, "RUN"}}, {"memory", {{"OFF", "RUN"}, {}}}}});
        input.chipMode.insert("BROKEN", {2, {{"compute", {{}, "RUN"}}, {"memory", {{}, "OFF"}}}});
        const auto report = QSocPrcmModeCheck::check(input);
        QCOMPARE(report.check.size(), 7);
        QCOMPARE(report.diagnostic.size(), 1);
        QCOMPARE(report.diagnostic[0].code, "PRCM_MODE_CONFLICT");
        for (const auto &check : report.check) {
            const auto expected = check.name.endsWith("BROKEN") ? Status::Unsat : Status::Sat;
            QCOMPARE(check.result.status, expected);
        }
        const auto active = findCase(report, "prcm.chip.mode.ACTIVE");
        QVERIFY(active);
        QVERIFY(active->value.value("prcm.domain.memory.mode.RUN"));
        QStringList source;
        for (const auto &entry : report.diagnostic[0].source) {
            source.append(entry.path);
        }
        QVERIFY(source.contains("prcm.domain.compute.require.memory.service"));
        QVERIFY(source.contains("prcm.chip.mode.BROKEN.domain.compute"));
        QVERIFY(source.contains("prcm.chip.mode.BROKEN.domain.memory"));
    }

    void stableStateTruthTable()
    {
        /* Independently enumerate power, clock, reset and isolation. */
        for (unsigned state = 0; state < 16; ++state) {
            const bool power     = (state & 8U) != 0;
            const bool clock     = (state & 4U) != 0;
            const bool reset     = (state & 2U) != 0;
            const bool isolation = (state & 1U) != 0;
            auto       input     = system();
            input.domain.remove("memory");
            auto &domain = input.domain["compute"];
            domain.require.clear();
            domain.transition.clear();
            domain.resetMode  = "TEST";
            domain.mode       = {{"TEST", {0, power, clock, reset, isolation}}};
            const auto report = QSocPrcmModeCheck::check(input);
            QCOMPARE(report.check.size(), 1);
            const bool legal = power || (!clock && reset && isolation);
            QCOMPARE(report.check[0].result.status, legal ? Status::Sat : Status::Unsat);
        }
    }

    void sharedSupplyAndFeedbackMeaning()
    {
        auto input                     = system();
        input.domain["compute"].supply = "memory";
        input.chipResetMode            = "IDLE";
        input.chipMode.insert("IDLE", {0, {{"compute", {{}, "OFF"}}, {"memory", {{}, "RUN"}}}});
        const auto report = QSocPrcmModeCheck::check(input);
        const auto idle   = findCase(report, "prcm.chip.mode.IDLE");
        QVERIFY(idle);
        QCOMPARE(idle->status, Status::Unsat);
        input.domain["compute"].mode["OFF"].power = true;
        const auto retained                       = QSocPrcmModeCheck::check(input);
        const auto retainedIdle                   = findCase(retained, "prcm.chip.mode.IDLE");
        QVERIFY(retainedIdle);
        QCOMPARE(retainedIdle->status, Status::Sat);
    }

    void dependencyCycleIsNotStaticConflict()
    {
        auto input = system();
        input.domain["compute"].service.insert("online", "RUN");
        input.domain["memory"].require.insert("compute", {"compute", "online", {"RUN"}});
        const auto report = QSocPrcmModeCheck::check(input);
        QVERIFY(report.diagnostic.isEmpty());
        QCOMPARE(report.check.size(), 4);
        for (const auto &check : report.check) {
            QCOMPARE(check.result.status, Status::Sat);
            QCOMPARE(
                check.result.value.value("prcm.domain.compute.mode.RUN"),
                check.result.value.value("prcm.domain.memory.mode.RUN"));
        }
    }

    void oneModeEvenWithEqualState()
    {
        auto input = system();
        input.domain["memory"].mode.insert("DMA", {2, true, true, false, false});
        input.domain["memory"].service.insert("dma", "DMA");
        input.domain["compute"].require.insert("dma", {"memory", "dma", {"RUN"}});
        const auto report = QSocPrcmModeCheck::check(input);
        const auto run    = findCase(report, "prcm.domain.compute.mode.RUN");
        QVERIFY(run);
        QCOMPARE(run->status, Status::Unsat);
        input.domain["memory"].service["dma"] = "RUN";
        const auto shared                     = QSocPrcmModeCheck::check(input);
        const auto sharedRun                  = findCase(shared, "prcm.domain.compute.mode.RUN");
        QVERIFY(sharedRun);
        QCOMPARE(sharedRun->status, Status::Sat);
    }

    void referenceError()
    {
        const QList<QString> fault{
            "supply",
            "reset_mode",
            "mode_code",
            "transition",
            "service_mode",
            "provider",
            "controller_supply",
            "chip_domain"};
        for (const auto &name : fault) {
            auto  input  = system();
            auto &domain = input.domain["compute"];
            if (name == "supply")
                domain.supply = "missing";
            if (name == "reset_mode")
                domain.resetMode = "missing";
            if (name == "mode_code")
                domain.mode["RUN"].code = 0;
            if (name == "transition")
                domain.transition.append(QSocPrcmTransition{"RUN", "missing"});
            if (name == "service_mode")
                input.domain["memory"].service["online"] = "missing";
            if (name == "provider")
                domain.require["memory"].domain = "missing";
            if (name == "controller_supply")
                input.supply = "compute";
            if (name == "chip_domain") {
                input.chipResetMode = "SLEEP";
                input.chipMode.insert("SLEEP", {0, {{"compute", {{}, "OFF"}}}});
            }
            const auto report = QSocPrcmModeCheck::check(input);
            QVERIFY2(report.check.isEmpty(), qPrintable(name));
            QCOMPARE(report.diagnostic.size(), 1);
            QCOMPARE(report.diagnostic[0].code, "PRCM_MODE_REFERENCE");
            QVERIFY(!report.diagnostic[0].source[0].path.isEmpty());
        }
    }

    void interruptedChecksStayUnproven()
    {
        std::stop_source stop;
        stop.request_stop();
        const auto cancelled = QSocPrcmModeCheck::check(system(), {}, stop.get_token());
        QCOMPARE(cancelled.check.size(), 1);
        QCOMPARE(cancelled.check[0].result.status, Status::Cancelled);
        const auto limited = QSocPrcmModeCheck::check(system(), {10000, 1});
        QCOMPARE(limited.check.size(), 4);
        for (const auto &check : limited.check) {
            QCOMPARE(check.result.status, Status::Unknown);
            QVERIFY(check.result.value.isEmpty());
        }
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocprcmmode.moc"
