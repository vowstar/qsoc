// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUICOMPLETIONPOPUP_H
#define QTUICOMPLETIONPOPUP_H

#include "tui/qtuiwidget.h"

#include <QString>
#include <QStringList>

/**
 * @brief Inline completion popup widget rendered just above the status bar.
 * @details Used for '@file' suggestion during live-as-you-type completion.
 *          Unlike QTuiMenu, this is a render-only widget controlled by the
 *          REPL — no blocking exec loop. The REPL updates items/highlight
 *          and the compositor picks it up on the next render tick.
 */
class QTuiCompletionPopup : public QTuiWidget
{
public:
    static constexpr int MAX_VISIBLE_ITEMS = 8;

    int  lineCount() const override;
    void render(QTuiScreen &screen, int startY, int width) override;

    void setVisible(bool visible);
    bool isVisible() const { return visible; }

    void               setItems(const QStringList &items);
    const QStringList &getItems() const { return items; }

    void setHighlight(int index);
    int  getHighlight() const { return highlight; }

    /* Wrapping navigation: delta=+1 next, delta=-1 prev */
    void moveHighlight(int delta);

    void setTitle(const QString &title) { this->title = title; }

    void setColorEnabled(bool enabled) { colorEnabled = enabled; }

private:
    bool        visible      = false;
    bool        colorEnabled = true;
    QStringList items;
    int         highlight = 0;
    int         viewStart = 0;
    QString     title     = QStringLiteral("@file");

    /* Keep highlighted item within the visible viewport */
    void adjustViewport();
};

#endif // QTUICOMPLETIONPOPUP_H
