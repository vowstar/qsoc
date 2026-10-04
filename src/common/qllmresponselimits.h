// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QLLMRESPONSELIMITS_H
#define QLLMRESPONSELIMITS_H

#include <string_view>
#include <QtGlobal>

struct QLLMResponseLimits
{
    qint64 maxBytes         = 64 * 1024 * 1024;
    qint64 maxEventBytes    = 8 * 1024 * 1024;
    qint64 maxArgumentBytes = 4 * 1024 * 1024;
    int    maxToolCalls     = 1024;
    int    maxJsonDepth     = 64;

    bool valid() const;
};

bool qllmJsonDepthAllowed(std::string_view source, int limit);

#endif // QLLMRESPONSELIMITS_H
