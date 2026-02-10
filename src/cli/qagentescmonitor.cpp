// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qagentescmonitor.h"

#ifndef _WIN32
#include <unistd.h>
#endif

QAgentEscMonitor::QAgentEscMonitor(QObject *parent)
    : QObject(parent)
{}

QAgentEscMonitor::~QAgentEscMonitor()
{
    stop();
}

void QAgentEscMonitor::start()
{
    if (active) {
        return;
    }

#ifndef _WIN32
    /* Save original terminal settings */
    if (tcgetattr(STDIN_FILENO, &origTermios) == 0) {
        termiosSaved = true;

        /* Enter raw mode: non-canonical, no echo */
        struct termios raw = origTermios;
        raw.c_lflag &= ~static_cast<tcflag_t>(ICANON | ECHO);
        raw.c_cc[VMIN]  = 0;
        raw.c_cc[VTIME] = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &raw);
    }

    /* Create socket notifier for stdin */
    notifier = new QSocketNotifier(STDIN_FILENO, QSocketNotifier::Read, this);
    connect(notifier, &QSocketNotifier::activated, this, [this]() {
        char buf[32];
        auto n = read(STDIN_FILENO, buf, sizeof(buf));
        for (int i = 0; i < n; i++) {
            if (buf[i] == 0x1B) {
                emit escPressed();
                return;
            }
        }
    });

    active = true;
#endif
}

void QAgentEscMonitor::stop()
{
    if (!active) {
        return;
    }

#ifndef _WIN32
    /* Destroy notifier first */
    if (notifier) {
        delete notifier;
        notifier = nullptr;
    }

    /* Restore terminal settings */
    if (termiosSaved) {
        tcsetattr(STDIN_FILENO, TCSANOW, &origTermios);
        termiosSaved = false;
    }

    active = false;
#endif
}

bool QAgentEscMonitor::isActive() const
{
    return active;
}
