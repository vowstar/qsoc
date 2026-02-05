// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "common/qllmservice.h"

#include <QDebug>
#include <QEventLoop>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>

/* Constructor and Destructor */

QLLMService::QLLMService(QObject *parent, QSocConfig *config)
    : QObject(parent)
    , networkManager_(new QNetworkAccessManager(this))
    , config_(config)
{
    loadConfigSettings();
    setupNetworkProxy();
}

QLLMService::~QLLMService() = default;

/* Configuration */

void QLLMService::setConfig(QSocConfig *config)
{
    config_ = config;
    loadConfigSettings();
    setupNetworkProxy();
}

QSocConfig *QLLMService::getConfig()
{
    return config_;
}

/* Endpoint management */

void QLLMService::addEndpoint(const LLMEndpoint &endpoint)
{
    endpoints_.append(endpoint);
}

void QLLMService::clearEndpoints()
{
    endpoints_.clear();
    currentEndpoint_ = 0;
}

int QLLMService::endpointCount() const
{
    return static_cast<int>(endpoints_.size());
}

bool QLLMService::hasEndpoint() const
{
    return !endpoints_.isEmpty();
}

void QLLMService::setFallbackStrategy(LLMFallbackStrategy strategy)
{
    fallbackStrategy_ = strategy;
}

/* LLM request methods */

LLMResponse QLLMService::sendRequest(
    const QString &prompt, const QString &systemPrompt, double temperature, bool jsonMode)
{
    if (!hasEndpoint()) {
        LLMResponse response;
        response.success      = false;
        response.errorMessage = "No LLM endpoint configured";
        return response;
    }

    /* Try endpoints with fallback */
    const int maxAttempts = static_cast<int>(endpoints_.size());
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        LLMEndpoint endpoint = selectEndpoint();

        LLMResponse response
            = sendRequestToEndpoint(endpoint, prompt, systemPrompt, temperature, jsonMode);

        if (response.success) {
            return response;
        }

        qWarning() << "Endpoint" << endpoint.name << "failed:" << response.errorMessage;
        advanceEndpoint();
    }

    LLMResponse response;
    response.success      = false;
    response.errorMessage = "All LLM endpoints failed";
    return response;
}

void QLLMService::sendRequestAsync(
    const QString                            &prompt,
    const std::function<void(LLMResponse &)> &callback,
    const QString                            &systemPrompt,
    double                                    temperature,
    bool                                      jsonMode)
{
    if (!hasEndpoint()) {
        LLMResponse response;
        response.success      = false;
        response.errorMessage = "No LLM endpoint configured";
        callback(response);
        return;
    }

    LLMEndpoint endpoint = selectEndpoint();

    QNetworkRequest request = prepareRequest(endpoint);
    json payload = buildRequestPayload(prompt, systemPrompt, temperature, jsonMode, endpoint.model);

    QNetworkReply *reply = networkManager_->post(request, QByteArray::fromStdString(payload.dump()));

    /* Set timeout */
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    timer->start(endpoint.timeout);

    connect(reply, &QNetworkReply::finished, [this, reply, callback, timer]() {
        timer->stop();
        timer->deleteLater();
        LLMResponse response = parseResponse(reply);
        reply->deleteLater();
        callback(response);
    });
}

/* Utility methods */

QMap<QString, QString> QLLMService::extractMappingsFromResponse(const LLMResponse &response)
{
    QMap<QString, QString> mappings;

    if (!response.success || response.content.isEmpty()) {
        return mappings;
    }

    const QString content = response.content.trimmed();

    /* Method 1: If the entire response is a JSON object */
    try {
        json jsonObj = json::parse(content.toStdString());
        if (jsonObj.is_object()) {
            for (auto it = jsonObj.begin(); it != jsonObj.end(); ++it) {
                if (it.value().is_string()) {
                    mappings[QString::fromStdString(it.key())] = QString::fromStdString(
                        it.value().get<std::string>());
                }
            }
            return mappings;
        }
    } catch (const json::parse_error &e) {
        qDebug() << "JSON parse error in extractMappingsFromResponse (Method 1):" << e.what();
    }

    /* Method 2: Extract JSON object from text */
    const QRegularExpression      jsonRegex(R"(\{[^\{\}]*\})");
    const QRegularExpressionMatch match = jsonRegex.match(content);

    if (match.hasMatch()) {
        const QString jsonString = match.captured(0);
        try {
            json mappingJson = json::parse(jsonString.toStdString());
            if (mappingJson.is_object()) {
                for (auto it = mappingJson.begin(); it != mappingJson.end(); ++it) {
                    if (it.value().is_string()) {
                        mappings[QString::fromStdString(it.key())] = QString::fromStdString(
                            it.value().get<std::string>());
                    }
                }
                return mappings;
            }
        } catch (const json::parse_error &e) {
            qDebug() << "JSON parse error in extractMappingsFromResponse (Method 2):" << e.what();
        }
    }

    /* Method 3: Parse from text format */
    const QStringList        lines = content.split("\n");
    const QRegularExpression mappingRegex("\"(.*?)\"\\s*:\\s*\"(.*?)\"");

    for (const QString &line : lines) {
        const QRegularExpressionMatch lineMatch = mappingRegex.match(line);
        if (lineMatch.hasMatch()) {
            const QString key   = lineMatch.captured(1);
            const QString value = lineMatch.captured(2);
            mappings[key]       = value;
        }
    }

    return mappings;
}

/* Private methods */

void QLLMService::loadConfigSettings()
{
    endpoints_.clear();
    currentEndpoint_ = 0;

    if (!config_) {
        return;
    }

    /* Load from llm.url, llm.key, llm.model */
    QString url   = config_->getValue("llm.url");
    QString key   = config_->getValue("llm.key");
    QString model = config_->getValue("llm.model");

    /* Add endpoint if URL is available */
    if (!url.isEmpty()) {
        LLMEndpoint endpoint;
        endpoint.name  = "primary";
        endpoint.url   = QUrl(url);
        endpoint.key   = key;
        endpoint.model = model;

        /* Get timeout if configured */
        QString timeoutStr = config_->getValue("llm.timeout");
        if (!timeoutStr.isEmpty()) {
            endpoint.timeout = timeoutStr.toInt();
        }

        endpoints_.append(endpoint);
    }

    /* Load fallback strategy */
    QString fallbackStr = config_->getValue("llm.fallback", "sequential").toLower();
    if (fallbackStr == "random") {
        fallbackStrategy_ = LLMFallbackStrategy::Random;
    } else if (fallbackStr == "round-robin" || fallbackStr == "roundrobin") {
        fallbackStrategy_ = LLMFallbackStrategy::RoundRobin;
    } else {
        fallbackStrategy_ = LLMFallbackStrategy::Sequential;
    }
}

void QLLMService::setupNetworkProxy()
{
    if (!config_ || !networkManager_) {
        return;
    }

    QString proxyType = config_->getValue("proxy.type", "system").toLower();

    QNetworkProxy proxy;

    if (proxyType == "none") {
        proxy.setType(QNetworkProxy::NoProxy);
    } else if (proxyType == "socks5") {
        proxy.setType(QNetworkProxy::Socks5Proxy);
        proxy.setHostName(config_->getValue("proxy.host", "127.0.0.1"));
        proxy.setPort(config_->getValue("proxy.port", "1080").toUInt());

        QString user = config_->getValue("proxy.user");
        if (!user.isEmpty()) {
            proxy.setUser(user);
            proxy.setPassword(config_->getValue("proxy.password"));
        }
    } else if (proxyType == "http") {
        proxy.setType(QNetworkProxy::HttpProxy);
        proxy.setHostName(config_->getValue("proxy.host", "127.0.0.1"));
        proxy.setPort(config_->getValue("proxy.port", "8080").toUInt());

        QString user = config_->getValue("proxy.user");
        if (!user.isEmpty()) {
            proxy.setUser(user);
            proxy.setPassword(config_->getValue("proxy.password"));
        }
    } else {
        /* Default to system proxy settings */
        QNetworkProxyFactory::setUseSystemConfiguration(true);
        networkManager_->setProxy(QNetworkProxy::DefaultProxy);
        return;
    }

    networkManager_->setProxy(proxy);
}

LLMEndpoint QLLMService::selectEndpoint()
{
    if (endpoints_.isEmpty()) {
        return {};
    }

    switch (fallbackStrategy_) {
    case LLMFallbackStrategy::Random: {
        auto index = QRandomGenerator::global()->bounded(endpoints_.size());
        return endpoints_.at(index);
    }
    case LLMFallbackStrategy::RoundRobin:
    case LLMFallbackStrategy::Sequential:
    default:
        return endpoints_.at(currentEndpoint_ % endpoints_.size());
    }
}

void QLLMService::advanceEndpoint()
{
    if (!endpoints_.isEmpty()) {
        currentEndpoint_ = (currentEndpoint_ + 1) % static_cast<int>(endpoints_.size());
    }
}

QNetworkRequest QLLMService::prepareRequest(const LLMEndpoint &endpoint) const
{
    QNetworkRequest request(endpoint.url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    /* All providers use Bearer token authentication */
    if (!endpoint.key.isEmpty()) {
        request.setRawHeader("Authorization", ("Bearer " + endpoint.key).toUtf8());
    }

    return request;
}

json QLLMService::buildRequestPayload(
    const QString &prompt,
    const QString &systemPrompt,
    double         temperature,
    bool           jsonMode,
    const QString &model) const
{
    /* Build messages array (OpenAI Chat Completions format) */
    json messages = json::array();

    /* Add system message */
    if (!systemPrompt.isEmpty()) {
        json systemMessage;
        systemMessage["role"]    = "system";
        systemMessage["content"] = systemPrompt.toStdString();
        messages.push_back(systemMessage);
    }

    /* Add user message */
    json userMessage;
    userMessage["role"]    = "user";
    userMessage["content"] = prompt.toStdString();
    messages.push_back(userMessage);

    /* Build payload */
    json payload;
    payload["messages"]    = messages;
    payload["temperature"] = temperature;
    payload["stream"]      = false;

    /* Set model if provided */
    if (!model.isEmpty()) {
        payload["model"] = model.toStdString();
    }

    /* Request JSON format if needed */
    if (jsonMode) {
        payload["response_format"] = {{"type", "json_object"}};
    }

    return payload;
}

LLMResponse QLLMService::parseResponse(QNetworkReply *reply) const
{
    LLMResponse response;

    if (reply->error() != QNetworkReply::NoError) {
        response.success           = false;
        response.errorMessage      = reply->errorString();
        const QByteArray errorData = reply->readAll();
        qWarning() << "LLM API request failed:" << reply->errorString();
        qWarning() << "Error response:" << errorData;
        return response;
    }

    const QByteArray responseData = reply->readAll();

    try {
        json jsonResponse = json::parse(responseData.toStdString());
        response.success  = true;
        response.jsonData = jsonResponse;

        /* Parse OpenAI Chat Completions format */
        if (jsonResponse.contains("choices") && jsonResponse["choices"].is_array()
            && !jsonResponse["choices"].empty()) {
            auto choice = jsonResponse["choices"][0];
            if (choice.contains("message") && choice["message"].contains("content")) {
                response.content = QString::fromStdString(
                    choice["message"]["content"].get<std::string>());
            } else if (choice.contains("text")) {
                /* Handle streaming response format */
                response.content = QString::fromStdString(choice["text"].get<std::string>());
            }
        }

        /* If content is empty but we have valid JSON, return formatted JSON */
        if (response.content.isEmpty() && !jsonResponse.empty()) {
            response.content = QString::fromStdString(jsonResponse.dump(2));
        }

    } catch (const json::parse_error &e) {
        response.success      = false;
        response.errorMessage = QString("JSON parse error: %1").arg(e.what());
        qWarning() << "JSON parse error:" << e.what();
        qWarning() << "Raw response:" << responseData;
    }

    return response;
}

LLMResponse QLLMService::sendRequestToEndpoint(
    const LLMEndpoint &endpoint,
    const QString     &prompt,
    const QString     &systemPrompt,
    double             temperature,
    bool               jsonMode)
{
    QNetworkRequest request = prepareRequest(endpoint);
    json payload = buildRequestPayload(prompt, systemPrompt, temperature, jsonMode, endpoint.model);

    QEventLoop loop;
    QNetworkReply *reply = networkManager_->post(request, QByteArray::fromStdString(payload.dump()));

    /* Set timeout */
    QTimer timer;
    timer.setSingleShot(true);
    connect(&timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    timer.start(endpoint.timeout);

    connect(reply, SIGNAL(finished()), &loop, SLOT(quit()));
    loop.exec();

    timer.stop();

    LLMResponse response = parseResponse(reply);
    reply->deleteLater();

    return response;
}

void QLLMService::sendChatCompletionStream(
    const json &messages, const json &tools, double temperature)
{
    if (!hasEndpoint()) {
        emit streamError("No LLM endpoint configured");
        return;
    }

    LLMEndpoint     endpoint = selectEndpoint();
    QNetworkRequest request  = prepareRequest(endpoint);

    /* Build payload with streaming enabled */
    json payload;
    payload["messages"]    = messages;
    payload["temperature"] = temperature;
    payload["stream"]      = true;

    if (!endpoint.model.isEmpty()) {
        payload["model"] = endpoint.model.toStdString();
    }

    if (!tools.empty()) {
        payload["tools"] = tools;
    }

    /* Reset streaming state */
    streamBuffer_.clear();
    streamAccumulatedContent_.clear();
    streamAccumulatedToolCalls_.clear();

    currentStreamReply_ = networkManager_->post(request, QByteArray::fromStdString(payload.dump()));

    /* Set timeout */
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, timer]() {
        timer->deleteLater();
        if (currentStreamReply_) {
            currentStreamReply_->abort();
            emit streamError("Request timeout");
        }
    });
    timer->start(endpoint.timeout);

    /* Handle incoming data */
    connect(currentStreamReply_, &QNetworkReply::readyRead, this, [this]() {
        if (!currentStreamReply_) {
            return;
        }

        streamBuffer_ += QString::fromUtf8(currentStreamReply_->readAll());

        /* Process complete SSE lines */
        while (true) {
            int lineEnd = streamBuffer_.indexOf('\n');
            if (lineEnd == -1) {
                break;
            }

            QString line  = streamBuffer_.left(lineEnd).trimmed();
            streamBuffer_ = streamBuffer_.mid(lineEnd + 1);

            /* Skip empty lines */
            if (line.isEmpty()) {
                continue;
            }

            /* Parse SSE data lines */
            if (line.startsWith("data: ")) {
                QString data = line.mid(6);

                bool isDone
                    = parseStreamLine(data, streamAccumulatedContent_, streamAccumulatedToolCalls_);

                if (isDone) {
                    json response
                        = buildStreamResponse(streamAccumulatedContent_, streamAccumulatedToolCalls_);
                    emit streamComplete(response);
                }
            }
        }
    });

    /* Handle completion */
    connect(currentStreamReply_, &QNetworkReply::finished, this, [this, timer]() {
        timer->stop();
        timer->deleteLater();

        if (!currentStreamReply_) {
            return;
        }

        if (currentStreamReply_->error() != QNetworkReply::NoError
            && currentStreamReply_->error() != QNetworkReply::OperationCanceledError) {
            emit streamError(currentStreamReply_->errorString());
        }

        currentStreamReply_->deleteLater();
        currentStreamReply_ = nullptr;
    });
}

bool QLLMService::parseStreamLine(
    const QString &line, QString &accumulatedContent, QMap<int, json> &accumulatedToolCalls)
{
    /* Check for stream end */
    if (line == "[DONE]") {
        return true;
    }

    /* Parse JSON */
    try {
        json chunk = json::parse(line.toStdString());

        if (!chunk.contains("choices") || chunk["choices"].empty()) {
            return false;
        }

        auto delta = chunk["choices"][0]["delta"];

        /* Handle content chunks */
        if (delta.contains("content") && delta["content"].is_string()) {
            QString content = QString::fromStdString(delta["content"].get<std::string>());
            accumulatedContent += content;
            emit streamChunk(content);
        }

        /* Handle tool calls */
        if (delta.contains("tool_calls")) {
            for (const auto &toolCall : delta["tool_calls"]) {
                int index = toolCall.value("index", 0);

                /* Initialize tool call entry if needed */
                if (!accumulatedToolCalls.contains(index)) {
                    accumulatedToolCalls[index]
                        = {{"id", ""},
                           {"type", "function"},
                           {"function", {{"name", ""}, {"arguments", ""}}}};
                }

                /* Update ID if present */
                if (toolCall.contains("id")) {
                    accumulatedToolCalls[index]["id"] = toolCall["id"];
                }

                /* Update function info */
                if (toolCall.contains("function")) {
                    auto &accFunc = accumulatedToolCalls[index]["function"];

                    if (toolCall["function"].contains("name")) {
                        accFunc["name"] = toolCall["function"]["name"];
                    }

                    if (toolCall["function"].contains("arguments")) {
                        std::string args = accFunc["arguments"].get<std::string>();
                        args += toolCall["function"]["arguments"].get<std::string>();
                        accFunc["arguments"] = args;
                    }
                }

                /* Emit signal with current state */
                QString toolId = QString::fromStdString(
                    accumulatedToolCalls[index]["id"].get<std::string>());
                QString funcName = QString::fromStdString(
                    accumulatedToolCalls[index]["function"]["name"].get<std::string>());
                QString funcArgs = QString::fromStdString(
                    accumulatedToolCalls[index]["function"]["arguments"].get<std::string>());

                emit streamToolCall(toolId, funcName, funcArgs);
            }
        }

        /* Check for finish reason */
        if (chunk["choices"][0].contains("finish_reason")
            && !chunk["choices"][0]["finish_reason"].is_null()) {
            return true;
        }

    } catch (const json::parse_error &err) {
        qWarning() << "Failed to parse stream chunk:" << err.what();
    }

    return false;
}

json QLLMService::buildStreamResponse(const QString &content, const QMap<int, json> &toolCalls) const
{
    json message;
    message["role"] = "assistant";

    if (!content.isEmpty()) {
        message["content"] = content.toStdString();
    }

    if (!toolCalls.isEmpty()) {
        json toolCallsArray = json::array();
        for (auto iter = toolCalls.constBegin(); iter != toolCalls.constEnd(); ++iter) {
            toolCallsArray.push_back(iter.value());
        }
        message["tool_calls"] = toolCallsArray;
    }

    return {{"choices", json::array({{{"message", message}}})}};
}

json QLLMService::sendChatCompletion(const json &messages, const json &tools, double temperature)
{
    if (!hasEndpoint()) {
        return {{"error", "No LLM endpoint configured"}};
    }

    /* Try endpoints with fallback */
    const int maxAttempts = static_cast<int>(endpoints_.size());
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        LLMEndpoint endpoint = selectEndpoint();

        QNetworkRequest request = prepareRequest(endpoint);

        /* Build payload with messages and tools */
        json payload;
        payload["messages"]    = messages;
        payload["temperature"] = temperature;
        payload["stream"]      = false;

        if (!endpoint.model.isEmpty()) {
            payload["model"] = endpoint.model.toStdString();
        }

        /* Add tools if provided */
        if (!tools.empty()) {
            payload["tools"] = tools;
        }

        QEventLoop     loop;
        QNetworkReply *reply
            = networkManager_->post(request, QByteArray::fromStdString(payload.dump()));

        /* Set timeout */
        QTimer timer;
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, reply, &QNetworkReply::abort);
        timer.start(endpoint.timeout);

        connect(reply, SIGNAL(finished()), &loop, SLOT(quit()));
        loop.exec();

        timer.stop();

        if (reply->error() != QNetworkReply::NoError) {
            qWarning() << "Endpoint" << endpoint.name << "failed:" << reply->errorString();
            reply->deleteLater();
            advanceEndpoint();
            continue;
        }

        const QByteArray responseData = reply->readAll();
        reply->deleteLater();

        try {
            return json::parse(responseData.toStdString());
        } catch (const json::parse_error &e) {
            qWarning() << "JSON parse error:" << e.what();
            advanceEndpoint();
            continue;
        }
    }

    return {{"error", "All LLM endpoints failed"}};
}
