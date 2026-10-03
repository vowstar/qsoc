// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
#ifndef QSOCAGENTPROTOCOL_H
#define QSOCAGENTPROTOCOL_H

#include "common/qsocipc.h"

namespace QSocAgentProtocol {
inline constexpr int version         = 1;
inline constexpr int headerBytes     = QSocIpc::headerBytes;
inline constexpr int maxPayloadBytes = QSocIpc::maxPayloadBytes;
using QSocIpc::frame;
using QSocIpc::payloadLength;
} // namespace QSocAgentProtocol
#endif
