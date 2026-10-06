// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCAGENTFOCUS_H
#define QSOCAGENTFOCUS_H

#include "agent/protocol/qsocagentruntimeevent.h"
#include "cli/qsoctranscriptrenderer.h"
#include "common/qsoctaskregistry.h"
#include "tui/qtuiscrollview.h"
#include "tui/qtuitodolist.h"

#include <nlohmann/json.hpp>

#include <functional>
#include <QJsonObject>
#include <QList>
#include <QString>

class QTuiCompositor;

/**
 * @brief Shows one sub-agent in place of the main transcript.
 * @details While a child is in view, its message history (paged through
 *          `task_tail`) is drawn by the shared transcript renderer. Main's
 *          blocks and todo pane are set aside, and main's events wait and
 *          are drawn on return. Esc returns to main and never stops the
 *          child; Ctrl+C stops the child. Inactive when the daemon does not
 *          advertise the `agents` capability.
 */
class QSocAgentFocus
{
public:
    using Request = std::function<QJsonObject(const QString &, const QJsonObject &)>;

    QSocAgentFocus(
        QTuiCompositor         &compositor,
        QSocTranscriptRenderer &mainRenderer,
        Request                 request,
        bool                    enabled);

    /** True when a daemon greeting offers the sub-agent features. */
    static bool supports(const QJsonObject &greeting);

    /**
     * @brief The agent after @p current in `[main] + live children`.
     * @details Empty means main. Children are deduplicated by agent and
     *          named by their newest run; @p current may be any run of one.
     */
    static QString cycle(
        const QString &current, const QList<QSocTaskRegistry::TaggedRow> &rows, int direction);

    bool    enabled() const { return enabled_; }
    bool    active() const { return !target_.isEmpty(); }
    QString target() const { return target_; }

    /** Title shown while main is in view. */
    void setMainTitle(const QString &title);

    /** Show the child behind run @p id. */
    void enter(const QString &id);

    /** Back to main, drawing the main events that arrived meanwhile. */
    void leave();

    /** Draw @p event now, or hold it while a child is in view. */
    void route(const QSocAgentRuntimeEvent &event);

    /** Esc: true when it returned to main. */
    bool escape();

    /** Ctrl+C: true when consumed. Stops the child unless input was cleared. */
    bool interrupt(bool hadInput);

    /** Message the child in view. Returns the status line to show. */
    QString send(const QString &text);

    /** Fetch new history and redraw. True when it changed. */
    bool refresh();

    const nlohmann::json &messages() const { return messages_; }

private:
    bool poll();
    void showTitle();

    QTuiCompositor               &compositor_;
    QSocTranscriptRenderer       &mainRenderer_;
    QSocTranscriptRenderer        renderer_;
    Request                       request_;
    bool                          enabled_ = false;
    QString                       target_;
    QString                       mainTitle_;
    QString                       mainPlaceholder_;
    nlohmann::json                messages_ = nlohmann::json::array();
    QTuiScrollView                mainTranscript_;
    QList<QTuiTodoList::TodoItem> mainTodos_;
    QList<QSocAgentRuntimeEvent>  backlog_;
};

#endif /* QSOCAGENTFOCUS_H */
