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

    /* Streaming markdown ingestion. Each chunk is appended to the
     * current assistant block; a fresh block is created on first
     * call after a tool/system print broke the run. finishStream
     * clears the cursor so the next chunk begins a new block. */
    void appendAssistantChunk(const QString &chunk);
    void appendReasoningChunk(const QString &chunk);
    void finishStream();

    /* Tool-call lifecycle hooks. beginToolUse pushes a bordered
     * QTuiToolBlock onto the scrollback and remembers it; appendBody
     * streams output into the active block; finishToolUse stamps the
     * footer status. Each turn may have multiple tool calls in flight
     * conceptually but only one active block at a time, since tool
     * output never interleaves. */
    void beginToolUse(const QString &toolName, const QString &detail);
    void appendToolUseBody(const QString &chunk);
    void finishToolUse(bool success, const QString &summary = QString());

    /* Push the user's input as a distinct scrollback block. Replaces
     * the prior `compositor.printContent("qsoc> " + input + "\n")`
     * pattern so a focused user message can be copied back as a
     * blockquote-formatted markdown string. */
    void appendUserMessage(const QString &text);

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

    /* Cursors into the scrollback for the active streaming blocks.
     * Non-owning; the scrollview owns the blocks. Cleared by
     * finishStream() and whenever the cursor stops pointing at
     * scrollback's last block (an unrelated print landed). */
    class QTuiAssistantTextBlock *activeAssistant = nullptr;
    class QTuiAssistantTextBlock *activeReasoning = nullptr;

    /* Active fenced-code-block cursors. The streaming markdown
     * splitter creates one whenever a triple-backtick fence opens in
     * the matching stream, and clears the pointer when the closing
     * fence arrives. */
    class QTuiCodeBlock *activeAssistantCode = nullptr;
    class QTuiCodeBlock *activeReasoningCode = nullptr;

    /* Carry buffers for incomplete trailing lines: streaming chunks
     * can split a triple-backtick fence across two messages, so the
     * splitter only commits a line when it sees a newline. The
     * `committed*Source` strings hold the prose lines already
     * finalised into the active text block; the active block's
     * markdown shows committed + pending so the user sees partial
     * lines instantly even before the next \n arrives. */
    QString pendingAssistantLine;
    QString pendingReasoningLine;
    QString committedAssistantSource;
    QString committedReasoningSource;

    /* Group identifiers tie all blocks created during one assistant
     * (or reasoning) run together, so the auto-fold policy can collapse
     * an entire prior reasoning monologue (text + code) as a unit. The
     * "current" id is non-zero while a stream is in flight; reset to
     * zero when the run ends so the next chunk knows to allocate a
     * fresh group. */
    int nextGroupId             = 0;
    int currentAssistantGroupId = 0;
    int currentReasoningGroupId = 0;

    /* Reasoning-run tracking grouped by run id. Older groups are
     * folded as a whole when a new reasoning group starts or
     * non-reasoning content arrives. */
    struct ReasoningGroup
    {
        int                            groupId;
        std::vector<class QTuiBlock *> blocks;
    };
    std::vector<ReasoningGroup> reasoningHistory;

    /* Active tool-call cursor. Cleared by finishToolUse() and any
     * intervening printContent / streaming chunk so the next tool
     * call lands on a fresh block. */
    class QTuiToolBlock *activeTool = nullptr;

    enum class StreamMode : std::uint8_t {
        Assistant,
        Reasoning,
    };

    /* Streaming markdown splitter shared by assistant and reasoning
     * streams. Routes incoming chunks line-by-line into either the
     * matching active text block or a freshly-created QTuiCodeBlock,
     * depending on whether triple-backtick fences are open. */
    void feedSplitChunk(const QString &chunk, StreamMode mode);

    /* Flush remaining pending-line content into whichever active block
     * is current, then clear the splitter cursors and group id for the
     * given stream so the next chunk starts a fresh run. */
    void sealStream(StreamMode mode);

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

    /* Block-level focus + clipboard. focusBlockAtScreenRow lets the
     * input monitor route a single mouse click into block focus when
     * the user is not dragging a text selection. copyFocusedBlock
     * emits the focused block's markdown payload as an OSC 52
     * clipboard write so any modern terminal (or tmux with
     * set-clipboard on) places it on the system clipboard. */
    void focusBlockAtScreenRow(int screenRow);
    void clearBlockFocus();
    bool copyFocusedBlock();

    /* Step the focused block's horizontal scroll. delta=+1 scrolls
     * right one cell, delta=-1 scrolls left. No-op when nothing is
     * focused or the focused block does not opt into h-scroll. */
    void scrollFocusedBlockHorizontal(int delta);

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
