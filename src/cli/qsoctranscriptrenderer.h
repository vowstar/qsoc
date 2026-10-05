// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTRANSCRIPTRENDERER_H
#define QSOCTRANSCRIPTRENDERER_H

#include "agent/protocol/qsocagentruntimeevent.h"

#include <nlohmann/json_fwd.hpp>

#include <QHash>
#include <QJsonObject>
#include <QString>

class QTuiCompositor;

/**
 * @brief Turns runtime events into transcript blocks.
 * @details Owns every write to the compositor's scrollback and todo pane.
 *          It never renders the screen and never touches the status bar,
 *          the input line or the daemon, so it accepts any event source.
 */
class QSocTranscriptRenderer
{
public:
    explicit QSocTranscriptRenderer(QTuiCompositor &compositor);

    /** Add the blocks for one event. Kinds without blocks are ignored. */
    void apply(const QSocAgentRuntimeEvent &event);

    /** Replace the scrollback with a stored message history. */
    void replaceHistory(const nlohmann::json &messages);

private:
    void startTool(const QSocAgentRuntimeEvent &event);
    void finishTool(const QSocAgentRuntimeEvent &event);
    void updateTodos(const QString &name, const QString &result);
    void appendDiff(const QString &path, const QString &before, const QString &after);
    void appendImage(const QSocAgentRuntimeEvent &event);

    QTuiCompositor             &compositor;
    QHash<QString, QJsonObject> pendingArgs;
    bool                        streamedContent = false;
};

#endif // QSOCTRANSCRIPTRENDERER_H
