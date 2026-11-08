// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "tui/qtuigraphics.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QHash>
#include <QPointer>
#include <QProcess>
#include <QRegularExpression>
#include <QStringList>
#include <QTimer>

#include <algorithm>
#include <utility>

#ifndef Q_OS_WIN
#include <sys/ioctl.h>
#include <sys/select.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace QTuiGraphics {
namespace {

ProbeReply storedReply;
ProbeReply paneReply;
quint64    graphicsRevision = 0;
bool       probeStarted     = false;
QString    clientFingerprint;

enum class Multiplexer { None, Tmux, Screen, Zellij };

bool hasValue(const QProcessEnvironment &env, const char *key)
{
    return !env.value(QLatin1String(key)).isEmpty();
}

Multiplexer multiplexer(const QProcessEnvironment &env)
{
    const QString term = env.value(QStringLiteral("TERM")).toLower();
    if (hasValue(env, "TMUX"))
        return Multiplexer::Tmux;
    if (hasValue(env, "STY"))
        return Multiplexer::Screen;
    if (hasValue(env, "ZELLIJ"))
        return Multiplexer::Zellij;
    if (term.startsWith(QStringLiteral("tmux")))
        return Multiplexer::Tmux;
    return term.startsWith(QStringLiteral("screen")) ? Multiplexer::Screen : Multiplexer::None;
}

struct TerminalInfo
{
    Protocol preferred    = Protocol::None;
    Support  placeholders = Support::Unknown;
};

QString terminalName(const QString &name)
{
    return name.trimmed().toLower().section(QRegularExpression(QStringLiteral("[\\s(]")), 0, 0);
}

TerminalInfo terminalInfo(const QString &name)
{
    const QString id = terminalName(name);
    if (id == QStringLiteral("kitty") || id == QStringLiteral("xterm-kitty")
        || id == QStringLiteral("ghostty") || id == QStringLiteral("xterm-ghostty")
        || id == QStringLiteral("rio") || id == QStringLiteral("otty")
        || id == QStringLiteral("tern") || id == QStringLiteral("monstar"))
        return {Protocol::Kitty, Support::Yes};
    if (id == QStringLiteral("konsole"))
        return {Protocol::Kitty, Support::Unknown};
    /* WezTerm erases direct Kitty placements when their text cells are written. */
    if (id == QStringLiteral("wezterm") || id == QStringLiteral("iterm2")
        || id == QStringLiteral("iterm.app") || id == QStringLiteral("mintty"))
        return {Protocol::ITerm2, Support::No};
    return {};
}

TerminalInfo envIdentity(const QProcessEnvironment &env)
{
    for (const char *key : {"TERM_PROGRAM", "LC_TERMINAL", "TERM"}) {
        const TerminalInfo info = terminalInfo(env.value(QLatin1String(key)));
        if (info.preferred != Protocol::None)
            return info;
    }
    for (const auto &marker :
         {std::pair{"KITTY_WINDOW_ID", "kitty"},
          std::pair{"GHOSTTY_RESOURCES_DIR", "ghostty"},
          std::pair{"KONSOLE_VERSION", "konsole"},
          std::pair{"WEZTERM_EXECUTABLE", "wezterm"},
          std::pair{"WEZTERM_PANE", "wezterm"},
          std::pair{"ITERM_SESSION_ID", "iterm2"}}) {
        if (hasValue(env, marker.first))
            return terminalInfo(QLatin1String(marker.second));
    }
    return {};
}

struct CommandResult
{
    bool    ok = false;
    QString output;
};

CommandResult runTmux(const QStringList &arguments, int timeoutMs)
{
    if (timeoutMs <= 0)
        return {};
    QProcess process;
    process.start(QStringLiteral("tmux"), arguments);
    if (!process.waitForFinished(qMin(timeoutMs, 2000))) {
        process.kill();
        process.waitForFinished();
        return {};
    }
    return {
        process.exitStatus() == QProcess::NormalExit && process.exitCode() == 0,
        QString::fromUtf8(process.readAllStandardOutput())};
}

struct TmuxSnapshot
{
    QString     pane;
    QStringList clients;
    QString     passthrough;
    QString     version;
    bool        sixel = false;
};

QStringList relatedClients(const QString &panes, const QString &clients, const QString &pane)
{
    QHash<QString, QString> windows;
    for (const QString &line : panes.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        if (line.section(QLatin1Char('\t'), 0, 0) == pane)
            windows
                .insert(line.section(QLatin1Char('\t'), 1, 1), line.section(QLatin1Char('\t'), 2, 2));
    }
    QStringList related;
    for (const QString &line : clients.split(QLatin1Char('\n'), Qt::SkipEmptyParts)) {
        const QString session = line.section(QLatin1Char('\t'), 1, 1);
        if (!windows.contains(session)
            || windows.value(session) != line.section(QLatin1Char('\t'), 5, 5))
            continue;
        if (line.section(QLatin1Char('\t'), 7, 7) == QStringLiteral("1")
            && line.section(QLatin1Char('\t'), 6, 6) != pane)
            continue;
        related.append(line);
    }
    related.sort();
    return related;
}

QList<QStringList> tmuxCommands(const QString &pane)
{
    return {
        {QStringLiteral("list-panes"),
         QStringLiteral("-a"),
         QStringLiteral("-F"),
         QStringLiteral("#{pane_id}\t#{session_id}\t#{window_id}")},
        {QStringLiteral("list-clients"),
         QStringLiteral("-F"),
         QStringLiteral(
             "#{client_name}\t#{session_id}\t#{client_termtype}\t#{client_termname}\t#{client_pid}"
             "\t#{window_id}\t#{pane_id}\t#{window_zoomed_flag}\t#{client_utf8}")},
        {QStringLiteral("show-options"),
         QStringLiteral("-Apqv"),
         QStringLiteral("-t"),
         pane,
         QStringLiteral("allow-passthrough")}};
}

bool isNestedClient(const QString &client)
{
    const QString type = terminalName(client.section(QLatin1Char('\t'), 2, 2));
    const QString term = client.section(QLatin1Char('\t'), 3, 3).toLower();
    return type == QStringLiteral("tmux") || type == QStringLiteral("screen")
           || type == QStringLiteral("zellij") || term.startsWith(QStringLiteral("tmux"))
           || term.startsWith(QStringLiteral("screen"));
}

Support passthroughSupport(const TmuxSnapshot &snapshot)
{
    if (snapshot.passthrough == QStringLiteral("on")
        || snapshot.passthrough == QStringLiteral("all"))
        return Support::Yes;
    if (!snapshot.passthrough.isEmpty())
        return snapshot.passthrough == QStringLiteral("off") ? Support::No : Support::Unknown;
    static const QRegularExpression version(QStringLiteral(R"(^(\d+)\.(\d+))"));
    const auto                      match = version.match(snapshot.version);
    if (!match.hasMatch())
        return Support::Unknown;
    const int major = match.captured(1).toInt();
    return major >= 2 && (major < 3 || (major == 3 && match.captured(2).toInt() < 3))
               ? Support::Yes
               : Support::Unknown;
}

bool hasSixel(const QString &description)
{
    static const QRegularExpression capability(
        QStringLiteral(R"((?:^|\n)\s*\d+: Sxl: \(flag\) true(?:\n|$))"));
    return capability.match(description).hasMatch();
}

void publishSnapshot(const TmuxSnapshot &snapshot)
{
    ProbeReply reply   = paneReply;
    reply.kitty        = Support::No;
    reply.placeholders = Support::No;
    reply.sixel        = snapshot.sixel ? Support::Yes : Support::No;
    reply.cells        = snapshot.clients.isEmpty() ? Support::Unknown : Support::No;
    for (const QString &client : snapshot.clients) {
        const QString utf8 = client.section(QLatin1Char('\t'), 8, 8);
        if (utf8 == QStringLiteral("1")) {
            reply.cells = Support::Yes;
            break;
        }
        if (utf8 != QStringLiteral("0"))
            reply.cells = Support::Unknown;
    }
    const auto env    = QProcessEnvironment::systemEnvironment();
    reply.evidence    = Evidence::Tmux;
    reply.passthrough = passthroughSupport(snapshot);
    if (reply.passthrough == Support::Yes && !hasValue(env, "STY") && !hasValue(env, "ZELLIJ")) {
        for (const QString &client : snapshot.clients) {
            if (isNestedClient(client)
                || client.section(QLatin1Char('\t'), 8, 8) != QStringLiteral("1"))
                continue;
            TerminalInfo info = terminalInfo(client.section(QLatin1Char('\t'), 2, 2));
            if (info.preferred == Protocol::None)
                info = terminalInfo(client.section(QLatin1Char('\t'), 3, 3));
            if (info.preferred == Protocol::Kitty && info.placeholders == Support::Yes) {
                reply.kitty        = Support::Yes;
                reply.placeholders = Support::Yes;
                break;
            }
        }
    }
    const QString fingerprint = snapshot.pane + QLatin1Char('\n')
                                + snapshot.clients.join(QLatin1Char('\n')) + QLatin1Char('\n')
                                + snapshot.passthrough + snapshot.version
                                + QString::number(snapshot.sixel);
    if (fingerprint != clientFingerprint) {
        clientFingerprint = fingerprint;
        storedReply       = reply;
        ++graphicsRevision;
    }
}

void completeSnapshot(TmuxSnapshot *snapshot, const QList<CommandResult> &results)
{
    snapshot->clients     = relatedClients(results[0].output, results[1].output, snapshot->pane);
    snapshot->passthrough = results[2].output.trimmed();
}

QList<QStringList> extraCommands(const TmuxSnapshot &snapshot)
{
    QList<QStringList> commands;
    if (snapshot.passthrough.isEmpty())
        commands.append(
            {QStringLiteral("display-message"), QStringLiteral("-p"), QStringLiteral("#{version}")});
    if (paneReply.sixel == Support::Yes) {
        for (const QString &client : snapshot.clients)
            commands.append(
                {QStringLiteral("show-messages"),
                 QStringLiteral("-T"),
                 QStringLiteral("-t"),
                 client.section(QLatin1Char('\t'), 0, 0)});
    }
    return commands;
}

void applyExtra(TmuxSnapshot *snapshot, const QStringList &command, const QString &output)
{
    if (command.first() == QStringLiteral("display-message"))
        snapshot->version = output.trimmed();
    else
        snapshot->sixel = snapshot->sixel || hasSixel(output);
}

void queryTmux()
{
    TmuxSnapshot snapshot;
    snapshot.pane = qEnvironmentVariable("TMUX_PANE");
    if (snapshot.pane.isEmpty())
        return;
    QList<CommandResult> results;
    QElapsedTimer        elapsed;
    elapsed.start();
    for (const QStringList &command : tmuxCommands(snapshot.pane)) {
        const CommandResult result = runTmux(command, int(4000 - elapsed.elapsed()));
        if (!result.ok)
            return;
        results.append(result);
    }
    completeSnapshot(&snapshot, results);
    for (const QStringList &command : extraCommands(snapshot)) {
        const CommandResult result = runTmux(command, int(4000 - elapsed.elapsed()));
        if (!result.ok)
            return;
        applyExtra(&snapshot, command, result.output);
    }
    publishSnapshot(snapshot);
}

class TmuxRefresh : public QObject
{
public:
    explicit TmuxRefresh(QObject *parent)
        : QObject(parent)
    {
        deadline.setSingleShot(true);
        connect(&deadline, &QTimer::timeout, this, [this] { process.kill(); });
        connect(&process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart)
                finish(false);
        });
        connect(&process, &QProcess::finished, this, [this](int code, QProcess::ExitStatus status) {
            deadline.stop();
            if (code != 0 || status != QProcess::NormalExit) {
                finish(false);
                return;
            }
            const QString output = QString::fromUtf8(process.readAllStandardOutput());
            results.append({true, output});
            if (results.size() == 3) {
                completeSnapshot(&snapshot, results);
                commands.append(extraCommands(snapshot));
            } else if (results.size() > 3) {
                applyExtra(&snapshot, commands[results.size() - 1], output);
            }
            if (results.size() == commands.size())
                finish(true);
            else
                next();
        });
    }

    ~TmuxRefresh() override
    {
        if (process.state() != QProcess::NotRunning) {
            process.kill();
            process.waitForFinished(2000);
        }
    }

    void refresh()
    {
        if (running || (lastAttempt.isValid() && lastAttempt.elapsed() < 2000))
            return;
        snapshot      = {};
        snapshot.pane = qEnvironmentVariable("TMUX_PANE");
        if (snapshot.pane.isEmpty())
            return;
        running = true;
        results.clear();
        commands = tmuxCommands(snapshot.pane);
        lastAttempt.start();
        cycle.start();
        next();
    }

private:
    void next()
    {
        const int remaining = int(4000 - cycle.elapsed());
        if (remaining <= 0) {
            finish(false);
            return;
        }
        process.start(QStringLiteral("tmux"), commands[results.size()]);
        deadline.start(qMin(2000, remaining));
    }

    void finish(bool success)
    {
        deadline.stop();
        if (success)
            publishSnapshot(snapshot);
        running = false;
    }

    QProcess             process;
    QTimer               deadline;
    QElapsedTimer        lastAttempt;
    QElapsedTimer        cycle;
    QList<QStringList>   commands;
    QList<CommandResult> results;
    TmuxSnapshot         snapshot;
    bool                 running = false;
};

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
            if (kind == '_' && body.startsWith("Gi=31;"))
                reply.kitty = body.mid(6) == "OK" ? Support::Yes : Support::No;
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
            if (reply.sixel != Support::Yes)
                reply.sixel = body.mid(1).split(';').contains("4") ? Support::Yes : Support::No;
        } else if (final == 'S' && body.startsWith("?2;")) {
            const auto parts = body.split(';');
            if (parts.size() >= 3 && parts[1] == "0") {
                const bool geometry
                    = std::any_of(parts.cbegin() + 2, parts.cend(), [](const QByteArray &part) {
                          return part.toInt() > 0;
                      });
                if (geometry)
                    reply.sixel = Support::Yes;
            }
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

Capabilities selectCapabilities(const QProcessEnvironment &env, const ProbeReply &reply)
{
    const QString forced = env.value(QStringLiteral("QSOC_IMAGE_PROTOCOL")).trimmed().toLower();
    if (hasValue(env, "QSOC_NO_IMAGE_GRAPHICS") || forced == QStringLiteral("none"))
        return {Protocol::None, Evidence::Override, Support::No};
    if (forced == QStringLiteral("kitty"))
        return {Protocol::Kitty, Evidence::Override, Support::Yes};
    if (forced == QStringLiteral("iterm2"))
        return {Protocol::ITerm2, Evidence::Override, Support::No};
    if (forced == QStringLiteral("sixel"))
        return {Protocol::Sixel, Evidence::Override, Support::No};
    if (forced == QStringLiteral("cells"))
        return {Protocol::Cells, Evidence::Override, Support::No};
    const QString      term  = env.value(QStringLiteral("TERM")).toLower();
    const Protocol     basic = ((term.isEmpty() && !hasValue(env, "WT_SESSION"))
                                || term == QStringLiteral("dumb"))
                                   ? Protocol::None
                                   : Protocol::Cells;
    const Capabilities fallback{basic, Evidence::None, Support::No};
    const Multiplexer  mux = multiplexer(env);
    if (mux != Multiplexer::None) {
        if (mux == Multiplexer::Tmux && reply.evidence == Evidence::Tmux) {
            if (reply.kitty == Support::Yes && reply.placeholders == Support::Yes
                && reply.passthrough == Support::Yes)
                return {Protocol::Kitty, Evidence::Tmux, Support::Yes};
            if (reply.sixel == Support::Yes)
                return {Protocol::Sixel, Evidence::Tmux, Support::No};
        }
        if (mux == Multiplexer::Zellij && reply.kitty == Support::Yes)
            return {Protocol::Kitty, Evidence::Terminal, Support::No};
        if (mux == Multiplexer::Zellij && reply.sixel == Support::Yes)
            return {Protocol::Sixel, Evidence::Terminal, Support::No};
        return mux == Multiplexer::Tmux && reply.evidence == Evidence::Tmux
                       && reply.cells == Support::No
                   ? Capabilities{Protocol::None, Evidence::Tmux, Support::No}
                   : fallback;
    }
    const TerminalInfo named = terminalInfo(reply.terminal);
    if (named.preferred == Protocol::ITerm2)
        return {named.preferred, Evidence::Terminal, named.placeholders};
    if (reply.kitty == Support::Yes)
        return {Protocol::Kitty, Evidence::Terminal, named.placeholders};
    if (reply.sixel == Support::Yes)
        return {Protocol::Sixel, Evidence::Terminal, Support::No};
    if (named.preferred != Protocol::None) {
        if (named.preferred == Protocol::Kitty && reply.kitty == Support::No)
            return fallback;
        return {named.preferred, Evidence::Terminal, named.placeholders};
    }
    if (!reply.terminal.isEmpty())
        return fallback;
    if (hasValue(env, "PASEO_TERMINAL_ID"))
        return fallback;
    const TerminalInfo identity = envIdentity(env);
    if (identity.preferred == Protocol::Kitty && reply.kitty == Support::No)
        return fallback;
    return identity.preferred == Protocol::None
               ? fallback
               : Capabilities{identity.preferred, Evidence::Environment, identity.placeholders};
}

Protocol selectProtocol(const QProcessEnvironment &env, const ProbeReply &reply)
{
    return selectCapabilities(env, reply).protocol;
}

void probeTerminal()
{
#ifndef Q_OS_WIN
    const auto env = QProcessEnvironment::systemEnvironment();
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || selectProtocol(env, {}) == Protocol::None)
        return;
    termios saved{};
    if (tcgetattr(STDIN_FILENO, &saved) != 0)
        return;
    termios raw = saved;
    raw.c_lflag &= static_cast<tcflag_t>(~(ICANON | ECHO));
    raw.c_cc[VMIN]  = 0;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
        return;
    probeStarted = true;
    QByteArray queries(
        "\x1b"
        "7\x1b[?7l\x1b[8m");
    if (multiplexer(env) == Multiplexer::None || multiplexer(env) == Multiplexer::Zellij)
        queries += "\x1b[16t\x1b_Gi=31,s=1,v=1,a=q,t=d,f=24;AAAA\x1b\\\x1b[>q";
    queries += "\x1b[?2;1;0S\x1b]11;?\x1b\\\x1b[c\r\x1b[K\x1b[28m\x1b[?7h\x1b"
               "8";
    if (write(STDOUT_FILENO, queries.constData(), size_t(queries.size())) == queries.size()) {
        QByteArray    bytes;
        QElapsedTimer elapsed;
        QElapsedTimer silence;
        elapsed.start();
        while (elapsed.elapsed() < 1500) {
            if (storedReply.answered && silence.isValid() && silence.elapsed() >= 60)
                break;
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
            storedReply = parseProbeReply(bytes);
            silence.restart();
        }
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &saved);
    paneReply = storedReply;
    if (multiplexer(env) == Multiplexer::Tmux) {
        storedReply.kitty = Support::Unknown;
        storedReply.sixel = Support::Unknown;
        if (hasValue(env, "TMUX"))
            queryTmux();
    }
    ++graphicsRevision;
#endif
}

void setProbeReply(const ProbeReply &reply)
{
    storedReply = reply;
    ++graphicsRevision;
}

void handleProbeReply(const QByteArray &bytes)
{
    if (!probeStarted)
        return;
    const ProbeReply reply = parseProbeReply(bytes);
    const auto       env   = QProcessEnvironment::systemEnvironment();
    if (multiplexer(env) == Multiplexer::Tmux) {
        if (reply.sixel == Support::Yes)
            paneReply.sixel = Support::Yes;
        return;
    }
    bool changed = false;
    if (reply.kitty != Support::Unknown && storedReply.kitty != reply.kitty) {
        storedReply.kitty = reply.kitty;
        changed           = true;
    }
    if (reply.sixel == Support::Yes && storedReply.sixel != Support::Yes) {
        storedReply.sixel = Support::Yes;
        changed           = true;
    }
    if (!reply.terminal.isEmpty() && storedReply.terminal != reply.terminal) {
        storedReply.terminal = reply.terminal;
        changed              = true;
    }
    if (!reply.cell.isEmpty() && storedReply.cell != reply.cell) {
        storedReply.cell = reply.cell;
        changed          = true;
    }
    if (changed)
        ++graphicsRevision;
}

void refreshTerminal()
{
    if (!probeStarted || qEnvironmentVariable("TMUX").isEmpty())
        return;
    static QPointer<TmuxRefresh> refresh;
    if (!refresh)
        refresh = new TmuxRefresh(QCoreApplication::instance());
    refresh->refresh();
}

quint64 revision()
{
    return graphicsRevision;
}
QByteArray takeTypeAhead()
{
    return std::exchange(storedReply.typeAhead, {});
}
Protocol protocol()
{
    return capabilities().protocol;
}
Capabilities capabilities()
{
    return selectCapabilities(QProcessEnvironment::systemEnvironment(), storedReply);
}
bool hasLightBackground()
{
    return storedReply.light;
}
bool isInlineBitmap(Protocol value)
{
    return value == Protocol::ITerm2 || value == Protocol::Sixel;
}

bool usesPlaceholders()
{
    const auto env = QProcessEnvironment::systemEnvironment();
    return protocol() == Protocol::Kitty && multiplexer(env) == Multiplexer::Tmux
           && (capabilities().placeholders == Support::Yes
               || env.value(QStringLiteral("QSOC_IMAGE_PROTOCOL"))
                          .trimmed()
                          .compare(QStringLiteral("kitty"), Qt::CaseInsensitive)
                      == 0);
}

QString passthrough(const QString &sequence)
{
    if (!hasValue(QProcessEnvironment::systemEnvironment(), "TMUX"))
        return sequence;
    QString doubled = sequence;
    doubled.replace(QLatin1Char('\x1b'), QStringLiteral("\x1b\x1b"));
    return QStringLiteral("\x1bPtmux;") + doubled + QStringLiteral("\x1b\\");
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

} // namespace QTuiGraphics
