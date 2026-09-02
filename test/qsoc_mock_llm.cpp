// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

/**
 * @file qsoc_mock_llm.cpp
 * @brief Mock chat-completions endpoint for qsoc agent tests.
 *
 * Speaks the streaming SSE and non-streaming JSON wire formats QLLMService
 * expects, and can inject HTTP failures and tool calls so a test can drive the
 * agent loop without a provider.
 *
 *   qsoc_mock_llm <port> [<failmode>]
 *
 * failmode is none (default), always, window:<seconds> or prob:<p>.
 * Behaviour is otherwise selected by MOCK_* environment variables; see the
 * qsoc-mock-llm skill for the full matrix.
 *
 * One event loop serves every connection. A per-connection thread is not
 * needed and would put the counters behind a mutex for nothing: MOCK_DELAY
 * defers a response through a timer rather than blocking.
 */

#include <csignal>

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRandomGenerator>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTextStream>
#include <QThread>
#include <QTimer>

namespace {

/* Environment, read once at startup like the script it replaces. */
struct MockConfig
{
    int         spawnCount        = 0;
    bool        spawnInBackground = true;
    QByteArray  reply             = "DONE";
    int         failCode          = 429;
    double      ttlSeconds        = 120;
    double      delaySeconds      = 0;
    double      chunkDelay        = 0;
    QByteArray  toolName;
    QJsonObject toolArgs;
    int         toolMax = 1;
    QString     toolGate;
    QString     requestLog;
    QString     failMode = QStringLiteral("none");
};

MockConfig config;

/* Counters reported by GET. Single threaded, so no lock. */
QHash<QByteArray, int> hits{
    {"fail", 0},
    {"200_toolcalls", 0},
    {"200_text", 0},
    {"200_sync", 0},
    {"alpn_h2", 0},
    {"alpn_http1", 0},
    {"alpn_none", 0},
};

int           emitted = 0;
QElapsedTimer firstRequest;

QByteArray envBytes(const char *name, const QByteArray &fallback)
{
    const QByteArray value = qgetenv(name);
    return value.isEmpty() ? fallback : value;
}

double envDouble(const char *name, double fallback)
{
    const QByteArray value = qgetenv(name);
    if (value.isEmpty()) {
        return fallback;
    }
    bool         ok  = false;
    const double out = value.toDouble(&ok);
    return ok ? out : fallback;
}

int envInt(const char *name, int fallback)
{
    const QByteArray value = qgetenv(name);
    if (value.isEmpty()) {
        return fallback;
    }
    bool      ok  = false;
    const int out = value.toInt(&ok);
    return ok ? out : fallback;
}

bool loadConfig(QString *error)
{
    config.spawnCount           = envInt("MOCK_SPAWN_N", 0);
    const QByteArray background = qgetenv("MOCK_SPAWN_BG").toLower();
    config.spawnInBackground = !(background == "0" || background == "false" || background == "no");
    config.reply             = envBytes("MOCK_REPLY", "DONE");
    config.failCode          = envInt("MOCK_FAIL_CODE", 429);
    config.ttlSeconds        = envDouble("MOCK_TTL", 120);
    config.delaySeconds      = envDouble("MOCK_DELAY", 0);
    config.chunkDelay        = envDouble("MOCK_CHUNK_DELAY", 0);
    config.toolName          = qgetenv("MOCK_TOOL_NAME");
    config.toolMax           = envInt("MOCK_TOOL_MAX", 1);
    config.toolGate          = QString::fromLocal8Bit(qgetenv("MOCK_TOOL_GATE"));
    config.requestLog        = QString::fromLocal8Bit(qgetenv("MOCK_REQUEST_LOG"));

    const QByteArray args = qgetenv("MOCK_TOOL_ARGS");
    if (!args.isEmpty()) {
        QJsonParseError     parse{};
        const QJsonDocument doc = QJsonDocument::fromJson(args, &parse);
        if (parse.error != QJsonParseError::NoError || !doc.isObject()) {
            *error = QStringLiteral("MOCK_TOOL_ARGS must be a JSON object");
            return false;
        }
        config.toolArgs = doc.object();
    }
    return true;
}

QByteArray compactJson(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact);
}

QByteArray compactJson(const QJsonArray &array)
{
    return QJsonDocument(array).toJson(QJsonDocument::Compact);
}

/** Decide whether this request gets the failure code. */
bool shouldFail()
{
    if (!firstRequest.isValid()) {
        firstRequest.start();
    }
    if (config.failMode == QLatin1String("always")) {
        return true;
    }
    if (config.failMode.startsWith(QLatin1String("window:"))) {
        const double window = config.failMode.mid(7).toDouble();
        return double(firstRequest.elapsed()) / 1000.0 < window;
    }
    if (config.failMode.startsWith(QLatin1String("prob:"))) {
        const double probability = config.failMode.mid(5).toDouble();
        return QRandomGenerator::global()->generateDouble() < probability;
    }
    return false;
}

QJsonArray buildToolCalls()
{
    QJsonArray calls;
    if (!config.toolName.isEmpty()) {
        QJsonObject function;
        function["name"]      = QString::fromUtf8(config.toolName);
        function["arguments"] = QString::fromUtf8(compactJson(config.toolArgs));
        QJsonObject call;
        call["index"]    = 0;
        call["id"]       = QStringLiteral("call_0");
        call["type"]     = QStringLiteral("function");
        call["function"] = function;
        calls.append(call);
        return calls;
    }
    for (int index = 0; index < config.spawnCount; ++index) {
        QJsonObject arguments;
        arguments["subagent_type"] = QStringLiteral("general-purpose");
        arguments["description"]   = QStringLiteral("child%1").arg(index);
        arguments["prompt"]
            = QStringLiteral("reply with the single word %1").arg(QString::fromUtf8(config.reply));
        arguments["run_in_background"] = config.spawnInBackground;
        QJsonObject function;
        function["name"]      = QStringLiteral("agent");
        function["arguments"] = QString::fromUtf8(compactJson(arguments));
        QJsonObject call;
        call["index"]    = index;
        call["id"]       = QStringLiteral("call_%1").arg(index);
        call["type"]     = QStringLiteral("function");
        call["function"] = function;
        calls.append(call);
    }
    return calls;
}

void appendRequestLog(const QJsonObject &request)
{
    if (config.requestLog.isEmpty()) {
        return;
    }
    QFile log(config.requestLog);
    if (!log.open(QIODevice::Append | QIODevice::Text)) {
        return;
    }
    log.write(compactJson(request));
    log.write("\n");
}

void writeHead(QTcpSocket *socket, int status, const QList<QByteArray> &headers)
{
    QByteArray head = "HTTP/1.0 " + QByteArray::number(status) + " OK\r\n";
    for (const QByteArray &header : headers) {
        head += header + "\r\n";
    }
    head += "\r\n";
    socket->write(head);
    socket->flush();
}

void writeBody(QTcpSocket *socket, int status, const QByteArray &type, const QByteArray &body)
{
    writeHead(
        socket,
        status,
        {"Content-Type: " + type, "Content-Length: " + QByteArray::number(body.size())});
    socket->write(body);
    socket->flush();
    socket->disconnectFromHost();
}

/** Stream the chunks as SSE, then the sentinel, honouring MOCK_CHUNK_DELAY. */
void writeSse(QTcpSocket *socket, const QList<QJsonObject> &chunks)
{
    writeHead(socket, 200, {"Content-Type: text/event-stream", "Cache-Control: no-cache"});
    for (int index = 0; index < chunks.size(); ++index) {
        socket->write("data: " + compactJson(chunks.at(index)) + "\n\n");
        socket->flush();
        if (config.chunkDelay > 0 && index + 1 < chunks.size()) {
            QThread::msleep(static_cast<unsigned long>(config.chunkDelay * 1000));
        }
    }
    socket->write("data: [DONE]\n\n");
    socket->flush();
    socket->disconnectFromHost();
}

QJsonObject deltaChunk(const QJsonObject &delta, const QString &finishReason)
{
    QJsonObject choice;
    choice["delta"] = delta;
    if (!finishReason.isNull()) {
        choice["finish_reason"] = finishReason;
    }
    QJsonArray choices;
    choices.append(choice);
    QJsonObject chunk;
    chunk["choices"] = choices;
    return chunk;
}

void respondPost(QTcpSocket *socket, const QByteArray &body)
{
    QJsonObject request;
    if (!body.isEmpty()) {
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (doc.isObject()) {
            request = doc.object();
        }
    }
    const bool streaming = request.value("stream").isBool() && request.value("stream").toBool();
    appendRequestLog(request);

    if (shouldFail()) {
        ++hits["fail"];
        QJsonObject error;
        error["message"] = QStringLiteral("rate limited");
        error["type"]    = QStringLiteral("rate_limit_error");
        QJsonObject payload;
        payload["error"]         = error;
        const QByteArray encoded = compactJson(payload);
        writeHead(
            socket,
            config.failCode,
            {"Content-Type: application/json",
             "Retry-After: 1",
             "Content-Length: " + QByteArray::number(encoded.size())});
        socket->write(encoded);
        socket->flush();
        socket->disconnectFromHost();
        return;
    }

    const bool isParent   = body.contains("Spawn a child sub-agent");
    const bool wantsTools = !config.toolName.isEmpty() || (config.spawnCount > 0 && isParent);
    bool       allowed    = false;
    if (!config.toolGate.isEmpty()) {
        /* The gate is the caller's clock: emit while the file is there, and
         * consume it so each creation buys exactly one tool call. */
        allowed = QFile::exists(config.toolGate);
    } else {
        allowed = config.toolMax <= 0 || emitted < config.toolMax;
    }
    const bool emitTools = wantsTools && allowed;
    if (emitTools) {
        ++emitted;
        if (!config.toolGate.isEmpty()) {
            QFile::remove(config.toolGate);
        }
    }
    ++hits[emitTools ? "200_toolcalls" : "200_text"];
    if (!streaming) {
        ++hits["200_sync"];
    }

    const QJsonArray toolCalls = emitTools ? buildToolCalls() : QJsonArray();

    auto send = [socket, streaming, emitTools, toolCalls]() {
        if (socket->state() != QAbstractSocket::ConnectedState) {
            return;
        }
        if (!streaming) {
            QJsonObject message;
            message["role"]      = QStringLiteral("assistant");
            QString finishReason = QStringLiteral("stop");
            if (emitTools) {
                message["content"]    = QJsonValue();
                message["tool_calls"] = toolCalls;
                finishReason          = QStringLiteral("tool_calls");
            } else {
                message["content"] = QString::fromUtf8(config.reply);
            }
            QJsonObject choice;
            choice["index"]         = 0;
            choice["message"]       = message;
            choice["finish_reason"] = finishReason;
            QJsonArray choices;
            choices.append(choice);
            QJsonObject usage;
            usage["prompt_tokens"]     = 1;
            usage["completion_tokens"] = 1;
            usage["total_tokens"]      = 2;
            QJsonObject payload;
            payload["choices"] = choices;
            payload["usage"]   = usage;
            writeBody(socket, 200, "application/json", compactJson(payload));
            return;
        }
        QJsonObject delta;
        delta["role"] = QStringLiteral("assistant");
        if (emitTools) {
            delta["tool_calls"] = toolCalls;
            writeSse(
                socket,
                {deltaChunk(delta, QString()),
                 deltaChunk(QJsonObject(), QStringLiteral("tool_calls"))});
        } else {
            delta["content"] = QString::fromUtf8(config.reply);
            writeSse(
                socket,
                {deltaChunk(delta, QString()), deltaChunk(QJsonObject(), QStringLiteral("stop"))});
        }
    };

    if (config.delaySeconds > 0) {
        QTimer::singleShot(static_cast<int>(config.delaySeconds * 1000), socket, send);
    } else {
        send();
    }
}

void respondGet(QTcpSocket *socket)
{
    QJsonObject payload;
    for (auto it = hits.constBegin(); it != hits.constEnd(); ++it) {
        payload[QString::fromUtf8(it.key())] = it.value();
    }
    writeBody(socket, 200, "application/json", compactJson(payload));
}

/** Per-connection read state: HTTP/1.0, one request per socket. */
struct Connection
{
    QByteArray buffer;
    bool       served = false;
};

void serve(QTcpSocket *socket, Connection *connection)
{
    if (connection->served) {
        return;
    }
    const int headerEnd = connection->buffer.indexOf("\r\n\r\n");
    if (headerEnd < 0) {
        return;
    }
    const QByteArray head   = connection->buffer.left(headerEnd);
    const int        space  = head.indexOf(' ');
    const QByteArray method = space > 0 ? head.left(space) : QByteArray();

    qsizetype contentLength = 0;
    for (const QByteArray &line : head.split('\n')) {
        const QByteArray trimmed = line.trimmed();
        if (trimmed.toLower().startsWith("content-length:")) {
            contentLength = trimmed.mid(15).trimmed().toLongLong();
        }
    }
    const QByteArray body = connection->buffer.mid(headerEnd + 4);
    if (body.size() < contentLength) {
        return;
    }

    connection->served = true;
    if (method == "POST") {
        respondPost(socket, body.left(contentLength));
    } else if (method == "GET") {
        respondGet(socket);
    } else {
        writeBody(socket, 501, "text/plain", "Unsupported method\n");
    }
}

} // namespace

int main(int argc, char *argv[])
{
    /* A client that vanishes mid-stream must not take the process down. */
#ifdef SIGPIPE
    signal(SIGPIPE, SIG_IGN);
#endif

    const QCoreApplication app(argc, argv);
    const QStringList      arguments = QCoreApplication::arguments();

    quint16 port = 18429;
    if (arguments.size() > 1) {
        bool       ok     = false;
        const uint parsed = arguments.at(1).toUInt(&ok);
        if (!ok || parsed > 65535) {
            QTextStream(stderr) << "invalid port: " << arguments.at(1) << "\n";
            return 2;
        }
        port = static_cast<quint16>(parsed);
    }
    if (arguments.size() > 2) {
        config.failMode = arguments.at(2);
    }

    QString error;
    if (!loadConfig(&error)) {
        QTextStream(stderr) << error << "\n";
        return 2;
    }

    /* Printed before the bind so a caller sees the process is alive. It proves
     * only that: readiness is a successful TCP connect, which is what the
     * tests wait for. */
    QTextStream(stdout) << "MOCK_READY " << port << "\n";
    QTextStream(stderr) << "MOCK_READY " << port << "\n";
    fflush(stdout);
    fflush(stderr);

    if (config.ttlSeconds > 0) {
        QTimer::singleShot(static_cast<int>(config.ttlSeconds * 1000), &app, [] {
            QCoreApplication::exit(0);
        });
    }

    QTcpServer                      server;
    QHash<QTcpSocket *, Connection> connections;

    QObject::connect(&server, &QTcpServer::newConnection, &server, [&server, &connections] {
        while (QTcpSocket *socket = server.nextPendingConnection()) {
            connections.insert(socket, Connection());
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [socket, &connections] {
                Connection &connection = connections[socket];
                connection.buffer += socket->readAll();
                serve(socket, &connection);
            });
            QObject::connect(socket, &QTcpSocket::disconnected, socket, [socket, &connections] {
                connections.remove(socket);
                socket->deleteLater();
            });
        }
    });

    /* The caller picked this port by binding and closing, so it can still be in
     * TIME_WAIT. Retrying for about a minute is half of the flake mitigation;
     * the ctest resource lock is the other half. */
    bool bound = false;
    for (int attempt = 0; attempt < 1200 && !bound; ++attempt) {
        bound = server.listen(QHostAddress::LocalHost, port);
        if (!bound) {
            QThread::msleep(50);
        }
    }
    if (!bound) {
        const QString message
            = QStringLiteral("MOCK_BIND_FAILED %1: %2").arg(port).arg(server.errorString());
        QTextStream(stdout) << message << "\n";
        QTextStream(stderr) << message << "\n";
        return 1;
    }

    return QCoreApplication::exec();
}
