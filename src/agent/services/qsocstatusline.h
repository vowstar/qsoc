// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCSTATUSLINE_H
#define QSOCSTATUSLINE_H

#include <QObject>
#include <QPointer>
#include <QProcess>
#include <QString>
#include <QTimer>

#include <nlohmann/json.hpp>

/**
 * @brief User-scriptable status line: runs a configured shell command
 *        with session state as JSON on stdin, publishes its stdout.
 * @details The command comes from the user-level configuration only
 *          (`agent.status_line.command`); a project `.qsoc.yml` cannot
 *          supply it, so checking out a repository never executes its
 *          code. Refreshes are debounced; a newer request kills the
 *          in-flight process. Exit code 0 publishes the first
 *          non-empty stdout line (ANSI colors allowed); any failure,
 *          timeout, or empty output clears the line.
 */
class QSocStatusLine : public QObject
{
    Q_OBJECT

public:
    explicit QSocStatusLine(QObject *parent = nullptr);
    ~QSocStatusLine() override;

    /** @brief Set the shell command; empty disables and clears. */
    void setCommand(const QString &command);

    /** @brief Whether a command is configured. */
    bool enabled() const { return !command_.isEmpty(); }

    /** @brief Working directory for the command (project path). */
    void setWorkingDirectory(const QString &dir);

    /** @brief Process kill timeout, default 5000 ms. */
    void setTimeoutMs(int timeoutMs);

    /** @brief Debounce interval, default 300 ms (test support). */
    void setDebounceMs(int debounceMs);

    /**
     * @brief Schedule a refresh with this payload.
     * @details Restarts the debounce timer; only the newest payload is
     *          delivered. No-op when disabled.
     */
    void requestRefresh(const nlohmann::json &payload);

signals:
    /** @brief New status text; empty string means clear the line. */
    void textReady(const QString &text);

private:
    void fire();
    void dropProcess();

    QString        command_;
    QString        workingDir_;
    nlohmann::json pending_;
    QTimer         debounce_;
    QTimer         killTimer_;
    int            timeoutMs_ = 5000;

    QPointer<QProcess> process_;
};

#endif // QSOCSTATUSLINE_H
