// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuipathpicker.h"

#include "tui/qtuiwidget.h"

#include <cstdio>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace {

constexpr int BG_NORMAL    = 237;
constexpr int BG_HIGHLIGHT = 239;
constexpr int FG_TITLE     = 214;
constexpr int FG_NORMAL    = 223;
constexpr int FG_DIM       = 246;
constexpr int FG_HIGHLIGHT = 214;

constexpr int MIN_COL_WIDTH = 24;

QString parentOf(const QString &path)
{
    if (path.isEmpty() || path == QStringLiteral("/")) {
        return QStringLiteral("/");
    }
    QString clean = path;
    while (clean.size() > 1 && clean.endsWith(QLatin1Char('/'))) {
        clean.chop(1);
    }
    const int slash = clean.lastIndexOf(QLatin1Char('/'));
    if (slash <= 0) {
        return QStringLiteral("/");
    }
    return clean.left(slash);
}

QString joinPath(const QString &base, const QString &child)
{
    if (base.endsWith(QLatin1Char('/'))) {
        return base + child;
    }
    return base + QLatin1Char('/') + child;
}

} // namespace

void QTuiPathPicker::setTitle(const QString &title)
{
    m_title = title;
}

void QTuiPathPicker::setStartPath(const QString &path)
{
    m_startPath = path;
}

void QTuiPathPicker::setListDirs(ListDirsFn listDirs)
{
    m_listDirs = std::move(listDirs);
}

QString QTuiPathPicker::exec()
{
    if (!m_listDirs) {
        return QString();
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

    QString currentPath = m_startPath.isEmpty() ? QStringLiteral("/") : m_startPath;

    /* Entries shown in the left column. kind discriminates the three
     * synthetic rows (parent, current-dir selector, real subdirectory). */
    enum class EntryKind : std::uint8_t { ParentUp, SelectCurrent, Subdir };
    struct Entry
    {
        EntryKind kind;
        QString   label;
        QString   path;
    };

    QList<Entry> leftEntries;
    int          leftHighlight = 0;
    QStringList  rightPreview;

    auto rebuildLeft = [&]() {
        leftEntries.clear();
        if (currentPath != QStringLiteral("/")) {
            leftEntries.append({EntryKind::ParentUp, QStringLiteral("<up>"), parentOf(currentPath)});
        }
        leftEntries.append({EntryKind::SelectCurrent, QStringLiteral("<current>"), currentPath});
        const QStringList subs = m_listDirs(currentPath);
        for (const QString &sub : subs) {
            if (sub.isEmpty() || sub == QStringLiteral(".") || sub == QStringLiteral("..")) {
                continue;
            }
            leftEntries.append(
                {EntryKind::Subdir, sub + QStringLiteral("/"), joinPath(currentPath, sub)});
        }
    };

    auto rebuildRight = [&]() {
        rightPreview.clear();
        if (leftHighlight < 0 || leftHighlight >= leftEntries.size()) {
            return;
        }
        const Entry &entry = leftEntries[leftHighlight];
        if (entry.kind != EntryKind::Subdir) {
            return;
        }
        const QStringList subs = m_listDirs(entry.path);
        for (const QString &sub : subs) {
            if (sub.isEmpty() || sub == QStringLiteral(".") || sub == QStringLiteral("..")) {
                continue;
            }
            rightPreview.append(sub + QStringLiteral("/"));
        }
    };

    rebuildLeft();
    rebuildRight();

    const int menuH    = qMax(6, termH - 5);
    const int bodyRows = menuH - 2;
    int       startY   = termH - 3 - menuH;
    if (startY < 1) {
        startY = 1;
    }

    const int gutter = 2;
    int       leftW  = qMax(MIN_COL_WIDTH, (termW - gutter) / 2);
    int       rightW = qMax(0, termW - leftW - gutter);
    if (rightW < MIN_COL_WIDTH) {
        rightW = 0;
    }

    int scrollOffset = 0;

    auto ensureVisible = [&]() {
        const int count   = leftEntries.size();
        const int visible = qMin(count, bodyRows);
        if (visible <= 0) {
            scrollOffset = 0;
            return;
        }
        if (leftHighlight < scrollOffset) {
            scrollOffset = leftHighlight;
        } else if (leftHighlight >= scrollOffset + visible) {
            scrollOffset = leftHighlight - visible + 1;
        }
        if (scrollOffset < 0) {
            scrollOffset = 0;
        }
        const int maxOffset = count - visible;
        if (scrollOffset > maxOffset) {
            scrollOffset = qMax(0, maxOffset);
        }
    };

    auto padTo = [](QString line, int width) {
        while (QTuiText::visualWidth(line) < width) {
            line.append(QLatin1Char(' '));
        }
        return line.left(width);
    };

    auto renderOverlay = [&]() {
        ensureVisible();

        for (int row = 0; row < menuH && (startY + row) < termH; row++) {
            fprintf(stdout, "\033[%d;1H\033[2K", startY + row + 1);
        }

        /* Title row shows current absolute path so the user always knows
         * where the view is anchored. */
        QString titleLine = QStringLiteral("  ") + m_title + QStringLiteral("  ") + currentPath;
        fprintf(stdout, "\033[%d;1H\033[38;5;%d;48;5;%dm", startY + 1, FG_TITLE, BG_NORMAL);
        fputs(padTo(titleLine, termW).toUtf8().constData(), stdout);
        fputs("\033[0m", stdout);

        /* Body rows: left column interactive, right column passive preview. */
        const int count   = leftEntries.size();
        const int visible = qMin(count, bodyRows);
        for (int row = 0; row < bodyRows; row++) {
            fprintf(stdout, "\033[%d;1H", startY + 2 + row);

            int     leftIdx = scrollOffset + row;
            QString leftCell;
            int     leftFg = FG_NORMAL;
            int     leftBg = BG_NORMAL;
            if (row < visible && leftIdx < count) {
                const Entry &entry = leftEntries[leftIdx];
                QString      label = entry.label;
                if (entry.kind == EntryKind::ParentUp) {
                    label = QStringLiteral("<up>  ..");
                } else if (entry.kind == EntryKind::SelectCurrent) {
                    label = QStringLiteral("<current>  .");
                }
                leftCell = QStringLiteral("  ") + label;
                if (leftIdx == leftHighlight) {
                    leftBg = BG_HIGHLIGHT;
                    leftFg = FG_HIGHLIGHT;
                }
            }
            leftCell = padTo(leftCell, leftW);
            fprintf(stdout, "\033[38;5;%d;48;5;%dm", leftFg, leftBg);
            fputs(leftCell.toUtf8().constData(), stdout);
            fputs("\033[0m", stdout);

            if (rightW <= 0) {
                continue;
            }

            /* Gutter between columns stays in normal background. */
            fprintf(stdout, "\033[38;5;%d;48;5;%dm", FG_DIM, BG_NORMAL);
            fputs(QString(gutter, QLatin1Char(' ')).toUtf8().constData(), stdout);

            QString rightCell;
            if (row < rightPreview.size()) {
                rightCell = QStringLiteral("  ") + rightPreview[row];
            }
            rightCell = padTo(rightCell, rightW);
            fprintf(stdout, "\033[38;5;%d;48;5;%dm", FG_DIM, BG_NORMAL);
            fputs(rightCell.toUtf8().constData(), stdout);
            fputs("\033[0m", stdout);
        }

        /* Footer row documents the navigation keys. */
        QString footer = QStringLiteral("  Enter/Right: open   Left: up   ESC: cancel");
        fprintf(stdout, "\033[%d;1H\033[38;5;%d;48;5;%dm", startY + menuH, FG_DIM, BG_NORMAL);
        fputs(padTo(footer, termW).toUtf8().constData(), stdout);
        fputs("\033[0m", stdout);

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
        struct termios pickerTerm = savedTerm;
        pickerTerm.c_cc[VMIN]     = 1;
        pickerTerm.c_cc[VTIME]    = 0;
        tcsetattr(STDIN_FILENO, TCSANOW, &pickerTerm);
    }
#endif

    QString result;
    bool    done = false;

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
                switch (rec.Event.KeyEvent.wVirtualKeyCode) {
                case VK_UP:
                    *out = 0;
                    return true;
                case VK_DOWN:
                    *out = 1;
                    return true;
                case VK_LEFT:
                    *out = 2;
                    return true;
                case VK_RIGHT:
                    *out = 3;
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
            struct timeval waitTime;
            waitTime.tv_sec  = timeoutMs / 1000;
            waitTime.tv_usec = (timeoutMs % 1000) * 1000;
            FD_ZERO(&fds);
            FD_SET(STDIN_FILENO, &fds);
            if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &waitTime) <= 0) {
                return false;
            }
        }
        return read(STDIN_FILENO, out, 1) == 1;
#endif
    };

    auto moveUp = [&]() {
        if (leftEntries.isEmpty()) {
            return;
        }
        leftHighlight = (leftHighlight <= 0) ? static_cast<int>(leftEntries.size()) - 1
                                             : leftHighlight - 1;
        rebuildRight();
    };
    auto moveDown = [&]() {
        if (leftEntries.isEmpty()) {
            return;
        }
        leftHighlight = (leftHighlight + 1) % static_cast<int>(leftEntries.size());
        rebuildRight();
    };

    auto descendTo = [&](const QString &path) {
        currentPath   = path;
        leftHighlight = 0;
        scrollOffset  = 0;
        rebuildLeft();
        rebuildRight();
    };

    auto goUp = [&]() {
        if (currentPath == QStringLiteral("/")) {
            return;
        }
        const QString oldName = currentPath.section(QLatin1Char('/'), -1);
        descendTo(parentOf(currentPath));
        /* Try to land the highlight on the directory we just came from
         * so Left feels like a reversible step. */
        for (int idx = 0; idx < leftEntries.size(); idx++) {
            if (leftEntries[idx].kind == EntryKind::Subdir
                && leftEntries[idx].label == oldName + QStringLiteral("/")) {
                leftHighlight = idx;
                rebuildRight();
                break;
            }
        }
    };

    auto activate = [&]() {
        if (leftHighlight < 0 || leftHighlight >= leftEntries.size()) {
            return;
        }
        const Entry &entry = leftEntries[leftHighlight];
        switch (entry.kind) {
        case EntryKind::ParentUp:
            goUp();
            return;
        case EntryKind::SelectCurrent:
            result = currentPath;
            done   = true;
            return;
        case EntryKind::Subdir:
            if (rightPreview.isEmpty()) {
                /* Terminal directory has no further children, select it. */
                result = entry.path;
                done   = true;
            } else {
                descendTo(entry.path);
            }
            return;
        }
    };

    while (!done) {
        char byte = 0;
        if (!readByte(&byte, -1)) {
            break;
        }

#ifdef Q_OS_WIN
        if (byte == 0) {
            moveUp();
            renderOverlay();
            continue;
        }
        if (byte == 1) {
            moveDown();
            renderOverlay();
            continue;
        }
        if (byte == 2) {
            goUp();
            renderOverlay();
            continue;
        }
        if (byte == 3) {
            activate();
            renderOverlay();
            continue;
        }
#endif

        if (byte == 0x1B) {
            char seq[2] = {};
            if (readByte(&seq[0], 50) && seq[0] == '[') {
                if (readByte(&seq[1], 50)) {
                    if (seq[1] == 'A') {
                        moveUp();
                        renderOverlay();
                    } else if (seq[1] == 'B') {
                        moveDown();
                        renderOverlay();
                    } else if (seq[1] == 'C') {
                        activate();
                        if (!done) {
                            renderOverlay();
                        }
                    } else if (seq[1] == 'D') {
                        goUp();
                        renderOverlay();
                    }
                }
            } else {
                done = true;
            }
        } else if (byte == 0x0D || byte == 0x0A) {
            activate();
            if (!done) {
                renderOverlay();
            }
        } else if (byte == 0x03) {
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
