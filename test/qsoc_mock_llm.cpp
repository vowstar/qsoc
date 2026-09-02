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

/**
 * @brief Which wire format a request is speaking.
 *
 * GET /v1/models exists in both APIs at the same method and path with
 * incompatible bodies, so the path alone cannot decide. anthropic-version is
 * required on every Anthropic request, which makes it the discriminator.
 */
enum class Wire { OpenAi, Anthropic };

Wire wireOf(const QByteArray &head)
{
    const QByteArray lowered = head.toLower();
    if (lowered.contains("\nanthropic-version:") || lowered.contains("\nx-api-key:")) {
        return Wire::Anthropic;
    }
    return Wire::OpenAi;
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

QJsonObject parseBody(const QByteArray &body)
{
    if (body.isEmpty()) {
        return QJsonObject();
    }
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    return doc.isObject() ? doc.object() : QJsonObject();
}

/** One envelope per wire format, so the two paths cannot drift apart. */
void respondFailure(QTcpSocket *socket, Wire wire)
{
    QJsonObject error;
    error["message"] = QStringLiteral("rate limited");
    error["type"]    = QStringLiteral("rate_limit_error");
    QJsonObject payload;
    payload["error"] = error;
    if (wire == Wire::Anthropic) {
        payload["type"]       = QStringLiteral("error");
        payload["request_id"] = QStringLiteral("req_mock");
    }
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
}

void respondPost(QTcpSocket *socket, const QByteArray &body, Wire wire)
{
    const QJsonObject request = parseBody(body);
    const bool streaming = request.value("stream").isBool() && request.value("stream").toBool();
    appendRequestLog(request);

    if (shouldFail()) {
        ++hits["fail"];
        respondFailure(socket, wire);
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

/** Anthropic frames every event with a name, and has no [DONE] sentinel. */
void writeNamedSse(QTcpSocket *socket, const QList<QPair<QByteArray, QJsonObject>> &events)
{
    writeHead(socket, 200, {"Content-Type: text/event-stream", "Cache-Control: no-cache"});
    for (const auto &event : events) {
        socket->write("event: " + event.first + "\n");
        socket->write("data: " + compactJson(event.second) + "\n\n");
        socket->flush();
    }
    socket->disconnectFromHost();
}

QJsonObject anthropicUsage(int outputTokens)
{
    QJsonObject usage;
    usage["input_tokens"]                = 1;
    usage["output_tokens"]               = outputTokens;
    usage["cache_creation_input_tokens"] = 0;
    usage["cache_read_input_tokens"]     = 0;
    return usage;
}

void respondAnthropicMessages(QTcpSocket *socket, const QJsonObject &request, bool streaming)
{
    /* max_tokens is required on every Messages request; a naive port omits it. */
    if (!request.contains("max_tokens")) {
        QJsonObject error;
        error["type"]    = QStringLiteral("invalid_request_error");
        error["message"] = QStringLiteral("max_tokens: field required");
        QJsonObject payload;
        payload["type"]       = QStringLiteral("error");
        payload["error"]      = error;
        payload["request_id"] = QStringLiteral("req_mock");
        writeBody(socket, 400, "application/json", compactJson(payload));
        return;
    }
    const QString model    = request.value("model").toString(QStringLiteral("claude-mock"));
    const bool    emitTool = !config.toolName.isEmpty();

    QJsonObject block;
    if (emitTool) {
        block["type"]  = QStringLiteral("tool_use");
        block["id"]    = QStringLiteral("toolu_0");
        block["name"]  = QString::fromUtf8(config.toolName);
        block["input"] = config.toolArgs;
    } else {
        block["type"] = QStringLiteral("text");
        block["text"] = QString::fromUtf8(config.reply);
    }
    const QString stopReason = emitTool ? QStringLiteral("tool_use") : QStringLiteral("end_turn");

    if (!streaming) {
        QJsonArray content;
        content.append(block);
        QJsonObject payload;
        payload["id"]            = QStringLiteral("msg_mock");
        payload["type"]          = QStringLiteral("message");
        payload["role"]          = QStringLiteral("assistant");
        payload["model"]         = model;
        payload["content"]       = content;
        payload["stop_reason"]   = stopReason;
        payload["stop_sequence"] = QJsonValue();
        payload["usage"]         = anthropicUsage(1);
        writeBody(socket, 200, "application/json", compactJson(payload));
        return;
    }

    QJsonObject opening;
    opening["id"]            = QStringLiteral("msg_mock");
    opening["type"]          = QStringLiteral("message");
    opening["role"]          = QStringLiteral("assistant");
    opening["model"]         = model;
    opening["content"]       = QJsonArray();
    opening["stop_reason"]   = QJsonValue();
    opening["stop_sequence"] = QJsonValue();
    opening["usage"]         = anthropicUsage(1);

    QJsonObject start;
    start["type"]    = QStringLiteral("message_start");
    start["message"] = opening;

    QJsonObject emptyBlock = block;
    QJsonObject delta;
    if (emitTool) {
        emptyBlock["input"]   = QJsonObject();
        delta["type"]         = QStringLiteral("input_json_delta");
        delta["partial_json"] = QString::fromUtf8(compactJson(config.toolArgs));
    } else {
        emptyBlock["text"] = QString();
        delta["type"]      = QStringLiteral("text_delta");
        delta["text"]      = QString::fromUtf8(config.reply);
    }

    QJsonObject blockStart;
    blockStart["type"]          = QStringLiteral("content_block_start");
    blockStart["index"]         = 0;
    blockStart["content_block"] = emptyBlock;

    QJsonObject blockDelta;
    blockDelta["type"]  = QStringLiteral("content_block_delta");
    blockDelta["index"] = 0;
    blockDelta["delta"] = delta;

    QJsonObject blockStop;
    blockStop["type"]  = QStringLiteral("content_block_stop");
    blockStop["index"] = 0;

    QJsonObject stopDelta;
    stopDelta["stop_reason"]   = stopReason;
    stopDelta["stop_sequence"] = QJsonValue();
    QJsonObject messageDelta;
    messageDelta["type"]  = QStringLiteral("message_delta");
    messageDelta["delta"] = stopDelta;
    /* Cumulative, not incremental. */
    messageDelta["usage"] = anthropicUsage(1);

    QJsonObject messageStop;
    messageStop["type"] = QStringLiteral("message_stop");

    writeNamedSse(
        socket,
        {{"message_start", start},
         {"content_block_start", blockStart},
         {"content_block_delta", blockDelta},
         {"content_block_stop", blockStop},
         {"message_delta", messageDelta},
         {"message_stop", messageStop}});
}

void respondCountTokens(QTcpSocket *socket, const QJsonObject &request)
{
    const QByteArray encoded = compactJson(request);
    QJsonObject      payload;
    payload["input_tokens"] = int(encoded.size() / 4) + 1;
    writeBody(socket, 200, "application/json", compactJson(payload));
}

void respondModels(QTcpSocket *socket, Wire wire)
{
    QJsonArray  data;
    QJsonObject model;
    if (wire == Wire::Anthropic) {
        model["id"]           = QStringLiteral("claude-mock");
        model["type"]         = QStringLiteral("model");
        model["display_name"] = QStringLiteral("Claude Mock");
        model["created_at"]   = QStringLiteral("2026-01-01T00:00:00Z");
        data.append(model);
        QJsonObject payload;
        payload["data"]     = data;
        payload["has_more"] = false;
        payload["first_id"] = QStringLiteral("claude-mock");
        payload["last_id"]  = QStringLiteral("claude-mock");
        writeBody(socket, 200, "application/json", compactJson(payload));
        return;
    }
    model["id"]       = QStringLiteral("mock");
    model["object"]   = QStringLiteral("model");
    model["created"]  = 0;
    model["owned_by"] = QStringLiteral("qsoc");
    data.append(model);
    QJsonObject payload;
    payload["object"] = QStringLiteral("list");
    payload["data"]   = data;
    writeBody(socket, 200, "application/json", compactJson(payload));
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

    const int        pathStart = space + 1;
    const int        pathEnd   = head.indexOf(' ', pathStart);
    const QByteArray path      = pathEnd > pathStart ? head.mid(pathStart, pathEnd - pathStart)
                                                     : QByteArray();
    const Wire       wire      = wireOf(head);

    connection->served = true;
    if (method == "POST") {
        if (path.startsWith("/v1/messages/count_tokens")) {
            respondCountTokens(socket, parseBody(body.left(contentLength)));
        } else if (path.startsWith("/v1/messages")) {
            const QJsonObject request = parseBody(body.left(contentLength));
            appendRequestLog(request);
            if (shouldFail()) {
                ++hits["fail"];
                respondFailure(socket, Wire::Anthropic);
                return;
            }
            ++hits["200_text"];
            const bool streaming = request.value("stream").toBool();
            if (!streaming) {
                ++hits["200_sync"];
            }
            respondAnthropicMessages(socket, request, streaming);
        } else {
            respondPost(socket, body.left(contentLength), wire);
        }
    } else if (method == "GET") {
        /* /v1/models means different documents to the two APIs, so it must be
         * answered before the catch-all counters. */
        if (path.startsWith("/v1/models")) {
            respondModels(socket, wire);
        } else {
            respondGet(socket);
        }
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
