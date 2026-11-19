// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QLLMRESPONSES_H
#define QLLMRESPONSES_H

#include "common/qllmservice.h"

namespace QLLMResponses {

json buildRequest(
    const json           &messages,
    const json           &tools,
    const LLMModelConfig &endpoint,
    double                temperature,
    const QString        &effort,
    bool                  stream,
    bool                  jsonMode);
json    toChatResponse(const json &response, const QString &route);
QString replayRoute(const LLMModelConfig &endpoint);
void    removeReplay(json &messages);

} // namespace QLLMResponses

#endif // QLLMRESPONSES_H
