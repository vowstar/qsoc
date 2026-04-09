// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUIQUEUEDLIST_H
#define QTUIQUEUEDLIST_H

#include "tui/qtuiwidget.h"

#include <QStringList>

/**
 * @brief Queued request display widget (dim text)
 */
class QTuiQueuedList : public QTuiWidget
{
public:
    int  lineCount() const override;
    void render(QTuiScreen &screen, int startY, int width) override;

    void addRequest(const QString &text);
    void removeRequest(const QString &text);
    void clearAll();

    static constexpr int MAX_VISIBLE = 3;

private:
    QStringList requests;
};

#endif // QTUIQUEUEDLIST_H
