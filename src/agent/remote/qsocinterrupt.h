// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCINTERRUPT_H
#define QSOCINTERRUPT_H

namespace QSocInterrupt {

/** Install the interrupt handler and its UI notification pipe. */
bool installBridge();

/** Whether POSIX signal edges can be delivered to the input monitor. */
bool bridgeReady();

/** Whether blocking SSH entry points may use the interrupt latch. */
bool handlerReady();

/** Whether SIGINT is blocked for the raw-byte fallback after bridge failure. */
bool byteFallbackReady();

/** Record from a signal handler; retain two pending edges for double-press. */
void request();

/** Mark one interrupt edge as handled by the UI. */
void acknowledge();

/** Whether the current request has observed an interrupt. */
bool requested();

/** Clear the request latch at the next request boundary. */
void clearRequest();

/** Read end of the POSIX signal pipe, or -1 when unavailable. */
int signalReadFd();

/** Drain the notification pipe and take UI edges without clearing the request. */
int drainSignalPipe();

/** Drop interrupt state owned by a foreground operation before resuming the UI. */
bool finishForegroundHandoff(bool *interrupted = nullptr);

/** Temporarily make Ctrl-C the cooked terminal interrupt key. */
class TerminalInterruptGuard
{
public:
    TerminalInterruptGuard();
    ~TerminalInterruptGuard();

    TerminalInterruptGuard(const TerminalInterruptGuard &)            = delete;
    TerminalInterruptGuard &operator=(const TerminalInterruptGuard &) = delete;

    bool isReady() const { return ready_; }
    bool restore();

private:
    bool          ready_        = false;
    bool          changed_      = false;
    bool          originalIsig_ = false;
    unsigned char originalIntr_ = 0;
};

} // namespace QSocInterrupt

#endif // QSOCINTERRUPT_H
