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
    QByteArray  hold;
    int         holdMax       = 0;
    qsizetype   overflowBytes = 0;
    QJsonArray  script;
    QString     failMode = QStringLiteral("none");
    QString     reasoning;
    QStringList reasoningFields;
    QByteArray  streamError;
    QByteArray  tokenize;
    int         tokenizeCount = -1;
    int         promptTokens  = -1;
};

MockConfig config;

/* Counters reported by GET. Single threaded, so no lock. */
QHash<QByteArray, int> hits{
    {"fail", 0},
    {"200_toolcalls", 0},
    {"200_text", 0},
    {"200_sync", 0},
    {"held", 0},
    {"overflow", 0},
    {"alpn_h2", 0},
    {"alpn_http1", 0},
    {"alpn_none", 0},
    {"tokenize", 0},
    {"tokenize_auth", 0},
    {"count_tokens", 0},
    {"count_tokens_auth", 0},
};

int             emitted = 0;
QHash<int, int> scriptSteps;
QElapsedTimer   firstRequest;

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
    config.hold              = qgetenv("MOCK_HOLD");
    config.holdMax           = envInt("MOCK_HOLD_MAX", 0);
    config.overflowBytes     = envInt("MOCK_OVERFLOW_BYTES", 0);
    config.reasoning         = QString::fromUtf8(qgetenv("MOCK_REASONING"));
    config.reasoningFields   = QString::fromLatin1(envBytes("MOCK_REASONING_FIELD", "reasoning"))
                                   .split(QLatin1Char(','), Qt::SkipEmptyParts);
    config.streamError       = qgetenv("MOCK_STREAM_ERROR");
    config.tokenize          = envBytes("MOCK_TOKENIZE", "ok");
    config.tokenizeCount     = envInt("MOCK_TOKENIZE_COUNT", -1);
    config.promptTokens      = envInt("MOCK_PROMPT_TOKENS", -1);
    const QString scriptPath = QString::fromLocal8Bit(qgetenv("MOCK_SCRIPT"));
    if (!scriptPath.isEmpty()) {
        QFile file(scriptPath);
        if (!file.open(QIODevice::ReadOnly)) {
            *error = QStringLiteral("Cannot read MOCK_SCRIPT");
            return false;
        }
        const auto document = QJsonDocument::fromJson(file.readAll());
        if (!document.isArray()) {
            *error = QStringLiteral("MOCK_SCRIPT must contain a JSON array");
            return false;
        }
        config.script = document.array();
    }

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

QJsonObject scriptedReply(const QJsonObject &request)
{
    QString firstUser;
    for (const auto &value : request.value(QStringLiteral("messages")).toArray()) {
        const auto message = value.toObject();
        if (message.value(QStringLiteral("role")).toString() == QStringLiteral("user")) {
            firstUser = message.value(QStringLiteral("content")).toString();
            break;
        }
    }
    for (int i = 0; i < config.script.size(); ++i) {
        const auto    route     = config.script.at(i).toObject();
        const QString match     = route.value(QStringLiteral("contains")).toString();
        const auto    responses = route.value(QStringLiteral("responses")).toArray();
        if (match.isEmpty() || !firstUser.contains(match) || responses.isEmpty())
            continue;
        const int step                                             = scriptSteps[i]++;
        hits[QByteArrayLiteral("script_") + QByteArray::number(i)] = step + 1;
        return responses.at(qMin(step, int(responses.size()) - 1)).toObject();
    }
    return {};
}

QJsonArray scriptedToolCalls(const QJsonObject &response)
{
    QJsonArray calls;
    for (const auto &value : response.value(QStringLiteral("tools")).toArray()) {
        const auto        tool = value.toObject();
        const QJsonObject function{
            {QStringLiteral("name"), tool.value(QStringLiteral("name"))},
            {QStringLiteral("arguments"),
             QString::fromUtf8(compactJson(tool.value(QStringLiteral("arguments")).toObject()))}};
        const int index = calls.size();
        calls.append(
            QJsonObject{
                {QStringLiteral("index"), index},
                {QStringLiteral("id"), QStringLiteral("script_%1_%2").arg(emitted).arg(index)},
                {QStringLiteral("type"), QStringLiteral("function")},
                {QStringLiteral("function"), function}});
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
    /* MOCK_PROMPT_TOKENS reports usage on the closing chunk. */
    if (!finishReason.isNull() && config.promptTokens >= 0) {
        chunk["usage"] = QJsonObject{
            {"prompt_tokens", config.promptTokens},
            {"completion_tokens", 1},
            {"total_tokens", config.promptTokens + 1}};
    }
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
void respondFailure(
    QTcpSocket    *socket,
    Wire           wire,
    int            code    = config.failCode,
    const QString &message = QStringLiteral("rate limited"),
    const QString &type    = QStringLiteral("rate_limit_error"))
{
    QJsonObject error;
    error["message"] = message;
    error["type"]    = code == 529 ? QStringLiteral("overloaded_error") : type;
    QJsonObject payload;
    payload["error"] = error;
    if (wire == Wire::Anthropic) {
        payload["type"]       = QStringLiteral("error");
        payload["request_id"] = QStringLiteral("req_mock");
    }
    const QByteArray encoded = compactJson(payload);
    writeHead(
        socket,
        code,
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

    /* MOCK_HOLD stalls matching requests: the connection stays open and no
     * reply is ever sent, so only a client timeout or cancel ends it. */
    if (!config.hold.isEmpty() && body.contains(config.hold)
        && (config.holdMax <= 0 || hits["held"] < config.holdMax)) {
        ++hits["held"];
        return;
    }

    /* MOCK_OVERFLOW_BYTES rejects a larger streaming body as too long. */
    if (streaming && config.overflowBytes > 0 && body.size() > config.overflowBytes) {
        ++hits["overflow"];
        respondFailure(
            socket,
            wire,
            400,
            QStringLiteral("maximum context length exceeded"),
            QStringLiteral("invalid_request_error"));
        return;
    }

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
    const QJsonObject scripted = streaming ? scriptedReply(request) : QJsonObject();
    const bool emitTools       = scripted.isEmpty()
                                     ? wantsTools && allowed
                                     : !scripted.value(QStringLiteral("tools")).toArray().isEmpty();
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

    const QJsonArray toolCalls = scripted.isEmpty() ? (emitTools ? buildToolCalls() : QJsonArray())
                                                    : scriptedToolCalls(scripted);
    const QString    reply
        = scripted.value(QStringLiteral("content")).toString(QString::fromUtf8(config.reply));
    const int delayMs = scripted.value(QStringLiteral("delay_ms"))
                            .toInt(static_cast<int>(config.delaySeconds * 1000));
    const QString reasoning = scripted.value(QStringLiteral("reasoning")).toString(config.reasoning);

    auto send = [socket, streaming, emitTools, toolCalls, reply, reasoning]() {
        if (socket->state() != QAbstractSocket::ConnectedState) {
            return;
        }
        if (!streaming) {
            QJsonObject message;
            message["role"]      = QStringLiteral("assistant");
            QString finishReason = QStringLiteral("stop");
            if (!reasoning.isEmpty()) {
                for (const QString &field : config.reasoningFields) {
                    message[field] = reasoning;
                }
            }
            if (emitTools) {
                message["content"]    = QJsonValue();
                message["tool_calls"] = toolCalls;
                finishReason          = QStringLiteral("tool_calls");
            } else {
                message["content"] = reply;
            }
            QJsonObject choice;
            choice["index"]         = 0;
            choice["message"]       = message;
            choice["finish_reason"] = finishReason;
            QJsonArray choices;
            choices.append(choice);
            QJsonObject usage;
            const int   prompt         = config.promptTokens >= 0 ? config.promptTokens : 1;
            usage["prompt_tokens"]     = prompt;
            usage["completion_tokens"] = 1;
            usage["total_tokens"]      = prompt + 1;
            QJsonObject payload;
            payload["choices"] = choices;
            payload["usage"]   = usage;
            writeBody(socket, 200, "application/json", compactJson(payload));
            return;
        }
        /* Thinking streams first, under every configured alias at once,
         * the way some servers mirror one text into two fields. */
        QList<QJsonObject> chunks;
        if (!reasoning.isEmpty()) {
            QJsonObject thinking;
            thinking["role"] = QStringLiteral("assistant");
            for (const QString &field : config.reasoningFields) {
                thinking[field] = reasoning;
            }
            chunks.append(deltaChunk(thinking, QString()));
        }
        QJsonObject delta;
        delta["role"] = QStringLiteral("assistant");
        if (emitTools) {
            delta["tool_calls"] = toolCalls;
            chunks.append(deltaChunk(delta, QString()));
            chunks.append(deltaChunk(QJsonObject(), QStringLiteral("tool_calls")));
        } else {
            delta["content"] = reply;
            chunks.append(deltaChunk(delta, QString()));
            chunks.append(deltaChunk(QJsonObject(), QStringLiteral("stop")));
        }
        writeSse(socket, chunks);
    };

    if (delayMs > 0) {
        QTimer::singleShot(delayMs, socket, send);
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
    usage["input_tokens"]                = config.promptTokens >= 0 ? config.promptTokens : 1;
    usage["output_tokens"]               = outputTokens;
    usage["cache_creation_input_tokens"] = 0;
    usage["cache_read_input_tokens"]     = 0;
    return usage;
}

QJsonObject anthropicEvent(const char *type, int index, const char *key, const QJsonObject &value)
{
    QJsonObject event;
    event["type"]                   = QString::fromLatin1(type);
    event["index"]                  = index;
    event[QString::fromLatin1(key)] = value;
    return event;
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
    const bool    emitTool = !config.toolName.isEmpty()
                             && (config.toolMax <= 0 || emitted < config.toolMax);
    if (emitTool) {
        ++emitted;
    }
    ++hits[emitTool ? "200_toolcalls" : "200_text"];

    /* Thinking first, signed, then the answer or the tool call. */
    QList<QJsonObject> blocks;
    if (!config.reasoning.isEmpty()) {
        blocks.append(
            {{"type", "thinking"},
             {"thinking", config.reasoning},
             {"signature", QStringLiteral("mock-signature-%1").arg(emitted)}});
    }
    if (emitTool) {
        blocks.append(
            {{"type", "tool_use"},
             {"id", QStringLiteral("toolu_%1").arg(emitted)},
             {"name", QString::fromUtf8(config.toolName)},
             {"input", config.toolArgs}});
    } else {
        blocks.append({{"type", "text"}, {"text", QString::fromUtf8(config.reply)}});
    }
    const QString stopReason = emitTool ? QStringLiteral("tool_use") : QStringLiteral("end_turn");

    if (!streaming) {
        QJsonArray content;
        for (const QJsonObject &block : blocks) {
            content.append(block);
        }
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

    QList<QPair<QByteArray, QJsonObject>> events{{"message_start", start}};
    if (!config.streamError.isEmpty()) {
        /* A mid-stream failure arrives as an error event on a 200 reply. */
        QJsonObject error;
        error["type"]    = QString::fromUtf8(config.streamError);
        error["message"] = QStringLiteral("Overloaded");
        QJsonObject event;
        event["type"]  = QStringLiteral("error");
        event["error"] = error;
        events.append({"error", event});
        writeNamedSse(socket, events);
        return;
    }

    /* Every delta arrives in two halves, the way servers split them. */
    const auto halves = [](const QString &text) {
        return QStringList{text.left(text.size() / 2), text.mid(text.size() / 2)};
    };
    for (int index = 0; index < blocks.size(); ++index) {
        const QJsonObject &block = blocks.at(index);
        const QString      type  = block.value("type").toString();
        QJsonObject        empty = block;
        if (type == QLatin1String("thinking")) {
            empty["thinking"]  = QString();
            empty["signature"] = QString();
        } else if (type == QLatin1String("tool_use")) {
            empty["input"] = QJsonObject();
        } else {
            empty["text"] = QString();
        }
        events.append(
            {"content_block_start",
             anthropicEvent("content_block_start", index, "content_block", empty)});
        QStringList parts;
        QString     deltaType;
        QString     field;
        if (type == QLatin1String("thinking")) {
            parts     = halves(block.value("thinking").toString());
            deltaType = QStringLiteral("thinking_delta");
            field     = QStringLiteral("thinking");
        } else if (type == QLatin1String("tool_use")) {
            parts     = halves(QString::fromUtf8(compactJson(block.value("input").toObject())));
            deltaType = QStringLiteral("input_json_delta");
            field     = QStringLiteral("partial_json");
        } else {
            parts     = halves(block.value("text").toString());
            deltaType = QStringLiteral("text_delta");
            field     = QStringLiteral("text");
        }
        for (const QString &part : parts) {
            const QJsonObject delta{{"type", deltaType}, {field, part}};
            events.append(
                {"content_block_delta",
                 anthropicEvent("content_block_delta", index, "delta", delta)});
        }
        if (type == QLatin1String("thinking")) {
            const QJsonObject delta{
                {"type", QStringLiteral("signature_delta")},
                {"signature", block.value("signature")}};
            events.append(
                {"content_block_delta",
                 anthropicEvent("content_block_delta", index, "delta", delta)});
        }
        QJsonObject stop;
        stop["type"]  = QStringLiteral("content_block_stop");
        stop["index"] = index;
        events.append({"content_block_stop", stop});
        QJsonObject ping;
        ping["type"] = QStringLiteral("ping");
        events.append({"ping", ping});
    }

    QJsonObject stopDelta;
    stopDelta["stop_reason"]   = stopReason;
    stopDelta["stop_sequence"] = QJsonValue();
    QJsonObject messageDelta;
    messageDelta["type"]  = QStringLiteral("message_delta");
    messageDelta["delta"] = stopDelta;
    /* Cumulative, not incremental. */
    messageDelta["usage"] = anthropicUsage(1);
    events.append({"message_delta", messageDelta});
    QJsonObject messageStop;
    messageStop["type"] = QStringLiteral("message_stop");
    events.append({"message_stop", messageStop});
    writeNamedSse(socket, events);
}

/**
 * @brief vLLM /tokenize in chat form, or Messages count_tokens.
 *
 * MOCK_TOKENIZE picks the reply for both: ok, 404, 500, 429, html or hold
 * (never answers). MOCK_TOKENIZE_COUNT fixes the count; otherwise bytes / 4.
 */
void respondTokenize(
    QTcpSocket *socket, const QByteArray &head, const QByteArray &body, bool anthropic = false)
{
    const QByteArray counter = anthropic ? "count_tokens" : "tokenize";
    const QByteArray lowered = head.toLower();
    ++hits[counter];
    if (lowered.contains("\nauthorization:") || lowered.contains("\nx-api-key:")) {
        ++hits[counter + "_auth"];
    }
    appendRequestLog(parseBody(body));
    const QByteArray mode = config.tokenize;
    if (mode == "hold") {
        return;
    }
    if (mode == "html") {
        writeBody(socket, 200, "text/html", "<html><body>gateway</body></html>");
        return;
    }
    if (mode == "404" || mode == "500" || mode == "429") {
        writeBody(socket, mode.toInt(), "application/json", R"({"error":"unavailable"})");
        return;
    }
    const int   count = config.tokenizeCount >= 0 ? config.tokenizeCount : int(body.size() / 4);
    QJsonObject payload;
    if (anthropic) {
        payload["input_tokens"] = count;
    } else {
        payload["count"]         = count;
        payload["max_model_len"] = 131072;
        payload["tokens"]        = QJsonArray();
    }
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
        if (path.endsWith("/tokenize")) {
            respondTokenize(socket, head, body.left(contentLength));
        } else if (path.startsWith("/v1/messages/count_tokens")) {
            respondTokenize(socket, head, body.left(contentLength), true);
        } else if (path.startsWith("/v1/messages")) {
            const QJsonObject request = parseBody(body.left(contentLength));
            appendRequestLog(request);
            if (wire != Wire::Anthropic) {
                respondFailure(
                    socket,
                    Wire::Anthropic,
                    400,
                    QStringLiteral("anthropic-version: header required"),
                    QStringLiteral("invalid_request_error"));
                return;
            }
            if (config.overflowBytes > 0 && contentLength > config.overflowBytes) {
                ++hits["overflow"];
                respondFailure(
                    socket,
                    Wire::Anthropic,
                    400,
                    QStringLiteral("prompt is too long: %1 tokens > %2 maximum")
                        .arg(contentLength)
                        .arg(config.overflowBytes),
                    QStringLiteral("invalid_request_error"));
                return;
            }
            if (shouldFail()) {
                ++hits["fail"];
                respondFailure(socket, Wire::Anthropic);
                return;
            }
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
