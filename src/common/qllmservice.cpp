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

struct QLLMService::StreamState
{
    QPointer<QNetworkReply> reply;
    QPointer<QTimer>        timer;
    QString                 buffer;
    QString                 content;
    QMap<int, json>         toolCalls;
    QString                 reasoning;
    QString                 finishReason;
    bool                    reasoningMode = false;
    json                    usage         = json::object();
    StreamOutcome           outcome       = StreamOutcome::Active;
};

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
    const QPointer<QLLMService> owner(this);
    const StreamStatePtr        state = currentStream;
    if (claimTerminal(state, StreamOutcome::OwnerDestroyed)) {
        stopStreamReply(owner, state);
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
    endpoint.authHeader      = modelConf.authHeader;
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
    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    timer->start(endpoint.timeout);

    connect(reply, &QNetworkReply::finished, this, [this, reply, callback, timer]() {
        timer->stop();
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
                if (node["auth_header"]) {
                    modelCfg.authHeader = QString::fromStdString(
                        node["auth_header"].as<std::string>());
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

    /* Auth header dispatch: empty or "Authorization" sends the
     * "Bearer <key>" pattern; any other value sends the bare key
     * under that header. */
    if (!endpoint.key.isEmpty()) {
        const bool isBearer = endpoint.authHeader.isEmpty()
                              || endpoint.authHeader.compare("Authorization", Qt::CaseInsensitive)
                                     == 0;
        if (isBearer) {
            request.setRawHeader("Authorization", ("Bearer " + endpoint.key).toUtf8());
        } else {
            request.setRawHeader(endpoint.authHeader.toUtf8(), endpoint.key.toUtf8());
        }
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
    const QPointer<QLLMService> owner(this);
    const StreamStatePtr        state = currentStream;
    if (!claimTerminal(state, StreamOutcome::Aborted)) {
        return;
    }
    stopStreamReply(owner, state);
    if (!owner.isNull()) {
        emit owner->streamError(QStringLiteral("Aborted by user"));
    }
}

void QLLMService::sendChatCompletionStream(
    const json    &messages,
    const json    &tools,
    double         temperature,
    const QString &reasoningEffort,
    const QString &modelOverride)
{
    if (!hasEndpoint()) {
        emit streamError(QStringLiteral("No LLM endpoint configured"));
        return;
    }

    /* Abort any existing stream request before starting a new one */
    const QPointer<QLLMService> owner(this);
    const StreamStatePtr        previous = currentStream;
    if (claimTerminal(previous, StreamOutcome::Superseded)) {
        stopStreamReply(owner, previous);
        if (owner.isNull()) {
            return;
        }
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

    QNetworkReply *reply = networkManager->post(request, QByteArray::fromStdString(payload.dump()));
    auto           state = std::make_shared<StreamState>();
    state->reply         = reply;
    state->reasoningMode = !reasoningEffort.isEmpty();
    currentStream        = state;

    /* Set timeout */
    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    state->timer = timer;
    connect(timer, &QTimer::timeout, this, [this, state]() {
        const StreamStatePtr        active = state;
        const QPointer<QLLMService> owner(this);
        if (!claimTerminal(active, StreamOutcome::TimedOut)) {
            return;
        }
        stopStreamReply(owner, active);
        if (!owner.isNull()) {
            emit owner->streamError(QStringLiteral("Request timeout"));
        }
    });
    timer->start(endpoint.timeout);

    /* Handle incoming data */
    connect(reply, &QNetworkReply::readyRead, this, [this, state]() {
        const StreamStatePtr        active = state;
        const QPointer<QLLMService> owner(this);
        if (!isStreamActive(owner, active)) {
            return;
        }

        /* Reset timeout timer on each data received */
        if (!active->timer.isNull()) {
            active->timer->start();
        }

        QNetworkReply *reply = active->reply.data();
        if (reply == nullptr) {
            return;
        }
        active->buffer += QString::fromUtf8(reply->readAll());
        const ParseResult result = processStreamBuffer(owner, active);
        if (result != ParseResult::Done || !isStreamActive(owner, active)) {
            return;
        }

        const json response = buildStreamResponse(active);
        if (!owner->claimTerminal(active, StreamOutcome::Completed)) {
            return;
        }
        drainStreamReply(owner, active);
        if (!owner.isNull()) {
            emit owner->streamComplete(response);
        }
    });

    /* Handle completion */
    connect(reply, &QNetworkReply::finished, this, [this, state]() {
        const StreamStatePtr        active = state;
        const QPointer<QLLMService> owner(this);
        if (!isStreamActive(owner, active)) {
            return;
        }

        if (!active->timer.isNull()) {
            active->timer->stop();
        }
        QNetworkReply *reply = active->reply.data();
        if (reply == nullptr) {
            return;
        }

        if (reply->error() != QNetworkReply::NoError) {
            /* Prefix the HTTP status code so downstream classification can
             * dispatch on an exact code instead of fuzzy-matching the body.
             * Status 0 means the request never reached the server (DNS,
             * TLS, connection-refused, abort, etc). */
            const int httpStatus
                = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            QString    errorMsg  = QString("[HTTP %1] ").arg(httpStatus) + reply->errorString();
            QByteArray errorBody = reply->readAll();
            if (!errorBody.isEmpty()) {
                errorMsg += "\n" + QString::fromUtf8(errorBody);
            }
            if (!active->buffer.isEmpty()) {
                errorMsg += "\n" + active->buffer;
            }
            if (!owner->claimTerminal(active, StreamOutcome::Failed)) {
                return;
            }
            stopStreamReply(owner, active);
            if (!owner.isNull()) {
                emit owner->streamError(errorMsg);
            }
            return;
        }

        active->buffer += QString::fromUtf8(reply->readAll());
        if (!active->buffer.isEmpty() && !active->buffer.endsWith(QLatin1Char('\n'))) {
            active->buffer += QLatin1Char('\n');
        }
        const ParseResult result = processStreamBuffer(owner, active);
        if (result == ParseResult::Stopped || !isStreamActive(owner, active)) {
            return;
        }

        const json response = buildStreamResponse(active);
        if (!owner->claimTerminal(active, StreamOutcome::Completed)) {
            return;
        }
        stopStreamReply(owner, active);
        if (!owner.isNull()) {
            emit owner->streamComplete(response);
        }
    });

    connect(reply, &QObject::destroyed, this, [this, state]() {
        const StreamStatePtr        active = state;
        const QPointer<QLLMService> owner(this);
        if (!claimTerminal(active, StreamOutcome::Failed)) {
            return;
        }
        active->reply.clear();
        active->timer.clear();
        emit owner->streamError(QStringLiteral("[HTTP 0] Network reply destroyed"));
    });
}

bool QLLMService::claimTerminal(const StreamStatePtr &state, StreamOutcome outcome)
{
    if (!state || currentStream != state || state->outcome != StreamOutcome::Active) {
        return false;
    }
    state->outcome = outcome;
    currentStream.reset();
    return true;
}

bool QLLMService::isStreamActive(const QPointer<QLLMService> &owner, const StreamStatePtr &state)
{
    return !owner.isNull() && state && owner->currentStream == state
           && state->outcome == StreamOutcome::Active && !state->reply.isNull();
}

void QLLMService::stopStreamReply(const QPointer<QLLMService> &owner, const StreamStatePtr &state)
{
    if (!state) {
        return;
    }

    QPointer<QTimer>        timer = state->timer;
    QPointer<QNetworkReply> reply = state->reply;
    state->timer.clear();
    state->reply.clear();

    if (!timer.isNull()) {
        timer->stop();
        if (!owner.isNull()) {
            QObject::disconnect(timer.data(), nullptr, owner.data(), nullptr);
        }
    }
    if (reply.isNull()) {
        return;
    }
    if (!owner.isNull()) {
        QObject::disconnect(reply.data(), nullptr, owner.data(), nullptr);
    }
    if (reply->isRunning()) {
        reply->abort();
    }
    if (!reply.isNull()) {
        reply->deleteLater();
    }
}

void QLLMService::drainStreamReply(const QPointer<QLLMService> &owner, const StreamStatePtr &state)
{
    if (!state) {
        return;
    }

    QPointer<QTimer>        timer = state->timer;
    QPointer<QNetworkReply> reply = state->reply;
    state->timer.clear();
    state->reply.clear();

    if (!timer.isNull()) {
        timer->stop();
        if (!owner.isNull()) {
            QObject::disconnect(timer.data(), nullptr, owner.data(), nullptr);
        }
    }
    if (reply.isNull()) {
        return;
    }
    if (!owner.isNull()) {
        QObject::disconnect(reply.data(), nullptr, owner.data(), nullptr);
    }
    if (!reply->isRunning()) {
        reply->deleteLater();
        return;
    }

    QObject::connect(reply.data(), &QNetworkReply::finished, reply.data(), &QObject::deleteLater);
    if (!timer.isNull()) {
        QObject::connect(timer.data(), &QTimer::timeout, reply.data(), [reply]() {
            if (reply.isNull()) {
                return;
            }
            if (reply->isRunning()) {
                reply->abort();
            }
            if (!reply.isNull()) {
                reply->deleteLater();
            }
        });
        timer->start();
    }
}

QLLMService::ParseResult QLLMService::processStreamBuffer(
    const QPointer<QLLMService> &owner, const StreamStatePtr &state)
{
    while (true) {
        if (!isStreamActive(owner, state)) {
            return ParseResult::Stopped;
        }

        const int lineEnd = state->buffer.indexOf(QLatin1Char('\n'));
        if (lineEnd == -1) {
            return ParseResult::NeedMore;
        }

        const QString line = state->buffer.left(lineEnd).trimmed();
        state->buffer      = state->buffer.mid(lineEnd + 1);
        if (!line.startsWith(QStringLiteral("data: "))) {
            continue;
        }

        const ParseResult result = parseStreamLine(owner, state, line.mid(6));
        if (result != ParseResult::NeedMore) {
            return result;
        }
    }
}

QLLMService::ParseResult QLLMService::parseStreamLine(
    const QPointer<QLLMService> &owner, const StreamStatePtr &state, const QString &line)
{
    if (!isStreamActive(owner, state)) {
        return ParseResult::Stopped;
    }

    /* Check for stream end */
    if (line == QStringLiteral("[DONE]")) {
        return ParseResult::Done;
    }

    /* Parse JSON */
    try {
        json chunk = json::parse(line.toStdString());

        /* Some servers (and the final include_usage chunk) carry
         * `usage` at the chunk root with an empty `choices` array.
         * Capture it before the empty-choices early-return so
         * buildStreamResponse can include the real numbers. */
        if (chunk.contains("usage") && chunk["usage"].is_object()) {
            state->usage = chunk["usage"];
        }

        if (!chunk.contains("choices") || chunk["choices"].empty()) {
            return ParseResult::NeedMore;
        }

        auto delta = chunk["choices"][0]["delta"];

        /* Handle content chunks */
        if (delta.contains("content") && delta["content"].is_string()) {
            QString content = QString::fromStdString(delta["content"].get<std::string>());
            state->content += content;
            emit owner->streamChunk(content);
            if (!isStreamActive(owner, state)) {
                return ParseResult::Stopped;
            }
        }

        /* Direct API format: delta.reasoning_content (DeepSeek R1) */
        if (delta.contains("reasoning_content") && delta["reasoning_content"].is_string()) {
            QString reasoning = QString::fromStdString(
                delta["reasoning_content"].get<std::string>());
            state->reasoning += reasoning;
            emit owner->streamReasoningChunk(reasoning);
            if (!isStreamActive(owner, state)) {
                return ParseResult::Stopped;
            }
        }

        /* OpenRouter format: delta.reasoning_details (array) */
        if (delta.contains("reasoning_details") && delta["reasoning_details"].is_array()) {
            for (const auto &detail : delta["reasoning_details"]) {
                if (detail.contains("text") && detail["text"].is_string()) {
                    QString reasoning = QString::fromStdString(detail["text"].get<std::string>());
                    state->reasoning += reasoning;
                    emit owner->streamReasoningChunk(reasoning);
                    if (!isStreamActive(owner, state)) {
                        return ParseResult::Stopped;
                    }
                }
            }
        }

        /* Handle tool calls */
        if (delta.contains("tool_calls") && delta["tool_calls"].is_array()) {
            for (const auto &toolCall : delta["tool_calls"]) {
                int index = toolCall.value("index", 0);

                /* Initialize tool call entry if needed */
                if (!state->toolCalls.contains(index)) {
                    state->toolCalls[index]
                        = {{"id", ""},
                           {"type", "function"},
                           {"function", {{"name", ""}, {"arguments", ""}}}};
                }

                /* Update ID if present. Some endpoints (e.g. Qwen3 streaming)
                 * emit `"id": null` in continuation chunks; only copy when it
                 * is a real string so accumulated values stay string-typed. */
                if (toolCall.contains("id") && toolCall["id"].is_string()) {
                    state->toolCalls[index]["id"] = toolCall["id"];
                }

                /* Update function info */
                if (toolCall.contains("function") && toolCall["function"].is_object()) {
                    auto &accFunc = state->toolCalls[index]["function"];

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
                    state->toolCalls[index]["id"].get<std::string>());
                QString funcName = QString::fromStdString(
                    state->toolCalls[index]["function"]["name"].get<std::string>());
                QString funcArgs = QString::fromStdString(
                    state->toolCalls[index]["function"]["arguments"].get<std::string>());

                emit owner->streamToolCall(toolId, funcName, funcArgs);
                if (!isStreamActive(owner, state)) {
                    return ParseResult::Stopped;
                }
            }
        }

        /* Preserve the finish reason, but keep reading through the usage
         * chunk and [DONE] marker that may follow it. */
        if (chunk["choices"][0].contains("finish_reason")
            && chunk["choices"][0]["finish_reason"].is_string()) {
            state->finishReason = QString::fromStdString(
                chunk["choices"][0]["finish_reason"].get<std::string>());
        }

    } catch (const json::parse_error &err) {
        QSocConsole::warn() << "Failed to parse stream chunk:" << err.what();
    }

    return ParseResult::NeedMore;
}

json QLLMService::buildStreamResponse(const StreamStatePtr &state)
{
    json message;
    message["role"] = "assistant";

    if (!state->content.isEmpty()) {
        message["content"] = state->content.toStdString();
    } else if (!state->toolCalls.isEmpty()) {
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
    if (!state->reasoning.isEmpty()) {
        message["reasoning_content"] = state->reasoning.toStdString();
    } else if (state->reasoningMode) {
        message["reasoning_content"] = "";
    }

    if (!state->toolCalls.isEmpty()) {
        json toolCallsArray = json::array();
        for (auto iter = state->toolCalls.constBegin(); iter != state->toolCalls.constEnd();
             ++iter) {
            toolCallsArray.push_back(iter.value());
        }
        message["tool_calls"] = toolCallsArray;
    }

    json choice = {{"message", message}};
    if (!state->finishReason.isEmpty()) {
        choice["finish_reason"] = state->finishReason.toStdString();
    }

    json response = {{"choices", json::array({choice})}};
    if (state->usage.is_object() && !state->usage.empty()) {
        response["usage"] = state->usage;
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
