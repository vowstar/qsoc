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

    /* Opt in to live fuzzy-search input. When enabled the digit shortcuts
     * go away so labels like "host05" or "foo-4-6" do not clash with
     * numeric selection. Printable keys feed a search buffer that
     * filters the visible list instead. */
    void setSearchable(bool enable);

    /* Blocking interactive loop. Returns selected index or -1 on cancel. */
    int exec();

private:
    QString         title;
    QList<MenuItem> items;
    int             highlighted  = 0;
    bool            colorEnabled = true;
    bool            searchable   = false;

    int computeBoxWidth() const;
};

#endif // QTUIMENU_H
