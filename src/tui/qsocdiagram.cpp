// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qsocdiagram_p.h"
#include "tui/qtuiwidget.h"

namespace QSocDiagram {
Result render(const QString &source, int width)
{
    using namespace Detail;
    try {
        require(source.size() <= sourceLimit && source.toUtf8().size() <= sourceLimit, Error::Limit);
        const auto lines = statements(source);
        require(!lines.isEmpty());
        const auto header = lines.first();
        const auto body   = lines.sliced(1);
        if (header == "sequenceDiagram")
            return sequence(body, width);
        const bool relationship = header == "classDiagram" || header == "erDiagram"
                                  || header == "stateDiagram" || header == "stateDiagram-v2";
        return drawGraph(relationship ? relations(header, body) : flowchart(header, body), width);
    } catch (const Failure &failure) {
        return {.rows = {}, .error = failure.error};
    }
}
} // namespace QSocDiagram
