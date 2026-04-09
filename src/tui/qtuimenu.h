// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUIMENU_H
#define QTUIMENU_H

#include "tui/qtuiwidget.h"

#include <QList>
#include <QString>

/**
 * @brief Interactive ASCII menu selector (overlay mode)
 * @details Renders a numbered list inside an ASCII box.
 *          exec() blocks and returns selected index (-1 = cancelled).
 */
class QTuiMenu : public QTuiWidget
{
public:
    struct MenuItem
    {
        QString label;
        QString hint;
        bool    marked;
    };

    int  lineCount() const override;
    void render(QTuiScreen &screen, int startY, int width) override;

    void setTitle(const QString &title);
    void setItems(const QList<MenuItem> &items);
    void setHighlight(int index);
    void setColorEnabled(bool enabled);

    /* Blocking interactive loop. Returns selected index or -1 on cancel. */
    int exec();

private:
    QString         title;
    QList<MenuItem> items;
    int             highlighted  = 0;
    bool            colorEnabled = true;

    int computeBoxWidth() const;
};

#endif // QTUIMENU_H
