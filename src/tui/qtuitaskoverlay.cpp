// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuitaskoverlay.h"

#include "common/qsoctasksource.h"
#include "tui/qsocdiagram_p.h"
#include "tui/qtuiansi.h"
#include <QSet>

#include <algorithm>
#include <climits>

#include <QChar>
#include <QDateTime>
#include <QMap>
#include <QScopeGuard>
#include <QString>

namespace {

constexpr int kMinHeight       = 6;
constexpr int kDetailTailBytes = 64 * 1024;
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

QString plainText(const QString &text, bool multiline = false)
{
    QString plain;
    for (const auto &span : QTuiAnsi::parse(text)) {
        for (const char32_t ch : span.text.toUcs4()) {
            if (!QChar::isPrint(ch) && !(multiline && ch == U'\n'))
                plain += QLatin1Char(' ');
            else
                plain += QString::fromUcs4(&ch, 1);
        }
    }
    return plain;
}

QString fitToWidth(const QString &text, int width)
{
    if (width <= 0)
        return {};
    QString plain = plainText(text);
    if (QTuiText::visualWidth(plain) > width)
        plain = width <= 3 ? QString(width, QLatin1Char('.')) : QTuiText::truncate(plain, width);
    return plain + QString(qMax(0, width - QTuiText::visualWidth(plain)), QLatin1Char(' '));
}

QString statusLabel(const QSocTask::Row &row)
{
    return row.status == QSocTask::Status::Running && row.waitingForPeer ? QStringLiteral("waiting")
                                                                         : statusLabel(row.status);
}

QString dependencyGraph(
    const QList<QSocTaskRegistry::TaggedRow> &rows, int width, const QString &selected)
{
    using namespace QSocDiagram::Detail;
    QList<QSocTask::Row> todos;
    QString              problem;
    for (const auto &entry : rows) {
        if (entry.row.kind != QSocTask::Kind::Todo)
            continue;
        if (entry.row.id == "error")
            problem = entry.row.summary;
        else
            todos.append(entry.row);
    }
    std::sort(todos.begin(), todos.end(), [](const auto &left, const auto &right) {
        return left.id.toInt() < right.id.toInt();
    });
    QString text = QStringLiteral("Dependencies connect prerequisites to dependents.\n");
    if (!problem.isEmpty())
        text += problem + QLatin1Char('\n');
    if (!todos.isEmpty() && !todos.first().dependencyCheck.isEmpty())
        text += todos.first().dependencyCheck + QLatin1Char('\n');
    if (todos.size() > nodeLimit)
        text += QStringLiteral(
            "Showing the selected task and nearby dependencies; use the list for all tasks.\n");
    QSet<QString> included;
    if (todos.size() <= nodeLimit) {
        for (const auto &row : todos)
            included.insert(row.id);
    } else {
        included.insert(selected);
        for (const auto &row : todos) {
            if (included.size() >= nodeLimit)
                break;
            if (row.id == selected || row.dependsOn.contains(selected))
                included.insert(row.id);
            if (row.id == selected)
                for (const auto &id : row.dependsOn)
                    if (included.size() < nodeLimit)
                        included.insert(id);
        }
    }
    QString fallback;
    Graph   graph;
    try {
        for (const auto &row : todos) {
            if (!included.contains(row.id))
                continue;
            const QString label      = QStringLiteral("%1#%2 %3 [%4]")
                                           .arg(row.id == selected ? "* " : "")
                                           .arg(row.id, plainText(row.label), statusLabel(row));
            const int     index      = graph.node(row.id);
            graph.nodes[index].label = QTuiText::truncate(label, labelLimit);
            fallback += label + QStringLiteral("\n  Prerequisites: ")
                        + (row.dependsOn.isEmpty() ? QStringLiteral("none")
                                                   : row.dependsOn.join(QStringLiteral(", ")))
                        + QLatin1Char('\n');
        }
        for (const auto &row : todos) {
            if (!included.contains(row.id))
                continue;
            for (const auto &id : row.dependsOn)
                if (included.contains(id))
                    graph.edge({.from = graph.node(id), .to = graph.node(row.id)});
        }
        const auto result = drawGraph(graph, width);
        if (result.valid()) {
            for (const auto &line : result.rows) {
                for (const auto &span : line)
                    text += span.text;
                text += QLatin1Char('\n');
            }
            return text;
        }
    } catch (const Failure &) {
    }
    return text + QStringLiteral("Dependency list (diagram does not fit):\n") + fallback;
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
        connect(
            registry_,
            &QSocTaskRegistry::estimatesChanged,
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
    selected_        = 0;
    graphVisible_    = false;
    detailFromGraph_ = false;
    detailSourceTag_.clear();
    detailId_.clear();
    detailContent_.clear();
    detailView_.clear();
    footerFlash_.clear();
    footerFlashUntil_ = 0;
    mode_             = Mode::List;
    emit invalidated();
}

void QTuiTaskOverlay::confirmInterrupt()
{
    mode_ = Mode::ConfirmInterrupt;
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
    if (mode_ == Mode::Detail)
        reloadDetailContent();
    clampSelection();
    emit invalidated();
}

void QTuiTaskOverlay::refreshRows()
{
    if (registry_ == nullptr) {
        cachedRows_.clear();
        summary_.clear();
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
    selected_   = cachedRows_.isEmpty() ? 0 : -1;
    cachedRows_ = ordered;
    summary_.clear();
    summary_ = summary();
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

int QTuiTaskOverlay::pageCapacity() const
{
    const int body = qMax(1, lineCount() - 4);
    const int cols = columns(terminalWidth_);
    return cols > 1 ? qMax(1, body / 2) * cols : body;
}

bool QTuiTaskOverlay::handleKey(int key, bool ctrl)
{
    Q_UNUSED(ctrl);
    if (mode_ == Mode::Hidden)
        return false;

    if (mode_ == Mode::ConfirmInterrupt) {
        const bool confirmed = key == Qt::Key_Y;
        if (confirmed || key == Qt::Key_Escape || key == Qt::Key_N || key == Qt::Key_Return
            || key == Qt::Key_Enter || key == (int(Qt::CTRL) | Qt::Key_C)) {
            close();
            if (confirmed)
                emit interruptConfirmed();
        }
        return true;
    }
    if (key == Qt::Key_G) {
        if (graphVisible_)
            exitDetailToList();
        else
            enterGraph();
        return true;
    }
    if (graphVisible_ && (key == Qt::Key_Return || key == Qt::Key_Enter)) {
        graphVisible_    = false;
        detailFromGraph_ = true;
        detailView_.clear();
        reloadDetailContent();
        emit invalidated();
        return true;
    }
    if (key == Qt::Key_H && mode_ == Mode::List) {
        historyVisible_ = !historyVisible_;
        emit historyRequested(historyVisible_);
        return true;
    }

    if (key == Qt::Key_M) {
        setAnimationEnabled(!animationEnabled_);
        return true;
    }
    if (key == Qt::Key_E) {
        estimatesVisible_ = !estimatesVisible_;
        reloadDetailContent();
        emit invalidated();
        return true;
    }
    if (key == Qt::Key_R && registry_) {
        emit registry_->estimateRefreshRequested();
        return true;
    }
    if (key == Qt::Key_P && mode_ == Mode::Detail) {
        rangesVisible_ = !rangesVisible_;
        reloadDetailContent();
        emit invalidated();
        return true;
    }
    if ((key == Qt::Key_F || key == Qt::Key_S) && actOnAgent(key))
        return true;
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
        case Qt::Key_PageUp:
        case Qt::Key_PageDown:
            selected_ = qBound(
                0,
                qMax(0, selected_) + (key == Qt::Key_PageUp ? -1 : 1) * pageCapacity(),
                qMax(0, int(cachedRows_.size()) - 1));
            emit invalidated();
            return true;
        case Qt::Key_Home:
        case Qt::Key_End:
            selected_ = key == Qt::Key_Home ? 0 : qMax(0, int(cachedRows_.size()) - 1);
            emit invalidated();
            return true;
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
        const int page = qMax(1, lineCount() - 4);
        if (key == Qt::Key_Up || key == Qt::Key_K || key == Qt::Key_PageUp || key == Qt::Key_Home)
            detailView_.scrollUp(key == Qt::Key_Home ? INT_MAX : key == Qt::Key_PageUp ? page : 1);
        else if (key == Qt::Key_Down || key == Qt::Key_J || key == Qt::Key_PageDown)
            detailView_.scrollDown(key == Qt::Key_PageDown ? page : 1);
        else if (key == Qt::Key_End)
            detailView_.scrollToBottom();
        emit invalidated();
        return true; /* swallow */
    }
    return false;
}

bool QTuiTaskOverlay::actOnAgent(int key)
{
    if (!agentActions_ || selected_ < 0 || selected_ >= cachedRows_.size())
        return false;
    const auto &row = cachedRows_.at(selected_).row;
    if (row.kind != QSocTask::Kind::SubAgent)
        return false;
    if (key == Qt::Key_S && !row.resumable) {
        flashFooter(QStringLiteral("%1 can no longer receive messages").arg(row.id));
        return true;
    }
    const QString id = row.id;
    close();
    if (key == Qt::Key_F)
        emit focusRequested(id);
    else
        emit messageRequested(id);
    return true;
}

QString QTuiTaskOverlay::agentKeys() const
{
    const bool agentRow = agentActions_ && selected_ >= 0 && selected_ < cachedRows_.size()
                          && cachedRows_.at(selected_).row.kind == QSocTask::Kind::SubAgent;
    return agentRow ? QStringLiteral("f focus  s send  ") : QString();
}

void QTuiTaskOverlay::enterDetail()
{
    if (selected_ < 0 || selected_ >= cachedRows_.size())
        return;
    const auto &row  = cachedRows_.at(selected_);
    detailSourceTag_ = row.sourceTag;
    detailId_        = row.row.id;
    detailContent_.clear();
    detailView_.clear();
    reloadDetailContent();
    mode_ = Mode::Detail;
    emit invalidated();
}

void QTuiTaskOverlay::enterGraph()
{
    auto chosen = cachedRows_.cend();
    if (selected_ >= 0 && selected_ < cachedRows_.size()
        && cachedRows_[selected_].row.kind == QSocTask::Kind::Todo
        && cachedRows_[selected_].row.id != "error")
        chosen = cachedRows_.cbegin() + selected_;
    if (chosen == cachedRows_.cend())
        chosen = std::find_if(cachedRows_.cbegin(), cachedRows_.cend(), [](const auto &row) {
            return row.row.kind == QSocTask::Kind::Todo && row.row.id != "error";
        });
    if (chosen == cachedRows_.cend()) {
        flashFooter(QStringLiteral("No todos in this workspace"));
        return;
    }
    detailSourceTag_ = chosen->sourceTag;
    detailId_        = chosen->row.id;
    graphAtStart_    = true;
    graphVisible_    = true;
    detailFromGraph_ = false;
    detailView_.clear();
    mode_ = Mode::Detail;
    reloadDetailContent();
    detailView_.scrollUp(INT_MAX);
    emit invalidated();
}

void QTuiTaskOverlay::exitDetailToList()
{
    if (detailFromGraph_ && !graphVisible_) {
        detailFromGraph_ = false;
        graphAtStart_    = true;
        graphVisible_    = true;
        reloadDetailContent();
        detailView_.scrollUp(INT_MAX);
        emit invalidated();
        return;
    }
    graphVisible_    = false;
    detailFromGraph_ = false;
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
    const QString previous = detailContent_;
    const QString source   = detailSourceTag_;
    const QString id       = detailId_;
    const auto    rebuild  = qScopeGuard([&] {
        if (previous == detailContent_)
            return;
        detailView_.clear(true);
        detailView_.appendPartial(plainText(detailContent_, true));
    });
    if (graphVisible_) {
        detailContent_ = dependencyGraph(cachedRows_, qMax(1, terminalWidth_ - 4), detailId_);
        return;
    }
    const QString tail = registry_->tailFor(detailSourceTag_, detailId_, kDetailTailBytes);
    if (detailSourceTag_ != source || detailId_ != id)
        return;
    if (tail.isEmpty() && !previous.isEmpty())
        return;
    detailContent_ = tail.isEmpty() ? QStringLiteral("(no output yet)\n") : tail;
    auto estimate  = registry_->estimateFor(detailSourceTag_, detailId_);
    if (detailSourceTag_ == "todo") {
        QString plan;
        for (const auto &entry : cachedRows_)
            if (entry.sourceTag == "todo" && entry.row.id == detailId_)
                plan = entry.row.planId;
        int executions = 0;
        for (const auto &entry : cachedRows_) {
            if (entry.row.kind == QSocTask::Kind::Todo || entry.row.todoId <= 0
                || QString::number(entry.row.todoId) != detailId_ || entry.row.planId != plan)
                continue;
            ++executions;
            detailContent_ += QStringLiteral("\nExecution %1: %2 | %3")
                                  .arg(entry.row.id, statusLabel(entry.row), entry.row.label);
            if (!QSocTask::isTerminal(entry.row.status) && !entry.estimate.summary.isEmpty()) {
                estimate = entry.estimate;
                estimate.summary += QStringLiteral(" (execution %1)").arg(entry.row.id);
            }
        }
        if (executions)
            detailContent_ += QStringLiteral("\n%1 linked execution records.\n").arg(executions);
    }
    if (!estimatesVisible_ || estimate.summary.isEmpty())
        return;
    QStringList lines;
    lines << QStringLiteral("Estimate: ") + estimate.summary;
    if (estimate.updatedAtMs > 0)
        lines << QStringLiteral("Updated %1 ago")
                     .arg(
                         QTuiText::formatDuration(
                             qMax(qint64(0), QDateTime::currentMSecsSinceEpoch() - estimate.updatedAtMs)
                             / 1000));
    for (const auto &item : estimate.remaining)
        lines << QStringLiteral("Remaining: ") + item;
    for (const auto &item : estimate.unknowns)
        lines << QStringLiteral("Unknown: ") + item;
    if (!estimate.reason.isEmpty())
        lines << QStringLiteral("Revision: ") + estimate.reason;
    if (!estimate.evidence.isEmpty())
        lines << QStringLiteral("Evidence: ") + estimate.evidence.join(QStringLiteral(", "))
                     + QStringLiteral(" (partial observations)");
    if (rangesVisible_) {
        lines << QStringLiteral("Provisional ranges, accuracy not calibrated:");
        lines
            << (estimate.progressLow < 0
                    ? QStringLiteral("Progress: unknown")
                    : QStringLiteral("Progress: %1-%2% (estimated work, not confidence)")
                          .arg((estimate.progressLow / 5) * 5)
                          .arg(qMin(99, ((estimate.progressHigh + 4) / 5) * 5)));
        lines
            << (estimate.secondsLow < 0 ? QStringLiteral("Remaining time: unknown")
                                        : QStringLiteral("Remaining time: %1-%2s (estimate)")
                                              .arg(estimate.secondsLow)
                                              .arg(estimate.secondsHigh));
    }
    detailContent_ += QLatin1Char('\n') + lines.join(QLatin1Char('\n'));
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
    if (tag == "todo") {
        flashFooter(QStringLiteral("Stop the linked execution; todo items are kept"));
        return;
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
    if (mode_ == Mode::ConfirmInterrupt)
        return kMinHeight;
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
    if (mode_ == Mode::ConfirmInterrupt) {
        renderBorder(
            screen, startY, lineCount(), width, QStringLiteral("Interrupt current operation?"));
        screen.putString(
            2,
            startY + 1,
            fitToWidth(QStringLiteral("Completed changes are not undone."), width - 4));
        screen.putString(
            2,
            startY + 3,
            fitToWidth(QStringLiteral("Y interrupt   Esc / Enter continue"), width - 4),
            true);
        return;
    }
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

QString QTuiTaskOverlay::marker(const QSocTask::Row &row) const
{
    const auto status = row.status;
    if (status == QSocTask::Status::Running && row.waitingForPeer)
        return QStringLiteral(".");
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

QString QTuiTaskOverlay::summary(bool includeTodos) const
{
    if (includeTodos && !summary_.isEmpty())
        return summary_;
    QMap<QString, int> counts;
    int                planTotal = 0, planDone = 0;
    for (const auto &entry : cachedRows_) {
        if (entry.row.kind == QSocTask::Kind::Todo) {
            if (includeTodos && entry.row.id != "error") {
                ++planTotal;
                planDone += entry.row.status == QSocTask::Status::Completed;
            }
        } else
            ++counts[statusLabel(entry.row)];
    }
    QStringList parts;
    for (const auto &state :
         {QStringLiteral("failed"),
          QStringLiteral("stuck"),
          QStringLiteral("aborted"),
          QStringLiteral("running"),
          QStringLiteral("waiting"),
          QStringLiteral("pending"),
          QStringLiteral("idle"),
          QStringLiteral("done")}) {
        const int count = counts.value(state);
        if (count == 0 && state != QStringLiteral("running") && state != QStringLiteral("done"))
            continue;
        const QString label = state == QStringLiteral("pending")   ? QStringLiteral("queued")
                              : state == QStringLiteral("aborted") ? QStringLiteral("stopped")
                                                                   : state;
        parts << QStringLiteral("%1 %2").arg(count).arg(label);
    }
    const QString executions = parts.join(QStringLiteral(" | "));
    if (!planTotal)
        return executions;
    const QString plan = QStringLiteral("Plan %1/%2 done").arg(planDone).arg(planTotal);
    return counts.isEmpty() ? plan : plan + QStringLiteral(" | Executions: ") + executions;
}

int QTuiTaskOverlay::previewHeight(int width, int availableHeight) const
{
    if (mode_ != Mode::Hidden || availableHeight < 8)
        return 0;
    const bool active = std::any_of(cachedRows_.cbegin(), cachedRows_.cend(), [](const auto &entry) {
        return entry.row.kind != QSocTask::Kind::Todo && !QSocTask::isTerminal(entry.row.status);
    });
    if (!active)
        return 0;
    const int cols = qMax(1, width / 36);
    const int count = std::count_if(cachedRows_.cbegin(), cachedRows_.cend(), [](const auto &entry) {
        return entry.row.kind != QSocTask::Kind::Todo;
    });
    const int rows = (count + cols - 1) / cols;
    return qMin(qMax(4, availableHeight / 3), qMin(10, 2 + 2 * rows));
}

void QTuiTaskOverlay::renderPreview(QTuiScreen &screen, int startY, int width, int height)
{
    if (height < 4)
        return;
    screen.putString(
        0,
        startY,
        fitToWidth(QStringLiteral("Tasks: ") + summary(false) + QStringLiteral(" | Ctrl+B"), width),
        true);
    renderCells(screen, startY + 1, width, height - 2, true);
    const int count = std::count_if(cachedRows_.cbegin(), cachedRows_.cend(), [](const auto &entry) {
        return entry.row.kind != QSocTask::Kind::Todo;
    });
    const int     shown  = qMin(count, ((height - 2) / 2) * qMax(1, width / 36));
    const QString footer = QStringLiteral("Showing %1/%2 | Ctrl+B details").arg(shown).arg(count);
    screen.putString(0, startY + height - 1, fitToWidth(footer, width), false, true);
}

void QTuiTaskOverlay::renderCells(QTuiScreen &screen, int startY, int width, int height, bool preview)
{
    auto rows = cachedRows_;
    if (preview)
        rows.removeIf([](const auto &entry) { return entry.row.kind == QSocTask::Kind::Todo; });
    const int    cols      = preview ? qMax(1, width / 36) : columns(width + 2);
    const int    cellWidth = qMax(1, width / cols);
    const int    capacity  = qMax(1, height / 2) * cols;
    const int    first     = preview || selected_ < 0 ? 0 : (selected_ / capacity) * capacity;
    const qint64 now       = QDateTime::currentMSecsSinceEpoch();
    for (int i = 0; i < capacity && first + i < rows.size(); ++i) {
        const auto   &entry    = rows.at(first + i);
        const auto   &row      = entry.row;
        const int     x        = (i % cols) * cellWidth + (preview ? 0 : 1);
        const int     y        = startY + (i / cols) * 2;
        const bool    selected = !preview && first + i == selected_;
        const auto    color    = row.status == QSocTask::Status::Failed  ? QTuiFgColor::Red
                                 : row.status == QSocTask::Status::Stuck ? QTuiFgColor::Yellow
                                                                         : QTuiFgColor::Default;
        const QString heading
            = QStringLiteral("[%1] %2/%3 %4").arg(marker(row), entry.sourceTag, row.id, row.label);
        screen.putString(x, y, fitToWidth(heading, cellWidth - 1), selected, false, selected, color);
        QString detail = row.kind == QSocTask::Kind::Todo ? row.summary : statusLabel(row);
        if (row.startedAtMs > 0)
            detail += QStringLiteral(" | %1").arg(
                QTuiText::formatDuration(qMax(qint64(0), now - row.startedAtMs) / 1000));
        if (QSocTask::isTerminal(row.status))
            detail = statusLabel(row);
        else if (estimatesVisible_ && !entry.estimate.summary.isEmpty())
            detail += QStringLiteral(" | est: ") + entry.estimate.summary;
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
    renderBorder(
        screen,
        startY,
        height,
        width,
        title + (historyVisible_ ? QStringLiteral(" | history") : QString()));
    screen.putString(1, startY + 1, fitToWidth(summary(), innerW), true);
    const int bodyHeight = qMax(0, height - 4);
    const int capacity   = pageCapacity();
    const int first      = selected_ < 0 ? 0 : (selected_ / capacity) * capacity;
    if (grid) {
        renderCells(screen, startY + 2, innerW, bodyHeight, false);
    } else {
        for (int i = 0; i < bodyHeight && first + i < cachedRows_.size(); ++i) {
            const auto &entry     = cachedRows_.at(first + i);
            const auto &row       = entry.row;
            const bool  selected  = first + i == selected_;
            QString     taskLabel = row.label;
            if (estimatesVisible_ && !entry.estimate.summary.isEmpty())
                taskLabel = fitToWidth(taskLabel, qMax(8, innerW / 3)) + QStringLiteral(" | est: ")
                            + entry.estimate.summary;
            const QString line = QStringLiteral("%1 [%2] %3 %4 %5")
                                     .arg(
                                         selected ? QStringLiteral(">") : QStringLiteral(" "),
                                         marker(row),
                                         fitToWidth(entry.sourceTag + QLatin1Char('/') + row.id, 14),
                                         fitToWidth(statusLabel(row), 8),
                                         taskLabel);
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
    const QString range = QStringLiteral("%1-%2/%3 ")
                              .arg(cachedRows_.isEmpty() ? 0 : first + 1)
                              .arg(qMin(first + capacity, static_cast<int>(cachedRows_.size())))
                              .arg(cachedRows_.size());
    const QString footer
        = footerFlash_.isEmpty()
              ? range + agentKeys()
                    + QStringLiteral("PgUp/PgDn  g graph  h history  Enter detail  x stop  Esc back")
              : footerFlash_;
    screen.putString(1, startY + height - 2, fitToWidth(footer, innerW), false, true);
}

void QTuiTaskOverlay::renderDetail(QTuiScreen &screen, int startY, int width)
{
    const int height   = lineCount();
    const int contentH = height - 2;
    const int innerW   = width - 2;
    QString   title    = graphVisible_ ? QStringLiteral("Task dependencies | #%1").arg(detailId_)
                                       : QStringLiteral("%1 / %2").arg(detailSourceTag_, detailId_);
    for (const auto &entry : cachedRows_) {
        if (!graphVisible_ && entry.sourceTag == detailSourceTag_ && entry.row.id == detailId_) {
            title += QStringLiteral(" | ") + statusLabel(entry.row) + QStringLiteral(" | ")
                     + entry.row.label;
            break;
        }
    }
    if (registry_ && estimatesVisible_
        && !registry_->estimateFor(detailSourceTag_, detailId_).summary.isEmpty())
        title += QStringLiteral(" | estimate");
    renderBorder(screen, startY, height, width, title);

    if (detailContent_.isEmpty()) {
        screen.putString(2, startY + 1, QStringLiteral("(no content)"), false, true);
    } else {
        QTuiScreen content(innerW, contentH - 1);
        detailView_.render(content, 0, contentH - 1, innerW);
        if (graphVisible_ && graphAtStart_) {
            detailView_.scrollUp(INT_MAX);
            detailView_.render(content, 0, contentH - 1, innerW);
            graphAtStart_ = false;
        }
        for (int row = 0; row < content.height(); ++row)
            for (int col = 0; col < innerW; ++col)
                screen.at(col + 1, startY + 1 + row) = content.at(col, row);
    }
    /* Footer */
    const QString footer = graphVisible_
                               ? QStringLiteral("PgUp/PgDn scroll  Enter task detail  g / Esc list")
                           : footerFlash_.isEmpty()
                                   && QDateTime::currentMSecsSinceEpoch() > footerFlashUntil_
                               ? QStringLiteral("PgUp/PgDn scroll  End follow  %1  Esc back")
                                     .arg(
                                         detailView_.isAtBottom() ? QStringLiteral("following")
                                                                  : QStringLiteral("reading"))
                               : footerFlash_;
    screen.putString(2, startY + height - 2, fitToWidth(footer, innerW - 2), false, true);
}

#include "moc_qtuitaskoverlay.cpp"
