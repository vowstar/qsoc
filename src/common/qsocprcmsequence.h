// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPRCMSEQUENCE_H
#define QSOCPRCMSEQUENCE_H

#include <optional>

enum class QSocPrcmTarget { Off, Reset, Run };

enum class QSocPrcmPhase {
    Init,
    Off,
    Power,
    Clock,
    Reset,
    Release,
    Connect,
    Resume,
    Run,
    Drain,
    Isolate,
    Stop,
    FaultRelease,
    Fault,
    FaultOff,
    FaultPower
};

struct QSocPrcmSequenceState
{
    QSocPrcmPhase  phase                                           = QSocPrcmPhase::Init;
    QSocPrcmTarget target                                          = QSocPrcmTarget::Off;
    bool           operator==(const QSocPrcmSequenceState &) const = default;
};

struct QSocPrcmControl
{
    bool power                                     = false;
    bool clock                                     = false;
    bool reset                                     = true;
    bool isolation                                 = true;
    bool quiesce                                   = true;
    bool operator==(const QSocPrcmControl &) const = default;
};

struct QSocPrcmObservation
{
    bool power     = false;
    bool reset     = true;
    bool isolation = true;
    bool idle      = true;
};

struct QSocPrcmSequenceStep
{
    QSocPrcmSequenceState state;
    bool                  powerLost = false;
};

class QSocPrcmSequence
{
public:
    /* One step follows a full controller cycle. Reset means asserted. */
    static QSocPrcmSequenceStep step(
        QSocPrcmSequenceState         state,
        std::optional<QSocPrcmTarget> request,
        const QSocPrcmObservation    &observation,
        bool                          serviceFault = false);
    static QSocPrcmControl control(const QSocPrcmSequenceState &state);
    static bool            fault(const QSocPrcmSequenceState &state);
    static bool            complete(
        const QSocPrcmSequenceState  &state,
        std::optional<QSocPrcmTarget> request,
        const QSocPrcmObservation    &observation);
};

#endif // QSOCPRCMSEQUENCE_H
