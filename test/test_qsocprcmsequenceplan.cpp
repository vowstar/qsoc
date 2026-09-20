// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmsequencecheck.h"
#include "common/qsocprcmsequenceplan.h"
#include "qsoc_prcm_fixture.h"
#include "qsoc_test.h"

#include <QtTest>

#include <limits>

namespace {

class Test : public QObject
{
    Q_OBJECT

private slots:
    void domainSelection()
    {
        auto parsed = QSocPrcmParser::parse(YAML::Load(qsocPrcmDeclaration()), "control.soc_net");
        QVERIFY(parsed.input);
        auto &input = *parsed.input;
        input.domain.insert("other", input.domain["periph"]);
        input.domain["other"].mode["RUN"].code = 7;
        input.domain["other"].resetMode        = "RUN";
        input.domain["other"].service.insert("access", "RUN");
        input.domain["periph"].require.insert("access", {"other", "access", {"RUN"}});
        input.chipMode.insert("normal", {});
        const auto first  = QSocPrcmSequencePlanner::buildDomain(input, "periph");
        const auto second = QSocPrcmSequencePlanner::buildDomain(input, "other");
        QVERIFY(first.plan);
        QVERIFY(second.plan);
        QCOMPARE(first.plan->domain, QString("periph"));
        QCOMPARE(second.plan->domain, QString("other"));
        QCOMPARE(first.plan->resetCode, quint64(0));
        QCOMPARE(second.plan->resetCode, quint64(7));
        QCOMPARE(second.plan->mode.value(7), QSocPrcmTarget::Run);
        QVERIFY(!first.plan->mode.contains(7));
        QVERIFY(!QSocPrcmSequencePlanner::build(input).plan);
        const auto absent = QSocPrcmSequencePlanner::buildDomain(input, "missing");
        QVERIFY(!absent.plan);
        QCOMPARE(absent.diagnostic.size(), 1);
        QCOMPARE(absent.diagnostic[0].source[0].path, QString("prcm.domain.missing"));
    }

    void progress()
    {
        for (unsigned choice = 0; choice < 4; ++choice) {
            QSocPrcmSequencePlan plan;
            plan.mode.insert(0, QSocPrcmTarget::Off);
            if ((choice & 1U) != 0)
                plan.mode.insert(1, QSocPrcmTarget::Reset);
            if ((choice & 2U) != 0)
                plan.mode.insert(2, QSocPrcmTarget::Run);
            const auto result = QSocPrcmSequenceCheck::progress(plan);
            QVERIFY2(result.status == QSocPrcmCheckStatus::Unsat, qPrintable(result.reason));
            QVERIFY(result.loop.isEmpty());
            QVERIFY(!result.target);
        }
    }

    void cancelledProgress()
    {
        QSocPrcmSequencePlan plan;
        plan.mode.insert(0, QSocPrcmTarget::Off);
        std::stop_source cancellation;
        cancellation.request_stop();
        const auto result = QSocPrcmSequenceCheck::progress(plan, cancellation.get_token());
        QCOMPARE(result.status, QSocPrcmCheckStatus::Cancelled);
        QVERIFY(result.loop.isEmpty());
    }

    void safety()
    {
        for (unsigned choice = 0; choice < 4; ++choice) {
            QSocPrcmSequencePlan plan;
            plan.mode.insert(0, QSocPrcmTarget::Off);
            if ((choice & 1U) != 0)
                plan.mode.insert(1, QSocPrcmTarget::Reset);
            if ((choice & 2U) != 0)
                plan.mode.insert(2, QSocPrcmTarget::Run);
            const auto result = QSocPrcmSequenceCheck::safety(plan);
            QVERIFY2(result.status == QSocPrcmCheckStatus::Unsat, qPrintable(result.reason));
            QVERIFY(!result.smt.isEmpty());
            QVERIFY(result.value.isEmpty());
        }
    }

    void cancelledSafety()
    {
        QSocPrcmSequencePlan plan;
        plan.mode.insert(0, QSocPrcmTarget::Off);
        std::stop_source cancellation;
        cancellation.request_stop();
        const auto result = QSocPrcmSequenceCheck::safety(plan, {}, cancellation.get_token());
        QCOMPARE(result.status, QSocPrcmCheckStatus::Cancelled);
    }

    void codeAndName()
    {
        auto parsed = QSocPrcmParser::parse(YAML::Load(qsocPrcmDeclaration()), "control.soc_net");
        QVERIFY(parsed.input);
        auto &domain      = parsed.input->domain["periph"];
        auto  off         = domain.mode.take("OFF");
        auto  run         = domain.mode.take("RUN");
        off.code          = 64;
        run.code          = std::numeric_limits<quint64>::max();
        domain.mode       = {{"park", off}, {"active", run}};
        domain.transition = {{"park", "active"}, {"active", "park"}};
        domain.resetMode  = "active";
        const auto result = QSocPrcmSequencePlanner::build(*parsed.input);
        QVERIFY(result.plan);
        QVERIFY(result.diagnostic.isEmpty());
        QCOMPARE(result.plan->domain, QString("periph"));
        QCOMPARE(result.plan->resetCode, run.code);
        QCOMPARE(result.plan->mode.size(), 2);
        QCOMPARE(result.plan->mode[64], QSocPrcmTarget::Off);
        QCOMPARE(result.plan->mode[run.code], QSocPrcmTarget::Run);

        auto alias = run;
        alias.code = 7;
        domain.mode.insert("operate", alias);
        domain.transition.append(
            {{"park", "operate"}, {"operate", "park"}, {"active", "operate"}, {"operate", "active"}});
        const auto shared = QSocPrcmSequencePlanner::build(*parsed.input);
        QVERIFY(shared.plan);
        QCOMPARE(shared.plan->mode.size(), 3);
        QCOMPARE(shared.plan->mode[alias.code], QSocPrcmTarget::Run);
        QCOMPARE(shared.plan->mode[run.code], QSocPrcmTarget::Run);
    }

    void stateTuple()
    {
        for (unsigned bits = 0; bits < 16; ++bits) {
            auto parsed
                = QSocPrcmParser::parse(YAML::Load(qsocPrcmDeclaration()), "control.soc_net");
            QVERIFY(parsed.input);
            auto &domain = parsed.input->domain["periph"];
            domain.mode.remove("RUN");
            domain.transition.clear();
            if (bits != 3) {
                domain.mode.insert(
                    "candidate",
                    {7, (bits & 8U) != 0, (bits & 4U) != 0, (bits & 2U) != 0, (bits & 1U) != 0});
                domain.transition = {{"OFF", "candidate"}, {"candidate", "OFF"}};
            }
            const auto result    = QSocPrcmSequencePlanner::build(*parsed.input);
            const bool supported = bits == 3 || bits == 12 || bits == 15;
            QCOMPARE(result.plan.has_value(), supported);
            if (supported) {
                QVERIFY(result.diagnostic.isEmpty());
                if (bits != 3)
                    QCOMPARE(
                        result.plan->mode[7],
                        bits == 15 ? QSocPrcmTarget::Reset : QSocPrcmTarget::Run);
            } else {
                QCOMPARE(result.diagnostic.size(), 1);
                QCOMPARE(result.diagnostic[0].code, QString("PRCM_SEQUENCE_UNSUPPORTED"));
                QCOMPARE(
                    result.diagnostic[0].source[0].path,
                    QString("prcm.domain.periph.mode.candidate"));
            }
        }
    }

    void boundary_data()
    {
        QTest::addColumn<QString>("fault");
        QTest::addColumn<QString>("path");
        const QMap<QString, QString> boundary{
            {"domain", "prcm.domain"},
            {"chip", "prcm.chip"},
            {"service", "prcm.domain.periph"},
            {"require", "prcm.domain.periph"},
            {"supply", "prcm.domain.periph.supply"},
            {"always-on", "prcm.domain.periph.supply"},
            {"code", "prcm.domain.periph.mode.RUN.code"},
            {"off", "prcm.domain.periph.mode"},
            {"reset", "prcm.domain.periph.reset_mode"},
            {"transition", "prcm.domain.periph.transition"}};
        for (auto item = boundary.cbegin(); item != boundary.cend(); ++item)
            QTest::newRow(qPrintable(item.key())) << item.key() << item.value();
    }

    void boundary()
    {
        QFETCH(QString, fault);
        QFETCH(QString, path);
        auto parsed = QSocPrcmParser::parse(YAML::Load(qsocPrcmDeclaration()), "control.soc_net");
        QVERIFY(parsed.input);
        auto  input  = *parsed.input;
        auto &domain = input.domain["periph"];
        if (fault == "domain")
            input.domain.insert("second", domain);
        if (fault == "chip")
            input.chipMode.insert("normal", {});
        if (fault == "service")
            domain.service.insert("access", "RUN");
        if (fault == "require")
            domain.require.insert("access", {"periph", "access", {"RUN"}});
        if (fault == "supply")
            domain.supply = "missing";
        if (fault == "always-on")
            input.supplyTable[domain.supply].alwaysOn = true;
        if (fault == "code")
            domain.mode["RUN"].code = domain.mode["OFF"].code;
        if (fault == "off")
            domain.mode.remove("OFF");
        if (fault == "reset")
            domain.resetMode = "missing";
        if (fault == "transition")
            domain.transition.removeLast();
        const auto result = QSocPrcmSequencePlanner::build(input);
        QVERIFY(!result.plan);
        QCOMPARE(result.diagnostic.size(), 1);
        QCOMPARE(result.diagnostic[0].code, QString("PRCM_SEQUENCE_UNSUPPORTED"));
        QCOMPARE(result.diagnostic[0].source[0].path, path);
        const auto location = parsed.input->source.value(path);
        QCOMPARE(result.diagnostic[0].source[0].file, location.file);
        QCOMPARE(result.diagnostic[0].source[0].line, location.line);
        QCOMPARE(result.diagnostic[0].source[0].column, location.column);
        if (fault == "transition")
            QVERIFY(result.diagnostic[0].message.contains("RUN to OFF"));
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocprcmsequenceplan.moc"
