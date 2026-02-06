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
{
    detect();
}

void QTerminalCapability::detect()
{
    /* Check if file descriptors are TTY */
    stdinIsatty  = isatty(STDIN_FILENO) != 0;
    stdoutIsatty = isatty(STDOUT_FILENO) != 0;

    /* Get TERM environment variable */
    const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    terminalType                  = env.value("TERM", "");

    /* Detect capabilities */
    colorSupport   = checkColorSupport();
    unicodeSupport = checkUnicodeSupport();

    /* Detect terminal size */
    detectSize();
}

void QTerminalCapability::detectSize()
{
    termColumns = 80;
    termRows    = 24;

#ifdef Q_OS_WIN
    CONSOLE_SCREEN_BUFFER_INFO csbi;
    if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
        termColumns = csbi.srWindow.Right - csbi.srWindow.Left + 1;
        termRows    = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
    }
#else
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_col > 0) {
        termColumns = ws.ws_col;
        termRows    = ws.ws_row;
    } else {
        /* Fallback to environment variables */
        const QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        bool                      ok  = false;

        int cols = env.value("COLUMNS", "80").toInt(&ok);
        if (ok && cols > 0) {
            termColumns = cols;
        }

        int lines = env.value("LINES", "24").toInt(&ok);
        if (ok && lines > 0) {
            termRows = lines;
        }
    }
#endif
}

bool QTerminalCapability::checkColorSupport() const
{
    /* No color support if not a TTY */
    if (!stdoutIsatty) {
        return false;
    }

    /* Check TERM for color capability */
    if (terminalType.isEmpty()) {
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
        if (terminalType == term || terminalType.startsWith(term + "-")) {
            return true;
        }
    }

    /* Check for common patterns */
    if (terminalType.contains("256color") || terminalType.contains("color")
        || terminalType.contains("ansi")) {
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
    return stdinIsatty;
}

bool QTerminalCapability::isOutputInteractive() const
{
    return stdoutIsatty;
}

bool QTerminalCapability::supportsColor() const
{
    return colorSupport;
}

bool QTerminalCapability::supportsUnicode() const
{
    return unicodeSupport;
}

int QTerminalCapability::columns() const
{
    return termColumns;
}

int QTerminalCapability::rows() const
{
    return termRows;
}

bool QTerminalCapability::useEnhancedMode() const
{
    /* Enhanced mode only when both stdin and stdout are TTY */
    return stdinIsatty && stdoutIsatty;
}

void QTerminalCapability::refreshSize()
{
    detectSize();
}

QString QTerminalCapability::termType() const
{
    return terminalType;
}
