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

int QTuiMenu::lineCount() const
{
    return static_cast<int>(items.size()) + 2;
}

void QTuiMenu::render(QTuiScreen &screen, int startY, int width)
{
    int boxWidth = qMin(computeBoxWidth(), width);

    /* Top border */
    QString topLine = "+- " + title + " ";
    while (QTuiText::visualWidth(topLine) < boxWidth - 1) {
        topLine += "-";
    }
    topLine += "+";
    screen.putString(0, startY, topLine.left(width), false, true);

    /* Items */
    for (int idx = 0; idx < items.size(); idx++) {
        const MenuItem &item    = items[idx];
        QString         num     = QString("[%1]").arg(idx + 1);
        QString         mark    = item.marked ? "*" : " ";
        QString         content = QString("| %1 %2 %3 %4").arg(num, mark, item.label, item.hint);

        while (QTuiText::visualWidth(content) < boxWidth - 1) {
            content += " ";
        }
        content += "|";

        bool inv = (idx == highlighted && colorEnabled);
        screen.putString(0, startY + 1 + idx, content.left(width), false, false, inv);
    }

    /* Bottom border */
    QString hint    = QString("1-%1/Up/Down+Enter/ESC").arg(items.size());
    QString botLine = "+- " + hint + " ";
    while (QTuiText::visualWidth(botLine) < boxWidth - 1) {
        botLine += "-";
    }
    botLine += "+";
    screen
        .putString(0, startY + 1 + static_cast<int>(items.size()), botLine.left(width), false, true);
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
    int maxWidth = QTuiText::visualWidth(title) + 6;
    for (const auto &item : items) {
        int itemW = QTuiText::visualWidth(item.label) + QTuiText::visualWidth(item.hint) + 12;
        maxWidth  = qMax(maxWidth, itemW);
    }
    return maxWidth + 4;
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

    /* Center menu vertically */
    int menuH  = lineCount();
    int startY = (termH - menuH) / 2;
    if (startY < 0) {
        startY = 0;
    }

    /* Overlay rendering: render menu rows directly onto current screen.
     * Only overwrite the rows the menu occupies, preserving the rest. */
    auto renderOverlay = [&]() {
        /* Position cursor at menu start and render each row */
        for (int row = 0; row < menuH && (startY + row) < termH; row++) {
            /* Move to row, clear it, then draw menu line */
            fprintf(stdout, "\033[%d;1H\033[2K", startY + row + 1);
        }

        /* Render menu to a temp screen buffer then output just the menu rows */
        QTuiScreen overlay(termW, menuH);
        render(overlay, 0, termW);

        for (int row = 0; row < menuH; row++) {
            fprintf(stdout, "\033[%d;1H", startY + row + 1);

            bool curBold = false;
            bool curDim  = false;
            bool curInv  = false;

            for (int col = 0; col < termW; col++) {
                const QTuiCell &cell = overlay.at(col, row);

                if (cell.bold != curBold || cell.dim != curDim || cell.inverted != curInv) {
                    fputs("\033[0m", stdout);
                    curBold = false;
                    curDim  = false;
                    curInv  = false;
                    QString attrs;
                    if (cell.bold) {
                        attrs += "1;";
                    }
                    if (cell.dim) {
                        attrs += "2;";
                    }
                    if (cell.inverted) {
                        attrs += "7;";
                    }
                    if (!attrs.isEmpty()) {
                        attrs.chop(1);
                        fprintf(stdout, "\033[%sm", attrs.toUtf8().constData());
                    }
                    curBold = cell.bold;
                    curDim  = cell.dim;
                    curInv  = cell.inverted;
                }
                fputc(cell.character.toLatin1(), stdout);
            }
            if (curBold || curDim || curInv) {
                fputs("\033[0m", stdout);
            }
        }
        fputs("\033[?25l", stdout); /* Hide cursor during menu */
        fflush(stdout);
    };

    renderOverlay();

    /* We're already in raw mode (inputMonitor owns terminal).
     * Use blocking read with VMIN=1 for menu interaction. */
    struct termios savedTerm = {};
    tcgetattr(STDIN_FILENO, &savedTerm);
    {
        struct termios menuTerm = savedTerm;
        menuTerm.c_cc[VMIN]     = 1; /* Blocking read */
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
            /* Check if more bytes follow within 50ms (CSI sequence vs bare ESC) */
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
                    /* Consume mouse and other CSI sequences silently */
                    if (seq[1] == '<') {
                        /* SGR mouse: read until M or m */
                        char mch = 0;
                        while (read(STDIN_FILENO, &mch, 1) == 1) {
                            if (mch == 'M' || mch == 'm') {
                                break;
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

    /* Restore terminal settings */
    tcsetattr(STDIN_FILENO, TCSANOW, &savedTerm);

    /* Don't clear — the compositor will redraw the whole screen on next render */
    fputs("\033[?25h", stdout);
    fflush(stdout);

    return result;
#endif
}
