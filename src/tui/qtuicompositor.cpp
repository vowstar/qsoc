// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuicompositor.h"

#include "tui/qtuiassistanttextblock.h"
#include "tui/qtuicodeblock.h"
#include "tui/qtuitoolblock.h"
#include "tui/qtuiuserblock.h"
#include "tui/qtuiwidget.h"

#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

QTuiCompositor::QTuiCompositor(QObject *parent)
    : QObject(parent)
    , timer(new QTimer(this))
{
    connect(timer, &QTimer::timeout, this, &QTuiCompositor::onTimer);
}

QTuiCompositor::~QTuiCompositor()
{
    if (active) {
        stop();
    }
}

void QTuiCompositor::setFocusOwner(FocusOwner owner)
{
    focusOwner_ = owner;
}

void QTuiCompositor::start(int intervalMs)
{
    if (active) {
        return;
    }
    active = true;
    enterAltScreen();

    int termW = getTerminalWidth();
    int termH = getTerminalHeight();
    screen.resize(termW, termH);
    recalculateLayout();

    timer->start(intervalMs);
    render();
}

void QTuiCompositor::stop()
{
    if (!active) {
        return;
    }
    timer->stop();
    active = false;
    exitAltScreen();

    /* Block-aware ANSI dump preserves headings, code blocks, tool
     * boxes, diff colors, and OSC 8 hyperlinks in the user's normal
     * scrollback after the alt screen unwinds. Falling back to plain
     * text would erase every styling cue the agent produced. */
    const int     dumpWidth = qMax(20, getTerminalWidth());
    const QString content   = scrollView.toAnsi(dumpWidth);
    if (!content.isEmpty()) {
        fputs(content.toUtf8().constData(), stdout);
        if (!content.endsWith('\n')) {
            fputs("\n", stdout);
        }
        fflush(stdout);
    }

    scrollView.clear();
}

bool QTuiCompositor::isActive() const
{
    return active;
}

void QTuiCompositor::pause()
{
    if (!active) {
        return;
    }
    timer->stop();
    exitAltScreen();
}

void QTuiCompositor::resume()
{
    if (!active) {
        return;
    }
    enterAltScreen();
    int termW = getTerminalWidth();
    int termH = getTerminalHeight();
    screen.resize(termW, termH);
    screen.invalidate();
    recalculateLayout();
    timer->start();
    render();
}

void QTuiCompositor::setTitle(const QString &newTitle)
{
    title = newTitle;
}

void QTuiCompositor::printContent(const QString &content, QTuiScrollView::LineStyle style)
{
    /* Anything routed through printContent counts as non-streaming
     * output; seal both assistant and reasoning streams first so the
     * next streaming chunk starts after this content rather than
     * back-filling onto the wrong block. Sealing also clears the
     * code-block cursors and pending line buffers, and folds older
     * reasoning groups by virtue of resetting the run-group id. */
    sealStream(StreamMode::Assistant);
    sealStream(StreamMode::Reasoning);
    /* Old reasoning monologue (across all groups) collapses since the
     * user is clearly past it. */
    for (auto &group : reasoningHistory) {
        for (auto *block : group.blocks) {
            block->setFolded(true);
        }
    }
    scrollView.appendPartial(content, style);
}

void QTuiCompositor::appendAssistantChunk(const QString &chunk)
{
    feedSplitChunk(chunk, StreamMode::Assistant);
}

void QTuiCompositor::appendReasoningChunk(const QString &chunk)
{
    feedSplitChunk(chunk, StreamMode::Reasoning);
}

void QTuiCompositor::finishStream()
{
    sealStream(StreamMode::Assistant);
    sealStream(StreamMode::Reasoning);
}

void QTuiCompositor::feedSplitChunk(const QString &chunk, StreamMode mode)
{
    if (chunk.isEmpty()) {
        return;
    }

    QString                 &pending   = (mode == StreamMode::Assistant) ? pendingAssistantLine
                                                                         : pendingReasoningLine;
    QString                 &committed = (mode == StreamMode::Assistant) ? committedAssistantSource
                                                                         : committedReasoningSource;
    QTuiAssistantTextBlock *&activeText = (mode == StreamMode::Assistant) ? activeAssistant
                                                                          : activeReasoning;
    QTuiCodeBlock          *&activeCode = (mode == StreamMode::Assistant) ? activeAssistantCode
                                                                          : activeReasoningCode;
    int       &currentGroup             = (mode == StreamMode::Assistant) ? currentAssistantGroupId
                                                                          : currentReasoningGroupId;
    const bool forceDim                 = (mode == StreamMode::Reasoning);

    /* Cursors stale because something else landed on top of our last
     * block: drop them and reset committed prose so the next line
     * creates a fresh block rather than back-filling. */
    if (activeText != nullptr && scrollView.lastBlock() != activeText
        && scrollView.lastBlock() != activeCode) {
        activeText = nullptr;
        committed.clear();
    }
    if (activeCode != nullptr && scrollView.lastBlock() != activeCode
        && scrollView.lastBlock() != activeText) {
        activeCode = nullptr;
    }

    auto ensureRunGroup = [&]() {
        if (currentGroup != 0) {
            return;
        }
        ++nextGroupId;
        currentGroup = nextGroupId;
        if (mode == StreamMode::Reasoning) {
            /* Starting a new reasoning run collapses every prior
             * reasoning group so only the freshest monologue stays
             * expanded in the scrollback. */
            for (auto &group : reasoningHistory) {
                for (auto *block : group.blocks) {
                    block->setFolded(true);
                }
            }
            reasoningHistory.push_back({.groupId = currentGroup, .blocks = {}});
        }
    };

    auto trackReasoningBlock = [&](QTuiBlock *block) {
        if (mode != StreamMode::Reasoning) {
            return;
        }
        if (reasoningHistory.empty() || reasoningHistory.back().groupId != currentGroup) {
            return;
        }
        reasoningHistory.back().blocks.push_back(block);
    };

    pending.append(chunk);

    while (true) {
        const int idx = pending.indexOf(QLatin1Char('\n'));
        if (idx < 0) {
            break;
        }
        const QString line       = pending.left(idx);
        pending                  = pending.mid(idx + 1);
        const QString lineWithNl = line + QLatin1Char('\n');

        const QString trimmed = line.trimmed();
        const bool    isFence = trimmed.startsWith(QStringLiteral("```"));

        if (isFence) {
            if (activeCode == nullptr) {
                /* Opening fence: language tag is the rest of the
                 * fence line; lower-cased so the highlighter and the
                 * markdown round-trip both see a normalised id.
                 * Strip any provisional pending-display from the text
                 * block by reverting its markdown to committed prose. */
                const QString lang = trimmed.mid(3).trimmed().toLower();
                if (activeText != nullptr) {
                    activeText->setMarkdown(committed);
                }
                activeText = nullptr;
                committed.clear();
                ensureRunGroup();
                auto block
                    = std::make_unique<QTuiCodeBlock>(lang, QString(), forceDim, currentGroup);
                activeCode = block.get();
                trackReasoningBlock(activeCode);
                scrollView.appendBlock(std::move(block));
            } else {
                /* Closing fence: code body is finalised; the next
                 * prose line will create a fresh assistant text block
                 * in the same run group. */
                activeCode = nullptr;
            }
            continue;
        }

        if (activeCode != nullptr) {
            activeCode->appendBody(lineWithNl);
            continue;
        }

        ensureRunGroup();
        if (activeText == nullptr) {
            auto block = std::make_unique<QTuiAssistantTextBlock>();
            if (forceDim) {
                block->setDimAll(true);
            }
            activeText = block.get();
            trackReasoningBlock(activeText);
            scrollView.appendBlock(std::move(block));
        }
        committed.append(lineWithNl);
        activeText->setMarkdown(committed);
    }

    /* Provisionally show the pending trailing line so streaming text
     * appears live, even when chunks lack newlines. The next chunk
     * either commits this line (newline arrives) or keeps extending
     * it. The text block's setMarkdown is idempotent — subsequent
     * chunks rewrite the source rather than accumulate. */
    if (activeText != nullptr && !pending.isEmpty()) {
        activeText->setMarkdown(committed + pending);
    } else if (activeText == nullptr && activeCode == nullptr && !pending.isEmpty()) {
        /* No active block yet but a partial line is being typed (e.g.,
         * a pure prose chunk without trailing \n). Open a fresh text
         * block so the user sees the partial line. */
        ensureRunGroup();
        auto block = std::make_unique<QTuiAssistantTextBlock>();
        if (forceDim) {
            block->setDimAll(true);
        }
        activeText = block.get();
        trackReasoningBlock(activeText);
        scrollView.appendBlock(std::move(block));
        activeText->setMarkdown(pending);
    }
}

void QTuiCompositor::sealStream(StreamMode mode)
{
    QString                 &pending   = (mode == StreamMode::Assistant) ? pendingAssistantLine
                                                                         : pendingReasoningLine;
    QString                 &committed = (mode == StreamMode::Assistant) ? committedAssistantSource
                                                                         : committedReasoningSource;
    QTuiAssistantTextBlock *&activeText = (mode == StreamMode::Assistant) ? activeAssistant
                                                                          : activeReasoning;
    QTuiCodeBlock          *&activeCode = (mode == StreamMode::Assistant) ? activeAssistantCode
                                                                          : activeReasoningCode;
    int &currentGroup                   = (mode == StreamMode::Assistant) ? currentAssistantGroupId
                                                                          : currentReasoningGroupId;

    /* Promote any incomplete trailing line into a final commit so the
     * sealed block reflects everything the user saw. The receiving
     * consumer is forgiving about a missing final \n. */
    if (!pending.isEmpty()) {
        if (activeCode != nullptr) {
            activeCode->appendBody(pending);
        } else if (activeText != nullptr) {
            committed.append(pending);
            activeText->setMarkdown(committed);
        }
        pending.clear();
    }
    committed.clear();
    activeText   = nullptr;
    activeCode   = nullptr;
    currentGroup = 0;
}

void QTuiCompositor::beginToolUse(const QString &toolName, const QString &detail)
{
    /* A new tool call seals both streaming runs so the box lands after
     * any in-flight prose / code, not retroactively before it. */
    sealStream(StreamMode::Assistant);
    sealStream(StreamMode::Reasoning);
    auto block = std::make_unique<QTuiToolBlock>(toolName, detail);
    activeTool = block.get();
    scrollView.appendBlock(std::move(block));
}

void QTuiCompositor::appendToolUseBody(const QString &chunk)
{
    if (activeTool == nullptr) {
        return;
    }
    if (scrollView.lastBlock() != activeTool) {
        /* Something else landed on top of the active tool block; abort
         * the cursor rather than silently inject mid-history. */
        activeTool = nullptr;
        return;
    }
    activeTool->appendBody(chunk);
}

void QTuiCompositor::finishToolUse(bool success, const QString &summary)
{
    if (activeTool == nullptr) {
        return;
    }
    activeTool
        ->finish(success ? QTuiToolBlock::Status::Success : QTuiToolBlock::Status::Failure, summary);
    activeTool = nullptr;
}

void QTuiCompositor::appendUserMessage(const QString &text)
{
    /* New user input ends every active streaming context; the next
     * agent chunk lands below the user's message in a fresh run group. */
    sealStream(StreamMode::Assistant);
    sealStream(StreamMode::Reasoning);
    activeTool = nullptr;
    scrollView.appendBlock(std::make_unique<QTuiUserBlock>(text));
}

void QTuiCompositor::focusBlockAtScreenRow(int screenRow)
{
    const int idx = scrollView.blockAtScreenRow(screenRow);
    if (idx < 0) {
        scrollView.setFocusedBlockIdx(-1);
    } else if (idx == scrollView.focusedBlockIdx()) {
        /* Re-clicking the focused block toggles its fold. Lets users
         * collapse and re-expand a thinking / tool / table block with
         * the same gesture used to focus it. */
        scrollView.toggleFocusedFold();
    } else {
        scrollView.setFocusedBlockIdx(idx);
    }
    screen.invalidate();
    render();
}

void QTuiCompositor::clearBlockFocus()
{
    scrollView.setFocusedBlockIdx(-1);
    screen.invalidate();
    render();
}

void QTuiCompositor::scrollFocusedBlockHorizontal(int delta)
{
    scrollView.scrollFocusedHorizontal(delta);
    screen.invalidate();
    render();
}

bool QTuiCompositor::copyFocusedBlock()
{
    /* Markdown form is the right copy default: it round-trips through
     * the next agent turn intact, so users can paste a block back as
     * input without losing structure. */
    const QString payload = scrollView.copyFocusedAsMarkdown();
    if (payload.isEmpty()) {
        return false;
    }
    /* OSC 52 clipboard write. tmux gates this behind
     * `set-clipboard on`; modern terminals (iTerm2, Alacritty, Kitty,
     * WezTerm, foot) accept it without configuration. */
    const QByteArray base64 = payload.toUtf8().toBase64();
    const QByteArray osc    = QByteArray("\x1b]52;c;") + base64 + QByteArray("\x1b\\");
    fwrite(osc.constData(), 1, osc.size(), stdout);
    fflush(stdout);
    return true;
}

void QTuiCompositor::dismissTopBanner()
{
    if (topBannerWidget.isHidden()) {
        return;
    }
    topBannerWidget.setHidden(true);
    screen.invalidate();
}

void QTuiCompositor::flushContent()
{
    /* Force partial line to become complete line */
    scrollView.appendPartial("\n", QTuiScrollView::Normal);
}

void QTuiCompositor::onTimer()
{
    /* Check terminal resize */
    int termW = getTerminalWidth();
    int termH = getTerminalHeight();
    if (termW != screen.width() || termH != screen.height()) {
        screen.resize(termW, termH);
        screen.invalidate();
        recalculateLayout();
    }

    /* Tick animations */
    todoWidget.tick();
    statusBarWidget.tick();
    topBannerWidget.tick();
    taskOverlayWidget.tick();
    emit tick();

    render();
}

void QTuiCompositor::resetExecution()
{
    statusBarWidget.stopTimers();
    statusBarWidget.setStatus("Ready");
    queueWidget.clearAll();
    scrollView.scrollToBottom();
    render();
}

void QTuiCompositor::invalidate()
{
    screen.invalidate();
}

void QTuiCompositor::scrollContentUp(int lineCount)
{
    scrollView.scrollUp(lineCount);
}

void QTuiCompositor::scrollContentDown(int lineCount)
{
    scrollView.scrollDown(lineCount);
}

void QTuiCompositor::render()
{
    if (!active) {
        return;
    }

    screen.clear();
    recalculateLayout();

    renderTitle();
    renderTopBanner();
    renderContent();
    renderTodo();
    renderQueued();
    renderCompletionPopup();
    renderTaskOverlay();
    renderStatusBar();
    renderSeparator();
    renderInput();

    /* Overlay selection highlight after all widgets so selected cells
     * invert regardless of which widget owns them. */
    if (selection.active) {
        applySelectionHighlight();
    }

    QString ansi = screen.toAnsi();
    fputs(ansi.toUtf8().constData(), stdout);

    /* Park real cursor at input line for IME support.
     * Terminal emulators render IME preedit at the physical cursor.
     * Disable auto-wrap to prevent IME preedit from causing line wrap.
     * Clamp the column to the screen width so a long pasted/typed
     * line does not push the cursor past the right edge — when the
     * cursor sits past the boundary, terminals clamp to the last
     * column and any subsequent typed character lingers there,
     * producing the column-of-stray-glyphs artifact during streams. */
    int cursorCol = qBound(1, inputWidget.cursorColumn() + 1, screen.width());
    int cursorRow = qBound(1, layout.inputRow + inputWidget.cursorLine() + 1, screen.height());
    fprintf(stdout, "\033[?7l\033[%d;%dH\033[?25h", cursorRow, cursorCol);

    fflush(stdout);
}

void QTuiCompositor::enterAltScreen()
{
    /* Hide cursor + enter alt screen + clear + disable auto-wrap.
     * Mouse: ?1000h (button press/release/wheel) + ?1002h (drag motion)
     * + ?1006h (SGR encoding). Enables in-app click-drag text selection
     * with auto-copy to clipboard via OSC 52. */
    fputs(
        "\033[?25l\033[?1049h\033[2J\033[H"
        "\033[?7l"
        "\033[?1000h\033[?1002h\033[?1006h",
        stdout);
    fflush(stdout);
}

void QTuiCompositor::exitAltScreen()
{
    /* Disable mouse + re-enable auto-wrap + exit alt screen + show cursor */
    fputs(
        "\033[?1006l\033[?1002l\033[?1000l"
        "\033[?7h"
        "\033[?1049l\033[?25h",
        stdout);
    fflush(stdout);
}

int QTuiCompositor::getTerminalWidth() const
{
#ifdef Q_OS_WIN
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        return csbi.srWindow.Right - csbi.srWindow.Left + 1;
    }
    return 80;
#else
    struct winsize winsz = {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &winsz) == 0 && winsz.ws_col > 0) {
        return winsz.ws_col;
    }
    return 80;
#endif
}

int QTuiCompositor::getTerminalHeight() const
{
#ifdef Q_OS_WIN
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        return csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
    return 24;
#else
    struct winsize winsz = {};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &winsz) == 0 && winsz.ws_row > 0) {
        return winsz.ws_row;
    }
    return 24;
#endif
}

void QTuiCompositor::recalculateLayout()
{
    int totalH = screen.height();
    int totalW = screen.width();

    /* Dynamic input height: grows with multi-line content, capped at 1/3 of
     * screen. Tell the input widget the current width first so soft-wrap
     * can be reflected in the visual row count. */
    inputWidget.setTerminalWidth(totalW);
    int inputH    = inputWidget.lineCount();
    int maxInputH = qMax(1, totalH / 3);
    inputH        = qBound(1, inputH, maxInputH);

    /* Fixed regions at bottom (from bottom up) — inputRow is the TOP of input area */
    layout.inputRow     = totalH - inputH;
    layout.separatorRow = layout.inputRow - 1;
    layout.statusRow    = layout.separatorRow - 1;

    /* Completion popup: appears just above the status bar when visible */
    int popupLines    = popupWidget.lineCount();
    layout.popupStart = layout.statusRow - popupLines;

    int queueLines    = queueWidget.lineCount();
    layout.queueStart = layout.popupStart - queueLines;

    int todoLines    = todoWidget.lineCount();
    layout.todoStart = layout.queueStart - todoLines;

    /* Top banner sits between title and content; its height depends
     * on terminal width (responsive layout) and visibility. */
    layout.titleRow     = 0;
    layout.topBannerRow = 1;
    topBannerWidget.setTerminalWidth(totalW);
    layout.topBannerH = topBannerWidget.lineCount();

    layout.contentStart  = layout.topBannerRow + layout.topBannerH;
    layout.contentHeight = layout.todoStart - layout.contentStart;

    layout.contentHeight = qMax(1, layout.contentHeight);
}

void QTuiCompositor::renderTitle()
{
    int width = screen.width();

    /* Fill title bar with inverted style */
    for (int col = 0; col < width; col++) {
        screen.putChar(col, 0, ' ', true, false, true);
    }

    /* Title text */
    QString titleText = " " + title;
    screen.putString(0, 0, QTuiText::truncate(titleText, width), true, false, true);
}

void QTuiCompositor::renderTopBanner()
{
    if (layout.topBannerH <= 0) {
        return;
    }
    topBannerWidget.render(screen, layout.topBannerRow, screen.width());
}

void QTuiCompositor::renderContent()
{
    scrollView.render(screen, layout.contentStart, layout.contentHeight, screen.width());
}

void QTuiCompositor::renderTodo()
{
    if (todoWidget.lineCount() == 0) {
        return;
    }
    todoWidget.render(screen, layout.todoStart, screen.width());
}

void QTuiCompositor::renderQueued()
{
    if (queueWidget.lineCount() == 0) {
        return;
    }
    queueWidget.render(screen, layout.queueStart, screen.width());
}

void QTuiCompositor::renderCompletionPopup()
{
    if (popupWidget.lineCount() == 0) {
        return;
    }
    popupWidget.render(screen, layout.popupStart, screen.width());
}

void QTuiCompositor::renderTaskOverlay()
{
    if (taskOverlayWidget.lineCount() == 0) {
        return;
    }
    /* Task overlay shares the content area with scrollView. Pass the
     * full available height as the upper bound so lineCount() can grow
     * up to it; otherwise a ratcheting setMaxHeight() (bumped up to
     * kMinHeight on a previous frame) would lock the overlay at 6 rows
     * even when there are more tasks than that. */
    const int contentH = layout.contentHeight > 0 ? layout.contentHeight : 0;
    if (contentH > 0) {
        taskOverlayWidget.setMaxHeight(contentH);
    }
    taskOverlayWidget.render(screen, layout.contentStart, screen.width());
}

void QTuiCompositor::renderStatusBar()
{
    statusBarWidget.render(screen, layout.statusRow, screen.width());
}

void QTuiCompositor::renderSeparator()
{
    screen.hline(layout.separatorRow, '-');
}

void QTuiCompositor::renderInput()
{
    inputWidget.render(screen, layout.inputRow, screen.width());
}

/* ── Mouse selection ─────────────────────────────────────────────────── */

void QTuiCompositor::selectionStart(int col, int row)
{
    selection.anchorCol = col;
    selection.anchorRow = row;
    selection.focusCol  = col;
    selection.focusRow  = row;
    selection.active    = true;
    /* Immediate feedback — invert the anchor cell. */
    invalidate();
}

void QTuiCompositor::selectionUpdate(int col, int row)
{
    if (!selection.active) {
        return;
    }
    selection.focusCol = col;
    selection.focusRow = row;
    invalidate();
}

void QTuiCompositor::selectionFinish(int col, int row)
{
    if (!selection.active) {
        return;
    }
    selection.focusCol = col;
    selection.focusRow = row;
    /* No drag happened (release at the same cell as press): treat as
     * a block-focus tap. Otherwise carry on with the text-range copy. */
    const bool noDrag
        = (selection.anchorCol == selection.focusCol && selection.anchorRow == selection.focusRow);
    if (noDrag) {
        focusBlockAtScreenRow(row);
    } else {
        copySelectionToClipboard();
    }
    selection.active = false;
    invalidate();
}

void QTuiCompositor::applySelectionHighlight()
{
    /* Normalise so start <= end in reading order (top-left to bottom-right). */
    int startRow = selection.anchorRow;
    int startCol = selection.anchorCol;
    int endRow   = selection.focusRow;
    int endCol   = selection.focusCol;
    if (startRow > endRow || (startRow == endRow && startCol > endCol)) {
        std::swap(startRow, endRow);
        std::swap(startCol, endCol);
    }

    for (int row = startRow; row <= endRow && row < screen.height(); row++) {
        int colBegin = (row == startRow) ? startCol : 0;
        int colEnd   = (row == endRow) ? endCol : screen.width() - 1;
        for (int col = colBegin; col <= colEnd && col < screen.width(); col++) {
            QTuiCell &cell = screen.at(col, row);
            /* Decorative cells (gutters, banners, frame borders, the
             * scrollbar track / thumb) are excluded from the visible
             * selection so the user sees exactly which payload cells
             * will end up on the clipboard. */
            if (cell.decorative) {
                continue;
            }
            cell.inverted = !cell.inverted;
        }
    }
}

void QTuiCompositor::copySelectionToClipboard()
{
    /* Normalise. */
    int startRow = selection.anchorRow;
    int startCol = selection.anchorCol;
    int endRow   = selection.focusRow;
    int endCol   = selection.focusCol;
    if (startRow > endRow || (startRow == endRow && startCol > endCol)) {
        std::swap(startRow, endRow);
        std::swap(startCol, endCol);
    }

    /* Extract text from screen buffer, skipping decorative cells (so
     * the clipboard never receives gutter / banner / frame chars or
     * the scrollbar track) and trimming trailing spaces per row. */
    QString text;
    for (int row = startRow; row <= endRow && row < screen.height(); row++) {
        int     colBegin = (row == startRow) ? startCol : 0;
        int     colEnd   = (row == endRow) ? endCol : screen.width() - 1;
        QString line;
        for (int col = colBegin; col <= colEnd && col < screen.width(); col++) {
            const QTuiCell &cell = screen.at(col, row);
            if (cell.decorative) {
                continue;
            }
            line += cell.character;
        }
        /* Trim trailing spaces from each line. */
        while (line.endsWith(QLatin1Char(' '))) {
            line.chop(1);
        }
        if (row > startRow) {
            text += QLatin1Char('\n');
        }
        text += line;
    }

    if (text.isEmpty()) {
        return;
    }

    /* Write to clipboard via OSC 52 escape sequence. Supported by most
     * modern terminals (xterm, kitty, iTerm2, alacritty, Windows Terminal).
     * Format: ESC ] 52 ; c ; <base64> ESC \ */
    const QByteArray base64 = text.toUtf8().toBase64();
    fprintf(stdout, "\033]52;c;%s\033\\", base64.constData());
    fflush(stdout);
}
