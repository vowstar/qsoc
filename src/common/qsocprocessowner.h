// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPROCESSOWNER_H
#define QSOCPROCESSOWNER_H

#include <memory>
#include <QtGlobal>

/** Terminates this process when its owner exits, even during a blocked tool. */
class QSocProcessOwner
{
public:
    QSocProcessOwner();
    ~QSocProcessOwner();
    bool watch(qint64 parentPid);

private:
    struct State;
    std::unique_ptr<State> state_;
};

#endif // QSOCPROCESSOWNER_H
