// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "common/qllmservice.h"
#include "common/qsocconsole.h"
#include "common/qsocproxy.h"

#include <algorithm>
#include <QDebug>
#include <QEventLoop>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QTimer>

/* Constructor and Destructor */

QLLMService::QLLMService(QObject *parent, QSocConfig *config)
    : QObject(parent)
    , networkManager(new QNetworkAccessManager(this))
    , config(config)
{
    loadConfigSettings();
    setupNetworkProxy();
}

QLLMService::~QLLMService()
{
    /* Tear down any in-flight stream so its slots can't fire on a
     * partially-destroyed `this`. */
    if (currentStreamReply != nullptr) {
        disconnect(currentStreamReply, nullptr, this, nullptr);
        currentStreamReply->abort();
        currentStreamReply->deleteLater();
        currentStreamReply = nullptr;
    }
    /* Best-effort wipe of API-key buffers. detach() forces COW
     * uniqueness so we zero our own copy, not a shared one. */
    auto wipe = [](QString &str) {
        if (str.isEmpty()) {
            return;
        }
        str.detach();
        std::fill(str.begin(), str.end(), QChar(u'\0'));
        str.clear();
    };
    for (auto &endpoint : endpoints) {
        wipe(endpoint.key);
    }
    for (auto &model : modelConfigs) {
        wipe(model.key);
    }
}

QLLMService *QLLMService::clone(QObject *parent) const
{
    /* The constructor calls loadConfigSettings(), which re-parses
     * endpoints + modelConfigs from the same QSocConfig. That gives
     * the clone its OWN copies of those structures (and own QNAM,
     * own streaming state). Then we align the clone with the
     * parent's currently selected model and fallback strategy. */
    auto *child             = new QLLMService(parent, config);
    child->fallbackStrategy = fallbackStrategy;
    if (!currentModelId.isEmpty()) {
        /* setCurrentModel keeps endpoint index + currentModelId in
         * sync against modelConfigs, so prefer it over manual copies. */
        child->setCurrentModel(currentModelId);
    } else {
        child->currentEndpoint = currentEndpoint;
    }
    return child;
}

/* Configuration */

void QLLMService::setConfig(QSocConfig *config)
{
    this->config = config;
    loadConfigSettings();
    setupNetworkProxy();
}

QSocConfig *QLLMService::getConfig()
{
    return config;
}

/* Endpoint management */

void QLLMService::addEndpoint(const LLMEndpoint &endpoint)
{
    endpoints.append(endpoint);
}

void QLLMService::clearEndpoints()
{
    endpoints.clear();
    currentEndpoint = 0;
}

int QLLMService::endpointCount() const
{
    return static_cast<int>(endpoints.size());
}

bool QLLMService::hasEndpoint() const
{
    return !endpoints.isEmpty();
}

QStringList QLLMService::availableModels() const
{
    return modelConfigs.keys();
}

LLMModelConfig QLLMService::getModelConfig(const QString &modelId) const
{
    return modelConfigs.value(modelId, LLMModelConfig());
}

QString QLLMService::getCurrentModelId() const
{
    return currentModelId;
}

LLMModelConfig QLLMService::getCurrentModelConfig() const
{
    return modelConfigs.value(currentModelId, LLMModelConfig());
}

bool QLLMService::currentSupportsImage() const
{
    return getCurrentModelConfig().acceptsImage;
}

bool QLLMService::setCurrentModel(const QString &modelId)
{
    if (!modelConfigs.contains(modelId)) {
        return false;
    }

    currentModelId                  = modelId;
    const LLMModelConfig &modelConf = modelConfigs[modelId];

    /* Rebuild primary endpoint from model config */
    endpoints.clear();
    currentEndpoint = 0;

    LLMEndpoint endpoint;
    endpoint.name            = modelConf.name.isEmpty() ? modelConf.id : modelConf.name;
    endpoint.url             = QUrl(modelConf.url);
    endpoint.key             = modelConf.key;
    endpoint.model           = modelConf.id;
    endpoint.timeout         = modelConf.timeout;
    endpoint.maxOutputTokens = modelConf.maxOutputTokens;
    endpoints.append(endpoint);

    return true;
}

void QLLMService::setFallbackStrategy(LLMFallbackStrategy strategy)
{
    fallbackStrategy = strategy;
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
    const int maxAttempts = static_cast<int>(endpoints.size());
    for (int attempt = 0; attempt < maxAttempts; ++attempt) {
        LLMEndpoint endpoint = selectEndpoint();

        LLMResponse response
            = sendRequestToEndpoint(endpoint, prompt, systemPrompt, temperature, jsonMode);

        if (response.success) {
            return response;
        }

        QSocConsole::warn() << "Endpoint" << endpoint.name << "failed:" << response.errorMessage;
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

    QNetworkReply *reply = networkManager->post(request, QByteArray::fromStdString(payload.dump()));

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
        QSocConsole::debug() << "JSON parse error in extractMappingsFromResponse (Method 1):"
                             << e.what();
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
            QSocConsole::debug() << "JSON parse error in extractMappingsFromResponse (Method 2):"
                                 << e.what();
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
    endpoints.clear();
    currentEndpoint = 0;
    modelConfigs.clear();
    defaultModelId.clear();
    currentModelId.clear();

    if (!config) {
        return;
    }

    /* Load model registry from llm.models (YAML 3-level nesting) */
    YAML::Node modelsNode = config->getYamlNode("llm.models");
    if (modelsNode.IsDefined() && modelsNode.IsMap()) {
        for (const auto &item : modelsNode) {
            try {
                LLMModelConfig modelCfg;
                modelCfg.id = QString::fromStdString(item.first.as<std::string>());

                YAML::Node node = item.second;
                if (node["name"]) {
                    modelCfg.name = QString::fromStdString(node["name"].as<std::string>());
                }
                if (node["url"]) {
                    modelCfg.url = QString::fromStdString(node["url"].as<std::string>());
                }
                if (node["key"]) {
                    modelCfg.key = QString::fromStdString(node["key"].as<std::string>());
                }
                if (node["timeout"]) {
                    modelCfg.timeout = node["timeout"].as<int>();
                }
                if (node["context"]) {
                    modelCfg.contextTokens = node["context"].as<int>();
                }
                if (node["max_output_tokens"]) {
                    modelCfg.maxOutputTokens = node["max_output_tokens"].as<int>();
                }
                if (node["reasoning"]) {
                    modelCfg.reasoning = node["reasoning"].as<bool>();
                }
                if (node["effort"]) {
                    modelCfg.effort = QString::fromStdString(node["effort"].as<std::string>());
                }

                /* Modality block: opt-in only. Absent or non-map -> all
                 * defaults (text-only). The block's keys are flat
                 * because the only modality we currently route is image;
                 * audio / video / pdf would extend this map. */
                if (node["modalities"] && node["modalities"].IsMap()) {
                    YAML::Node mod = node["modalities"];
                    if (mod["image"]) {
                        modelCfg.acceptsImage = mod["image"].as<bool>();
                    }
                    if (mod["image_max_tokens"]) {
                        modelCfg.imageMaxTokens = mod["image_max_tokens"].as<int>();
                    }
                    if (mod["image_max_dimension"]) {
                        modelCfg.imageMaxDimension = mod["image_max_dimension"].as<int>();
                    }
                    if (mod["image_max_bytes"]) {
                        modelCfg.imageMaxBytes = mod["image_max_bytes"].as<int>();
                    }
                    if (mod["image_provider_hint"]) {
                        modelCfg.imageProviderHint = QString::fromStdString(
                            mod["image_provider_hint"].as<std::string>());
                    }
                }

                modelConfigs[modelCfg.id] = modelCfg;
            } catch (const YAML::Exception &err) {
                QSocConsole::warn() << "Failed to parse model config:" << err.what();
            }
        }
    }

    /* Select default model */
    defaultModelId = config->getValue("llm.model");

    if (!defaultModelId.isEmpty() && modelConfigs.contains(defaultModelId)) {
        /* Use model registry */
        setCurrentModel(defaultModelId);
    } else if (!modelConfigs.isEmpty()) {
        /* Fall back to first model in registry */
        setCurrentModel(modelConfigs.firstKey());
    } else {
        /* Legacy: load from flat llm.url/llm.key/llm.model */
        QString url   = config->getValue("llm.url");
        QString key   = config->getValue("llm.key");
        QString model = config->getValue("llm.model");

        if (!url.isEmpty()) {
            LLMEndpoint endpoint;
            endpoint.name  = "primary";
            endpoint.url   = QUrl(url);
            endpoint.key   = key;
            endpoint.model = model;

            QString timeoutStr = config->getValue("llm.timeout");
            if (!timeoutStr.isEmpty()) {
                endpoint.timeout = timeoutStr.toInt();
            }

            QString maxOutputStr = config->getValue("llm.max_output_tokens");
            if (!maxOutputStr.isEmpty()) {
                endpoint.maxOutputTokens = maxOutputStr.toInt();
            }

            endpoints.append(endpoint);
        }
    }

    /* Load fallback strategy */
    QString fallbackStr = config->getValue("llm.fallback", "sequential").toLower();
    if (fallbackStr == "random") {
        fallbackStrategy = LLMFallbackStrategy::Random;
    } else if (fallbackStr == "round-robin" || fallbackStr == "roundrobin") {
        fallbackStrategy = LLMFallbackStrategy::RoundRobin;
    } else {
        fallbackStrategy = LLMFallbackStrategy::Sequential;
    }
}

void QLLMService::setupNetworkProxy()
{
    if (!networkManager) {
        return;
    }
    /* Honour the qsoc-wide spec; per-endpoint LLM `proxy:` overrides
     * are applied per-request when an endpoint carries one. The system
     * bootstrap is owned by main.cpp / QSocProxy so DefaultProxy still
     * resolves to the env / libproxy proxy when nothing is configured. */
    QSocProxy::apply(networkManager, QSocProxy::fromLegacyConfig(config));
}

LLMEndpoint QLLMService::selectEndpoint()
{
    if (endpoints.isEmpty()) {
        return {};
    }

    switch (fallbackStrategy) {
    case LLMFallbackStrategy::Random: {
        auto index = QRandomGenerator::global()->bounded(endpoints.size());
        return endpoints.at(index);
    }
    case LLMFallbackStrategy::RoundRobin:
    case LLMFallbackStrategy::Sequential:
    default:
        return endpoints.at(currentEndpoint % endpoints.size());
    }
}

void QLLMService::advanceEndpoint()
{
    if (!endpoints.isEmpty()) {
        currentEndpoint = (currentEndpoint + 1) % static_cast<int>(endpoints.size());
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
        QSocConsole::warn() << "LLM API request failed:" << reply->errorString();
        QSocConsole::warn() << "received error response:" << errorData;
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
        QSocConsole::warn() << "JSON parse error:" << e.what();
        QSocConsole::warn() << "Raw response:" << responseData;
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

    QEventLoop     loop;
    QNetworkReply *reply = networkManager->post(request, QByteArray::fromStdString(payload.dump()));

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

void QLLMService::abortStream()
{
    if (!currentStreamReply || streamCompleted) {
        return;
    }
    streamCompleted = true;
    disconnect(currentStreamReply, nullptr, this, nullptr);
    currentStreamReply->abort();
    currentStreamReply->deleteLater();
    currentStreamReply = nullptr;
    emit streamError("Aborted by user");
}

void QLLMService::sendChatCompletionStream(
    const json    &messages,
    const json    &tools,
    double         temperature,
    const QString &reasoningEffort,
    const QString &modelOverride)
{
    if (!hasEndpoint()) {
        emit streamError("No LLM endpoint configured");
        return;
    }

    /* Abort any existing stream request before starting a new one */
    if (currentStreamReply) {
        /* Disconnect all signals first to prevent callbacks during cleanup */
        disconnect(currentStreamReply, nullptr, this, nullptr);
        currentStreamReply->abort();
        currentStreamReply->deleteLater();
        currentStreamReply = nullptr;
    }

    LLMEndpoint     endpoint = selectEndpoint();
    QNetworkRequest request  = prepareRequest(endpoint);

    /* Build payload with streaming enabled */
    json payload;
    payload["messages"]    = messages;
    payload["temperature"] = temperature;
    payload["stream"]      = true;
    /* Ask the server to emit a final chunk carrying token usage so
     * downstream code can anchor estimates on the real prompt size
     * instead of recomputing from scratch each turn. */
    payload["stream_options"] = {{"include_usage", true}};

    if (!reasoningEffort.isEmpty()) {
        /* Direct OpenAI/DeepSeek format */
        payload["reasoning_effort"] = reasoningEffort.toStdString();
        /* OpenRouter unified format */
        payload["reasoning"] = {{"effort", reasoningEffort.toStdString()}};
        /* Remove temperature - reasoning models reject it */
        payload.erase("temperature");
    }

    if (!modelOverride.isEmpty()) {
        payload["model"] = modelOverride.toStdString();
    } else if (!endpoint.model.isEmpty()) {
        payload["model"] = endpoint.model.toStdString();
    }

    if (!tools.empty()) {
        payload["tools"] = tools;
    }

    /* Set max output tokens from endpoint config */
    if (endpoint.maxOutputTokens > 0) {
        payload["max_tokens"] = endpoint.maxOutputTokens;
    }

    /* Track whether reasoning mode is active for buildStreamResponse */
    reasoningModeActive = !reasoningEffort.isEmpty();

    /* Reset streaming state */
    streamBuffer.clear();
    streamAccumulatedContent.clear();
    streamAccumulatedToolCalls.clear();
    streamAccumulatedReasoning.clear();
    streamAccumulatedUsage = json::object();
    streamCompleted        = false;

    currentStreamReply = networkManager->post(request, QByteArray::fromStdString(payload.dump()));

    /* Set timeout */
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, timer]() {
        timer->deleteLater();
        if (currentStreamReply) {
            currentStreamReply->abort();
            emit streamError("Request timeout");
        }
    });
    timer->start(endpoint.timeout);

    /* Handle incoming data */
    connect(currentStreamReply, &QNetworkReply::readyRead, this, [this, timer]() {
        if (!currentStreamReply || streamCompleted) {
            return;
        }

        /* Reset timeout timer on each data received */
        timer->start();

        streamBuffer += QString::fromUtf8(currentStreamReply->readAll());

        /* Process complete SSE lines */
        while (true) {
            int lineEnd = streamBuffer.indexOf('\n');
            if (lineEnd == -1) {
                break;
            }

            QString line = streamBuffer.left(lineEnd).trimmed();
            streamBuffer = streamBuffer.mid(lineEnd + 1);

            /* Skip empty lines */
            if (line.isEmpty()) {
                continue;
            }

            /* Parse SSE data lines */
            if (line.startsWith("data: ")) {
                QString data = line.mid(6);

                bool isDone
                    = parseStreamLine(data, streamAccumulatedContent, streamAccumulatedToolCalls);

                if (isDone) {
                    streamCompleted = true;

                    /* Disconnect all signals from reply and timer to prevent
                     * finished/timeout from firing during nested QEventLoop
                     * (e.g. bash tool execution). This prevents use-after-free
                     * when deleteLater processes during nested event loop while
                     * network thread still has posted events for the reply. */
                    disconnect(currentStreamReply, nullptr, this, nullptr);
                    disconnect(timer, nullptr, this, nullptr);
                    timer->stop();
                    timer->deleteLater();
                    currentStreamReply->abort();

                    json response
                        = buildStreamResponse(streamAccumulatedContent, streamAccumulatedToolCalls);
                    emit streamComplete(response);
                    return;
                }
            }
        }
    });

    /* Handle completion */
    connect(currentStreamReply, &QNetworkReply::finished, this, [this, timer]() {
        timer->stop();
        timer->deleteLater();

        if (!currentStreamReply) {
            return;
        }

        bool hasError = currentStreamReply->error() != QNetworkReply::NoError
                        && currentStreamReply->error() != QNetworkReply::OperationCanceledError;

        if (hasError) {
            /* Include any data already read by readyRead (error response body) */
            QString    errorMsg  = currentStreamReply->errorString();
            QByteArray errorBody = currentStreamReply->readAll();
            if (!errorBody.isEmpty()) {
                errorMsg += "\n" + QString::fromUtf8(errorBody);
            }
            if (!streamBuffer.isEmpty()) {
                errorMsg += "\n" + streamBuffer;
            }
            emit streamError(errorMsg);
        } else if (!streamCompleted) {
            /* Only process remaining data if streamComplete wasn't already emitted.
             * This prevents double emission when the finished signal fires during
             * a nested QEventLoop (e.g. bash tool execution). */

            /* Process any remaining data in buffer */
            QString remaining = streamBuffer.trimmed();
            if (!remaining.isEmpty() && remaining.startsWith("data: ")) {
                QString data = remaining.mid(6);
                bool    isDone
                    = parseStreamLine(data, streamAccumulatedContent, streamAccumulatedToolCalls);
                if (isDone) {
                    streamCompleted = true;
                    json response
                        = buildStreamResponse(streamAccumulatedContent, streamAccumulatedToolCalls);
                    emit streamComplete(response);
                }
            }

            /* If we have accumulated content or tool calls but didn't get [DONE],
               still emit streamComplete to avoid hanging */
            if (!streamCompleted
                && (!streamAccumulatedContent.isEmpty() || !streamAccumulatedToolCalls.isEmpty())) {
                streamCompleted = true;
                json response
                    = buildStreamResponse(streamAccumulatedContent, streamAccumulatedToolCalls);
                emit streamComplete(response);
            }
        }

        /* Disconnect and abort before deleteLater to ensure network thread
         * has stopped posting events before the reply object is deleted. */
        disconnect(currentStreamReply, nullptr, this, nullptr);
        currentStreamReply->abort();
        currentStreamReply->deleteLater();
        currentStreamReply = nullptr;
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

        /* Some servers (and the final include_usage chunk) carry
         * `usage` at the chunk root with an empty `choices` array.
         * Capture it before the empty-choices early-return so
         * buildStreamResponse can include the real numbers. */
        if (chunk.contains("usage") && chunk["usage"].is_object()) {
            streamAccumulatedUsage = chunk["usage"];
        }

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

        /* Direct API format: delta.reasoning_content (DeepSeek R1) */
        if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
            QString reasoning = QString::fromStdString(
                delta["reasoning_content"].get<std::string>());
            streamAccumulatedReasoning += reasoning;
            emit streamReasoningChunk(reasoning);
        }

        /* OpenRouter format: delta.reasoning_details (array) */
        if (delta.contains("reasoning_details") && delta["reasoning_details"].is_array()) {
            for (const auto &detail : delta["reasoning_details"]) {
                if (detail.contains("text") && detail["text"].is_string()) {
                    QString reasoning = QString::fromStdString(detail["text"].get<std::string>());
                    streamAccumulatedReasoning += reasoning;
                    emit streamReasoningChunk(reasoning);
                }
            }
        }

        /* Handle tool calls */
        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
            for (const auto &toolCall : delta["tool_calls"]) {
                int index = toolCall.value("index", 0);

                /* Initialize tool call entry if needed */
                if (!accumulatedToolCalls.contains(index)) {
                    accumulatedToolCalls[index]
                        = {{"id", ""},
                           {"type", "function"},
                           {"function", {{"name", ""}, {"arguments", ""}}}};
                }

                /* Update ID if present. Some endpoints (e.g. Qwen3 streaming)
                 * emit `"id": null` in continuation chunks; only copy when it
                 * is a real string so accumulated values stay string-typed. */
                if (toolCall.contains("id") && toolCall["id"].is_string()) {
                    accumulatedToolCalls[index]["id"] = toolCall["id"];
                }

                /* Update function info */
                if (toolCall.contains("function") && toolCall["function"].is_object()) {
                    auto &accFunc = accumulatedToolCalls[index]["function"];

                    if (toolCall["function"].contains("name")
                        && toolCall["function"]["name"].is_string()) {
                        accFunc["name"] = toolCall["function"]["name"];
                    }

                    if (toolCall["function"].contains("arguments")
                        && toolCall["function"]["arguments"].is_string()) {
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
        QSocConsole::warn() << "Failed to parse stream chunk:" << err.what();
    }

    return false;
}

json QLLMService::buildStreamResponse(const QString &content, const QMap<int, json> &toolCalls) const
{
    json message;
    message["role"] = "assistant";

    if (!content.isEmpty()) {
        message["content"] = content.toStdString();
    } else if (!toolCalls.isEmpty()) {
        /* Tool-call-only messages: content must be null, not missing. Some
         * endpoints reject the message when the key is absent entirely. */
        message["content"] = nullptr;
    } else {
        /* No content, no tool calls: use empty string instead of null.
         * Some endpoints reject null content on non-tool-call messages. */
        message["content"] = "";
    }

    /* DeepSeek R1 requires reasoning_content in ALL assistant messages when thinking
     * mode is active. Without this, subsequent API calls fail with
     * "Missing reasoning_content field". Always include the field. */
    if (!streamAccumulatedReasoning.isEmpty()) {
        message["reasoning_content"] = streamAccumulatedReasoning.toStdString();
    } else if (reasoningModeActive) {
        message["reasoning_content"] = "";
    }

    if (!toolCalls.isEmpty()) {
        json toolCallsArray = json::array();
        for (auto iter = toolCalls.constBegin(); iter != toolCalls.constEnd(); ++iter) {
            toolCallsArray.push_back(iter.value());
        }
        message["tool_calls"] = toolCallsArray;
    }

    json response = {{"choices", json::array({{{"message", message}}})}};
    if (streamAccumulatedUsage.is_object() && !streamAccumulatedUsage.empty()) {
        response["usage"] = streamAccumulatedUsage;
    }
    return response;
}

json QLLMService::sendChatCompletion(const json &messages, const json &tools, double temperature)
{
    if (!hasEndpoint()) {
        return {{"error", "No LLM endpoint configured"}};
    }

    /* Try endpoints with fallback */
    const int maxAttempts = static_cast<int>(endpoints.size());
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

        /* Set max output tokens from endpoint config */
        if (endpoint.maxOutputTokens > 0) {
            payload["max_tokens"] = endpoint.maxOutputTokens;
        }

        QEventLoop     loop;
        QNetworkReply *reply
            = networkManager->post(request, QByteArray::fromStdString(payload.dump()));

        /* Set timeout */
        QTimer timer;
        timer.setSingleShot(true);
        connect(&timer, &QTimer::timeout, reply, &QNetworkReply::abort);
        timer.start(endpoint.timeout);

        connect(reply, SIGNAL(finished()), &loop, SLOT(quit()));
        loop.exec();

        timer.stop();

        if (reply->error() != QNetworkReply::NoError) {
            QSocConsole::warn() << "Endpoint" << endpoint.name << "failed:" << reply->errorString();
            reply->deleteLater();
            advanceEndpoint();
            continue;
        }

        const QByteArray responseData = reply->readAll();
        reply->deleteLater();

        try {
            return json::parse(responseData.toStdString());
        } catch (const json::parse_error &e) {
            QSocConsole::warn() << "JSON parse error:" << e.what();
            advanceEndpoint();
            continue;
        }
    }

    return {{"error", "All LLM endpoints failed"}};
}
