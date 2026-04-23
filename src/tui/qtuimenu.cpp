// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuimenu.h"

#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

/* 256-palette colours used for the menu overlay. */
static constexpr int BG_NORMAL    = 237; /* dark warm gray  — menu background */
static constexpr int BG_HIGHLIGHT = 239; /* lighter gray    — selected row */
static constexpr int FG_TITLE     = 214; /* amber           — title text */
static constexpr int FG_NORMAL    = 223; /* warm cream      — item text */
static constexpr int FG_HINT      = 246; /* neutral gray    — hint + footer */
static constexpr int FG_HIGHLIGHT = 214; /* amber           — selected item */

int QTuiMenu::lineCount() const
{
    /* title + items + footer */
    return static_cast<int>(items.size()) + 2;
}

void QTuiMenu::render(QTuiScreen &screen, int startY, int width)
{
    int menuW = qMin(computeBoxWidth(), width);

    /* Title row */
    QString titleLine = QStringLiteral("  ") + title;
    while (QTuiText::visualWidth(titleLine) < menuW) {
        titleLine += QLatin1Char(' ');
    }
    screen.putString(
        0,
        startY,
        titleLine.left(width),
        true,
        false,
        false,
        QTuiFgColor::Yellow,
        static_cast<QTuiBgColor>(BG_NORMAL));

    /* Items */
    for (int idx = 0; idx < items.size(); idx++) {
        const MenuItem &item = items[idx];
        QString         num  = QString("  %1 ").arg(idx + 1, 2);
        QString         body = item.label;
        if (!item.hint.isEmpty()) {
            body += QStringLiteral(" ") + item.hint;
        }
        QString line = num + body;
        while (QTuiText::visualWidth(line) < menuW) {
            line += QLatin1Char(' ');
        }
        bool        isHL  = (idx == highlighted && colorEnabled);
        QTuiFgColor fgCol = isHL ? static_cast<QTuiFgColor>(FG_HIGHLIGHT)
                                 : static_cast<QTuiFgColor>(FG_NORMAL);
        auto        bgCol = static_cast<QTuiBgColor>(isHL ? BG_HIGHLIGHT : BG_NORMAL);
        screen.putString(0, startY + 1 + idx, line.left(width), isHL, false, false, fgCol, bgCol);
    }

    /* Footer */
    QString hint = QString("  1-%1/Up/Down+Enter/Click/ESC").arg(items.size());
    while (QTuiText::visualWidth(hint) < menuW) {
        hint += QLatin1Char(' ');
    }
    screen.putString(
        0,
        startY + 1 + static_cast<int>(items.size()),
        hint.left(width),
        false,
        true,
        false,
        QTuiFgColor::Gray,
        static_cast<QTuiBgColor>(BG_NORMAL));
}

void QTuiMenu::setTitle(const QString &newTitle)
{
    title = newTitle;
}
void QTuiMenu::setItems(const QList<MenuItem> &newItems)
{
    items = newItems;
}
void QTuiMenu::setHighlight(int index)
{
    highlighted = index;
}
void QTuiMenu::setColorEnabled(bool enabled)
{
    colorEnabled = enabled;
}

int QTuiMenu::computeBoxWidth() const
{
    int maxWidth = QTuiText::visualWidth(title) + 4;
    for (const auto &item : items) {
        int itemW = QTuiText::visualWidth(item.label) + QTuiText::visualWidth(item.hint) + 8;
        maxWidth  = qMax(maxWidth, itemW);
    }
    return maxWidth;
}

int QTuiMenu::exec()
{
    if (items.isEmpty()) {
        return -1;
    }

    int termW = 80;
    int termH = 24;
    {
#ifdef Q_OS_WIN
        CONSOLE_SCREEN_BUFFER_INFO csbi;
        if (GetConsoleScreenBufferInfo(GetStdHandle(STD_OUTPUT_HANDLE), &csbi)) {
            termW = csbi.srWindow.Right - csbi.srWindow.Left + 1;
            termH = csbi.srWindow.Bottom - csbi.srWindow.Top + 1;
        }
#else
        struct winsize winsz = {};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &winsz) == 0) {
            if (winsz.ws_col > 0) {
                termW = winsz.ws_col;
            }
            if (winsz.ws_row > 0) {
                termH = winsz.ws_row;
            }
        }
#endif
    }

    /* Clamp item rows so the menu fits on screen; remaining items scroll into
     * view as the highlight moves past the viewport edge. Reserve 5 rows for
     * the input line, status bar, and title/footer of the menu itself. */
    const int itemsTotal      = static_cast<int>(items.size());
    const int maxVisibleItems = qMax(1, termH - 5);
    const int visibleCount    = qMin(itemsTotal, maxVisibleItems);
    const int menuH           = visibleCount + 2;
    int       startY          = termH - 3 - menuH;
    if (startY < 1) {
        startY = 1;
    }

    int scrollOffset = 0;

    auto ensureHighlightVisible = [&]() {
        if (highlighted < scrollOffset) {
            scrollOffset = highlighted;
        } else if (highlighted >= scrollOffset + visibleCount) {
            scrollOffset = highlighted - visibleCount + 1;
        }
        if (scrollOffset < 0) {
            scrollOffset = 0;
        }
        if (scrollOffset > itemsTotal - visibleCount) {
            scrollOffset = qMax(0, itemsTotal - visibleCount);
        }
    };

    /* Render overlay using 256-palette background colours directly. */
    auto renderOverlay = [&]() {
        ensureHighlightVisible();
        int menuW = qMin(computeBoxWidth(), termW);

        for (int row = 0; row < menuH && (startY + row) < termH; row++) {
            fprintf(stdout, "\033[%d;1H\033[2K", startY + row + 1);
        }

        const bool scrollable = itemsTotal > visibleCount;

        for (int row = 0; row < menuH; row++) {
            fprintf(stdout, "\033[%d;1H", startY + row + 1);

            int bgColor = BG_NORMAL;
            int fgColor = FG_NORMAL;

            if (row == 0) {
                /* Title */
                fgColor = FG_TITLE;
            } else if (row == menuH - 1) {
                /* Footer */
                fgColor = FG_HINT;
            } else {
                /* Item row */
                int itemIdx = scrollOffset + row - 1;
                if (itemIdx == highlighted) {
                    bgColor = BG_HIGHLIGHT;
                    fgColor = FG_HIGHLIGHT;
                }
            }

            fprintf(stdout, "\033[38;5;%d;48;5;%dm", fgColor, bgColor);

            /* Build the line content. */
            QString line;
            if (row == 0) {
                line = QStringLiteral("  ") + title;
                if (scrollable) {
                    line += QString(" [%1/%2]").arg(highlighted + 1).arg(itemsTotal);
                }
            } else if (row == menuH - 1) {
                if (scrollable) {
                    line = QStringLiteral("  Up/Down/Wheel+Enter/ESC");
                } else {
                    line = QString("  1-%1/Up/Down+Enter/Click/ESC").arg(items.size());
                }
            } else {
                const int itemIdx = scrollOffset + row - 1;
                if (itemIdx < 0 || itemIdx >= itemsTotal) {
                    line = QString();
                } else {
                    const MenuItem &item = items[itemIdx];
                    line                 = QString("  %1 %2").arg(itemIdx + 1, 2).arg(item.label);
                    if (!item.hint.isEmpty()) {
                        line += QStringLiteral(" ") + item.hint;
                    }
                }
            }

            /* Pad to menu width and print. */
            while (QTuiText::visualWidth(line) < menuW) {
                line += QLatin1Char(' ');
            }
            fputs(line.left(termW).toUtf8().constData(), stdout);
            fputs("\033[0m", stdout);
        }
        fputs("\033[?25l", stdout);
        fflush(stdout);
    };

    renderOverlay();

#ifdef Q_OS_WIN
    HANDLE hStdin    = GetStdHandle(STD_INPUT_HANDLE);
    DWORD  savedMode = 0;
    GetConsoleMode(hStdin, &savedMode);
    SetConsoleMode(hStdin, ENABLE_VIRTUAL_TERMINAL_INPUT | ENABLE_WINDOW_INPUT);
#else
    struct termios savedTerm = {};
    tcgetattr(STDIN_FILENO, &savedTerm);
    {
        struct termios menuTerm = savedTerm;
        menuTerm.c_cc[VMIN]     = 1;
        menuTerm.c_cc[VTIME]    = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &menuTerm);
    }
#endif

    int  result = -1;
    bool done   = false;

    /* Platform-agnostic helper: read one byte with timeout */
    auto readByte = [&](char *out, int timeoutMs) -> bool {
#ifdef Q_OS_WIN
        if (WaitForSingleObject(hStdin, timeoutMs) != WAIT_OBJECT_0) {
            return false;
        }
        DWORD numEvents = 0;
        if (!GetNumberOfConsoleInputEvents(hStdin, &numEvents) || numEvents == 0) {
            return false;
        }
        INPUT_RECORD rec;
        DWORD        numRead = 0;
        while (ReadConsoleInput(hStdin, &rec, 1, &numRead) && numRead > 0) {
            if (rec.EventType == KEY_EVENT && rec.Event.KeyEvent.bKeyDown) {
                char ch = rec.Event.KeyEvent.uChar.AsciiChar;
                if (ch != 0) {
                    *out = ch;
                    return true;
                }
                /* Map virtual keys for arrow keys */
                switch (rec.Event.KeyEvent.wVirtualKeyCode) {
                case VK_UP:
                    *out = 0;
                    return true;
                case VK_DOWN:
                    *out = 1;
                    return true;
                }
            }
            if (!GetNumberOfConsoleInputEvents(hStdin, &numEvents) || numEvents == 0) {
                break;
            }
        }
        return false;
#else
        if (timeoutMs >= 0) {
            fd_set         fds;
            struct timeval tv = {.tv_sec = timeoutMs / 1000, .tv_usec = (timeoutMs % 1000) * 1000};
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) <= 0) {
                return false;
            }
        }
        return read(STDIN_FILENO, out, 1) == 1;
#endif
    };

    while (!done) {
        char byte = 0;
        if (!readByte(&byte, -1)) {
            break;
        }

#ifdef Q_OS_WIN
        /* Handle virtual key codes for arrow keys */
        if (byte == 0) { /* Up */
            highlighted = (highlighted <= 0) ? static_cast<int>(items.size()) - 1 : highlighted - 1;
            renderOverlay();
            continue;
        }
        if (byte == 1) { /* Down */
            highlighted = (highlighted + 1) % static_cast<int>(items.size());
            renderOverlay();
            continue;
        }
#endif

        if (byte == 0x1B) {
            char seq[2] = {};
            if (readByte(&seq[0], 50) && seq[0] == '[') {
                if (readByte(&seq[1], 50)) {
                    if (seq[1] == 'A') { /* Up */
                        highlighted = (highlighted <= 0) ? static_cast<int>(items.size()) - 1
                                                         : highlighted - 1;
                        renderOverlay();
                    } else if (seq[1] == 'B') { /* Down */
                        highlighted = (highlighted + 1) % static_cast<int>(items.size());
                        renderOverlay();
                    }
                    /* SGR mouse: ESC [ < params M/m */
                    if (seq[1] == '<') {
                        QByteArray params;
                        char       mch = 0;
                        while (readByte(&mch, 50)) {
                            if (mch == 'M' || mch == 'm') {
                                break;
                            }
                            params.append(mch);
                        }
                        /* Parse btn;col;row */
                        QList<QByteArray> parts = params.split(';');
                        if (parts.size() == 3 && mch == 'M') {
                            int btnFlags = parts[0].toInt();
                            int mouseRow = parts[2].toInt(); /* 1-based */
                            /* Left-click press on an item row → select */
                            if ((btnFlags & 3) == 0 && !(btnFlags & 64)) {
                                int visibleRow  = mouseRow - startY - 1; /* 0-based */
                                int clickedItem = scrollOffset + visibleRow;
                                if (visibleRow >= 0 && visibleRow < visibleCount && clickedItem >= 0
                                    && clickedItem < itemsTotal) {
                                    result = clickedItem;
                                    done   = true;
                                }
                            }
                            /* Scroll wheel */
                            if (btnFlags & 64) {
                                int dir = btnFlags & 1;
                                if (dir == 0) { /* up */
                                    highlighted = (highlighted <= 0)
                                                      ? static_cast<int>(items.size()) - 1
                                                      : highlighted - 1;
                                } else { /* down */
                                    highlighted = (highlighted + 1)
                                                  % static_cast<int>(items.size());
                                }
                                renderOverlay();
                            }
                        }
                    }
                }
            } else {
                done = true; /* Bare ESC = cancel */
            }
        } else if (byte == 0x0D || byte == 0x0A) {
            result = highlighted;
            done   = true;
        } else if (byte >= '1' && byte <= '9') {
            int idx = byte - '1';
            if (idx < items.size()) {
                result = idx;
            }
            done = true;
        } else if (byte == 'q' || byte == 0x03) {
            done = true;
        }
    }

#ifdef Q_OS_WIN
    SetConsoleMode(hStdin, savedMode);
#else
    tcsetattr(STDIN_FILENO, TCSANOW, &savedTerm);
#endif
    fputs("\033[?25h", stdout);
    fflush(stdout);

    return result;
}
