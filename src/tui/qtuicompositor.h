// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUICOMPOSITOR_H
#define QTUICOMPOSITOR_H

#include "tui/qtuicompletionpopup.h"
#include "tui/qtuiinputline.h"
#include "tui/qtuiqueuedlist.h"
#include "tui/qtuiscreen.h"
#include "tui/qtuiscrollview.h"
#include "tui/qtuistatusbar.h"
#include "tui/qtuitodolist.h"

#include <QObject>
#include <QTimer>

/**
 * @brief Full-screen TUI compositor for agent mode
 * @details Manages alt-screen buffer, layout regions, and render cycle.
 *
 *          Layout (top to bottom):
 *          Row 0:        Title bar (bold, inverted)
 *          Row 1..N:     Scrollable content area (thinking=dim, normal, tool=bold)
 *          Row N+1..N+5: TODO list (0-5 lines)
 *          Row N+6:      Queued requests (0-3 lines)
 *          Row H-3:      Status bar (spinner + tokens + time)
 *          Row H-2:      Separator "---"
 *          Row H-1:      Input line "> ..."
 */
class QTuiCompositor : public QObject
{
    Q_OBJECT

public:
    explicit QTuiCompositor(QObject *parent = nullptr);
    ~QTuiCompositor() override;

    /* Lifecycle */
    void start(int intervalMs = 100);
    void stop();
    bool isActive() const;

    /* Pause/resume (for shell escape) */
    void pause();
    void resume();

    /* Title bar */
    void setTitle(const QString &title);

    /* Streaming content output */
    void printContent(
        const QString &content, QTuiScrollView::LineStyle style = QTuiScrollView::Normal);

    /* Flush remaining partial line to scrollback as complete line */
    void flushContent();

    /* Force full redraw */
    void render();

    /* Invalidate screen buffer (force full repaint on next render) */
    void invalidate();

    /* Direct access to child widgets */
    QTuiScrollView      &contentView() { return scrollView; }
    QTuiTodoList        &todoList() { return todoWidget; }
    QTuiStatusBar       &statusBar() { return statusBarWidget; }
    QTuiInputLine       &inputLine() { return inputWidget; }
    QTuiQueuedList      &queuedList() { return queueWidget; }
    QTuiCompletionPopup &completionPopup() { return popupWidget; }

signals:
    void tick();

private slots:
    void onTimer();

private:
    QTuiScreen          screen;
    QTuiScrollView      scrollView;
    QTuiTodoList        todoWidget;
    QTuiQueuedList      queueWidget;
    QTuiStatusBar       statusBarWidget;
    QTuiInputLine       inputWidget;
    QTuiCompletionPopup popupWidget;

    QTimer *timer  = nullptr;
    bool    active = false;
    QString title;

    /* Terminal management */
    void enterAltScreen();
    void exitAltScreen();
    int  getTerminalWidth() const;
    int  getTerminalHeight() const;

    /* Scroll control (called from external mouse/key events) */
public:
    void scrollContentUp(int lines = 3);
    void scrollContentDown(int lines = 3);

    /* Reset execution state without stopping the compositor (stay in alt screen) */
    void resetExecution();

private:
    /* Layout calculation */
    struct Layout
    {
        int titleRow      = 0;
        int contentStart  = 1;
        int contentHeight = 0;
        int todoStart     = 0;
        int queueStart    = 0;
        int popupStart    = 0; /* Completion popup: above status bar when visible */
        int statusRow     = 0;
        int separatorRow  = 0;
        int inputRow      = 0;
    } layout;

    void recalculateLayout();

    /* Render each region to screen buffer */
    void renderTitle();
    void renderContent();
    void renderTodo();
    void renderQueued();
    void renderCompletionPopup();
    void renderStatusBar();
    void renderSeparator();
    void renderInput();
};

#endif // QTUICOMPOSITOR_H
