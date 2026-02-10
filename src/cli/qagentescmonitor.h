// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QAGENTESCMONITOR_H
#define QAGENTESCMONITOR_H

#include <QObject>
#include <QSocketNotifier>

#ifndef _WIN32
#include <termios.h>
#endif

/**
 * @brief ESC key monitor for interrupting agent operations
 * @details Uses termios raw mode and QSocketNotifier to detect ESC keypress
 *          during agent execution. Works inside nested QEventLoops.
 */
class QAgentEscMonitor : public QObject
{
    Q_OBJECT

public:
    explicit QAgentEscMonitor(QObject *parent = nullptr);
    ~QAgentEscMonitor() override;

    /**
     * @brief Start monitoring stdin for ESC key
     * @details Saves terminal settings, enters raw mode, creates QSocketNotifier
     */
    void start();

    /**
     * @brief Stop monitoring and restore terminal settings
     */
    void stop();

    /**
     * @brief Check if monitor is currently active
     * @return True if monitoring is active
     */
    bool isActive() const;

signals:
    /**
     * @brief Emitted when ESC key is detected
     */
    void escPressed();

private:
#ifndef _WIN32
    struct termios origTermios;
#endif
    QSocketNotifier *notifier     = nullptr;
    bool             active       = false;
    bool             termiosSaved = false;
};

#endif // QAGENTESCMONITOR_H
