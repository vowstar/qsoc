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
    QSocProcessLimits();
    ~QSocProcessLimits();
    bool        configure(QProcess &process, quint64 memoryBytes);
    static bool apply(quint64 memoryBytes);

private:
    struct State;
    std::unique_ptr<State> state_;
};

#endif
