// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCCONSOLE_H
#define QSOCCONSOLE_H

#include <QString>
#include <QStringView>
#include <QTextStream>
#include <QtGlobal>

#include <cstdint>
#include <string>
#include <string_view>

class QIODevice;

/**
 * @brief Unified console output for the qsoc CLI.
 *
 * Two decision axes, one family:
 *   1. Filtered by --verbose level?
 *      yes: use severity functions critical/error/warn/info/debug
 *      no:  use out()/err() (raw QTextStream, never filtered)
 *   2. Decorated with prefix?
 *      yes: bare suffix (handler adds 'error: ', 'warn: ', etc.)
 *      no:  'Plain' suffix for pre-rendered blocks (slang, LSP)
 *
 * Severity functions return a Stream proxy supporting Qt-style << chains
 * with auto-spacing (qDebug.noquote() ergonomics) and auto-newline on
 * destruction. Print functions return raw QTextStream for partial writes.
 */
class QSocConsole final
{
public:
    QSocConsole()  = delete;
    ~QSocConsole() = delete;

    /** Verbosity levels. Numeric values match the --verbose CLI flag. */
    enum class Level : std::uint8_t {
        Silent = 0,
        Error  = 1,
        Warn   = 2,
        Info   = 3, /* default */
        Debug  = 4,
    };

    /** Color emission policy. Auto resolves via NO_COLOR > FORCE_COLOR > isatty. */
    enum class ColorMode : std::uint8_t { Auto, Always, Never };

    /* Stream proxy: accumulates << values, emits on destruction. */
    class Stream final
    {
    public:
        ~Stream();
        Stream(Stream &&) noexcept;
        Stream(const Stream &)            = delete;
        Stream &operator=(Stream &&)      = delete;
        Stream &operator=(const Stream &) = delete;

        Stream &operator<<(QStringView text);
        Stream &operator<<(const QString &text);
        Stream &operator<<(const QByteArray &bytes);
        Stream &operator<<(const QStringList &list);
        Stream &operator<<(const char *text);
        Stream &operator<<(const std::string &text);
        Stream &operator<<(std::string_view text);
        Stream &operator<<(QChar ch);
        Stream &operator<<(char ch);
        Stream &operator<<(bool value);
        Stream &operator<<(int value);
        Stream &operator<<(long value);
        Stream &operator<<(long long value);
        Stream &operator<<(unsigned value);
        Stream &operator<<(unsigned long value);
        Stream &operator<<(unsigned long long value);
        Stream &operator<<(double value);
        Stream &operator<<(const void *ptr);
        /** Accept Qt::endl / Qt::flush manipulators as no-ops; the Stream
            auto-flushes and adds a trailing newline on destruction. */
        Stream &operator<<(QTextStream &(*manipulator)(QTextStream &) );

        /** Disable auto-spacing for the rest of this chain (qDebug.nospace() equivalent). */
        Stream &nospace();
        /** Re-enable auto-spacing (default). */
        Stream &space();
        /** No-ops kept for source compatibility with qDebug-style chains.
            Stream already passes strings through without quoting. */
        Stream &noquote() { return *this; }
        Stream &quote() { return *this; }

    private:
        friend class QSocConsole;
        enum class Routing : std::uint8_t {
            Severity,      /* with prefix + color, level-filtered */
            SeverityPlain, /* no prefix, no color, level-filtered */
        };

        Stream(Routing routing, Level level);
        void appendSpace();

        Routing m_routing;
        Level   m_level;
        QString m_buffer;
        bool    m_first     = true;
        bool    m_autoSpace = true;
        bool    m_alive     = true;
    };

    /* Lifecycle */
    static void install();
    static void restore();

    /* Verbosity */
    static void  setLevel(Level lvl);
    static Level level();

    /* Color */
    static void      setColorMode(ColorMode mode);
    static ColorMode colorMode();

    /* Severity, with prefix + color, level-filtered, routed to stderr. */
    static Stream error();
    static Stream warn();
    static Stream info();
    static Stream debug();
    /** Highest severity. Same color/label as error in current palette. */
    static Stream critical();

    /* Severity, no prefix, no color, level-filtered, routed to stderr.
       For pre-rendered blocks (slang, LSP) that already format themselves. */
    static Stream errorPlain();
    static Stream warnPlain();
    static Stream infoPlain();
    static Stream debugPlain();
    static Stream criticalPlain();

    /* Raw streams: never filtered, no decoration, support partial writes. */
    static QTextStream &out(); /* stdout: products */
    static QTextStream &err(); /* stderr: hints, help */

    /* ANSI helper for ad-hoc dim text (warm gray, isatty-aware). */
    static QString dim(QStringView text);

    /* Test injection: redirect raw streams to a custom device (e.g. QBuffer). */
    static void setOutputDevice(QIODevice *device);
    static void setErrorDevice(QIODevice *device);

    /* Test helper: mirror raw stream writes through the Qt message handler. */
    static void setTeeToMessageHandler(bool enabled);

private:
    static void messageHandler(
        QtMsgType type, const QMessageLogContext &context, const QString &message);
};

#endif /* QSOCCONSOLE_H */
