// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include <cstdio>

#include <QCoreApplication>
#include <QFile>
#include <QStringList>

#include <csignal>

#ifdef Q_OS_WIN
#include <fcntl.h>
#include <io.h>
#include <windows.h>
#endif

namespace {

constexpr auto kContentLengthKey = "Content-Length:";

bool writeBytes(QFile &output, const QByteArray &bytes)
{
    return output.write(bytes) == bytes.size() && output.flush();
}

bool writeLine(QFile &output, const QByteArray &line)
{
    return writeBytes(output, line + '\n');
}

/* QFile treats its source as a random-access file. On a Windows pipe only
   the first read behaves; the next one never returns. Read stdin through the
   C runtime, whose blocking semantics on a binary-mode pipe are defined. */
QByteArray readBytes(qsizetype size)
{
    QByteArray   bytes(size, Qt::Uninitialized);
    const size_t got = fread(bytes.data(), 1, static_cast<size_t>(size), stdin);
    if (static_cast<qsizetype>(got) != size) {
        fprintf(
            stderr,
            "peer: short body, have %lld of %lld bytes\n",
            static_cast<long long>(got),
            static_cast<long long>(size));
        return {};
    }
    return bytes;
}

QByteArray readLineBytes()
{
    QByteArray line;
    int        character = 0;
    while ((character = fgetc(stdin)) != EOF) {
        line.append(static_cast<char>(character));
        if (character == '\n') {
            break;
        }
    }
    return line;
}

QByteArray readContentLengthMessage()
{
    const QByteArray header = readLineBytes();
    if (!header.startsWith(kContentLengthKey)) {
        fprintf(stderr, "peer: header was '%s'\n", header.trimmed().constData());
        return {};
    }
    bool         parsed = false;
    const qint64 size   = header.mid(static_cast<qsizetype>(qstrlen(kContentLengthKey)))
                              .trimmed()
                              .toLongLong(&parsed);
    if (!parsed || size <= 0) {
        fprintf(stderr, "peer: unparsable length in '%s'\n", header.trimmed().constData());
        return {};
    }
    const QByteArray separator = readLineBytes();
    if (!separator.trimmed().isEmpty()) {
        fprintf(stderr, "peer: separator was '%s'\n", separator.trimmed().constData());
        return {};
    }
    return readBytes(static_cast<qsizetype>(size));
}

QByteArray contentLengthFrame(const QByteArray &message)
{
    return QByteArrayLiteral("Content-Length: ") + QByteArray::number(message.size())
           + QByteArrayLiteral("\r\n\r\n") + message;
}

int runPeer(QCoreApplication &app, QFile &input, QFile &output)
{
    const QStringList args = app.arguments();
    if (args.size() < 2) {
        return 64;
    }

    const QString mode = args.at(1);
    if (mode == QStringLiteral("echo")) {
        const QByteArray line = readLineBytes();
        return !line.isEmpty() && writeBytes(output, line) ? 0 : 65;
    }
    if (mode == QStringLiteral("legacy-echo")) {
        const QByteArray message = readContentLengthMessage();
        if (message.isEmpty()) {
            return 66;
        }
        return writeBytes(output, contentLengthFrame(message)) ? 0 : 65;
    }
    if (mode == QStringLiteral("legacy-multiple")) {
        return writeBytes(
                   output,
                   contentLengthFrame(QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":1}"))
                       + contentLengthFrame(QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":2}"))
                       + contentLengthFrame(QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":3}")))
                   ? 0
                   : 65;
    }
    if (mode == QStringLiteral("legacy-split")) {
        const QByteArray message = QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":42}");
        const QByteArray header  = QByteArrayLiteral("Content-Length: ")
                                   + QByteArray::number(message.size()) + QByteArrayLiteral("\r\n");
        if (!writeBytes(output, header)) {
            return 65;
        }
        if (readContentLengthMessage() != QByteArrayLiteral("{\"gate\":\"open\"}")) {
            return 66;
        }
        return writeBytes(output, QByteArrayLiteral("\r\n") + message) ? 0 : 65;
    }
    if (mode == QStringLiteral("legacy-invalid")) {
        return writeBytes(
                   output, QByteArrayLiteral("Content-Type: application/json\r\n\r\n{\"id\":1}"))
                   ? 0
                   : 65;
    }
    if (mode == QStringLiteral("single-crlf")) {
        QByteArray message = QByteArrayLiteral(
            "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{\"text\":\"caf");
        message.append(char(0xc3));
        message.append(char(0xa9));
        message.append(QByteArrayLiteral("\"}}\r\n"));
        return writeBytes(output, message) ? 0 : 65;
    }
    if (mode == QStringLiteral("multiple")) {
        return writeBytes(
                   output,
                   QByteArrayLiteral(
                       "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n"
                       "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}\n"
                       "{\"jsonrpc\":\"2.0\",\"id\":3,\"result\":{}}\n"))
                   ? 0
                   : 65;
    }
    if (mode == QStringLiteral("split")) {
        QByteArray prefix = QByteArrayLiteral(
            "{\"jsonrpc\":\"2.0\",\"id\":42,\"result\":{\"text\":\"caf");
        prefix.append(char(0xc3));
        if (!writeBytes(output, prefix)) {
            return 65;
        }
        if (readLineBytes() != QByteArrayLiteral("{\"gate\":\"open\"}\n")) {
            return 66;
        }
        QByteArray suffix(1, char(0xa9));
        suffix.append(QByteArrayLiteral("\"}}\n"));
        return writeBytes(output, suffix) ? 0 : 65;
    }
    if (mode == QStringLiteral("invalid")) {
        if (!writeLine(output, QByteArrayLiteral("not-json"))) {
            return 65;
        }
        return app.exec();
    }
    if (mode == QStringLiteral("two-hold")) {
        if (!writeBytes(
                output,
                QByteArrayLiteral(
                    "{\"jsonrpc\":\"2.0\",\"id\":1,\"result\":{}}\n"
                    "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}\n"))) {
            return 65;
        }
        return app.exec();
    }
    if (mode == QStringLiteral("ignore")) {
        return app.exec();
    }
    if (mode == QStringLiteral("ignore-term")) {
#ifndef Q_OS_WIN
        std::signal(SIGTERM, SIG_IGN);
#endif
        return app.exec();
    }
    if (mode == QStringLiteral("close-input")) {
        input.close();
        if (!writeLine(output, QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":8,\"result\":{}}"))) {
            return 65;
        }
        return app.exec();
    }
    if (mode == QStringLiteral("partial")) {
        return writeBytes(output, QByteArrayLiteral("{\"jsonrpc\":\"2.0\"}")) ? 0 : 65;
    }
    if (mode == QStringLiteral("restart")) {
        if (args.size() < 3) {
            return 64;
        }
        QFile marker(args.at(2));
        if (marker.open(QIODevice::WriteOnly | QIODevice::NewOnly)) {
            marker.write("1");
            marker.close();
            return writeBytes(output, QByteArrayLiteral("{\"jsonrpc\":\"2.0\"")) ? 0 : 65;
        }
        return writeLine(output, QByteArrayLiteral("{\"jsonrpc\":\"2.0\",\"id\":99,\"result\":{}}"))
                   ? 0
                   : 65;
    }
    if (mode == QStringLiteral("nonzero")) {
        return 23;
    }
    if (mode == QStringLiteral("crash")) {
#ifdef Q_OS_WIN
        /* QProcess calls an exit code a crash only below -1, so -1 itself
           reads as an ordinary exit. Use the access-violation status the
           platform raises for a real crash. */
        TerminateProcess(GetCurrentProcess(), 0xC0000005U);
#else
        std::raise(SIGKILL);
#endif
        return 70;
    }
    return 64;
}

} // namespace

int main(int argc, char **argv)
{
#ifdef Q_OS_WIN
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    QCoreApplication app(argc, argv);
    QFile            input;
    QFile            output;
    if (!input.open(stdin, QIODevice::ReadOnly, QFileDevice::AutoCloseHandle)
        || !output.open(stdout, QIODevice::WriteOnly, QFileDevice::AutoCloseHandle)) {
        return 65;
    }
    return runPeer(app, input, output);
}
