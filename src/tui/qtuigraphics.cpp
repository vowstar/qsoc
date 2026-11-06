// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuigraphics.h"

#include <QProcess>
#include <QRegularExpression>
#include <QStringList>

#include <algorithm>

#ifndef Q_OS_WIN
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

#include <utility>

namespace QTuiGraphics {

namespace {

ProbeReply storedReply;

/* Probe queries are hidden on terminals that print what they cannot parse. */
constexpr char kProbeGuardBegin[] = "\x1b"
                                    "7\x1b[?7l\x1b[8m";
constexpr char kProbeGuardEnd[]   = "\r\x1b[K\x1b[28m\x1b[?7h\x1b"
                                    "8";
constexpr char kProbeQueries[]    = "\x1b[16t\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\\x1b[>q"
                                    "\x1b]11;?\x1b\\\x1b[c";
/* A multiplexer answers these itself, and color is all it can tell. */
constexpr char kMultiplexedQueries[] = "\x1b]11;?\x1b\\\x1b[c";
/* The limit a silent terminal adds to startup. A reply later than this falls
 * back to the environment, and the input parser drops it. */
constexpr int kProbeTimeoutMs = 300;
/* Title, metadata, status, separator, and input lines stay visible. */
constexpr int kReservedRows = 5;

bool isMultiplexed(const QProcessEnvironment &env)
{
    return env.contains(QStringLiteral("TMUX")) || env.contains(QStringLiteral("STY"))
           || env.contains(QStringLiteral("ZELLIJ"));
}

Protocol terminalNameProtocol(const QString &name)
{
    const QString lower = name.toLower();
    /* WezTerm keeps kitty images in text cells, so the next write erases them. */
    if (lower.contains(QStringLiteral("iterm2")) || lower.contains(QStringLiteral("wezterm")))
        return Protocol::ITerm2;
    if (lower.contains(QStringLiteral("kitty")) || lower.contains(QStringLiteral("ghostty")))
        return Protocol::Kitty;
    return Protocol::None;
}

Protocol envIdentityProtocol(const QProcessEnvironment &env)
{
    const QString term        = env.value(QStringLiteral("TERM")).toLower();
    const QString termProgram = env.value(QStringLiteral("TERM_PROGRAM")).toLower();
    if (env.contains(QStringLiteral("KITTY_WINDOW_ID"))
        || env.contains(QStringLiteral("GHOSTTY_RESOURCES_DIR"))
        || env.contains(QStringLiteral("KONSOLE_VERSION")) || term == QStringLiteral("xterm-kitty")
        || term == QStringLiteral("xterm-ghostty") || termProgram == QStringLiteral("ghostty")
        || termProgram == QStringLiteral("rio"))
        return Protocol::Kitty;
    if (env.contains(QStringLiteral("WEZTERM_EXECUTABLE"))
        || termProgram == QStringLiteral("wezterm") || termProgram == QStringLiteral("iterm.app")
        || termProgram == QStringLiteral("mintty") || term == QStringLiteral("mintty")
        || env.value(QStringLiteral("LC_TERMINAL"))
                   .compare(QStringLiteral("iTerm2"), Qt::CaseInsensitive)
               == 0)
        return Protocol::ITerm2;
    return Protocol::None;
}

QString runTmux(const QStringList &arguments)
{
    QProcess process;
    process.start(QStringLiteral("tmux"), arguments);
    return process.waitForFinished(500) ? QString::fromUtf8(process.readAllStandardOutput())
                                        : QString();
}

/* tmux answers queries for its own emulator. A session can have several clients. */
void askTmuxAboutClient(ProbeReply *reply)
{
    const bool drawsSixel = reply->sixel;
    reply->kitty          = false;
    reply->sixel          = false;
    const auto env        = QProcessEnvironment::systemEnvironment();
    if (!env.contains(QStringLiteral("TMUX")))
        return;
    QStringList target;
    if (const QString pane = env.value(QStringLiteral("TMUX_PANE")); !pane.isEmpty())
        target = {QStringLiteral("-t"), pane};
    const QStringList clients
        = runTmux(
              QStringList{QStringLiteral("list-clients")} + target
              + QStringList{QStringLiteral("-F"), QStringLiteral("#{client_name}\t#{client_termtype}\t#{client_termname}")})
              .split(QLatin1Char('\n'), Qt::SkipEmptyParts);
    const QString passthrough
        = runTmux(
              QStringList{QStringLiteral("show-options"), QStringLiteral("-Apqv")} + target
              + QStringList{QStringLiteral("allow-passthrough")})
              .trimmed();
    reply->kitty = (passthrough == QStringLiteral("on") || passthrough == QStringLiteral("all"))
                   && std::any_of(clients.cbegin(), clients.cend(), [](const QString &client) {
                          return terminalNameProtocol(client.section(QLatin1Char('\t'), 1))
                                 == Protocol::Kitty;
                      });
    if (reply->kitty || !drawsSixel)
        return;
    static const QRegularExpression sixelCapability(QStringLiteral(R"(\d+: Sxl: \(flag\) true)"));
    for (const QString &client : clients) {
        if (sixelCapability
                .match(runTmux(
                    {QStringLiteral("show-messages"),
                     QStringLiteral("-T"),
                     QStringLiteral("-t"),
                     client.section(QLatin1Char('\t'), 0, 0)}))
                .hasMatch()) {
            reply->sixel = true;
            break;
        }
    }
}

} // namespace

ProbeReply parseProbeReply(const QByteArray &bytes)
{
    ProbeReply reply;
    qsizetype  pos = 0;
    while (pos < bytes.size()) {
        if (bytes[pos] != '\x1b' || pos + 1 >= bytes.size()) {
            reply.typeAhead.append(bytes[pos++]);
            continue;
        }
        const char kind = bytes[pos + 1];
        if (kind == '_' || kind == 'P') {
            const qsizetype end = bytes.indexOf("\x1b\\", pos + 2);
            if (end < 0)
                break;
            const QByteArray body = bytes.mid(pos + 2, end - pos - 2);
            if (kind == '_' && body.startsWith("Gi=31;") && body.contains("OK"))
                reply.kitty = true;
            if (kind == 'P' && body.startsWith(">|"))
                reply.terminal = QString::fromUtf8(body.mid(2));
            pos = end + 2;
            continue;
        }
        if (kind == ']') {
            qsizetype       end = bytes.indexOf('\a', pos + 2);
            const qsizetype st  = bytes.indexOf("\x1b\\", pos + 2);
            if (end < 0 || (st >= 0 && st < end))
                end = st;
            if (end < 0)
                break;
            const QByteArray body = bytes.mid(pos + 2, end - pos - 2);
            if (body.startsWith("11;rgb:")) {
                const auto parts = body.mid(7).split('/');
                if (parts.size() == 3) {
                    const auto channel = [&parts](int index) {
                        return parts[index].left(2).toInt(nullptr, 16);
                    };
                    reply.light = 2 * channel(0) + 5 * channel(1) + channel(2) > 8 * 128;
                }
            }
            pos = end + (bytes[end] == '\a' ? 1 : 2);
            continue;
        }
        if (kind == '\x1b') {
            reply.typeAhead.append(bytes[pos++]);
            continue;
        }
        if (kind != '[') {
            reply.typeAhead.append(bytes.mid(pos, 2));
            pos += 2;
            continue;
        }
        qsizetype end = pos + 2;
        while (end < bytes.size() && (bytes[end] < 0x40 || bytes[end] > 0x7e))
            ++end;
        if (end >= bytes.size())
            break;
        const QByteArray body  = bytes.mid(pos + 2, end - pos - 2);
        const char       final = bytes[end];
        if (final == 'c' && body.startsWith('?')) {
            reply.answered = true;
            reply.sixel    = body.mid(1).split(';').contains("4");
        } else if (final == 't' && body.startsWith("6;")) {
            const auto parts = body.split(';');
            if (parts.size() == 3)
                reply.cell = QSize(parts[2].toInt(), parts[1].toInt());
        } else {
            reply.typeAhead.append(bytes.mid(pos, end - pos + 1));
        }
        pos = end + 1;
    }
    return reply;
}

Protocol selectProtocol(const QProcessEnvironment &env, const ProbeReply &reply)
{
    const QString forced = env.value(QStringLiteral("QSOC_IMAGE_PROTOCOL")).trimmed().toLower();
    if (!env.value(QStringLiteral("QSOC_NO_IMAGE_GRAPHICS")).isEmpty()
        || forced == QStringLiteral("none"))
        return Protocol::None;
    if (forced == QStringLiteral("kitty"))
        return Protocol::Kitty;
    if (forced == QStringLiteral("iterm2"))
        return Protocol::ITerm2;
    if (forced == QStringLiteral("sixel"))
        return Protocol::Sixel;
    if (forced == QStringLiteral("cells"))
        return Protocol::Cells;

    const QString term = env.value(QStringLiteral("TERM")).toLower();
    /* Windows Terminal draws cells but leaves TERM empty. */
    const bool     noCells = (term.isEmpty() && !env.contains(QStringLiteral("WT_SESSION")))
                             || term == QStringLiteral("dumb");
    const Protocol basic   = noCells ? Protocol::None : Protocol::Cells;
    /* A multiplexer answers for itself and owns the grid, so outer identity
     * variables that leak into the pane prove nothing. */
    if (isMultiplexed(env)) {
        if (reply.kitty)
            return Protocol::Kitty;
        return reply.sixel ? Protocol::Sixel : basic;
    }
    const Protocol named = terminalNameProtocol(reply.terminal);
    if (named != Protocol::None)
        return named;
    if (reply.kitty)
        return Protocol::Kitty;
    const Protocol identity = envIdentityProtocol(env);
    if (identity != Protocol::None)
        return identity;
    return reply.sixel ? Protocol::Sixel : basic;
}

void probeTerminal()
{
#ifndef Q_OS_WIN
    const auto env = QProcessEnvironment::systemEnvironment();
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || selectProtocol(env, {}) == Protocol::None)
        return;
    const bool multiplexed = isMultiplexed(env);
    termios    saved{};
    if (tcgetattr(STDIN_FILENO, &saved) != 0)
        return;
    termios raw = saved;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
        return;

    const QByteArray queries = QByteArray(kProbeGuardBegin)
                               + (multiplexed ? kMultiplexedQueries : kProbeQueries)
                               + kProbeGuardEnd;
    if (write(STDOUT_FILENO, queries.constData(), size_t(queries.size())) == queries.size()) {
        QByteArray bytes;
        ProbeReply reply;
        for (int waited = 0; waited < kProbeTimeoutMs && !reply.answered; waited += 10) {
            fd_set readable;
            FD_ZERO(&readable);
            FD_SET(STDIN_FILENO, &readable);
            timeval tick{0, 10000};
            if (select(STDIN_FILENO + 1, &readable, nullptr, nullptr, &tick) <= 0)
                continue;
            char          chunk[4096];
            const ssize_t got = read(STDIN_FILENO, chunk, sizeof(chunk));
            if (got <= 0)
                continue;
            bytes.append(chunk, got);
            reply = parseProbeReply(bytes);
        }
        storedReply = std::move(reply);
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    if (multiplexed)
        askTmuxAboutClient(&storedReply);
#endif
}

void setProbeReply(const ProbeReply &reply)
{
    storedReply = reply;
}

QByteArray takeTypeAhead()
{
    return std::exchange(storedReply.typeAhead, {});
}

bool isInlineBitmap(Protocol protocol)
{
    return protocol == Protocol::ITerm2 || protocol == Protocol::Sixel;
}

bool usesPlaceholders()
{
    return protocol() == Protocol::Kitty && isMultiplexed(QProcessEnvironment::systemEnvironment());
}

QString passthrough(const QString &sequence)
{
    if (!qEnvironmentVariableIsSet("TMUX"))
        return sequence;
    QString doubled = sequence;
    doubled.replace(QLatin1Char('\x1b'), QStringLiteral("\x1b\x1b"));
    return QStringLiteral("\x1bPtmux;") + doubled + QStringLiteral("\x1b\\");
}

QByteArray encodeSixel(const QImage &image)
{
    const QImage indexed = image.convertToFormat(QImage::Format_ARGB32)
                               .convertToFormat(QImage::Format_Indexed8, Qt::AvoidDither);
    const int    width   = indexed.width();
    const int    height  = indexed.height();
    const auto   table   = indexed.colorTable();
    QByteArray   out = QStringLiteral("\x1bP0;1;0q\"1;1;%1;%2").arg(width).arg(height).toLatin1();
    for (int index = 0; index < table.size(); ++index) {
        out += QStringLiteral("#%1;2;%2;%3;%4")
                   .arg(index)
                   .arg(qRed(table[index]) * 100 / 255)
                   .arg(qGreen(table[index]) * 100 / 255)
                   .arg(qBlue(table[index]) * 100 / 255)
                   .toLatin1();
    }
    /* P2=1 leaves pixels no register draws transparent. */
    QList<QByteArray> columns(table.size());
    for (int top = 0; top < height; top += 6) {
        QList<int> used;
        for (int row = top; row < qMin(top + 6, height); ++row) {
            const uchar *line = indexed.constScanLine(row);
            for (int col = 0; col < width; ++col) {
                const int color = line[col];
                if (qAlpha(table[color]) < 128)
                    continue;
                if (columns[color].isEmpty()) {
                    columns[color] = QByteArray(width, '\0');
                    used.append(color);
                }
                columns[color][col] = char(columns[color][col] | (1 << (row - top)));
            }
        }
        std::sort(used.begin(), used.end());
        for (qsizetype index = 0; index < used.size(); ++index) {
            if (index > 0)
                out += '$';
            out += '#' + QByteArray::number(used[index]);
            const QByteArray &bits = columns[used[index]];
            for (int col = 0; col < width;) {
                int run = 1;
                while (col + run < width && bits[col + run] == bits[col])
                    ++run;
                const char symbol = char(63 + bits[col]);
                out += run > 3 ? '!' + QByteArray::number(run) + symbol : QByteArray(run, symbol);
                col += run;
            }
            columns[used[index]].clear();
        }
        out += '-';
    }
    out += "\x1b\\";
    return out;
}

Protocol protocol()
{
    return selectProtocol(QProcessEnvironment::systemEnvironment(), storedReply);
}

bool hasLightBackground()
{
    return storedReply.light;
}

bool hasTrueColor()
{
    const QByteArray colorTerm = qgetenv("COLORTERM").toLower();
    return colorTerm == "truecolor" || colorTerm == "24bit";
}

int maxImageRows()
{
    constexpr int kMaxRows = 30;
#ifndef Q_OS_WIN
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_row > 0)
        return qBound(1, int(size.ws_row) - kReservedRows, kMaxRows);
#endif
    return kMaxRows;
}

QSize cellSize()
{
#ifndef Q_OS_WIN
    winsize size{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0 && size.ws_row > 0
        && size.ws_xpixel >= size.ws_col && size.ws_ypixel >= size.ws_row)
        return {size.ws_xpixel / size.ws_col, size.ws_ypixel / size.ws_row};
#endif
    return storedReply.cell.isValid() && !storedReply.cell.isEmpty() ? storedReply.cell
                                                                     : QSize(8, 16);
}

ImageStore &ImageStore::instance()
{
    static ImageStore store;
    return store;
}

quint32 ImageStore::allocateId()
{
    return nextId++;
}

bool ImageStore::isResident(quint32 id) const
{
    return std::any_of(residents.cbegin(), residents.cend(), [id](const Resident &resident) {
        return resident.id == id;
    });
}

QList<quint32> ImageStore::admit(quint32 id, qint64 bytes)
{
    forget(id);
    QList<quint32> evicted;
    for (qsizetype index = 0; index < residents.size() && residentBytes + bytes > limit;) {
        if (placedIds.contains(residents[index].id)) {
            ++index;
            continue;
        }
        residentBytes -= residents[index].bytes;
        evicted.append(residents[index].id);
        residents.removeAt(index);
    }
    residents.append({id, bytes});
    residentBytes += bytes;
    return evicted;
}

void ImageStore::setPlaced(quint32 id, bool placed)
{
    if (!placed) {
        placedIds.remove(id);
        return;
    }
    placedIds.insert(id);
    for (qsizetype index = 0; index < residents.size(); ++index) {
        if (residents[index].id == id) {
            residents.move(index, residents.size() - 1);
            return;
        }
    }
}

void ImageStore::forget(quint32 id)
{
    placedIds.remove(id);
    for (qsizetype index = 0; index < residents.size(); ++index) {
        if (residents[index].id == id) {
            residentBytes -= residents[index].bytes;
            residents.removeAt(index);
            return;
        }
    }
}

} // namespace QTuiGraphics
