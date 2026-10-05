// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTRANSCRIPTRENDERER_H
#define QSOCTRANSCRIPTRENDERER_H

#include "agent/protocol/qsocagentruntimeevent.h"

#include <nlohmann/json.hpp>

#include <QHash>
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

    /**
     * Add the blocks for one event. Kinds without blocks are ignored. Text
     * from the model, tools, files and providers loses its terminal control
     * sequences first.
     */
    void apply(const QSocAgentRuntimeEvent &event);

    /**
     * Replace the scrollback with a stored message history, rendered from
     * the events the runtime raised live. The todo pane follows the latest
     * todo_list result still in the history.
     */
    void replaceHistory(const nlohmann::json &messages);

private:
    void render(const QSocAgentRuntimeEvent &event);
    void startTool(const QSocAgentRuntimeEvent &event);
    void finishTool(const QSocAgentRuntimeEvent &event);
    void resetExecution();
    void updateTodos(const QString &name, const QString &result);
    void appendDiff(const QString &path, const QString &before, const QString &after);
    void appendImage(const QSocAgentRuntimeEvent &event);
    void appendSummary(const QSocAgentRuntimeEvent &event);

    QTuiCompositor                &compositor;
    QHash<QString, nlohmann::json> pendingArgs;
    bool                           streamedContent = false;
    bool                           replaying       = false;
    bool                           todoPaneKnown   = true;
};

#endif // QSOCTRANSCRIPTRENDERER_H
