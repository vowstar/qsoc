// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSESSIONTRANSCRIPT_H
#define QSOCSESSIONTRANSCRIPT_H

#include <nlohmann/json_fwd.hpp>

class QTuiScrollView;

namespace QSocSessionTranscript {

void appendTo(const nlohmann::json &messages, QTuiScrollView &view);

} // namespace QSocSessionTranscript

#endif // QSOCSESSIONTRANSCRIPT_H
