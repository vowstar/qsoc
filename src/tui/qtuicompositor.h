// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUICOMPOSITOR_H
#define QTUICOMPOSITOR_H

#include "tui/qtuichipbanner.h"
#include "tui/qtuicompletionpopup.h"
#include "tui/qtuiinputline.h"
#include "tui/qtuiqueuedlist.h"
#include "tui/qtuiscreen.h"
#include "tui/qtuiscrollview.h"
#include "tui/qtuistatusbar.h"
#include "tui/qtuitaskoverlay.h"
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

    /**
     * @brief Which UI element currently consumes keyboard input.
     * @details Existing popup logic remains gated by isVisible() inline
     *          at the REPL — those paths are functionally equivalent to
     *          a CompletionPopup focus check. The enum exists so the
     *          task overlay (and future modal widgets) can declare a
     *          focus state the REPL can branch on without ad-hoc flags.
     *          Defaults to Input; flipped by setFocusOwner.
     */
    enum class FocusOwner {
        Input,
        CompletionPopup,
        TaskPill,    /* Down-arrow from empty input parks here; Enter opens overlay. */
        TaskOverlay, /* Modal task list / detail. */
    };

    void       setFocusOwner(FocusOwner owner);
    FocusOwner currentFocus() const { return focusOwner_; }

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

    /* Retire the top banner; freed rows fold into scroll viewport. */
    void dismissTopBanner();

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
    QTuiChipBanner      &topBanner() { return topBannerWidget; }
    QTuiTaskOverlay     &taskOverlay() { return taskOverlayWidget; }

signals:
    void tick();

private slots:
    void onTimer();

private:
    QTuiScreen          screen;
    QTuiChipBanner      topBannerWidget;
    QTuiScrollView      scrollView;
    QTuiTodoList        todoWidget;
    QTuiQueuedList      queueWidget;
    QTuiStatusBar       statusBarWidget;
    QTuiInputLine       inputWidget;
    QTuiCompletionPopup popupWidget;
    QTuiTaskOverlay     taskOverlayWidget;

    QTimer    *timer  = nullptr;
    bool       active = false;
    QString    title;
    FocusOwner focusOwner_ = FocusOwner::Input;

    /* Terminal management */
    void enterAltScreen();
    void exitAltScreen();

    /* Scroll control (called from external mouse/key events) */
public:
    int  getTerminalWidth() const;
    int  getTerminalHeight() const;
    void scrollContentUp(int lines = 3);
    void scrollContentDown(int lines = 3);

    /* Mouse text selection + auto-copy. Call from input monitor signals. */
    void selectionStart(int col, int row);
    void selectionUpdate(int col, int row);
    void selectionFinish(int col, int row);

    /* Reset execution state without stopping the compositor (stay in alt screen) */
    void resetExecution();

private:
    /* Layout calculation */
    struct Layout
    {
        int titleRow      = 0;
        int topBannerRow  = 1;
        int topBannerH    = 0;
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

    /* Selection state for click-drag copy. Coords are screen-space. */
    struct Selection
    {
        int  anchorCol = 0;
        int  anchorRow = 0;
        int  focusCol  = 0;
        int  focusRow  = 0;
        bool active    = false;
    } selection;

    void applySelectionHighlight();
    void copySelectionToClipboard();

    /* Render each region to screen buffer */
    void renderTitle();
    void renderTopBanner();
    void renderContent();
    void renderTodo();
    void renderQueued();
    void renderCompletionPopup();
    void renderTaskOverlay();
    void renderStatusBar();
    void renderSeparator();
    void renderInput();
};

#endif // QTUICOMPOSITOR_H
