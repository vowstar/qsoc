// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuistatusbar.h"

const QStringList QTuiStatusBar::spinnerFrames = {"-", "\\", "|", "/"};
const QStringList QTuiStatusBar::dotFrames     = {"   ", ".  ", ".. ", "..."};

int QTuiStatusBar::lineCount() const
{
    return 1;
}

void QTuiStatusBar::render(QTuiScreen &screen, int startY, int width)
{
    QString line;

    /* Right-aligned task pill (▶ N tasks). Drawn first so the main line
     * truncation can leave room. Empty when count == 0. */
    QString taskPill;
    if (taskCount_ > 0) {
        /* Plain ASCII to avoid wide-character ambiguity at the right
         * margin (the BLACK RIGHT-POINTING TRIANGLE is Narrow per
         * Unicode but East Asian Ambiguous in some terminals, which
         * clips the trailing 's'). */
        taskPill = QStringLiteral("> %1 task%2")
                       .arg(taskCount_)
                       .arg(taskCount_ == 1 ? QString() : QStringLiteral("s"));
    }
    const int pillWidth = QTuiText::visualWidth(taskPill);
    /* Leave a 2-column right margin: terminal emulators commonly treat
     * the very last column as an auto-scroll sentinel and the
     * second-to-last as a pre-wrap reservation. Without the margin the
     * trailing glyph gets eaten on both VT and tmux passthrough. */
    const int pillStart = pillWidth > 0 ? qMax(0, width - pillWidth - 2) : width;
    const int mainWidth = pillWidth > 0 ? qMax(0, pillStart - 1) : width;

    if (!running) {
        /* Idle state: static, no spinner/timer */
        line = currentStatus;
        if (!effortLevel.isEmpty()) {
            line += " [E:" + effortLevel + "]";
        }
        if (!modelId.isEmpty()) {
            line += " [" + modelId + "]";
        }
        screen.putString(0, startY, QTuiText::truncate(line, mainWidth), false, true); /* dim */
        if (!taskPill.isEmpty()) {
            /* Focus state takes precedence: pill is bold + inverted so the
             * user sees the focus jump unambiguously. Alert (stuck task)
             * also inverts; focus implies alert-style invert. */
            const bool focused = taskPillFocused_;
            screen.putString(
                pillStart,
                startY,
                taskPill,
                /*bold*/ focused,
                /*dim*/ !taskAlert_ && !focused,
                /*inverted*/ taskAlert_ || focused);
        }
        return;
    }

    /* Running state: spinner + status + dots + (tokens time) + tools */
    QString spinner = spinnerFrames[spinnerIndex % spinnerFrames.size()];
    QString dots    = dotFrames[dotIndex % dotFrames.size()];

    qint64  totalSeconds = totalTimer.isValid() ? totalTimer.elapsed() / 1000 : 0;
    qint64  stepSeconds  = stepTimer.isValid() ? stepTimer.elapsed() / 1000 : 0;
    QString timeStr      = QTuiText::formatDuration(totalSeconds);

    QString tokenStr;
    if (inputTokens > 0 || outputTokens > 0) {
        tokenStr
            = QString("%1/%2")
                  .arg(QTuiText::formatNumber(inputTokens), QTuiText::formatNumber(outputTokens));
    }

    QString toolInfo;
    if (toolCallCount > 0) {
        toolInfo = QString("[%1 tools]").arg(toolCallCount);
    }

    QString warning;
    if (stepSeconds >= 120) {
        warning = " [Slow!]";
    } else if (stepSeconds >= 60) {
        warning = " [Slow]";
    }

    /* Surface a short detail (e.g. the file path for read_file or the
     * shell command for bash) so a 60s tool call does not look like a
     * frozen "Working" line. Cap detail length so the right-side meta
     * (tokens, time, tools, model) still has room to render. */
    QString status = currentStatus;
    if (!lastToolDetail.isEmpty()) {
        const int detailCap = 40;
        QString   trimmed   = lastToolDetail.simplified();
        if (trimmed.size() > detailCap) {
            trimmed = trimmed.left(detailCap - 1) + QStringLiteral("…");
        }
        status += QStringLiteral(" ") + trimmed;
    }
    if (tokenStr.isEmpty()) {
        line = QString("%1 %2%3 (%4)").arg(spinner, status, dots, timeStr);
    } else {
        line = QString("%1 %2%3 (%4 %5)").arg(spinner, status, dots, tokenStr, timeStr);
    }
    if (!toolInfo.isEmpty()) {
        line += " " + toolInfo;
    }
    if (!effortLevel.isEmpty()) {
        line += QString(" [E:%1]").arg(effortLevel);
    }
    if (!modelId.isEmpty()) {
        line += QString(" [%1]").arg(modelId);
    }
    if (!goalText_.isEmpty()) {
        QString tag = goalStatusTag_.isEmpty() ? QString()
                                               : QStringLiteral("|%1").arg(goalStatusTag_);
        line += QString(" [Goal: %1%2]").arg(goalText_, tag);
    }
    line += warning;

    screen.putString(0, startY, QTuiText::truncate(line, mainWidth));
    if (!taskPill.isEmpty()) {
        screen.putString(
            width - pillWidth,
            startY,
            taskPill,
            /*bold*/ false,
            /*dim*/ !taskAlert_,
            /*inverted*/ taskAlert_);
    }
}

void QTuiStatusBar::setStatus(const QString &status)
{
    currentStatus = status;
    /* Drop the per-tool detail when the caller swaps to a generic
     * status (e.g. "Reasoning", "Compacting", "Ready"). Keeping the
     * stale tool argument visible after the tool has finished is
     * confusing — the user just saw the command they ran linger
     * across the next phase of the loop. */
    lastToolDetail.clear();
}

void QTuiStatusBar::setTaskCount(int count)
{
    taskCount_ = count;
}

void QTuiStatusBar::setTaskAlert(bool alert)
{
    taskAlert_ = alert;
}

void QTuiStatusBar::setTaskPillFocused(bool focused)
{
    taskPillFocused_ = focused;
}

void QTuiStatusBar::setGoalIndicator(const QString &text, const QString &statusTag)
{
    goalText_      = text;
    goalStatusTag_ = statusTag;
}

void QTuiStatusBar::toolCalled(const QString &toolName, const QString &detail)
{
    toolCallCount++;
    lastToolDetail = detail;
    currentStatus  = toolName;
    stepTimer.restart();
}

void QTuiStatusBar::updateTokens(qint64 input, qint64 output)
{
    inputTokens  = input;
    outputTokens = output;
}

void QTuiStatusBar::setEffortLevel(const QString &level)
{
    effortLevel = level;
}

void QTuiStatusBar::setModel(const QString &model)
{
    modelId = model;
}

void QTuiStatusBar::resetProgress()
{
    stepTimer.restart();
}

void QTuiStatusBar::startTimers()
{
    running       = true;
    toolCallCount = 0;
    inputTokens   = 0;
    outputTokens  = 0;
    stepTimer.start();
    totalTimer.start();
}

void QTuiStatusBar::stopTimers()
{
    running = false;
    stepTimer.invalidate();
    totalTimer.invalidate();
}

void QTuiStatusBar::tick()
{
    if (!running) {
        return; /* Don't animate when idle */
    }
    spinnerIndex = (spinnerIndex + 1) % spinnerFrames.size();
    dotIndex     = (dotIndex + 1) % dotFrames.size();
}
