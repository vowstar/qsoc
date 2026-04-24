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

    if (!running) {
        /* Idle state: static, no spinner/timer */
        line = currentStatus;
        if (!effortLevel.isEmpty()) {
            line += " [E:" + effortLevel + "]";
        }
        if (!modelId.isEmpty()) {
            line += " [" + modelId + "]";
        }
        screen.putString(0, startY, QTuiText::truncate(line, width), false, true); /* dim */
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

    if (tokenStr.isEmpty()) {
        line = QString("%1 %2%3 (%4)").arg(spinner, currentStatus, dots, timeStr);
    } else {
        line = QString("%1 %2%3 (%4 %5)").arg(spinner, currentStatus, dots, tokenStr, timeStr);
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
    line += warning;

    screen.putString(0, startY, QTuiText::truncate(line, width));
}

void QTuiStatusBar::setStatus(const QString &status)
{
    currentStatus = status;
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
