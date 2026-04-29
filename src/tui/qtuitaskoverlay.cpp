// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuitaskoverlay.h"

#include "agent/qsoctasksource.h"

#include <QChar>
#include <QDateTime>
#include <QString>

namespace {

constexpr int kMinHeight       = 6;
constexpr int kDetailTailBytes = 8 * 1024;
constexpr int kFooterFlashMs   = 600;
constexpr int kIdColWidth      = 8;
constexpr int kSourceTagWidth  = 6;
constexpr int kStatusWidth     = 8;
constexpr int kSummaryColWidth = 18;
constexpr int kPaddingChars    = 6; /* spaces between columns + leading marker */

QString statusLabel(QSocTask::Status status)
{
    switch (status) {
    case QSocTask::Status::Running:
        return QStringLiteral("running");
    case QSocTask::Status::Pending:
        return QStringLiteral("pending");
    case QSocTask::Status::Idle:
        return QStringLiteral("idle");
    case QSocTask::Status::Stuck:
        return QStringLiteral("stuck");
    case QSocTask::Status::Completed:
        return QStringLiteral("done");
    case QSocTask::Status::Failed:
        return QStringLiteral("failed");
    }
    return QStringLiteral("?");
}

QString fitToWidth(const QString &text, int width)
{
    if (width <= 0)
        return QString();
    if (text.size() == width)
        return text;
    if (text.size() < width)
        return text + QString(width - text.size(), QLatin1Char(' '));
    return text.left(width - 1) + QStringLiteral("…");
}

} /* namespace */

QTuiTaskOverlay::QTuiTaskOverlay(QObject *parent)
    : QObject(parent)
{}

void QTuiTaskOverlay::setRegistry(QSocTaskRegistry *registry)
{
    if (registry_ == registry)
        return;
    if (registry_ != nullptr)
        disconnect(registry_, nullptr, this, nullptr);
    registry_ = registry;
    if (registry_ != nullptr) {
        connect(
            registry_,
            &QSocTaskRegistry::anySourceChanged,
            this,
            &QTuiTaskOverlay::handleRegistryChanged);
    }
}

void QTuiTaskOverlay::setMaxHeight(int rows)
{
    maxHeight_ = rows < kMinHeight ? kMinHeight : rows;
}

void QTuiTaskOverlay::open()
{
    if (registry_ == nullptr)
        return;
    refreshRows();
    selected_ = 0;
    detailSourceTag_.clear();
    detailId_.clear();
    detailContent_.clear();
    footerFlash_.clear();
    footerFlashUntil_ = 0;
    mode_             = Mode::List;
    emit invalidated();
}

void QTuiTaskOverlay::close()
{
    if (mode_ == Mode::Hidden)
        return;
    mode_ = Mode::Hidden;
    cachedRows_.clear();
    detailSourceTag_.clear();
    detailId_.clear();
    detailContent_.clear();
    selected_ = 0;
    emit invalidated();
    emit closed();
}

void QTuiTaskOverlay::handleRegistryChanged()
{
    if (mode_ == Mode::Hidden)
        return;
    refreshRows();
    if (mode_ == Mode::Detail) {
        /* Detail target may have disappeared. */
        bool stillThere = false;
        for (const auto &row : cachedRows_) {
            if (row.sourceTag == detailSourceTag_ && row.row.id == detailId_) {
                stillThere = true;
                break;
            }
        }
        if (!stillThere) {
            detailContent_ = QStringLiteral("Task no longer exists. Press ESC.\n");
        } else {
            reloadDetailContent();
        }
    }
    clampSelection();
    emit invalidated();
}

void QTuiTaskOverlay::refreshRows()
{
    if (registry_ == nullptr) {
        cachedRows_.clear();
        return;
    }
    cachedRows_ = registry_->listAll();
}

void QTuiTaskOverlay::clampSelection()
{
    if (cachedRows_.isEmpty()) {
        selected_ = 0;
        return;
    }
    if (selected_ < 0)
        selected_ = 0;
    if (selected_ >= cachedRows_.size())
        selected_ = cachedRows_.size() - 1;
}

bool QTuiTaskOverlay::handleKey(int key, bool ctrl)
{
    Q_UNUSED(ctrl);
    if (mode_ == Mode::Hidden)
        return false;

    /* ESC always backs out: List -> close, Detail -> List. */
    if (key == Qt::Key_Escape) {
        if (mode_ == Mode::Detail) {
            exitDetailToList();
        } else {
            close();
        }
        return true;
    }

    if (mode_ == Mode::List) {
        switch (key) {
        case Qt::Key_Up:
        case Qt::Key_K:
            if (selected_ > 0)
                --selected_;
            emit invalidated();
            return true;
        case Qt::Key_Down:
        case Qt::Key_J:
            if (selected_ < cachedRows_.size() - 1)
                ++selected_;
            emit invalidated();
            return true;
        case Qt::Key_Return:
        case Qt::Key_Enter:
            enterDetail();
            return true;
        case Qt::Key_X:
            killSelected();
            return true;
        default:
            return true; /* swallow other keys while overlay is up */
        }
    }

    if (mode_ == Mode::Detail) {
        if (key == Qt::Key_X) {
            killSelected();
            return true;
        }
        return true; /* swallow */
    }
    return false;
}

void QTuiTaskOverlay::enterDetail()
{
    if (cachedRows_.isEmpty())
        return;
    const auto &row  = cachedRows_.at(selected_);
    detailSourceTag_ = row.sourceTag;
    detailId_        = row.row.id;
    reloadDetailContent();
    mode_ = Mode::Detail;
    emit invalidated();
}

void QTuiTaskOverlay::exitDetailToList()
{
    detailSourceTag_.clear();
    detailId_.clear();
    detailContent_.clear();
    mode_ = Mode::List;
    refreshRows();
    clampSelection();
    emit invalidated();
}

void QTuiTaskOverlay::reloadDetailContent()
{
    if (registry_ == nullptr) {
        detailContent_.clear();
        return;
    }
    const QString tail = registry_->tailFor(detailSourceTag_, detailId_, kDetailTailBytes);
    detailContent_     = tail.isEmpty() ? QStringLiteral("(no output yet)\n") : tail;
}

void QTuiTaskOverlay::killSelected()
{
    if (registry_ == nullptr || cachedRows_.isEmpty())
        return;
    QString tag;
    QString id;
    if (mode_ == Mode::Detail) {
        tag = detailSourceTag_;
        id  = detailId_;
    } else {
        const auto &row = cachedRows_.at(selected_);
        tag             = row.sourceTag;
        id              = row.row.id;
    }
    if (id.isEmpty())
        return;
    const bool ok = registry_->killTask(tag, id);
    flashFooter(
        ok ? QStringLiteral("Killed %1").arg(id) : QStringLiteral("Kill failed: %1").arg(id));
    refreshRows();
    if (mode_ == Mode::Detail) {
        /* If the detail target is gone, drop back to list. */
        bool stillThere = false;
        for (const auto &row : cachedRows_) {
            if (row.sourceTag == tag && row.row.id == id) {
                stillThere = true;
                break;
            }
        }
        if (!stillThere)
            exitDetailToList();
    }
    if (cachedRows_.isEmpty()) {
        close();
        return;
    }
    clampSelection();
    emit invalidated();
}

void QTuiTaskOverlay::flashFooter(const QString &message)
{
    footerFlash_      = message;
    footerFlashUntil_ = QDateTime::currentMSecsSinceEpoch() + kFooterFlashMs;
}

int QTuiTaskOverlay::lineCount() const
{
    if (mode_ == Mode::Hidden)
        return 0;
    /* Both modes have the same chrome: top border + footer + bottom border
     * (3 rows). The body is rows in List mode and detail-content lines in
     * Detail mode. Cap to maxHeight_ so a multi-KB bash tail does not
     * displace the scroll view; the renderer shows the tail when content
     * exceeds the box. */
    int body = 1;
    if (mode_ == Mode::List) {
        body = cachedRows_.isEmpty() ? 1 : static_cast<int>(cachedRows_.size());
    } else {
        const int contentLines = static_cast<int>(detailContent_.count(QLatin1Char('\n'))) + 1;
        body                   = contentLines < 1 ? 1 : contentLines;
    }
    const int total = body + 3; /* header is part of border title; footer + 2 borders */
    return total > maxHeight_ ? maxHeight_ : total;
}

void QTuiTaskOverlay::render(QTuiScreen &screen, int startY, int width)
{
    if (mode_ == Mode::Hidden)
        return;
    if (mode_ == Mode::Detail) {
        renderDetail(screen, startY, width);
    } else {
        renderList(screen, startY, width);
    }
}

void QTuiTaskOverlay::tick()
{
    if (mode_ == Mode::Hidden)
        return;
    ++tickCounter_;
    /* Detail mode: re-read tail once per ~10 ticks (compositor ticks at
     * 100ms, so this samples roughly every second). List mode: still
     * invalidate every 10 ticks so loop ETAs visibly tick down without
     * waiting on registry change events. */
    if (tickCounter_ % 10 == 0) {
        if (mode_ == Mode::Detail) {
            reloadDetailContent();
        }
        emit invalidated();
    }
    /* Footer flash expiry triggers a redraw immediately, regardless of
     * the 10-tick cadence. */
    if (footerFlashUntil_ > 0 && QDateTime::currentMSecsSinceEpoch() > footerFlashUntil_) {
        footerFlash_.clear();
        footerFlashUntil_ = 0;
        emit invalidated();
    }
}

void QTuiTaskOverlay::renderBorder(
    QTuiScreen &screen, int startY, int height, int width, const QString &title)
{
    if (height < 2 || width < 4)
        return;
    /* Clear the box interior so widgets rendered earlier in the same
     * frame (scroll content, queued list) do not bleed through. */
    for (int row = startY + 1; row < startY + height - 1; ++row) {
        for (int col = 1; col < width - 1; ++col) {
            screen.putChar(col, row, QChar::fromLatin1(' '));
        }
    }
    /* Top border */
    screen.putChar(0, startY, QChar::fromLatin1('+'));
    for (int col = 1; col < width - 1; ++col) {
        screen.putChar(col, startY, QChar::fromLatin1('-'));
    }
    screen.putChar(width - 1, startY, QChar::fromLatin1('+'));
    /* Title overlay near left */
    const QString shaped = QStringLiteral(" %1 ").arg(title);
    if (shaped.size() < width - 4) {
        screen.putString(2, startY, shaped, /*bold*/ true);
    }
    /* Bottom border */
    const int bottomRow = startY + height - 1;
    screen.putChar(0, bottomRow, QChar::fromLatin1('+'));
    for (int col = 1; col < width - 1; ++col) {
        screen.putChar(col, bottomRow, QChar::fromLatin1('-'));
    }
    screen.putChar(width - 1, bottomRow, QChar::fromLatin1('+'));
    /* Side borders for interior rows */
    for (int row = startY + 1; row < bottomRow; ++row) {
        screen.putChar(0, row, QChar::fromLatin1('|'));
        screen.putChar(width - 1, row, QChar::fromLatin1('|'));
    }
}

void QTuiTaskOverlay::renderList(QTuiScreen &screen, int startY, int width)
{
    const int     height   = lineCount();
    const int     contentH = height - 2;
    const int     innerW   = width - 2;
    const QString title    = QStringLiteral("Tasks (%1)").arg(cachedRows_.size());
    renderBorder(screen, startY, height, width, title);

    if (cachedRows_.isEmpty()) {
        const QString msg = QStringLiteral("No active tasks.");
        screen.putString(2, startY + 1, msg.left(innerW - 2), false, true);
        const QString footer = QStringLiteral("ESC close");
        screen.putString(2, startY + height - 1 - 0, QString(), false);
        screen.putString(2, startY + contentH, fitToWidth(footer, innerW - 2), false, true);
        return;
    }

    const int rowsAvail = contentH - 1; /* last interior row is footer */
    const int firstIdx  = qMax(0, selected_ - rowsAvail + 1);
    for (int i = 0; i < rowsAvail && firstIdx + i < cachedRows_.size(); ++i) {
        const int   rowIdx   = firstIdx + i;
        const auto &tagged   = cachedRows_.at(rowIdx);
        const auto &row      = tagged.row;
        const int   y        = startY + 1 + i;
        const bool  selected = (rowIdx == selected_);

        const QString marker = selected ? QStringLiteral("> ") : QStringLiteral("  ");
        const QString tag
            = fitToWidth(QStringLiteral("[%1]").arg(tagged.sourceTag), kSourceTagWidth);
        const QString id      = fitToWidth(row.id.left(kIdColWidth), kIdColWidth);
        const QString status  = fitToWidth(statusLabel(row.status), kStatusWidth);
        const QString summary = fitToWidth(row.summary, kSummaryColWidth);
        const int     used    = marker.size() + tag.size() + 1 + id.size() + 1 + status.size() + 1
                                + summary.size() + 1;
        const int     labelW  = qMax(0, innerW - used);
        const QString label   = fitToWidth(row.label, labelW);
        const QString line    = marker + tag + QChar::fromLatin1(' ') + id + QChar::fromLatin1(' ')
                                + status + QChar::fromLatin1(' ') + summary + QChar::fromLatin1(' ')
                                + label;
        screen.putString(
            1, y, line.left(innerW), /*bold*/ selected, /*dim*/ !selected, /*inverted*/ selected);
    }

    /* Footer */
    const QString footer = footerFlash_.isEmpty()
                                   && QDateTime::currentMSecsSinceEpoch() > footerFlashUntil_
                               ? QStringLiteral("↑↓ select  ↵ details  x kill  ESC close")
                               : footerFlash_;
    screen.putString(2, startY + height - 2, fitToWidth(footer, innerW - 2), false, true);
}

void QTuiTaskOverlay::renderDetail(QTuiScreen &screen, int startY, int width)
{
    const int     height   = lineCount();
    const int     contentH = height - 2;
    const int     innerW   = width - 2;
    const QString title    = QStringLiteral("%1 / %2").arg(detailSourceTag_, detailId_);
    renderBorder(screen, startY, height, width, title);

    if (detailContent_.isEmpty()) {
        screen.putString(2, startY + 1, QStringLiteral("(no content)"), false, true);
    } else {
        const QStringList lines     = detailContent_.split(QLatin1Char('\n'));
        const int         rowsAvail = contentH - 1;
        const int         total     = lines.size();
        const int         start     = qMax(0, total - rowsAvail);
        for (int i = 0; i < rowsAvail && start + i < total; ++i) {
            const QString &line = lines.at(start + i);
            screen.putString(1, startY + 1 + i, line.left(innerW), false);
        }
    }
    /* Footer */
    const QString footer = footerFlash_.isEmpty()
                                   && QDateTime::currentMSecsSinceEpoch() > footerFlashUntil_
                               ? QStringLiteral("ESC back  x kill")
                               : footerFlash_;
    screen.putString(2, startY + height - 2, fitToWidth(footer, innerW - 2), false, true);
}
