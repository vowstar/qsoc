// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUISTATUSBAR_H
#define QTUISTATUSBAR_H

#include "tui/qtuiwidget.h"

#include <QElapsedTimer>
#include <QString>
#include <QStringList>

/**
 * @brief Status bar widget: spinner + status + tokens + time + tools
 */
class QTuiStatusBar : public QTuiWidget
{
public:
    int  lineCount() const override;
    void render(QTuiScreen &screen, int startY, int width) override;

    void setStatus(const QString &status);
    void toolCalled(const QString &toolName, const QString &detail = QString());
    void updateTokens(qint64 input, qint64 output);
    void setEffortLevel(const QString &level);
    void setModel(const QString &modelId);
    /**
     * @brief Number of active background tasks (loops + bash + future
     *        sub-agents) for the right-aligned `▶ N tasks` pill.
     *        Hidden when count == 0.
     */
    void setTaskCount(int count);
    /** @brief Whether to draw the pill in attention (blink) mode. */
    void setTaskAlert(bool alert);
    /** @brief Whether the pill currently has keyboard focus (Down-arrow parked). */
    void setTaskPillFocused(bool focused);
    void resetProgress();
    void startTimers();
    void stopTimers(); /* Stop animation + timer (idle state) */
    void tick();

    int     getToolCallCount() const { return toolCallCount; }
    QString getLastToolDetail() const { return lastToolDetail; }

private:
    QString       currentStatus;
    int           spinnerIndex  = 0;
    int           dotIndex      = 0;
    int           toolCallCount = 0;
    QString       lastToolDetail;
    qint64        inputTokens  = 0;
    qint64        outputTokens = 0;
    QString       effortLevel;
    QString       modelId;
    QElapsedTimer stepTimer;
    QElapsedTimer totalTimer;
    bool          running = false; /* true = agent executing, false = idle */

    int  taskCount_       = 0;
    bool taskAlert_       = false;
    bool taskPillFocused_ = false;

    static const QStringList spinnerFrames;
    static const QStringList dotFrames;
};

#endif // QTUISTATUSBAR_H
