// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuimenu.h"

#include <cstdio>

#ifndef Q_OS_WIN
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
#ifdef Q_OS_WIN
    return -1;
#else
    if (items.isEmpty()) {
        return -1;
    }

    int termW = 80;
    int termH = 24;
    {
        struct winsize winsz = {};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &winsz) == 0) {
            if (winsz.ws_col > 0) {
                termW = winsz.ws_col;
            }
            if (winsz.ws_row > 0) {
                termH = winsz.ws_row;
            }
        }
    }

    int menuH  = lineCount();
    int startY = termH - 3 - menuH;
    if (startY < 1) {
        startY = 1;
    }

    /* Render overlay using 256-palette background colours directly. */
    auto renderOverlay = [&]() {
        int menuW = qMin(computeBoxWidth(), termW);

        for (int row = 0; row < menuH && (startY + row) < termH; row++) {
            fprintf(stdout, "\033[%d;1H\033[2K", startY + row + 1);
        }

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
                int itemIdx = row - 1;
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
            } else if (row == menuH - 1) {
                line = QString("  1-%1/Up/Down+Enter/Click/ESC").arg(items.size());
            } else {
                int             itemIdx = row - 1;
                const MenuItem &item    = items[itemIdx];
                line                    = QString("  %1 %2").arg(itemIdx + 1, 2).arg(item.label);
                if (!item.hint.isEmpty()) {
                    line += QStringLiteral(" ") + item.hint;
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

    struct termios savedTerm = {};
    tcgetattr(STDIN_FILENO, &savedTerm);
    {
        struct termios menuTerm = savedTerm;
        menuTerm.c_cc[VMIN]     = 1;
        menuTerm.c_cc[VTIME]    = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &menuTerm);
    }

    int  result = -1;
    bool done   = false;

    while (!done) {
        char byte = 0;
        if (read(STDIN_FILENO, &byte, 1) != 1) {
            break;
        }

        if (byte == 0x1B) {
            fd_set         fds;
            struct timeval tv = {.tv_sec = 0, .tv_usec = 50000};
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);

            char seq[2] = {};
            if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0
                && read(STDIN_FILENO, &seq[0], 1) == 1 && seq[0] == '[') {
                if (read(STDIN_FILENO, &seq[1], 1) == 1) {
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
                        while (read(STDIN_FILENO, &mch, 1) == 1) {
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
                                int clickedItem = mouseRow - startY - 1; /* 0-based */
                                if (clickedItem >= 0
                                    && clickedItem < static_cast<int>(items.size())) {
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

    tcsetattr(STDIN_FILENO, TCSANOW, &savedTerm);
    fputs("\033[?25h", stdout);
    fflush(stdout);

    return result;
#endif
}
