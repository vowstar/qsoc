// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "common/qllmservice.h"
#include "common/qsocconsole.h"
#include "common/qsocproxy.h"

#include <algorithm>
#include <limits>
#include <QDebug>
#include <QEventLoop>
#include <QNetworkRequest>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QTimer>

struct QLLMService::StreamState
{
    QPointer<QNetworkReply> reply;
    QPointer<QTimer>        timer;
    QByteArray              buffer;
    QString                 content;
    QMap<int, json>         toolCalls;
    QString                 reasoning;
    QString                 finishReason;
    QString                 terminalError;
    quint64                 generation         = 0;
    bool                    reasoningMode      = false;
    bool                    consumeScheduled   = false;
    bool                    processing         = false;
    bool                    replyFinished      = false;
    bool                    transportFailed    = false;
    bool                    sawAssistantChoice = false;
    json                    usage              = json::object();
    StreamOutcome           outcome            = StreamOutcome::Active;
};

namespace {

struct AsyncRequestState
{
    QPointer<QNetworkReply> reply;
    QPointer<QTimer>        timer;
    bool                    terminal = false;
};

bool isNullableString(const json &object, const char *field)
{
    return !object.contains(field) || object[field].is_null() || object[field].is_string();
}

QString providerStreamError(const json &error)
{
    QString message;
    if (error.is_string()) {
        message = QString::fromStdString(error.get<std::string>());
    } else if (error.is_object() && error.contains("message") && error["message"].is_string()) {
        message = QString::fromStdString(error["message"].get<std::string>());
    }
    message = message.trimmed();
    return message.isEmpty() ? QStringLiteral("LLM provider returned a streaming error") : message;
}

QString validateStreamChunk(const json &chunk)
{
    if (!chunk.is_object()) {
        return QStringLiteral("root is not an object");
    }
    if (chunk.contains("usage") && !chunk["usage"].is_null() && !chunk["usage"].is_object()) {
        return QStringLiteral("usage is not an object");
    }
    if (!chunk.contains("choices")) {
        return {};
    }

    const json &choices = chunk["choices"];
    if (!choices.is_array()) {
        return QStringLiteral("choices is not an array");
    }
    if (choices.empty()) {
        return {};
    }

    const json &choice = choices.front();
    if (!choice.is_object()) {
        return QStringLiteral("choice is not an object");
    }
    if (!isNullableString(choice, "finish_reason")) {
        return QStringLiteral("finish_reason is not a string");
    }
    if (!choice.contains("delta") || choice["delta"].is_null()) {
        return {};
    }

    const json &delta = choice["delta"];
    if (!delta.is_object()) {
        return QStringLiteral("delta is not an object");
    }
    if (!isNullableString(delta, "content")) {
        return QStringLiteral("content is not a string");
    }
    if (!isNullableString(delta, "reasoning_content")) {
        return QStringLiteral("reasoning_content is not a string");
    }

    if (delta.contains("reasoning_details") && !delta["reasoning_details"].is_null()) {
        const json &details = delta["reasoning_details"];
        if (!details.is_array()) {
            return QStringLiteral("reasoning_details is not an array");
        }
        for (const json &detail : details) {
            if (!detail.is_object() || !isNullableString(detail, "text")) {
                return QStringLiteral("reasoning detail is invalid");
            }
        }
    }

    if (!delta.contains("tool_calls") || delta["tool_calls"].is_null()) {
        return {};
    }

    const json &toolCalls = delta["tool_calls"];
    if (!toolCalls.is_array()) {
        return QStringLiteral("tool_calls is not an array");
    }
    for (const json &toolCall : toolCalls) {
        if (!toolCall.is_object()) {
            return QStringLiteral("tool call is not an object");
        }
        if (!isNullableString(toolCall, "id") || !isNullableString(toolCall, "type")) {
            return QStringLiteral("tool call identifier is invalid");
        }
        if (toolCall.contains("type") && toolCall["type"].is_string()) {
            const std::string &type = toolCall["type"].get_ref<const std::string &>();
            if (!type.empty() && type != "function") {
                return QStringLiteral("tool call type is unsupported");
            }
        }
        if (toolCall.contains("index") && !toolCall["index"].is_null()) {
            const json &index = toolCall["index"];
            if (!index.is_number_integer()) {
                return QStringLiteral("tool call index is not an integer");
            }
            const bool inRange = index.is_number_unsigned()
                                     ? index.get<std::uint64_t>() <= static_cast<std::uint64_t>(
                                           std::numeric_limits<int>::max())
                                     : index.get<std::int64_t>() >= 0
                                           && index.get<std::int64_t>()
                                                  <= std::numeric_limits<int>::max();
            if (!inRange) {
                return QStringLiteral("tool call index is out of range");
            }
        }
        if (!toolCall.contains("function") || toolCall["function"].is_null()) {
            continue;
        }
        const json &function = toolCall["function"];
        if (!function.is_object() || !isNullableString(function, "name")
            || !isNullableString(function, "arguments")) {
            return QStringLiteral("tool call function is invalid");
        }
    }
    return {};
}

struct NetworkWaitResult
{
    QPointer<QNetworkReply> reply;
    bool                    cancelled = false;
};

void drainNetworkReply(QNetworkReply *reply)
{
    if (reply == nullptr) {
        return;
    }
    const QPointer<QNetworkReply> guardedReply(reply);
    const auto                    drain = [guardedReply]() {
        if (!guardedReply.isNull()) {
            guardedReply->readAll();
        }
    };
    QObject::connect(reply, &QNetworkReply::readyRead, reply, drain);
    QObject::connect(reply, &QNetworkReply::finished, reply, [guardedReply, drain]() {
        drain();
        if (!guardedReply.isNull()) {
            guardedReply->deleteLater();
        }
    });
    drain();
    if (!guardedReply.isNull() && guardedReply->isFinished()) {
        guardedReply->deleteLater();
    }
}

NetworkWaitResult waitForNetworkReply(
    QNetworkReply *reply, int timeout, std::stop_token stopToken = {})
{
    if (reply == nullptr) {
        return {};
    }
    QPointer<QNetworkReply> guardedReply(reply);
    QEventLoop              loop;
    auto                   *timer = new QTimer(reply);
    timer->setSingleShot(true);

    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QObject::destroyed, &loop, &QEventLoop::quit);
    QObject::connect(reply, &QNetworkReply::finished, timer, &QTimer::stop);
    QObject::connect(timer, &QTimer::timeout, reply, &QNetworkReply::abort);
    QObject::connect(timer, &QTimer::timeout, &loop, &QEventLoop::quit);
    const std::stop_callback stopCallback(stopToken, [&loop]() {
        QMetaObject::invokeMethod(&loop, &QEventLoop::quit, Qt::QueuedConnection);
    });

    timer->start(timeout);
    if (!guardedReply->isFinished() && !stopToken.stop_requested()) {
        loop.exec();
    }
    const bool cancelled = stopToken.stop_requested();
    if (!cancelled && !guardedReply.isNull()) {
        timer->stop();
    }
    return {guardedReply, cancelled};
}

} // namespace

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
    ++endpointRevision;
}

void QLLMService::clearEndpoints()
{
    endpoints.clear();
    currentEndpoint = 0;
    ++endpointRevision;
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
    ++endpointRevision;

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
    if (fallbackStrategy == strategy) {
        return;
    }
    fallbackStrategy = strategy;
    ++endpointRevision;
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

    const QPointer<QLLMService> owner(this);

    const QList<EndpointAttempt> attempts = endpointAttempts();
    for (const EndpointAttempt &attempt : attempts) {
        const LLMEndpoint &endpoint = attempt.endpoint;
        LLMResponse        response
            = sendRequestToEndpoint(endpoint, prompt, systemPrompt, temperature, jsonMode);

        if (owner.isNull()) {
            return response;
        }
        if (owner->networkManager.isNull()) {
            return response;
        }
        if (response.success) {
            owner->commitEndpoint(attempt);
            return response;
        }

        QSocConsole::warn() << "Endpoint" << endpoint.name << "failed:" << response.errorMessage;
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
        if (callback) {
            callback(response);
        }
        return;
    }

    LLMEndpoint endpoint = selectEndpoint();

    QNetworkRequest request = prepareRequest(endpoint);
    json payload = buildRequestPayload(prompt, systemPrompt, temperature, jsonMode, endpoint.model);

    if (networkManager.isNull()) {
        LLMResponse response;
        response.success      = false;
        response.errorMessage = QStringLiteral("Network manager destroyed");
        if (callback) {
            callback(response);
        }
        return;
    }
    QNetworkReply *reply = networkManager->post(request, QByteArray::fromStdString(payload.dump()));
    if (reply == nullptr) {
        LLMResponse response;
        response.success      = false;
        response.errorMessage = QStringLiteral("Network request failed to start");
        if (callback) {
            callback(response);
        }
        return;
    }

    /* Set timeout */
    auto *timer = new QTimer(reply);
    timer->setSingleShot(true);
    auto state   = std::make_shared<AsyncRequestState>();
    state->reply = reply;
    state->timer = timer;
    const QPointer<QLLMService> owner(this);

    const auto complete = [owner, state, callback](LLMResponse response, bool abortReply) {
        if (state->terminal) {
            return;
        }
        state->terminal = true;

        const QPointer<QNetworkReply> guardedReply = state->reply;
        const QPointer<QTimer>        guardedTimer = state->timer;
        state->reply.clear();
        state->timer.clear();

        if (!guardedTimer.isNull()) {
            guardedTimer->stop();
        }
        if (!guardedReply.isNull()) {
            if (abortReply) {
                guardedReply->abort();
            }
            if (!guardedReply.isNull()) {
                guardedReply->deleteLater();
            }
        }
        if (!callback || owner.isNull()) {
            return;
        }
        QTimer::singleShot(0, [owner, callback, response]() mutable {
            if (!owner.isNull()) {
                callback(response);
            }
        });
    };

    const auto handleFinished = [this, state, complete]() {
        if (state->terminal || state->reply.isNull()) {
            return;
        }
        LLMResponse response = parseResponse(state->reply.data());
        complete(response, false);
    };

    connect(reply, &QNetworkReply::finished, this, handleFinished);
    connect(reply, &QObject::destroyed, this, [state, complete]() {
        state->reply.clear();
        LLMResponse response;
        response.success      = false;
        response.errorMessage = QStringLiteral("Network reply destroyed");
        complete(response, false);
    });
    connect(timer, &QTimer::timeout, this, [complete]() {
        LLMResponse response;
        response.success      = false;
        response.errorMessage = QStringLiteral("Request timeout");
        complete(response, true);
    });
    timer->start(endpoint.timeout);

    if (reply->isFinished()) {
        QTimer::singleShot(0, this, handleFinished);
    }
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
    ++endpointRevision;
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

    int index = currentEndpoint % static_cast<int>(endpoints.size());
    switch (fallbackStrategy) {
    case LLMFallbackStrategy::Random:
        index = QRandomGenerator::global()->bounded(static_cast<int>(endpoints.size()));
        break;
    case LLMFallbackStrategy::RoundRobin:
        currentEndpoint = (index + 1) % static_cast<int>(endpoints.size());
        break;
    case LLMFallbackStrategy::Sequential:
    default:
        break;
    }
    ++endpointRevision;
    return endpoints.at(index);
}

QList<QLLMService::EndpointAttempt> QLLMService::endpointAttempts()
{
    QList<EndpointAttempt> attempts;
    attempts.reserve(endpoints.size());
    if (endpoints.isEmpty()) {
        return attempts;
    }

    const quint64 revision = ++endpointRevision;

    if (fallbackStrategy == LLMFallbackStrategy::Random) {
        for (qsizetype index = 0; index < endpoints.size(); ++index) {
            attempts.append({endpoints.at(index), static_cast<int>(index), revision});
        }
        std::shuffle(attempts.begin(), attempts.end(), *QRandomGenerator::global());
        return attempts;
    }

    const int start = currentEndpoint % static_cast<int>(endpoints.size());
    if (fallbackStrategy == LLMFallbackStrategy::RoundRobin) {
        currentEndpoint = (start + 1) % static_cast<int>(endpoints.size());
    }
    for (qsizetype offset = 0; offset < endpoints.size(); ++offset) {
        const int index = (start + static_cast<int>(offset)) % static_cast<int>(endpoints.size());
        attempts.append({endpoints.at(index), index, revision});
    }
    return attempts;
}

void QLLMService::commitEndpoint(const EndpointAttempt &attempt)
{
    if (attempt.revision != endpointRevision || attempt.index < 0
        || attempt.index >= endpoints.size()) {
        return;
    }

    switch (fallbackStrategy) {
    case LLMFallbackStrategy::Sequential:
        currentEndpoint = attempt.index;
        break;
    case LLMFallbackStrategy::RoundRobin:
        currentEndpoint = (attempt.index + 1) % static_cast<int>(endpoints.size());
        break;
    case LLMFallbackStrategy::Random:
        break;
    }
    ++endpointRevision;
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
            const json &choice     = jsonResponse["choices"][0];
            const bool  hasContent = choice.is_object() && choice.contains("message")
                                     && choice["message"].is_object()
                                     && choice["message"].contains("content");
            if (hasContent && choice["message"]["content"].is_string()) {
                response.content = QString::fromStdString(
                    choice["message"]["content"].get<std::string>());
            } else if (hasContent && !choice["message"]["content"].is_null()) {
                response.success      = false;
                response.errorMessage = QStringLiteral(
                    "JSON parse error: message content is not a string");
                return response;
            } else if (choice.is_object() && choice.contains("text") && choice["text"].is_string()) {
                /* Handle streaming response format */
                response.content = QString::fromStdString(choice["text"].get<std::string>());
            } else if (choice.is_object() && choice.contains("text") && !choice["text"].is_null()) {
                response.success      = false;
                response.errorMessage = QStringLiteral("JSON parse error: text is not a string");
                return response;
            }
        }

        /* If content is empty but we have valid JSON, return formatted JSON */
        if (response.content.isEmpty() && !jsonResponse.empty()) {
            response.content = QString::fromStdString(jsonResponse.dump(2));
        }

    } catch (const json::exception &e) {
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

    const QPointer<QLLMService> owner(this);
    if (networkManager.isNull()) {
        LLMResponse response;
        response.success      = false;
        response.errorMessage = QStringLiteral("Network manager destroyed");
        return response;
    }
    QNetworkReply *networkReply
        = networkManager->post(request, QByteArray::fromStdString(payload.dump()));
    QPointer<QNetworkReply> reply = waitForNetworkReply(networkReply, endpoint.timeout).reply;

    if (owner.isNull()) {
        LLMResponse response;
        response.success      = false;
        response.errorMessage = QStringLiteral("LLM service destroyed");
        return response;
    }
    if (owner->networkManager.isNull()) {
        LLMResponse response;
        response.success      = false;
        response.errorMessage = QStringLiteral("Network manager destroyed");
        return response;
    }
    if (reply.isNull()) {
        LLMResponse response;
        response.success      = false;
        response.errorMessage = QStringLiteral("Network reply destroyed");
        return response;
    }

    LLMResponse response = owner->parseResponse(reply.data());
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
    const quint64 generation = ++streamGeneration;

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
    /* Qt can dispatch an HTTP/2 read after a retired stream closes its TLS socket. */
    request.setAttribute(QNetworkRequest::Http2AllowedAttribute, false);

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

    if (networkManager.isNull()) {
        emit streamError(QStringLiteral("Network manager destroyed"));
        return;
    }
    QNetworkReply *reply = networkManager->post(request, QByteArray::fromStdString(payload.dump()));
    if (reply == nullptr) {
        emit streamError(QStringLiteral("Network request failed to start"));
        return;
    }
    auto state           = std::make_shared<StreamState>();
    state->reply         = reply;
    state->reasoningMode = !reasoningEffort.isEmpty();
    state->generation    = generation;
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
        active->buffer += reply->readAll();
        scheduleStreamConsumption(owner, active);
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
        active->replyFinished = true;

        if (reply->error() != QNetworkReply::NoError) {
            /* Prefix the HTTP status code so downstream classification can
             * dispatch on an exact code instead of fuzzy-matching the body.
             * Status 0 means the request never reached the server (DNS,
             * TLS, connection-refused, abort, etc). */
            const int httpStatus
                = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            active->terminalError = QString("[HTTP %1] ").arg(httpStatus) + reply->errorString();
            active->buffer += reply->readAll();
            active->transportFailed = true;
            scheduleStreamConsumption(owner, active);
            return;
        }

        active->buffer += reply->readAll();
        if (!active->buffer.isEmpty() && !active->buffer.endsWith('\n')) {
            active->buffer += '\n';
        }
        scheduleStreamConsumption(owner, active);
    });

    connect(reply, &QObject::destroyed, this, [this, state]() {
        const StreamStatePtr        active = state;
        const QPointer<QLLMService> owner(this);
        if (!claimTerminal(active, StreamOutcome::Failed)) {
            return;
        }
        active->reply.clear();
        active->timer.clear();
        const quint64 generation = active->generation;
        QTimer::singleShot(0, [owner, generation]() {
            if (owner.isNull() || owner->streamGeneration != generation) {
                return;
            }
            emit owner->streamError(QStringLiteral("[HTTP 0] Network reply destroyed"));
        });
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

void QLLMService::scheduleStreamConsumption(
    const QPointer<QLLMService> &owner, const StreamStatePtr &state)
{
    if (!isStreamActive(owner, state) || state->consumeScheduled) {
        return;
    }
    state->consumeScheduled = true;
    QTimer::singleShot(0, [owner, state]() {
        state->consumeScheduled = false;
        consumeStream(owner, state);
    });
}

void QLLMService::consumeStream(const QPointer<QLLMService> &owner, const StreamStatePtr &state)
{
    if (!isStreamActive(owner, state) || state->processing) {
        return;
    }

    state->processing        = true;
    const ParseResult result = processStreamBuffer(owner, state);
    state->processing        = false;

    if (!isStreamActive(owner, state)) {
        return;
    }
    if (result == ParseResult::TransportError && !state->buffer.isEmpty()) {
        state->terminalError += "\n" + QString::fromUtf8(state->buffer);
        state->buffer.clear();
    }
    if (result == ParseResult::ProviderError || result == ParseResult::TransportError) {
        failStream(owner, state, state->terminalError);
        return;
    }
    if (result == ParseResult::Malformed) {
        failStream(owner, state, QStringLiteral("Malformed streaming response from LLM"));
        return;
    }
    if (result == ParseResult::Stopped || (result != ParseResult::Done && !state->replyFinished)) {
        return;
    }
    if (!state->sawAssistantChoice) {
        failStream(owner, state, QStringLiteral("Malformed streaming response from LLM"));
        return;
    }

    const bool replyFinished = state->replyFinished;
    const json response      = buildStreamResponse(state);
    QString    validationError;
    if (!extractAssistantMessage(response, nullptr, &validationError)) {
        failStream(owner, state, validationError);
        return;
    }
    if (!owner->claimTerminal(state, StreamOutcome::Completed)) {
        return;
    }
    if (replyFinished) {
        stopStreamReply(owner, state);
    } else {
        drainStreamReply(owner, state);
    }
    if (!owner.isNull()) {
        emit owner->streamComplete(response);
    }
}

void QLLMService::failStream(
    const QPointer<QLLMService> &owner, const StreamStatePtr &state, const QString &error)
{
    if (owner.isNull() || !owner->claimTerminal(state, StreamOutcome::Failed)) {
        return;
    }
    stopStreamReply(owner, state);
    if (!owner.isNull()) {
        emit owner->streamError(error);
    }
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
        const qsizetype lineEnd = state->buffer.indexOf('\n');
        if (lineEnd == -1) {
            return state->transportFailed ? ParseResult::TransportError : ParseResult::NeedMore;
        }

        QByteArray rawLine = state->buffer.left(lineEnd);
        state->buffer      = state->buffer.mid(lineEnd + 1);
        if (rawLine.endsWith('\r')) {
            rawLine.chop(1);
        }
        if (rawLine.isEmpty() || rawLine.startsWith(':')) {
            continue;
        }

        const qsizetype  separator = rawLine.indexOf(':');
        const QByteArray field     = separator < 0 ? rawLine : rawLine.left(separator);
        if (field != QByteArrayLiteral("data")) {
            if (state->transportFailed) {
                state->terminalError += "\n" + QString::fromUtf8(rawLine);
            }
            continue;
        }

        QByteArray data = separator < 0 ? QByteArray() : rawLine.mid(separator + 1);
        if (data.startsWith(' ')) {
            data.remove(0, 1);
        }
        if (data.isEmpty()) {
            continue;
        }

        const ParseResult result = parseStreamLine(owner, state, data);
        if (result != ParseResult::NeedMore) {
            return result;
        }
    }
}

QLLMService::ParseResult QLLMService::parseStreamLine(
    const QPointer<QLLMService> &owner, const StreamStatePtr &state, const QByteArray &line)
{
    if (!isStreamActive(owner, state)) {
        return ParseResult::Stopped;
    }
    /* Check for stream end */
    if (line == QByteArrayLiteral("[DONE]")) {
        return ParseResult::Done;
    }

    json chunk;
    try {
        chunk = json::parse(line.constBegin(), line.constEnd());
    } catch (const json::exception &error) {
        QSocConsole::warn() << "Failed to parse stream chunk:" << error.what();
        return ParseResult::Malformed;
    }

    if (chunk.is_object() && chunk.contains("error") && !chunk["error"].is_null()) {
        state->terminalError = providerStreamError(chunk["error"]);
        return ParseResult::ProviderError;
    }

    const QString validationError = validateStreamChunk(chunk);
    if (!validationError.isEmpty()) {
        QSocConsole::warn() << "Malformed stream chunk:" << validationError;
        return ParseResult::Malformed;
    }
    if (chunk.contains("choices") && chunk["choices"].is_array() && !chunk["choices"].empty()) {
        state->sawAssistantChoice = true;
    }

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

    const json &choice     = chunk["choices"].front();
    const json  emptyDelta = json::object();
    const json &delta = choice.contains("delta") && choice["delta"].is_object() ? choice["delta"]
                                                                                : emptyDelta;

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
        QString reasoning = QString::fromStdString(delta["reasoning_content"].get<std::string>());
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
            int index = 0;
            if (toolCall.contains("index") && toolCall["index"].is_number_unsigned()) {
                index = static_cast<int>(toolCall["index"].get<std::uint64_t>());
            } else if (toolCall.contains("index") && toolCall["index"].is_number_integer()) {
                index = static_cast<int>(toolCall["index"].get<std::int64_t>());
            }

            /* Initialize tool call entry if needed */
            if (!state->toolCalls.contains(index)) {
                state->toolCalls[index]
                    = {{"id", ""},
                       {"type", "function"},
                       {"function", {{"name", ""}, {"arguments", ""}}}};
            }

            const auto mergeStableString = [](json &target, const json &fragment) {
                const std::string incoming = fragment.get<std::string>();
                if (incoming.empty()) {
                    return true;
                }
                const std::string current = target.get<std::string>();
                if (!current.empty() && current != incoming) {
                    return false;
                }
                target = incoming;
                return true;
            };

            /* Update ID if present. Some endpoints (e.g. Qwen3 streaming)
             * emit `"id": null` in continuation chunks; only copy when it
             * is a real string so accumulated values stay string-typed. */
            if (toolCall.contains("id") && toolCall["id"].is_string()) {
                if (!mergeStableString(state->toolCalls[index]["id"], toolCall["id"])) {
                    return ParseResult::Malformed;
                }
            }

            /* Update function info */
            if (toolCall.contains("function") && toolCall["function"].is_object()) {
                auto &accFunc = state->toolCalls[index]["function"];

                if (toolCall["function"].contains("name")
                    && toolCall["function"]["name"].is_string()) {
                    if (!mergeStableString(accFunc["name"], toolCall["function"]["name"])) {
                        return ParseResult::Malformed;
                    }
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
    if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) {
        state->finishReason = QString::fromStdString(choice["finish_reason"].get<std::string>());
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

bool QLLMService::extractAssistantMessage(const json &response, json *message, QString *errorMessage)
{
    const auto fail = [errorMessage](const QString &error) {
        if (errorMessage != nullptr) {
            *errorMessage = error;
        }
        return false;
    };
    if (!response.is_object()) {
        return fail(QStringLiteral("Invalid response from LLM"));
    }

    const auto providerError = response.find("error");
    if (providerError != response.end() && !providerError->is_null()) {
        if (providerError->is_string() && !providerError->get<std::string>().empty()) {
            return fail(QString::fromStdString(providerError->get<std::string>()));
        }
        if (providerError->is_object()) {
            const auto detail = providerError->find("message");
            if (detail != providerError->end() && detail->is_string()
                && !detail->get<std::string>().empty()) {
                return fail(QString::fromStdString(detail->get<std::string>()));
            }
        }
        return fail(QStringLiteral("LLM provider returned an error"));
    }

    const auto choices = response.find("choices");
    if (choices == response.end() || !choices->is_array() || choices->empty()
        || !choices->front().is_object()) {
        return fail(QStringLiteral("Invalid response from LLM"));
    }
    const auto assistant = choices->front().find("message");
    if (assistant == choices->front().end() || !assistant->is_object()) {
        return fail(QStringLiteral("Invalid response from LLM"));
    }

    const auto content = assistant->find("content");
    if (content != assistant->end() && !content->is_null() && !content->is_string()) {
        return fail(QStringLiteral("Invalid response from LLM"));
    }

    bool       hasToolCalls = false;
    const auto toolCalls    = assistant->find("tool_calls");
    if (toolCalls != assistant->end() && !toolCalls->is_null()) {
        if (!toolCalls->is_array()) {
            return fail(QStringLiteral("Invalid tool call from LLM"));
        }
        QSet<QString> ids;
        for (const auto &call : *toolCalls) {
            if (!call.is_object()) {
                return fail(QStringLiteral("Invalid tool call from LLM"));
            }
            const auto id       = call.find("id");
            const auto type     = call.find("type");
            const auto function = call.find("function");
            if (id == call.end() || !id->is_string() || id->get<std::string>().empty()
                || type == call.end() || !type->is_string()
                || type->get<std::string>() != "function" || function == call.end()
                || !function->is_object()) {
                return fail(QStringLiteral("Invalid tool call from LLM"));
            }
            const QString callId = QString::fromStdString(id->get<std::string>());
            if (ids.contains(callId)) {
                return fail(QStringLiteral("Invalid tool call from LLM"));
            }
            ids.insert(callId);
            const auto name      = function->find("name");
            const auto arguments = function->find("arguments");
            if (name == function->end() || !name->is_string() || name->get<std::string>().empty()
                || arguments == function->end() || !arguments->is_string()) {
                return fail(QStringLiteral("Invalid tool call from LLM"));
            }
            hasToolCalls = true;
        }
    }
    if (!hasToolCalls && (content == assistant->end() || content->is_null())) {
        return fail(QStringLiteral("Invalid response from LLM"));
    }
    if (message != nullptr) {
        *message = *assistant;
    }
    if (errorMessage != nullptr) {
        errorMessage->clear();
    }
    return true;
}

json QLLMService::sendChatCompletion(const json &messages, const json &tools, double temperature)
{
    return sendChatCompletion(messages, tools, temperature, {});
}

json QLLMService::sendChatCompletion(
    const json &messages, const json &tools, double temperature, std::stop_token stopToken)
{
    if (stopToken.stop_requested()) {
        return {{"error", "Request cancelled"}};
    }
    if (!hasEndpoint()) {
        return {{"error", "No LLM endpoint configured"}};
    }

    const QPointer<QLLMService> owner(this);

    QString                      lastError = QStringLiteral("All LLM endpoints failed");
    const QList<EndpointAttempt> attempts  = endpointAttempts();
    for (const EndpointAttempt &attempt : attempts) {
        if (stopToken.stop_requested()) {
            return {{"error", "Request cancelled"}};
        }
        const LLMEndpoint &endpoint = attempt.endpoint;
        QNetworkRequest    request  = owner->prepareRequest(endpoint);

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

        if (owner->networkManager.isNull()) {
            return {{"error", "Network manager destroyed"}};
        }
        if (stopToken.stop_requested()) {
            return {{"error", "Request cancelled"}};
        }
        QNetworkReply *networkReply
            = owner->networkManager->post(request, QByteArray::fromStdString(payload.dump()));
        const NetworkWaitResult wait
            = waitForNetworkReply(networkReply, endpoint.timeout, stopToken);
        QPointer<QNetworkReply> reply = wait.reply;

        if (wait.cancelled || stopToken.stop_requested()) {
            drainNetworkReply(reply.data());
            return {{"error", "Request cancelled"}};
        }

        if (owner.isNull()) {
            return {{"error", "LLM service destroyed"}};
        }
        if (owner->networkManager.isNull()) {
            return {{"error", "Network manager destroyed"}};
        }
        if (reply.isNull()) {
            QSocConsole::warn() << "Endpoint" << endpoint.name << "failed: Network reply destroyed";
            continue;
        }

        if (reply->error() != QNetworkReply::NoError) {
            QSocConsole::warn() << "Endpoint" << endpoint.name << "failed:" << reply->errorString();
            reply->deleteLater();
            if (stopToken.stop_requested()) {
                return {{"error", "Request cancelled"}};
            }
            continue;
        }

        const QByteArray responseData = reply->readAll();
        reply->deleteLater();

        try {
            json    response = json::parse(responseData.toStdString());
            QString validationError;
            if (!extractAssistantMessage(response, nullptr, &validationError)) {
                lastError = validationError;
                QSocConsole::warn()
                    << "Endpoint" << endpoint.name << "returned an invalid chat response";
                if (stopToken.stop_requested()) {
                    return {{"error", "Request cancelled"}};
                }
                continue;
            }
            if (stopToken.stop_requested()) {
                return {{"error", "Request cancelled"}};
            }
            owner->commitEndpoint(attempt);
            return response;
        } catch (const json::exception &e) {
            QSocConsole::warn() << "JSON parse error:" << e.what();
            if (stopToken.stop_requested()) {
                return {{"error", "Request cancelled"}};
            }
            continue;
        }
    }

    return {{"error", lastError.toStdString()}};
}
