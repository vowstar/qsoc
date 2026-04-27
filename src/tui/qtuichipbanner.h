// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUICHIPBANNER_H
#define QTUICHIPBANNER_H

#include "tui/qtuiwidget.h"

#include <QStringList>

/**
 * @brief Top-of-screen banner with a chip mascot and intro text.
 * @details Pinned above the scroll content while the agent is idle.
 *          Layout is responsive: side-by-side when the terminal is
 *          wide enough, otherwise stacked. Eyes blink occasionally
 *          while the banner is visible. The compositor hides the
 *          banner (lineCount returns 0) on the first scroll-content
 *          line so the banner area is reclaimed once work begins.
 */
class QTuiChipBanner : public QTuiWidget
{
public:
    QTuiChipBanner();

    int  lineCount() const override;
    void render(QTuiScreen &screen, int startY, int width) override;

    void setIntro(const QStringList &intro);
    void setHidden(bool value);
    bool isHidden() const { return hidden; }

    /**
     * @brief Cache the terminal width used by lineCount() and render().
     *        Called by the compositor before each layout recompute.
     */
    void setTerminalWidth(int cols);

    /**
     * @brief Advance the blink state machine. Called once per
     *        compositor tick (~100 ms cadence).
     */
    void tick();

private:
    QStringList introLines;
    bool        hidden        = false;
    bool        eyesOpen      = true;
    qint64      stateSinceMs  = 0;
    qint64      nextBlinkInMs = 0;
    int         cachedWidth   = 80;

    bool sideBySideLayout() const;
    int  introMaxWidth() const;
};

#endif // QTUICHIPBANNER_H
