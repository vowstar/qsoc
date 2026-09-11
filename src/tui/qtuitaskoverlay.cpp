// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuitaskoverlay.h"

#include "agent/qsoctasksource.h"
#include "tui/qtuiansi.h"

#include <algorithm>

#include <QChar>
#include <QDateTime>
#include <QMap>
#include <QString>

namespace {

constexpr int kMinHeight       = 6;
constexpr int kDetailTailBytes = 8 * 1024;
constexpr int kFooterFlashMs   = 600;

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
    case QSocTask::Status::Aborted:
        return QStringLiteral("aborted");
    }
    return QStringLiteral("?");
}

QString fitToWidth(const QString &text, int width)
{
    if (width <= 0)
        return {};
    QString plain;
    for (const auto &span : QTuiAnsi::parse(text)) {
        for (const QChar ch : span.text) {
            if (!ch.isPrint())
                plain += QLatin1Char(' ');
            else
                plain += ch;
        }
    }
    if (QTuiText::visualWidth(plain) > width)
        plain = width <= 3 ? QString(width, QLatin1Char('.')) : QTuiText::truncate(plain, width);
    return plain + QString(qMax(0, width - QTuiText::visualWidth(plain)), QLatin1Char(' '));
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
    cachedRows_.clear();
    refreshRows();
}

void QTuiTaskOverlay::setMaxHeight(int rows)
{
    maxHeight_ = qMax(0, rows);
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
    detailSourceTag_.clear();
    detailId_.clear();
    detailContent_.clear();
    selected_ = 0;
    emit invalidated();
    emit closed();
}

void QTuiTaskOverlay::handleRegistryChanged()
{
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
    QString tag;
    QString id;
    if (selected_ >= 0 && selected_ < cachedRows_.size()) {
        tag = cachedRows_.at(selected_).sourceTag;
        id  = cachedRows_.at(selected_).row.id;
    }
    const auto                                                 fresh = registry_->listAll();
    QMap<QPair<QString, QString>, QSocTaskRegistry::TaggedRow> remaining;
    for (const auto &row : fresh)
        remaining.insert(qMakePair(row.sourceTag, row.row.id), row);
    QList<QSocTaskRegistry::TaggedRow> ordered;
    ordered.reserve(fresh.size());
    for (const auto &previous : std::as_const(cachedRows_)) {
        const auto key   = qMakePair(previous.sourceTag, previous.row.id);
        const auto found = remaining.find(key);
        if (found != remaining.end()) {
            ordered.append(found.value());
            remaining.erase(found);
        }
    }
    for (const auto &row : fresh) {
        const auto key = qMakePair(row.sourceTag, row.row.id);
        if (remaining.remove(key) != 0)
            ordered.append(row);
    }
    cachedRows_ = ordered;
    selected_   = -1;
    for (int i = 0; i < cachedRows_.size(); ++i) {
        if (cachedRows_.at(i).sourceTag == tag && cachedRows_.at(i).row.id == id) {
            selected_ = i;
            break;
        }
    }
}

void QTuiTaskOverlay::clampSelection()
{
    if (cachedRows_.isEmpty()) {
        selected_ = 0;
        return;
    }
    if (selected_ >= cachedRows_.size())
        selected_ = cachedRows_.size() - 1;
}

bool QTuiTaskOverlay::handleKey(int key, bool ctrl)
{
    Q_UNUSED(ctrl);
    if (mode_ == Mode::Hidden)
        return false;

    if (key == Qt::Key_M) {
        setAnimationEnabled(!animationEnabled_);
        return true;
    }
    if (key == Qt::Key_Escape || key == Qt::Key_Q) {
        if (mode_ == Mode::Detail) {
            exitDetailToList();
        } else {
            close();
        }
        return true;
    }

    if (mode_ == Mode::List) {
        switch (key) {
        case Qt::Key_V:
            layout_ = columns(terminalWidth_) > 1 ? Layout::Table : Layout::Grid;
            emit invalidated();
            return true;
        case Qt::Key_Left:
            selected_ = qMax(0, selected_ - 1);
            emit invalidated();
            return true;
        case Qt::Key_Right:
            selected_ = qMin(static_cast<int>(cachedRows_.size()) - 1, selected_ + 1);
            emit invalidated();
            return true;
        case Qt::Key_Up:
        case Qt::Key_K:
            if (selected_ < 0 && !cachedRows_.isEmpty())
                selected_ = 0;
            else if (selected_ > 0)
                selected_ = qMax(0, selected_ - columns(terminalWidth_));
            emit invalidated();
            return true;
        case Qt::Key_Down:
        case Qt::Key_J:
            if (selected_ < cachedRows_.size() - 1)
                selected_ = qMin(
                    static_cast<int>(cachedRows_.size()) - 1,
                    selected_ < 0 ? 0 : selected_ + columns(terminalWidth_));
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
    if (selected_ < 0 || selected_ >= cachedRows_.size())
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
        if (selected_ < 0 || selected_ >= cachedRows_.size())
            return;
        const auto &row = cachedRows_.at(selected_);
        tag             = row.sourceTag;
        id              = row.row.id;
    }
    if (id.isEmpty())
        return;
    const bool ok = registry_->killTask(tag, id);
    flashFooter(
        ok ? QStringLiteral("Stop requested: %1").arg(id)
           : QStringLiteral("Stop request failed: %1").arg(id));
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
    if (mode_ == Mode::Hidden || maxHeight_ < kMinHeight)
        return 0;
    /* Both modes have the same chrome: top border + footer + bottom border
     * (3 rows). The body is rows in List mode and detail-content lines in
     * Detail mode. Cap to maxHeight_ so a multi-KB bash tail does not
     * displace the scroll view; the renderer shows the tail when content
     * exceeds the box. */
    int body = 1;
    if (mode_ == Mode::List) {
        const int count = static_cast<int>(cachedRows_.size());
        const int cols  = columns(terminalWidth_);
        body            = cols > 1 ? 1 + 2 * ((count + cols - 1) / cols) : 1 + count;
        body            = qMax(2, body);
    } else {
        const int contentLines = static_cast<int>(detailContent_.count(QLatin1Char('\n'))) + 1;
        body                   = contentLines < 1 ? 1 : contentLines;
    }
    const int total = body + 3; /* header is part of border title; footer + 2 borders */
    return total > maxHeight_ ? maxHeight_ : total;
}

void QTuiTaskOverlay::render(QTuiScreen &screen, int startY, int width)
{
    setTerminalWidth(width);
    if (lineCount() == 0)
        return;
    if (mode_ == Mode::Detail) {
        renderDetail(screen, startY, width);
    } else {
        renderList(screen, startY, width);
    }
}

void QTuiTaskOverlay::tick()
{
    tickCounter_ = (tickCounter_ + 1) % 10;
    if (animationEnabled_)
        frame_ = (frame_ + 1) % 4;
    if (tickCounter_ == 0) {
        refreshRows();
        if (mode_ == Mode::Detail)
            reloadDetailContent();
    }
    if (footerFlashUntil_ > 0 && QDateTime::currentMSecsSinceEpoch() > footerFlashUntil_) {
        footerFlash_.clear();
        footerFlashUntil_ = 0;
    }
}

void QTuiTaskOverlay::setTerminalWidth(int width)
{
    terminalWidth_ = qMax(1, width);
}

void QTuiTaskOverlay::setAnimationEnabled(bool enabled)
{
    animationEnabled_ = enabled;
    emit invalidated();
}

int QTuiTaskOverlay::columns(int width) const
{
    if (layout_ == Layout::Table)
        return 1;
    const int cols = qMax(1, (width - 2) / 36);
    if (layout_ == Layout::Automatic && cachedRows_.size() <= qMax(1, maxHeight_ - 4))
        return 1;
    return cols;
}

QString QTuiTaskOverlay::marker(QSocTask::Status status) const
{
    if (status == QSocTask::Status::Running)
        return animationEnabled_ ? QString(QLatin1Char("-\\|/"[frame_])) : QStringLiteral("*");
    if (status == QSocTask::Status::Failed || status == QSocTask::Status::Stuck)
        return QStringLiteral("!");
    if (status == QSocTask::Status::Completed)
        return QStringLiteral("+");
    if (status == QSocTask::Status::Aborted)
        return QStringLiteral("x");
    return QStringLiteral(".");
}

QString QTuiTaskOverlay::summary() const
{
    int active  = 0;
    int done    = 0;
    int failed  = 0;
    int stopped = 0;
    for (const auto &entry : cachedRows_) {
        if (entry.row.status == QSocTask::Status::Completed)
            ++done;
        else if (entry.row.status == QSocTask::Status::Failed)
            ++failed;
        else if (entry.row.status == QSocTask::Status::Aborted)
            ++stopped;
        else
            ++active;
    }
    return QStringLiteral("%1 active | %2 done | %3 failed | %4 stopped")
        .arg(active)
        .arg(done)
        .arg(failed)
        .arg(stopped);
}

int QTuiTaskOverlay::previewHeight(int width, int availableHeight) const
{
    if (mode_ != Mode::Hidden || availableHeight < 8)
        return 0;
    const bool active = std::any_of(cachedRows_.cbegin(), cachedRows_.cend(), [](const auto &entry) {
        return !QSocTask::isTerminal(entry.row.status);
    });
    if (!active)
        return 0;
    const int cols = qMax(1, width / 36);
    const int rows = (static_cast<int>(cachedRows_.size()) + cols - 1) / cols;
    return qMin(qMax(4, availableHeight / 3), qMin(10, 2 + 2 * rows));
}

void QTuiTaskOverlay::renderPreview(QTuiScreen &screen, int startY, int width, int height)
{
    if (height < 4)
        return;
    screen.putString(
        0,
        startY,
        fitToWidth(QStringLiteral("Tasks: ") + summary() + QStringLiteral(" | Ctrl+B"), width),
        true);
    renderCells(screen, startY + 1, width, height - 2, true);
    const int shown
        = qMin(static_cast<int>(cachedRows_.size()), ((height - 2) / 2) * qMax(1, width / 36));
    const QString footer
        = QStringLiteral("Showing %1/%2 | Ctrl+B details").arg(shown).arg(cachedRows_.size());
    screen.putString(0, startY + height - 1, fitToWidth(footer, width), false, true);
}

void QTuiTaskOverlay::renderCells(QTuiScreen &screen, int startY, int width, int height, bool preview)
{
    const int    cols      = preview ? qMax(1, width / 36) : columns(width + 2);
    const int    cellWidth = qMax(1, width / cols);
    const int    capacity  = qMax(1, height / 2) * cols;
    const int    first     = preview || selected_ < 0 ? 0 : (selected_ / capacity) * capacity;
    const qint64 now       = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < capacity && first + i < cachedRows_.size(); ++i) {
        const auto   &entry    = cachedRows_.at(first + i);
        const auto   &row      = entry.row;
        const int     x        = (i % cols) * cellWidth + (preview ? 0 : 1);
        const int     y        = startY + (i / cols) * 2;
        const bool    selected = !preview && first + i == selected_;
        const auto    color    = row.status == QSocTask::Status::Failed  ? QTuiFgColor::Red
                                 : row.status == QSocTask::Status::Stuck ? QTuiFgColor::Yellow
                                                                         : QTuiFgColor::Default;
        const QString heading  = QStringLiteral("[%1] %2/%3 %4")
                                     .arg(marker(row.status), entry.sourceTag, row.id, row.label);
        screen.putString(x, y, fitToWidth(heading, cellWidth - 1), selected, false, selected, color);
        QString detail = statusLabel(row.status);
        if (row.startedAtMs > 0)
            detail += QStringLiteral(" | %1").arg(
                QTuiText::formatDuration(qMax(qint64(0), now - row.startedAtMs) / 1000));
        if (QSocTask::isTerminal(row.status))
            detail = statusLabel(row.status);
        if (y + 1 < startY + height)
            screen.putString(
                x, y + 1, fitToWidth(QStringLiteral("    ") + detail, cellWidth - 1), false, true);
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
    const int     height = lineCount();
    const int     innerW = qMax(0, width - 2);
    const bool    grid   = columns(width) > 1;
    const QString title  = QStringLiteral("Tasks (%1) | %2")
                               .arg(cachedRows_.size())
                               .arg(grid ? QStringLiteral("Grid") : QStringLiteral("Table"));
    renderBorder(screen, startY, height, width, title);
    screen.putString(1, startY + 1, fitToWidth(summary(), innerW), true);
    const int bodyHeight = qMax(0, height - 4);
    const int capacity   = grid ? qMax(1, bodyHeight / 2) * columns(width) : qMax(1, bodyHeight);
    const int first      = selected_ < 0 ? 0 : (selected_ / capacity) * capacity;
    if (grid) {
        renderCells(screen, startY + 2, innerW, bodyHeight, false);
    } else {
        for (int i = 0; i < bodyHeight && first + i < cachedRows_.size(); ++i) {
            const auto &entry    = cachedRows_.at(first + i);
            const auto &row      = entry.row;
            const bool  selected = first + i == selected_;
            const QString line = QStringLiteral("%1 [%2] %3 %4 %5")
                                     .arg(
                                         selected ? QStringLiteral(">") : QStringLiteral(" "),
                                         marker(row.status),
                                         fitToWidth(entry.sourceTag + QLatin1Char('/') + row.id, 14),
                                         fitToWidth(statusLabel(row.status), 8),
                                         row.label);
            screen.putString(
                1,
                startY + 2 + i,
                fitToWidth(line, innerW),
                selected,
                false,
                selected,
                row.status == QSocTask::Status::Failed ? QTuiFgColor::Red : QTuiFgColor::Default);
        }
    }
    const QString range  = QStringLiteral("%1-%2/%3 ")
                               .arg(cachedRows_.isEmpty() ? 0 : first + 1)
                               .arg(qMin(first + capacity, static_cast<int>(cachedRows_.size())))
                               .arg(cachedRows_.size());
    const QString footer = footerFlash_.isEmpty()
                               ? range
                                     + QStringLiteral(
                                         "v view  m motion  Enter details  x stop  Esc back")
                               : footerFlash_;
    screen.putString(1, startY + height - 2, fitToWidth(footer, innerW), false, true);
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
            screen.putString(1, startY + 1 + i, fitToWidth(line, innerW), false);
        }
    }
    /* Footer */
    const QString footer = footerFlash_.isEmpty()
                                   && QDateTime::currentMSecsSinceEpoch() > footerFlashUntil_
                               ? QStringLiteral("ESC back  x kill")
                               : footerFlash_;
    screen.putString(2, startY + height - 2, fitToWidth(footer, innerW - 2), false, true);
}
