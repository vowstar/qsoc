// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocconsole.h"

#include <QByteArray>
#include <QDebug>
#include <QIODevice>
#include <QtGlobal>

#include <cstdio>

#ifdef Q_OS_WIN
#include <io.h>
#define qsoc_isatty(fd) _isatty(fd)
#define qsoc_fileno(f) _fileno(f)
#else
#include <unistd.h>
#define qsoc_isatty(fd) isatty(fd)
#define qsoc_fileno(f) fileno(f)
#endif

namespace {

QSocConsole::Level     g_level        = QSocConsole::Level::Info;
QSocConsole::ColorMode g_colorMode    = QSocConsole::ColorMode::Auto;
QtMessageHandler       g_priorHandler = nullptr;
bool                   g_handlerOwned = false;
bool                   g_teeToHandler = false;

bool envSetNonEmpty(const char *name)
{
    return !qgetenv(name).isEmpty();
}

bool colorEnabledOn(FILE *stream)
{
    switch (g_colorMode) {
    case QSocConsole::ColorMode::Always:
        return true;
    case QSocConsole::ColorMode::Never:
        return false;
    case QSocConsole::ColorMode::Auto:
        break;
    }
    if (envSetNonEmpty("NO_COLOR")) {
        return false;
    }
    if (envSetNonEmpty("FORCE_COLOR")) {
        return true;
    }
    return qsoc_isatty(qsoc_fileno(stream)) != 0;
}

/* Per-level prefix + color. Critical maps to error label/color (cargo idiom). */
struct LevelStyle
{
    const char *label;     /* e.g. "error: " */
    const char *colorOpen; /* ANSI 24-bit, e.g. warm red */
};

LevelStyle styleFor(QSocConsole::Level lvl)
{
    switch (lvl) {
    case QSocConsole::Level::Error:
        return {"error: ", "\033[1;38;2;204;36;29m"}; /* warm red */
    case QSocConsole::Level::Warn:
        return {"warning: ", "\033[1;38;2;215;153;33m"}; /* warm yellow */
    case QSocConsole::Level::Info:
        return {"info: ", "\033[1;38;2;69;133;136m"}; /* warm blue */
    case QSocConsole::Level::Debug:
        return {"debug: ", "\033[1;38;2;146;131;116m"}; /* warm gray */
    case QSocConsole::Level::Silent:
        return {"", ""};
    }
    return {"", ""};
}

constexpr const char *kReset = "\033[0m";

class StreamSink : public QIODevice
{
public:
    StreamSink(FILE *file, QtMsgType teeType)
        : m_file(file)
        , m_teeType(teeType)
    {
        setOpenMode(WriteOnly);
    }

    void setOverride(QIODevice *override) { m_override = override; }

protected:
    qint64 writeData(const char *data, qint64 len) override
    {
        if (m_override != nullptr) {
            m_override->write(data, len);
        } else {
            std::fwrite(data, 1, static_cast<std::size_t>(len), m_file);
            std::fflush(m_file);
        }
        if (g_teeToHandler) {
            QString chunk = QString::fromUtf8(data, len);
            while (chunk.endsWith(QChar::fromLatin1('\n'))) {
                chunk.chop(1);
            }
            if (!chunk.isEmpty()) {
                if (m_teeType == QtInfoMsg) {
                    qInfo().noquote() << chunk;
                } else {
                    qCritical().noquote() << chunk;
                }
            }
        }
        return len;
    }

    qint64 readData(char * /*data*/, qint64 /*len*/) override { return -1; }

private:
    FILE      *m_file;
    QtMsgType  m_teeType;
    QIODevice *m_override = nullptr;
};

StreamSink &outSink()
{
    static StreamSink s_sink(stdout, QtInfoMsg);
    return s_sink;
}

StreamSink &errSink()
{
    static StreamSink s_sink(stderr, QtCriticalMsg);
    return s_sink;
}

QTextStream &outStream()
{
    static QTextStream s_stream(&outSink());
    return s_stream;
}

QTextStream &errStream()
{
    static QTextStream s_stream(&errSink());
    return s_stream;
}

void writeSeverity(QSocConsole::Level lvl, const QString &text, bool plain)
{
    if (lvl == QSocConsole::Level::Silent || lvl > QSocConsole::level()) {
        return;
    }
    QTextStream &out   = errStream();
    const bool   color = colorEnabledOn(stderr);
    if (!plain) {
        const auto style = styleFor(lvl);
        if (color) {
            out << QString::fromLatin1(style.colorOpen);
        }
        out << QString::fromLatin1(style.label);
        if (color) {
            out << QString::fromLatin1(kReset);
        }
    }
    out << text;
    if (!text.endsWith(QChar::fromLatin1('\n'))) {
        out << QChar::fromLatin1('\n');
    }
    out.flush();
}

} /* namespace */

/* ===== Stream ===== */

QSocConsole::Stream::Stream(Routing routing, Level level)
    : m_routing(routing)
    , m_level(level)
{}

QSocConsole::Stream::Stream(Stream &&other) noexcept
    : m_routing(other.m_routing)
    , m_level(other.m_level)
    , m_buffer(std::move(other.m_buffer))
    , m_first(other.m_first)
    , m_autoSpace(other.m_autoSpace)
    , m_alive(other.m_alive)
{
    other.m_alive = false;
}

QSocConsole::Stream::~Stream()
{
    if (!m_alive) {
        return;
    }
    /* All Stream instances are Severity or SeverityPlain. The raw streams
       returned by out()/err() are QTextStream&, not Stream. */
    writeSeverity(m_level, m_buffer, m_routing == Routing::SeverityPlain);
}

void QSocConsole::Stream::appendSpace()
{
    if (!m_first && m_autoSpace) {
        m_buffer.append(QChar::fromLatin1(' '));
    }
    m_first = false;
}

QSocConsole::Stream &QSocConsole::Stream::operator<<(QStringView text)
{
    appendSpace();
    m_buffer.append(text);
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::operator<<(const QString &text)
{
    appendSpace();
    m_buffer.append(text);
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::operator<<(const QByteArray &bytes)
{
    appendSpace();
    m_buffer.append(QString::fromUtf8(bytes));
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::operator<<(const QStringList &list)
{
    appendSpace();
    m_buffer.append(QChar::fromLatin1('('));
    for (qsizetype i = 0; i < list.size(); ++i) {
        if (i > 0) {
            m_buffer.append(QStringLiteral(", "));
        }
        m_buffer.append(QChar::fromLatin1('"'));
        m_buffer.append(list.at(i));
        m_buffer.append(QChar::fromLatin1('"'));
    }
    m_buffer.append(QChar::fromLatin1(')'));
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::operator<<(const char *text)
{
    appendSpace();
    m_buffer.append(QString::fromUtf8(text));
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::operator<<(const std::string &text)
{
    appendSpace();
    m_buffer.append(QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())));
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::operator<<(std::string_view text)
{
    appendSpace();
    m_buffer.append(QString::fromUtf8(text.data(), static_cast<qsizetype>(text.size())));
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::operator<<(QChar ch)
{
    appendSpace();
    m_buffer.append(ch);
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::operator<<(char ch)
{
    appendSpace();
    m_buffer.append(QChar::fromLatin1(ch));
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::operator<<(bool value)
{
    appendSpace();
    m_buffer.append(QString::fromLatin1(value ? "true" : "false"));
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::operator<<(int value)
{
    appendSpace();
    m_buffer.append(QString::number(value));
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::operator<<(long value)
{
    appendSpace();
    m_buffer.append(QString::number(value));
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::operator<<(long long value)
{
    appendSpace();
    m_buffer.append(QString::number(value));
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::operator<<(unsigned value)
{
    appendSpace();
    m_buffer.append(QString::number(value));
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::operator<<(unsigned long value)
{
    appendSpace();
    m_buffer.append(QString::number(value));
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::operator<<(unsigned long long value)
{
    appendSpace();
    m_buffer.append(QString::number(value));
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::operator<<(double value)
{
    appendSpace();
    m_buffer.append(QString::number(value));
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::operator<<(const void *ptr)
{
    appendSpace();
    m_buffer.append(QString::asprintf("%p", ptr));
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::operator<<(QTextStream &(*manipulator)(QTextStream &) )
{
    /* Qt::endl / Qt::flush are no-ops here; Stream auto-flushes on destruction
       and adds trailing newline if missing. */
    Q_UNUSED(manipulator);
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::nospace()
{
    m_autoSpace = false;
    return *this;
}

QSocConsole::Stream &QSocConsole::Stream::space()
{
    m_autoSpace = true;
    return *this;
}

/* ===== QSocConsole statics ===== */

void QSocConsole::install()
{
    if (g_handlerOwned) {
        return;
    }
    g_priorHandler = qInstallMessageHandler(&QSocConsole::messageHandler);
    g_handlerOwned = true;
}

void QSocConsole::restore()
{
    if (!g_handlerOwned) {
        return;
    }
    qInstallMessageHandler(g_priorHandler);
    g_priorHandler = nullptr;
    g_handlerOwned = false;
}

void QSocConsole::setLevel(Level lvl)
{
    g_level = lvl;
}

QSocConsole::Level QSocConsole::level()
{
    return g_level;
}

void QSocConsole::setColorMode(ColorMode mode)
{
    g_colorMode = mode;
}

QSocConsole::ColorMode QSocConsole::colorMode()
{
    return g_colorMode;
}

QSocConsole::Stream QSocConsole::error()
{
    return Stream(Stream::Routing::Severity, Level::Error);
}
QSocConsole::Stream QSocConsole::warn()
{
    return Stream(Stream::Routing::Severity, Level::Warn);
}
QSocConsole::Stream QSocConsole::info()
{
    return Stream(Stream::Routing::Severity, Level::Info);
}
QSocConsole::Stream QSocConsole::debug()
{
    return Stream(Stream::Routing::Severity, Level::Debug);
}
QSocConsole::Stream QSocConsole::critical()
{
    return Stream(Stream::Routing::Severity, Level::Error);
}

QSocConsole::Stream QSocConsole::errorPlain()
{
    return Stream(Stream::Routing::SeverityPlain, Level::Error);
}
QSocConsole::Stream QSocConsole::warnPlain()
{
    return Stream(Stream::Routing::SeverityPlain, Level::Warn);
}
QSocConsole::Stream QSocConsole::infoPlain()
{
    return Stream(Stream::Routing::SeverityPlain, Level::Info);
}
QSocConsole::Stream QSocConsole::debugPlain()
{
    return Stream(Stream::Routing::SeverityPlain, Level::Debug);
}
QSocConsole::Stream QSocConsole::criticalPlain()
{
    return Stream(Stream::Routing::SeverityPlain, Level::Error);
}

QTextStream &QSocConsole::out()
{
    return outStream();
}

QTextStream &QSocConsole::err()
{
    return errStream();
}

void QSocConsole::setOutputDevice(QIODevice *device)
{
    outStream().flush();
    outSink().setOverride(device);
}

void QSocConsole::setErrorDevice(QIODevice *device)
{
    errStream().flush();
    errSink().setOverride(device);
}

void QSocConsole::setTeeToMessageHandler(bool enabled)
{
    g_teeToHandler = enabled;
}

QString QSocConsole::dim(QStringView text)
{
    if (!colorEnabledOn(stdout)) {
        return text.toString();
    }
    /* 24-bit foreground #928374: warm muted gray for secondary text. */
    return QStringLiteral("\033[38;2;146;131;116m") + text.toString() + QStringLiteral("\033[0m");
}

void QSocConsole::messageHandler(
    QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    Q_UNUSED(context);
    /* Backward compat for any remaining qDebug/qInfo/qWarning/qCritical/qC* calls.
       Routes Info → stdout, others → stderr; no prefix injection. */
    FILE *stream = (type == QtInfoMsg) ? stdout : stderr;
    std::fputs(qPrintable(message), stream);
    std::fputc('\n', stream);
    std::fflush(stream);
}
