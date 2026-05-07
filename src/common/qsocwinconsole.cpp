// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocwinconsole.h"

#ifdef Q_OS_WIN

#include <windows.h>

namespace {

bool  g_bootstrapped    = false;
UINT  g_savedInputCP    = 0;
UINT  g_savedOutputCP   = 0;
DWORD g_savedOutputMode = 0;
DWORD g_savedInputMode  = 0;
bool  g_haveOutputMode  = false;
bool  g_haveInputMode   = false;

} /* namespace */

void QSocWinConsole::bootstrap()
{
    if (g_bootstrapped) {
        return;
    }
    g_bootstrapped = true;

    /* Code page: snapshot, then force UTF-8 (65001). Effect is scoped to
       this process's attached conhost; SetConsoleCP only fails when no
       console is attached (e.g. /SUBSYSTEM:WINDOWS without AllocConsole),
       which is harmless. */
    g_savedInputCP  = GetConsoleCP();
    g_savedOutputCP = GetConsoleOutputCP();
    SetConsoleCP(CP_UTF8);
    SetConsoleOutputCP(CP_UTF8);

    /* Output handle: enable VT (ANSI escape rendering). Older Win10 builds
       silently ignore unknown flags; new flags are additive to whatever the
       parent shell configured. */
    HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
    if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr) {
        DWORD mode = 0;
        if (GetConsoleMode(hOut, &mode)) {
            g_savedOutputMode = mode;
            g_haveOutputMode  = true;
            SetConsoleMode(hOut, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        }
    }

    /* Input handle: enable VT input so the agent's escape-sequence parser
       sees raw VT (cursor keys, bracketed paste) instead of cooked records. */
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    if (hIn != INVALID_HANDLE_VALUE && hIn != nullptr) {
        DWORD mode = 0;
        if (GetConsoleMode(hIn, &mode)) {
            g_savedInputMode = mode;
            g_haveInputMode  = true;
            SetConsoleMode(hIn, mode | ENABLE_VIRTUAL_TERMINAL_INPUT);
        }
    }
}

void QSocWinConsole::restore()
{
    if (!g_bootstrapped) {
        return;
    }
    g_bootstrapped = false;

    if (g_savedOutputCP != 0) {
        SetConsoleOutputCP(g_savedOutputCP);
    }
    if (g_savedInputCP != 0) {
        SetConsoleCP(g_savedInputCP);
    }

    if (g_haveOutputMode) {
        HANDLE hOut = GetStdHandle(STD_OUTPUT_HANDLE);
        if (hOut != INVALID_HANDLE_VALUE && hOut != nullptr) {
            SetConsoleMode(hOut, g_savedOutputMode);
        }
    }
    if (g_haveInputMode) {
        HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
        if (hIn != INVALID_HANDLE_VALUE && hIn != nullptr) {
            SetConsoleMode(hIn, g_savedInputMode);
        }
    }
}

#else /* !Q_OS_WIN */

void QSocWinConsole::bootstrap() {}
void QSocWinConsole::restore() {}

#endif /* Q_OS_WIN */
