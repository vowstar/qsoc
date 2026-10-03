// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMEMORYBUDGET_H
#define QSOCMEMORYBUDGET_H

#include <chrono>
#include <optional>
#include <QString>

namespace QSocMemoryBudget {

using Clock = std::chrono::steady_clock;

struct Policy
{
    bool    strictSampling = false;
    quint64 reserveBytes   = 0;
};

enum class Coverage { Effective, HostOnly };

struct Snapshot
{
    std::optional<quint64> availableBytes;
    Coverage               coverage = Coverage::HostOnly;
    Clock::time_point      sampledAt;
    QString                kind;
};

enum class Admission { Admitted, Partial, Unverified, WaitingMemory, WaitingMeasurement };

inline bool fresh(const Snapshot &sample, Clock::time_point now)
{
    const auto age = now - sample.sampledAt;
    return age >= Clock::duration::zero() && age <= std::chrono::seconds(1);
}

inline Admission evaluate(
    const Policy     &policy,
    const Snapshot   &sample,
    quint64           reserved,
    quint64           demand,
    Clock::time_point now)
{
    if (!sample.availableBytes || !fresh(sample, now))
        return policy.strictSampling ? Admission::WaitingMeasurement : Admission::Unverified;
    quint64 remaining = *sample.availableBytes;
    for (quint64 requirement : {policy.reserveBytes, reserved, demand}) {
        if (remaining < requirement)
            return Admission::WaitingMemory;
        remaining -= requirement;
    }
    if (sample.coverage == Coverage::HostOnly)
        return policy.strictSampling ? Admission::WaitingMeasurement : Admission::Partial;
    return Admission::Admitted;
}

inline QString name(Admission admission)
{
    switch (admission) {
    case Admission::Admitted:
        return QStringLiteral("admitted");
    case Admission::Partial:
        return QStringLiteral("admission_partial");
    case Admission::Unverified:
        return QStringLiteral("admission_unverified");
    case Admission::WaitingMemory:
        return QStringLiteral("waiting_memory");
    case Admission::WaitingMeasurement:
        return QStringLiteral("waiting_measurement");
    }
    return {};
}

} // namespace QSocMemoryBudget

#endif
