// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QAGENTINPUTMONITOR_H
#define QAGENTINPUTMONITOR_H

#include <QByteArray>
#include <QObject>
#include <QSocketNotifier>
#include <QString>

#ifndef _WIN32
#include <termios.h>
#endif

/**
 * @brief Stdin monitor for ESC interrupt and user input during agent execution
 * @details Uses termios raw mode and QSocketNotifier to detect ESC keypress
 *          and buffer user input. Supports full UTF-8 including CJK (3-byte)
 *          and emoji (4-byte) characters. Line editing: backspace, Ctrl-U, Ctrl-W.
 */
class QAgentInputMonitor : public QObject
{
    Q_OBJECT

public:
    explicit QAgentInputMonitor(QObject *parent = nullptr);
    ~QAgentInputMonitor() override;

    /**
     * @brief Start monitoring stdin for ESC key and user input
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

    /**
     * @brief Process raw bytes as if they came from stdin
     * @details Public for testability. Feeds bytes through the same state machine
     *          used by the stdin callback.
     * @param data Raw bytes to process
     * @param len Number of bytes
     */
    void processBytes(const char *data, int len);

signals:
    /**
     * @brief Emitted when ESC key is detected
     */
    void escPressed();

    /**
     * @brief Emitted when user presses Enter with non-empty input
     * @param text Complete input line
     */
    void inputReady(const QString &text);

    /**
     * @brief Emitted on each keystroke for live display
     * @param text Current input buffer content
     */
    void inputChanged(const QString &text);

private:
    static int  utf8SeqLen(unsigned char lead);
    static bool isUtf8Continuation(unsigned char byte);
    void        appendToInput(const QString &decoded);

#ifndef _WIN32
    struct termios origTermios;
#endif
    QSocketNotifier *notifier     = nullptr;
    bool             active       = false;
    bool             termiosSaved = false;
    QString          inputBuffer;
    QByteArray       utf8Pending;
};

#endif // QAGENTINPUTMONITOR_H
