// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCPRCMSEQUENCERTL_H
#define QSOCPRCMSEQUENCERTL_H

#include <QString>

class QSocPrcmSequenceRtl
{
public:
    /* The caller supplies one decoded target and retains it for an invalid request. */
    static QString generate();
    static QString generateService();
};

#endif // QSOCPRCMSEQUENCERTL_H
