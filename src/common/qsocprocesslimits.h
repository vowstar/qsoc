// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPROCESSLIMITS_H
#define QSOCPROCESSLIMITS_H

#include <memory>
#include <QtGlobal>

class QProcess;

class QSocProcessLimits
{
public:
    enum class ApplyResult {
        Success                  = 0,
        Unsupported              = 30,
        InvalidPageSize          = 31,
        ProbeSizeOverflow        = 32,
        ProbeMapping             = 33,
        ProbeUnmapping           = 34,
        TaskInfo                 = 35,
        AddressSizeOverflow      = 36,
        ReadAddressLimit         = 37,
        SetAddressLimit          = 38,
        SetCoreLimit             = 39,
        ReadInstalledLimit       = 40,
        InstalledLimitMismatch   = 41,
        ProbeUnexpectedlyAllowed = 42,
        ProbeUnexpectedError     = 43,
        QueryJob                 = 44,
        JobLimitMismatch         = 45
    };

    QSocProcessLimits();
    ~QSocProcessLimits();
    bool               configure(QProcess &process, quint64 memoryBytes);
    static ApplyResult apply(quint64 memoryBytes);

private:
    struct State;
    std::unique_ptr<State> state_;
};

#endif
