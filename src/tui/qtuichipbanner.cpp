// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuichipbanner.h"

#include <QDateTime>
#include <QRandomGenerator>
#include <QString>

#include <algorithm>

namespace {

/* Single 6x4 quadrant-block frame; the chip silhouette has 3 detached
 * pins on every side and the eye holes baked into the body. */
constexpr const char OPEN_ROW_0[]   = "▖▟▟▟▖▖";
constexpr const char OPEN_ROW_1[]   = "▖█▜▜▌▖";
constexpr const char OPEN_ROW_2[]   = "▖███▌▖";
constexpr const char OPEN_ROW_3[]   = " ▜▜▜▘ ";
constexpr const char CLOSED_ROW_1[] = "▖███▌▖";

constexpr int BANNER_WIDTH = 6;
constexpr int BANNER_LINES = 4;
constexpr int GUTTER_COLS  = 2;

constexpr qint64 BLINK_HOLD_MS    = 140;  /* eyes-closed duration */
constexpr qint64 BLINK_MIN_GAP_MS = 3500; /* minimum eyes-open span */
constexpr qint64 BLINK_MAX_GAP_MS = 6000; /* maximum eyes-open span */

QStringList openFrame()
{
    return QStringList() << QString::fromUtf8(OPEN_ROW_0) << QString::fromUtf8(OPEN_ROW_1)
                         << QString::fromUtf8(OPEN_ROW_2) << QString::fromUtf8(OPEN_ROW_3);
}

QStringList closedFrame()
{
    /* Only the eye row differs from the open frame. */
    return QStringList() << QString::fromUtf8(OPEN_ROW_0) << QString::fromUtf8(CLOSED_ROW_1)
                         << QString::fromUtf8(OPEN_ROW_2) << QString::fromUtf8(OPEN_ROW_3);
}

qint64 nowMs()
{
    return QDateTime::currentMSecsSinceEpoch();
}

qint64 randomGap()
{
    return BLINK_MIN_GAP_MS
           + QRandomGenerator::global()->bounded(BLINK_MAX_GAP_MS - BLINK_MIN_GAP_MS);
}

} // namespace

QTuiChipBanner::QTuiChipBanner()
    : stateSinceMs(nowMs())
    , nextBlinkInMs(randomGap())
{}

void QTuiChipBanner::setIntro(const QStringList &intro)
{
    introLines = intro;
}

void QTuiChipBanner::setHidden(bool value)
{
    hidden = value;
}

void QTuiChipBanner::setTerminalWidth(int cols)
{
    cachedWidth = std::max(1, cols);
}

int QTuiChipBanner::introMaxWidth() const
{
    int width = 0;
    for (const QString &line : introLines) {
        width = std::max(width, static_cast<int>(line.size()));
    }
    return width;
}

bool QTuiChipBanner::sideBySideLayout() const
{
    return cachedWidth >= BANNER_WIDTH + GUTTER_COLS + introMaxWidth();
}

int QTuiChipBanner::lineCount() const
{
    if (hidden) {
        return 0;
    }
    if (sideBySideLayout()) {
        return std::max(BANNER_LINES, static_cast<int>(introLines.size()));
    }
    return BANNER_LINES + static_cast<int>(introLines.size());
}

void QTuiChipBanner::tick()
{
    if (hidden) {
        return;
    }
    const qint64 elapsed = nowMs() - stateSinceMs;
    if (eyesOpen) {
        if (elapsed >= nextBlinkInMs) {
            eyesOpen     = false;
            stateSinceMs = nowMs();
        }
    } else {
        if (elapsed >= BLINK_HOLD_MS) {
            eyesOpen      = true;
            stateSinceMs  = nowMs();
            nextBlinkInMs = randomGap();
        }
    }
}

void QTuiChipBanner::render(QTuiScreen &screen, int startY, int width)
{
    if (hidden) {
        return;
    }
    /* Width may have shifted between recalculateLayout and render
     * (e.g. SIGWINCH between layout pass and paint). Re-pick layout
     * with the live width so cells we draw never overshoot. */
    cachedWidth                  = std::max(1, width);
    const QStringList chip       = eyesOpen ? openFrame() : closedFrame();
    const bool        sideBySide = sideBySideLayout();

    if (sideBySide) {
        const int rows = std::max(BANNER_LINES, static_cast<int>(introLines.size()));
        for (int i = 0; i < rows; ++i) {
            const QString chipLine = (i < chip.size()) ? chip.at(i)
                                                       : QString(BANNER_WIDTH, QLatin1Char(' '));
            screen.putString(0, startY + i, chipLine);
            const QString introLine = (i < introLines.size()) ? introLines.at(i) : QString();
            if (!introLine.isEmpty()) {
                screen.putString(BANNER_WIDTH + GUTTER_COLS, startY + i, introLine);
            }
        }
        return;
    }
    /* Stacked: chip on top, intro below. */
    int row = startY;
    for (const QString &line : chip) {
        screen.putString(0, row++, line);
    }
    for (const QString &line : introLines) {
        screen.putString(0, row++, line);
    }
}
