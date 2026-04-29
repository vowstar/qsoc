// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QTUITASKOVERLAY_H
#define QTUITASKOVERLAY_H

#include "agent/qsoctaskregistry.h"
#include "tui/qtuiwidget.h"

#include <QObject>

/**
 * @brief Modal overlay listing every active background task.
 * @details Reads rows from a QSocTaskRegistry; renders a half-screen
 *          bordered list above the scroll region. Two modes share the
 *          same widget surface:
 *
 *          - List: arrow keys / j-k navigate; ↵ enters detail; x kills
 *            the highlighted task; ESC closes the overlay.
 *          - Detail: shows source-specific tail content for the
 *            selected task; ESC returns to List; x still kills.
 *
 *          Selection is tracked as `(sourceTag, id)` so id collisions
 *          across sources cannot cause the overlay to act on the wrong
 *          row when the underlying list reshuffles between renders.
 */
class QTuiTaskOverlay : public QObject, public QTuiWidget
{
    Q_OBJECT

public:
    enum class Mode {
        Hidden, /* not rendered, lineCount() == 0 */
        List,   /* list of tasks */
        Detail, /* tail / metadata of selected task */
    };

    explicit QTuiTaskOverlay(QObject *parent = nullptr);
    ~QTuiTaskOverlay() override = default;

    void setRegistry(QSocTaskRegistry *registry);
    void setMaxHeight(int rows); /* default 14, capped to terminal/2 by compositor */

    /* Open the overlay in List mode at top selection. */
    void open();
    /* Hide unconditionally; selection state cleared. */
    void close();

    Mode mode() const { return mode_; }

    /**
     * @brief Consume a key while overlay is in List or Detail mode.
     * @return true when the key was handled and should not propagate.
     */
    bool handleKey(int key, bool ctrl);

    /* QTuiWidget */
    int  lineCount() const override;
    void render(QTuiScreen &screen, int startY, int width) override;

    /**
     * @brief Compositor tick. Throttles tail re-reads to ~1 Hz when in
     *        Detail mode; in List mode bumps render-time-only counters
     *        (eta) and clears expired footer flashes.
     */
    void tick();

signals:
    /** @brief Emitted whenever the overlay needs the compositor to redraw. */
    void invalidated();

    /** @brief Emitted when the overlay closes back to Input focus. */
    void closed();

private slots:
    void handleRegistryChanged();

private:
    QSocTaskRegistry                  *registry_  = nullptr;
    Mode                               mode_      = Mode::Hidden;
    int                                selected_  = 0;
    int                                maxHeight_ = 14;
    QList<QSocTaskRegistry::TaggedRow> cachedRows_;
    QString                            detailSourceTag_;
    QString                            detailId_;
    QString                            detailContent_;
    QString                            footerFlash_;
    qint64                             footerFlashUntil_ = 0;
    int                                tickCounter_      = 0;

    void refreshRows();
    void clampSelection();
    void enterDetail();
    void exitDetailToList();
    void killSelected();
    void reloadDetailContent();
    void flashFooter(const QString &message);

    void renderList(QTuiScreen &screen, int startY, int width);
    void renderDetail(QTuiScreen &screen, int startY, int width);
    void renderBorder(QTuiScreen &screen, int startY, int height, int width, const QString &title);
};

#endif /* QTUITASKOVERLAY_H */
