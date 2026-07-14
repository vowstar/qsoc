// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qlongtaskmonitor.h"

#include <QThread>
#include <QTimer>

QLongTaskMonitor::QLongTaskMonitor(QObject *parent, const Config &cfg)
    : QObject(parent)
    , tick_(new QTimer(this))
    , cfg_(cfg)
{
    tick_->setInterval(cfg_.tickIntervalMs > 0 ? cfg_.tickIntervalMs : 1000);
    tick_->setSingleShot(false);
    connect(tick_, &QTimer::timeout, this, &QLongTaskMonitor::onTick);
}

void QLongTaskMonitor::start()
{
    Q_ASSERT_X(
        thread() == QThread::currentThread(),
        "QLongTaskMonitor::start",
        "start() must run on the monitor's own thread");
    if (terminated_.loadRelaxed() != 0) {
        return;
    }
    wall_.start();
    lastProgressMs_.store(0, std::memory_order_relaxed);
    idleStreak_ = 0;
    wallClockFired_.storeRelaxed(0);
    stalledFiredThisGap_.storeRelaxed(0);
    tick_->start();
}

void QLongTaskMonitor::notifyProgress()
{
    if (terminated_.loadRelaxed() != 0) {
        return;
    }
    lastProgressMs_.store(wall_.isValid() ? wall_.elapsed() : 0, std::memory_order_relaxed);
    idleStreak_ = 0;
    stalledFiredThisGap_.storeRelaxed(0);
}

void QLongTaskMonitor::finish()
{
    if (!terminated_.testAndSetOrdered(0, 1)) {
        return;
    }
    teardown();
}

void QLongTaskMonitor::cancel(const QString &reason)
{
    if (!terminated_.testAndSetOrdered(0, 1)) {
        return;
    }
    cancelled_.storeRelaxed(1);
    teardown();
    emit cancelled(reason);
}

bool QLongTaskMonitor::isCancelled() const noexcept
{
    return cancelled_.loadRelaxed() != 0;
}

qint64 QLongTaskMonitor::elapsedMs() const
{
    return wall_.isValid() ? wall_.elapsed() : 0;
}

void QLongTaskMonitor::teardown()
{
    tick_->stop();
}

void QLongTaskMonitor::onTick()
{
    if (terminated_.loadRelaxed() != 0) {
        return;
    }
    if (!wall_.isValid()) {
        return;
    }

    const qint64 elapsed = wall_.elapsed();

    if (cfg_.wallClockMs > 0 && elapsed >= cfg_.wallClockMs
        && wallClockFired_.testAndSetOrdered(0, 1)) {
        emit wallClockExceeded(static_cast<int>(elapsed));
        return;
    }

    if (cfg_.stallThresholdMs <= 0) {
        return;
    }

    const qint64 silent = elapsed - lastProgressMs_.load(std::memory_order_relaxed);
    if (silent < cfg_.stallThresholdMs) {
        idleStreak_ = 0;
        return;
    }

    ++idleStreak_;
    if (idleStreak_ < cfg_.consecutiveIdleTicks) {
        return;
    }
    if (!stalledFiredThisGap_.testAndSetOrdered(0, 1)) {
        return;
    }
    emit stalled(static_cast<int>(silent), idleStreak_);
}
