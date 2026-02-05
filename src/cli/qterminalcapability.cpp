// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "cli/qterminalcapability.h"

#include <QProcessEnvironment>

#ifdef Q_OS_WIN
#include <io.h>
#include <windows.h>
#define isatty _isatty
#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#else
#include <sys/ioctl.h>
#include <unistd.h>
#endif

QTerminalCapability::QTerminalCapability()
    : stdinIsatty_(false)
    , stdoutIsatty_(false)
    , colorSupport_(false)
    , unicodeSupport_(false)
    , columns_(80)
    , rows_(24)
{
    detect();
}

void QTerminalCapability::detect()
{
    /* Check if file descriptors are TTY */
    stdinIsatty_  = isatty(STDIN_FILENO) != 0;
    stdoutIsatty_ = isatty(STDOUT_FILENO) != 0;

    /* Get TERM environment variable */
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    termType_                     = env.value("TERM", "");

    /* Detect capabilities */
    colorSupport_   = checkColorSupport();
    unicodeSupport_ = checkUnicodeSupport();

    /* Detect terminal size */
    detectSize();
}

void QTerminalCapability::detectSize()
{
    columns_ = 80;
    rows_    = 24;

#ifdef Q_OS_WIN
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        columns_ = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        rows_    = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        columns_ = ws.ws_col;
        rows_    = ws.ws_row;
    } else {
        /* Fallback to environment variables */
        const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        bool                      ok  = false;

        int cols = env.value("COLUMNS", "80").toInt(&ok);
        if (ok && cols > 0) {
            columns_ = cols;
        }

        int lines = env.value("LINES", "24").toInt(&ok);
        if (ok && lines > 0) {
            rows_ = lines;
        }
    }
#endif
}

bool QTerminalCapability::checkColorSupport() const
{
    /* No color support if not a TTY */
    if (!stdoutIsatty_) {
        return false;
    }

    /* Check TERM for color capability */
    if (termType_.isEmpty()) {
        return false;
    }

    /* Common color-capable terminals */
    static const QStringList colorTerms
        = {"xterm",  "xterm-color",    "xterm-256color", "screen",        "screen-256color",
           "tmux",   "tmux-256color",  "linux",          "cygwin",        "vt100",
           "rxvt",   "rxvt-unicode",   "rxvt-256color",  "ansi",          "konsole",
           "gnome",  "gnome-256color", "alacritty",      "kitty",         "iterm",
           "iterm2", "eterm",          "putty",          "putty-256color"};

    /* Check for exact match or prefix match */
    for (const QString &term : colorTerms) {
        if (termType_ == term || termType_.startsWith(term + "-")) {
            return true;
        }
    }

    /* Check for common patterns */
    if (termType_.contains("256color") || termType_.contains("color")
        || termType_.contains("ansi")) {
        return true;
    }

    /* Check COLORTERM environment variable */
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    if (env.contains("COLORTERM")) {
        return true;
    }

    /* Check for force color environment variable */
    if (env.contains("FORCE_COLOR") || env.value("CLICOLOR", "0") != "0") {
        return true;
    }

    return false;
}

bool QTerminalCapability::checkUnicodeSupport() const
{
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    /* Check LANG and LC_* for UTF-8 */
    QStringList localeVars = {"LANG", "LC_ALL", "LC_CTYPE"};
    for (const QString &var : localeVars) {
        QString value = env.value(var, "").toUpper();
        if (value.contains("UTF-8") || value.contains("UTF8")) {
            return true;
        }
    }

#ifdef Q_OS_WIN
    /* Windows 10+ generally supports Unicode in console */
    return true;
#else
    return false;
#endif
}

bool QTerminalCapability::isInteractive() const
{
    return stdinIsatty_;
}

bool QTerminalCapability::isOutputInteractive() const
{
    return stdoutIsatty_;
}

bool QTerminalCapability::supportsColor() const
{
    return colorSupport_;
}

bool QTerminalCapability::supportsUnicode() const
{
    return unicodeSupport_;
}

int QTerminalCapability::columns() const
{
    return columns_;
}

int QTerminalCapability::rows() const
{
    return rows_;
}

bool QTerminalCapability::useEnhancedMode() const
{
    /* Enhanced mode only when both stdin and stdout are TTY */
    return stdinIsatty_ && stdoutIsatty_;
}

void QTerminalCapability::refreshSize()
{
    detectSize();
}

QString QTerminalCapability::termType() const
{
    return termType_;
}
