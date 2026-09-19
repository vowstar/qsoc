// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmsequence.h"
#include "qsoc_test.h"

#include <QtTest>

#include <set>
#include <tuple>
#include <vector>

namespace {

using Phase    = QSocPrcmPhase;
using Target   = QSocPrcmTarget;
using Sequence = QSocPrcmSequence;

struct Domain
{
    QSocPrcmSequenceState state;
    QSocPrcmObservation   observation;

    QSocPrcmSequenceStep tick(std::optional<Target> request, bool serviceFault = false)
    {
        const auto result = Sequence::step(state, request, observation, serviceFault);
        state             = result.state;
        return result;
    }

    void respond()
    {
        const auto control = Sequence::control(state);
        observation        = {control.power, control.reset, control.isolation, control.quiesce};
    }

    bool reach(Target target, Phase phase)
    {
        for (int cycle = 0; cycle < 32; ++cycle) {
            tick(target);
            if (state.phase == phase)
                return true;
            respond();
        }
        return false;
    }

    bool settle(Target target)
    {
        for (int cycle = 0; cycle < 32; ++cycle) {
            respond();
            tick(target);
            if (Sequence::complete(state, target, observation))
                return true;
        }
        return false;
    }
};

class Test : public QObject
{
    Q_OBJECT

private slots:
    void modeChange()
    {
        for (auto first : {Target::Off, Target::Reset, Target::Run}) {
            for (auto second : {Target::Off, Target::Reset, Target::Run}) {
                Domain domain;
                QVERIFY(domain.settle(first));
                QVERIFY(domain.settle(second));
                const auto control = Sequence::control(domain.state);
                QCOMPARE(control.power, second != Target::Off);
                QCOMPARE(control.clock, second != Target::Off);
                QCOMPARE(control.reset, second != Target::Run);
                QCOMPARE(control.isolation, second != Target::Run);
                QCOMPARE(control.quiesce, second != Target::Run);
            }
        }
    }

    void powerPending()
    {
        Domain domain;
        QVERIFY(domain.reach(Target::Run, Phase::Power));
        QVERIFY(!domain.observation.power);
        for (int cycle = 0; cycle < 8; ++cycle) {
            domain.tick(Target::Off);
            QVERIFY(Sequence::control(domain.state).power);
            QVERIFY(!Sequence::complete(domain.state, Target::Off, domain.observation));
        }
        domain.observation.power = true;
        domain.tick(Target::Off);
        QVERIFY(!Sequence::control(domain.state).power);
        QVERIFY(!Sequence::complete(domain.state, Target::Off, domain.observation));
        domain.observation.power = false;
        QVERIFY(domain.settle(Target::Off));
    }

    void resetPending()
    {
        for (auto target : {Target::Off, Target::Reset}) {
            Domain domain;
            QVERIFY(domain.reach(Target::Run, Phase::Release));
            QVERIFY(domain.observation.reset);
            for (int cycle = 0; cycle < 8; ++cycle) {
                domain.tick(target);
                const auto control = Sequence::control(domain.state);
                QVERIFY(control.power && control.clock && !control.reset);
                QVERIFY(control.isolation && control.quiesce);
                QVERIFY(!Sequence::complete(domain.state, target, domain.observation));
            }
            domain.observation.reset = false;
            domain.tick(target);
            QVERIFY(Sequence::control(domain.state).reset);
            QVERIFY(domain.settle(target));
        }
    }

    void serviceFailure()
    {
        Domain domain;
        QVERIFY(domain.settle(Target::Run));
        QVERIFY(!domain.tick(Target::Run, true).powerLost);
        QVERIFY(Sequence::fault(domain.state));
        const auto control = Sequence::control(domain.state);
        QVERIFY(control.power);
        QVERIFY(!control.clock);
        QVERIFY(control.reset && control.isolation && control.quiesce);
        QVERIFY(!domain.settle(Target::Run));
        QVERIFY(domain.settle(Target::Off));
        QVERIFY(domain.settle(Target::Run));
    }

    void serviceFailureBeforePower()
    {
        Domain domain;
        QVERIFY(domain.settle(Target::Off));
        QVERIFY(!domain.tick(Target::Run, true).powerLost);
        QVERIFY(Sequence::fault(domain.state));
        QVERIFY(!Sequence::control(domain.state).power);
        QVERIFY(domain.settle(Target::Off));
    }

    void serviceFailureDuringPower()
    {
        Domain domain;
        QVERIFY(domain.reach(Target::Run, Phase::Power));
        QVERIFY(!domain.observation.power);
        QVERIFY(!domain.tick(Target::Run, true).powerLost);
        for (int cycle = 0; cycle < 8; ++cycle) {
            domain.tick(Target::Off);
            const auto control = Sequence::control(domain.state);
            QVERIFY(control.power);
            QVERIFY(!control.clock);
            QVERIFY(control.reset && control.isolation && control.quiesce);
            QVERIFY(Sequence::fault(domain.state));
        }
        domain.observation.power = true;
        domain.tick(Target::Off);
        QVERIFY(domain.settle(Target::Off));
    }

    void isolationPending()
    {
        Domain domain;
        QVERIFY(domain.reach(Target::Run, Phase::Connect));
        QVERIFY(domain.observation.isolation);
        for (int cycle = 0; cycle < 8; ++cycle) {
            domain.tick(Target::Off);
            QVERIFY(!Sequence::control(domain.state).isolation);
            QVERIFY(Sequence::control(domain.state).quiesce);
        }
        domain.observation.isolation = false;
        domain.tick(Target::Off);
        QVERIFY(Sequence::control(domain.state).isolation);
        for (int cycle = 0; cycle < 8; ++cycle) {
            domain.tick(Target::Run);
            QVERIFY(Sequence::control(domain.state).isolation);
            QVERIFY(Sequence::control(domain.state).quiesce);
        }
        domain.observation.isolation = true;
        QVERIFY(domain.settle(Target::Run));
    }

    void drainPending()
    {
        Domain domain;
        QVERIFY(domain.settle(Target::Run));
        domain.observation.idle = false;
        for (int cycle = 0; cycle < 8; ++cycle) {
            domain.tick(Target::Off);
            const auto control = Sequence::control(domain.state);
            QVERIFY(control.power && control.clock && !control.reset && !control.isolation);
            QVERIFY(control.quiesce);
        }
        domain.tick(Target::Run);
        QVERIFY(Sequence::control(domain.state).quiesce);
        domain.observation.idle = true;
        domain.tick(Target::Run);
        QVERIFY(!Sequence::control(domain.state).quiesce);
        QVERIFY(domain.settle(Target::Off));
    }

    void invalidRequest()
    {
        Domain domain;
        QVERIFY(domain.reach(Target::Run, Phase::Power));
        for (int cycle = 0; cycle < 16; ++cycle) {
            domain.respond();
            domain.tick(std::nullopt);
            QCOMPARE(domain.state.target, Target::Run);
            QVERIFY(!Sequence::complete(domain.state, std::nullopt, domain.observation));
        }
        QCOMPARE(domain.state.phase, Phase::Run);
        QVERIFY(Sequence::complete(domain.state, Target::Run, domain.observation));
        QVERIFY(!Sequence::complete(domain.state, Target::Off, domain.observation));
    }

    void powerLoss()
    {
        for (auto phase :
             {Phase::Clock,
              Phase::Reset,
              Phase::Release,
              Phase::Connect,
              Phase::Resume,
              Phase::Run,
              Phase::Drain,
              Phase::Isolate,
              Phase::Stop}) {
            Domain domain;
            if (phase == Phase::Drain || phase == Phase::Isolate || phase == Phase::Stop) {
                QVERIFY(domain.settle(Target::Run));
                QVERIFY(domain.reach(Target::Off, phase));
            } else {
                QVERIFY(domain.reach(phase == Phase::Reset ? Target::Reset : Target::Run, phase));
            }
            domain.observation.power = false;
            const bool releasing     = !Sequence::control(domain.state).quiesce
                                       && domain.observation.idle;
            QVERIFY(domain.tick(Target::Run).powerLost);
            QVERIFY(Sequence::fault(domain.state));
            const auto control = Sequence::control(domain.state);
            QVERIFY(control.power && !control.clock && control.reset && control.isolation);
            QCOMPARE(control.quiesce, !releasing);
            for (int cycle = 0; cycle < 8; ++cycle) {
                domain.respond();
                QVERIFY(!domain.tick(Target::Run).powerLost);
                QVERIFY(Sequence::fault(domain.state));
                QVERIFY(!Sequence::complete(domain.state, Target::Run, domain.observation));
            }
            QVERIFY(domain.settle(Target::Off));
            QVERIFY(!Sequence::fault(domain.state));
            QVERIFY(domain.settle(Target::Run));
        }
    }

    void quiesceReturn()
    {
        Domain domain;
        QVERIFY(domain.settle(Target::Run));
        domain.tick(Target::Off);
        domain.observation.idle = true;
        domain.tick(Target::Run);
        for (int cycle = 0; cycle < 8; ++cycle) {
            domain.tick(Target::Off);
            QVERIFY(!Sequence::control(domain.state).quiesce);
            QVERIFY(!Sequence::control(domain.state).isolation);
            QVERIFY(!Sequence::complete(domain.state, Target::Run, domain.observation));
        }
        domain.observation.idle = false;
        domain.tick(Target::Off);
        QVERIFY(Sequence::control(domain.state).quiesce);
        QVERIFY(!Sequence::control(domain.state).isolation);
        QVERIFY(domain.settle(Target::Off));
    }

    void faultDuringResume()
    {
        Domain domain;
        QVERIFY(domain.reach(Target::Run, Phase::Resume));
        QVERIFY(domain.observation.idle);
        domain.observation.power = false;
        QVERIFY(domain.tick(Target::Off).powerLost);
        for (int cycle = 0; cycle < 8; ++cycle) {
            domain.tick(Target::Off);
            const auto control = Sequence::control(domain.state);
            QVERIFY(Sequence::fault(domain.state));
            QVERIFY(control.power && control.reset && control.isolation && !control.clock);
            QVERIFY(!control.quiesce);
        }
        domain.observation.idle = false;
        domain.tick(Target::Off);
        QVERIFY(Sequence::control(domain.state).quiesce);
        QVERIFY(Sequence::fault(domain.state));
        QVERIFY(domain.settle(Target::Off));
    }

    void delayedFeedback()
    {
        using Key      = std::tuple<Phase, Target, bool, bool, bool, bool>;
        const auto key = [](const Domain &domain) -> Key {
            const auto &value = domain.observation;
            return {
                domain.state.phase,
                domain.state.target,
                value.power,
                value.reset,
                value.isolation,
                value.idle};
        };
        std::vector<Domain> pending(1);
        std::set<Key>       visited{key(pending.front())};
        std::set<Phase>     reached;
        for (size_t index = 0; index < pending.size(); ++index) {
            const auto domain = pending[index];
            const auto old    = Sequence::control(domain.state);
            reached.insert(domain.state.phase);
            for (auto target : {Target::Off, Target::Reset, Target::Run}) {
                auto       next   = domain;
                const auto result = next.tick(target);
                QVERIFY(!result.powerLost);
                const auto  control = Sequence::control(next.state);
                const auto &value   = domain.observation;
                if (old.power != control.power)
                    QCOMPARE(value.power, old.power);
                if (old.isolation != control.isolation)
                    QCOMPARE(value.isolation, old.isolation);
                if (old.quiesce != control.quiesce)
                    QCOMPARE(value.idle, old.quiesce);
                if (old.power && !control.power)
                    QVERIFY(value.reset && value.isolation && !old.clock);
                if (old.clock && !control.clock)
                    QVERIFY(value.reset && value.isolation);
                if (old.reset && !control.reset)
                    QVERIFY(value.power && old.clock && value.isolation);
                if (old.isolation && !control.isolation)
                    QVERIFY(value.power && !value.reset && old.clock);
                if (!old.isolation && control.isolation)
                    QVERIFY(old.quiesce && value.idle);
                if (!control.quiesce)
                    QVERIFY(value.power && !value.reset && !value.isolation);
                for (unsigned delay = 0; delay < 16; ++delay) {
                    auto  response = next;
                    auto &observed = response.observation;
                    if ((delay & 1U) != 0)
                        observed.power = control.power;
                    if ((delay & 2U) != 0)
                        observed.reset = control.reset;
                    if ((delay & 4U) != 0)
                        observed.isolation = control.isolation;
                    if ((delay & 8U) != 0)
                        observed.idle = control.quiesce;
                    if (visited.insert(key(response)).second)
                        pending.push_back(response);
                }
            }
        }
        for (auto phase :
             {Phase::Off,
              Phase::Power,
              Phase::Clock,
              Phase::Reset,
              Phase::Release,
              Phase::Connect,
              Phase::Resume,
              Phase::Run,
              Phase::Drain,
              Phase::Isolate,
              Phase::Stop})
            QVERIFY(reached.contains(phase));
        for (const auto &domain : pending) {
            for (auto target : {Target::Off, Target::Reset, Target::Run}) {
                auto resumed = domain;
                QVERIFY(resumed.settle(target));
            }
        }
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocprcmsequence.moc"
