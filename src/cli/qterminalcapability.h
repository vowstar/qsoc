// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QTERMINALCAPABILITY_H
#define QTERMINALCAPABILITY_H

#include <QString>

/**
 * @brief Terminal capability detection for adaptive CLI behavior
 * @details Detects terminal features to enable enhanced mode (readline, colors)
 *          when running interactively, or fallback to simple mode when piped.
 */
class QTerminalCapability
{
public:
    /**
     * @brief Constructor - detects terminal capabilities
     */
    QTerminalCapability();

    /**
     * @brief Check if stdin is interactive (TTY)
     * @return True if stdin is connected to a terminal
     */
    bool isInteractive() const;

    /**
     * @brief Check if stdout is interactive (TTY)
     * @return True if stdout is connected to a terminal
     */
    bool isOutputInteractive() const;

    /**
     * @brief Check if terminal supports ANSI colors
     * @return True if TERM environment suggests color support
     */
    bool supportsColor() const;

    /**
     * @brief Check if terminal supports Unicode
     * @return True if LANG/LC_* environment suggests UTF-8 support
     */
    bool supportsUnicode() const;

    /**
     * @brief Get terminal width in columns
     * @return Number of columns, or 80 as default
     */
    int columns() const;

    /**
     * @brief Get terminal height in rows
     * @return Number of rows, or 24 as default
     */
    int rows() const;

    /**
     * @brief Check if enhanced readline mode should be used
     * @details Returns true only if both stdin and stdout are TTY
     * @return True if enhanced mode is appropriate
     */
    bool useEnhancedMode() const;

    /**
     * @brief Refresh terminal size (call after SIGWINCH)
     */
    void refreshSize();

    /**
     * @brief Get terminal type from TERM environment
     * @return Terminal type string (e.g., "xterm-256color")
     */
    QString termType() const;

private:
    bool    stdinIsatty    = false;
    bool    stdoutIsatty   = false;
    bool    colorSupport   = false;
    bool    unicodeSupport = false;
    int     termColumns    = 80;
    int     termRows       = 24;
    QString terminalType;

    /**
     * @brief Detect all terminal capabilities
     */
    void detect();

    /**
     * @brief Detect terminal size using ioctl or environment
     */
    void detectSize();

    /**
     * @brief Check TERM environment for color support
     */
    bool checkColorSupport() const;

    /**
     * @brief Check LANG/LC_* for Unicode support
     */
    bool checkUnicodeSupport() const;
};

#endif // QTERMINALCAPABILITY_H
