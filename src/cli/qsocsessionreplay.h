// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSESSIONREPLAY_H
#define QSOCSESSIONREPLAY_H

#include "agent/protocol/qsocagentruntimeevent.h"

#include <nlohmann/json_fwd.hpp>

#include <QList>

namespace QSocSessionReplay {

/**
 * @brief The events the runtime raises live for a stored message history.
 * @details Pure: reads the history and never throws.
 */
QList<QSocAgentRuntimeEvent> events(const nlohmann::json &messages);

} // namespace QSocSessionReplay

#endif // QSOCSESSIONREPLAY_H
