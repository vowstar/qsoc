// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmsequence.h"

namespace {

using Phase  = QSocPrcmPhase;
using Target = QSocPrcmTarget;

bool powered(Phase phase)
{
    switch (phase) {
    case Phase::Clock:
    case Phase::Reset:
    case Phase::Release:
    case Phase::Connect:
    case Phase::Resume:
    case Phase::Run:
    case Phase::Drain:
    case Phase::Isolate:
    case Phase::Stop:
        return true;
    default:
        return false;
    }
}

Phase advance(Phase phase, Target target, const QSocPrcmObservation &value)
{
    const bool protectedDomain = value.reset && value.isolation;
    switch (phase) {
    case Phase::Init:
        return Phase::Off;
    case Phase::Off:
        if (target != Target::Off && !value.power && protectedDomain)
            return Phase::Power;
        break;
    case Phase::Power:
        if (value.power && protectedDomain)
            return target == Target::Off ? Phase::Off : Phase::Clock;
        break;
    case Phase::Clock:
    case Phase::Reset:
        if (protectedDomain) {
            if (target == Target::Off)
                return Phase::Stop;
            return target == Target::Run ? Phase::Release : Phase::Reset;
        }
        break;
    case Phase::Release:
        if (target != Target::Run)
            return Phase::Reset;
        if (!value.reset && value.isolation)
            return Phase::Connect;
        break;
    case Phase::Connect:
        if (!value.isolation && value.idle)
            return target == Target::Run ? Phase::Resume : Phase::Isolate;
        break;
    case Phase::Resume:
        if (!value.idle)
            return target == Target::Run ? Phase::Run : Phase::Drain;
        break;
    case Phase::Run:
        if (target != Target::Run)
            return Phase::Drain;
        break;
    case Phase::Drain:
        if (value.idle)
            return target == Target::Run ? Phase::Resume : Phase::Isolate;
        break;
    case Phase::Isolate:
        if (value.isolation)
            return target == Target::Run ? Phase::Connect : Phase::Reset;
        break;
    case Phase::Stop:
        if (protectedDomain)
            return target == Target::Off ? Phase::Off : Phase::Clock;
        break;
    case Phase::FaultRelease:
        if (!value.idle)
            return Phase::Fault;
        break;
    case Phase::Fault:
        if (protectedDomain && value.idle)
            return Phase::FaultOff;
        break;
    case Phase::FaultPower:
        if (value.power)
            return Phase::Fault;
        break;
    case Phase::FaultOff:
        if (target == Target::Off && !value.power && protectedDomain)
            return Phase::Off;
        break;
    }
    return phase;
}

} // namespace

QSocPrcmSequenceStep QSocPrcmSequence::step(
    QSocPrcmSequenceState         state,
    std::optional<QSocPrcmTarget> request,
    const QSocPrcmObservation    &observation,
    bool                          serviceFault)
{
    if (request)
        state.target = *request;
    const bool lost      = powered(state.phase) && !observation.power;
    const bool releasing = !control(state).quiesce && observation.idle;
    if (serviceFault && !fault(state)) {
        if (state.phase == Phase::Init || state.phase == Phase::Off)
            state.phase = Phase::FaultOff;
        else if (state.phase == Phase::Power)
            state.phase = Phase::FaultPower;
        else
            state.phase = releasing ? Phase::FaultRelease : Phase::Fault;
        return {state, lost};
    }
    state.phase = lost ? (releasing ? Phase::FaultRelease : Phase::Fault)
                       : advance(state.phase, state.target, observation);
    return {state, lost};
}

QSocPrcmControl QSocPrcmSequence::control(const QSocPrcmSequenceState &state)
{
    switch (state.phase) {
    case Phase::Init:
    case Phase::Off:
    case Phase::FaultOff:
        return {};
    case Phase::Power:
    case Phase::FaultPower:
    case Phase::Stop:
    case Phase::Fault:
        return {true, false, true, true, true};
    case Phase::Clock:
    case Phase::Reset:
        return {true, true, true, true, true};
    case Phase::Release:
    case Phase::Isolate:
        return {true, true, false, true, true};
    case Phase::Connect:
    case Phase::Drain:
        return {true, true, false, false, true};
    case Phase::Run:
    case Phase::Resume:
        return {true, true, false, false, false};
    case Phase::FaultRelease:
        return {true, false, true, true, false};
    }
    return {};
}

bool QSocPrcmSequence::fault(const QSocPrcmSequenceState &state)
{
    return state.phase == Phase::FaultRelease || state.phase == Phase::Fault
           || state.phase == Phase::FaultOff || state.phase == Phase::FaultPower;
}

bool QSocPrcmSequence::complete(
    const QSocPrcmSequenceState  &state,
    std::optional<QSocPrcmTarget> request,
    const QSocPrcmObservation    &observation)
{
    if (!request || *request != state.target)
        return false;
    switch (*request) {
    case Target::Off:
        return state.phase == Phase::Off && !observation.power && observation.reset
               && observation.isolation && observation.idle;
    case Target::Reset:
        return state.phase == Phase::Reset && observation.power && observation.reset
               && observation.isolation && observation.idle;
    case Target::Run:
        return state.phase == Phase::Run && observation.power && !observation.reset
               && !observation.isolation && !observation.idle;
    }
    return false;
}
