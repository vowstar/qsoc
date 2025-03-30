// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#include "common/qllmservice.h"

#include <fstream>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkProxy>
#include <QNetworkProxyFactory>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>
#include <QUrlQuery>

#include <yaml-cpp/yaml.h>

/* Constructor and Destructor */

QLLMService::QLLMService(QObject *parent, QSocConfig *config)
    : QObject(parent)
    , config(config)
{
    /* Initialize network manager */
    networkManager = new QNetworkAccessManager(this);
    /* Configure network proxy */
    setupNetworkProxy();
}

QLLMService::~QLLMService() = default;

/* Configuration related methods */

void QLLMService::setConfig(QSocConfig *config)
{
    this->config = config;

    /* Reload settings from new config */
    loadConfigSettings();

    /* Update network proxy */
    setupNetworkProxy();
}

QSocConfig *QLLMService::getConfig()
{
    return config;
}

/* Provider related methods */

void QLLMService::setProvider(Provider newProvider)
{
    this->provider = newProvider;

    /* Reload API key, URL and model for the new provider */
    if (config) {
        loadConfigSettings();
    }
}

QLLMService::Provider QLLMService::getProvider() const
{
    return provider;
}

QString QLLMService::getProviderName(Provider provider) const
{
    switch (provider) {
    case DEEPSEEK:
        return "deepseek";
    case OPENAI:
        return "openai";
    case GROQ:
        return "groq";
    case CLAUDE:
        return "claude";
    case OLLAMA:
        return "ollama";
    default:
        return QString();
    }
}

/* API key related methods */

bool QLLMService::isApiKeyConfigured() const
{
    return !apiKey.isEmpty();
}

QString QLLMService::getApiKey() const
{
    return apiKey;
}

void QLLMService::setApiKey(const QString &newApiKey)
{
    this->apiKey = newApiKey;

    /* If config is available, save to it */
    if (config) {
        /* Use modern nested format */
        QString providerName        = getProviderName(provider);
        QString providerSpecificKey = providerName + ".api_key";
        config->setValue(providerSpecificKey, newApiKey);
    }
}

/* API endpoint related methods */

QUrl QLLMService::getApiEndpoint() const
{
    return apiUrl;
}

/* LLM request methods */

LLMResponse QLLMService::sendRequest(
    const QString &prompt, const QString &systemPrompt, double temperature, bool jsonMode)
{
    /* Check if API key is configured */
    if (!isApiKeyConfigured()) {
        LLMResponse response;
        response.success = false;
        response.errorMessage
            = QString("API key for provider %1 is not configured").arg(getProviderName(provider));
        return response;
    }

    /* Prepare request */
    QNetworkRequest request = prepareRequest();

    /* Build request payload */
    json payload = buildRequestPayload(prompt, systemPrompt, temperature, jsonMode);

    /* Send request and wait for response */
    QEventLoop     loop;
    QNetworkReply *reply = networkManager->post(request, QByteArray::fromStdString(payload.dump()));

    QObject::connect(reply, SIGNAL(finished()), &loop, SLOT(quit()));
    loop.exec();

    /* Parse response */
    LLMResponse response = parseResponse(reply);
    reply->deleteLater();

    return response;
}

void QLLMService::sendRequestAsync(
    const QString                     &prompt,
    std::function<void(LLMResponse &)> callback,
    const QString                     &systemPrompt,
    double                             temperature,
    bool                               jsonMode)
{
    /* Check if API key is configured */
    if (!isApiKeyConfigured()) {
        LLMResponse response;
        response.success = false;
        response.errorMessage
            = QString("API key for provider %1 is not configured").arg(getProviderName(provider));
        callback(response);
        return;
    }

    /* Prepare request */
    QNetworkRequest request = prepareRequest();

    /* Build request payload */
    json payload = buildRequestPayload(prompt, systemPrompt, temperature, jsonMode);

    /* Send asynchronous request */
    QNetworkReply *reply = networkManager->post(request, QByteArray::fromStdString(payload.dump()));

    /* Connect finished signal to handler function */
    QObject::connect(reply, &QNetworkReply::finished, [this, reply, callback]() {
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

    /* Try to parse JSON from the response */
    QString content = response.content.trimmed();

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
    } catch (const json::parse_error &) {
        // Continue with other methods if JSON parsing fails
    }

    /* Method 2: Extract JSON object from text */
    QRegularExpression      jsonRegex("\\{[^\\{\\}]*\\}");
    QRegularExpressionMatch match = jsonRegex.match(content);

    if (match.hasMatch()) {
        QString jsonString = match.captured(0);
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
        } catch (const json::parse_error &) {
            // Continue with other methods if JSON parsing fails
        }
    }

    /* Method 3: Parse from text format */
    QStringList        lines = content.split("\n");
    QRegularExpression mappingRegex("\"(.*?)\"\\s*:\\s*\"(.*?)\"");

    for (const QString &line : lines) {
        QRegularExpressionMatch match = mappingRegex.match(line);
        if (match.hasMatch()) {
            QString key   = match.captured(1);
            QString value = match.captured(2);
            mappings[key] = value;
        }
    }

    return mappings;
}

/* Private methods */

void QLLMService::loadConfigSettings()
{
    /* Skip if no config */
    if (!config) {
        return;
    }

    /* 1. Load provider from config */
    if (config->hasKey("ai_provider")) {
        QString configProvider = config->getValue("ai_provider").toLower();

        if (configProvider == "deepseek") {
            provider = DEEPSEEK;
        } else if (configProvider == "openai") {
            provider = OPENAI;
        } else if (configProvider == "groq") {
            provider = GROQ;
        } else if (configProvider == "claude") {
            provider = CLAUDE;
        } else if (configProvider == "ollama") {
            provider = OLLAMA;
        }
    }

    /* Get provider name for further lookups */
    QString providerName = getProviderName(provider);

    /* 2. Load API key using priority rules */
    /* Priority 1: Global key when ai_provider matches current provider */
    if (config->hasKey("api_key") && config->hasKey("ai_provider")) {
        QString configProvider = config->getValue("ai_provider").toLower();
        if (configProvider == providerName) {
            apiKey = config->getValue("api_key");
        }
    }

    /* Priority 2: Global api_key regardless of provider */
    if (apiKey.isEmpty() && config->hasKey("api_key")) {
        apiKey = config->getValue("api_key");
    }

    /* Priority 3: Provider-specific key */
    if (apiKey.isEmpty()) {
        QString providerSpecificKey = providerName + ".api_key";
        if (config->hasKey(providerSpecificKey)) {
            apiKey = config->getValue(providerSpecificKey);
        }
    }

    /* 3. Load API URL using priority rules */
    /* Priority 1: Global URL when ai_provider matches current provider */
    if (config->hasKey("api_url") && !config->getValue("api_url").isEmpty()
        && config->hasKey("ai_provider")
        && config->getValue("ai_provider").toLower() == providerName) {
        apiUrl = QUrl(config->getValue("api_url"));
    }
    /* Priority 2: Global URL regardless of provider */
    else if (config->hasKey("api_url") && !config->getValue("api_url").isEmpty()) {
        apiUrl = QUrl(config->getValue("api_url"));
    }
    /* Priority 3: Provider-specific URL */
    else {
        QString providerSpecificUrl = providerName + ".api_url";
        if (config->hasKey(providerSpecificUrl)
            && !config->getValue(providerSpecificUrl).isEmpty()) {
            apiUrl = QUrl(config->getValue(providerSpecificUrl));
        } else {
            /* Fall back to default URL if none specified */
            apiUrl = getDefaultApiEndpoint(provider);
        }
    }

    /* 4. Load AI model using priority rules */
    /* Priority 1: Global model when ai_provider matches current provider */
    if (config->hasKey("ai_model") && config->hasKey("ai_provider")
        && config->getValue("ai_provider").toLower() == providerName) {
        aiModel = config->getValue("ai_model");
    }
    /* Priority 2: Global model regardless of provider */
    else if (config->hasKey("ai_model")) {
        aiModel = config->getValue("ai_model");
    }
    /* Priority 3: Provider-specific model */
    else {
        QString providerSpecificModel = providerName + ".ai_model";
        if (config->hasKey(providerSpecificModel)) {
            aiModel = config->getValue(providerSpecificModel);
        } else {
            /* Leave empty, default models will be provided in buildRequestPayload */
            aiModel = "";
        }
    }
}

QUrl QLLMService::getDefaultApiEndpoint(Provider provider) const
{
    /* Default endpoints for each provider */
    switch (provider) {
    case DEEPSEEK:
        return QUrl("https://api.deepseek.com/chat/completions");
    case OPENAI:
        return QUrl("https://api.openai.com/v1/chat/completions");
    case GROQ:
        return QUrl("https://api.groq.com/openai/v1/chat/completions");
    case CLAUDE:
        return QUrl("https://api.anthropic.com/v1/messages");
    case OLLAMA:
        return QUrl("http://localhost:11434/api/generate");
    default:
        return QUrl();
    }
}

QLLMService::Provider QLLMService::getCurrentProvider() const
{
    /* Use provider from config if available */
    if (config && config->hasKey("ai_provider")) {
        QString configProvider = config->getValue("ai_provider").toLower();

        if (configProvider == "deepseek") {
            return DEEPSEEK;
        } else if (configProvider == "openai") {
            return OPENAI;
        } else if (configProvider == "groq") {
            return GROQ;
        } else if (configProvider == "claude") {
            return CLAUDE;
        } else if (configProvider == "ollama") {
            return OLLAMA;
        }
    }

    /* Return current provider as default */
    return provider;
}

QNetworkRequest QLLMService::prepareRequest() const
{
    QNetworkRequest request(getApiEndpoint());
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    /* Set authentication headers based on different providers */
    switch (provider) {
    case DEEPSEEK:
    case OPENAI:
    case GROQ:
        request.setRawHeader("Authorization", QString("Bearer %1").arg(getApiKey()).toUtf8());
        break;
    case CLAUDE:
        request.setRawHeader("x-api-key", getApiKey().toUtf8());
        request.setRawHeader("anthropic-version", "2023-06-01");
        request.setRawHeader("Content-Type", "application/json");
        break;
    case OLLAMA:
        /* Ollama typically doesn't need authentication when running locally */
        break;
    }

    return request;
}

json QLLMService::buildRequestPayload(
    const QString &prompt, const QString &systemPrompt, double temperature, bool jsonMode) const
{
    json payload;

    switch (provider) {
    case DEEPSEEK: {
        /* Set model from stored value or use default */
        payload["model"] = aiModel.isEmpty() ? "deepseek-chat" : aiModel.toStdString();

        /* Create messages array with system and user messages */
        json messages = json::array();

        /* Add system message */
        json systemMessage;
        systemMessage["role"]    = "system";
        systemMessage["content"] = systemPrompt.toStdString();
        messages.push_back(systemMessage);

        /* Add user message */
        json userMessage;
        userMessage["role"]    = "user";
        userMessage["content"] = prompt.toStdString();
        messages.push_back(userMessage);

        payload["messages"]    = messages;
        payload["stream"]      = false;
        payload["temperature"] = temperature;

        /* Only add JSON format for models that support it (deepseek-reasoner doesn't) */
        if (jsonMode && !aiModel.contains("reasoner", Qt::CaseInsensitive)) {
            payload["response_format"] = {{"type", "json_object"}};
        }
        break;
    }
    case OPENAI: {
        /* Set model from stored value or use default */
        payload["model"] = aiModel.isEmpty() ? "gpt-4o-mini" : aiModel.toStdString();

        json messages = json::array();

        /* Add system message */
        json systemMessage;
        systemMessage["role"]    = "system";
        systemMessage["content"] = systemPrompt.toStdString();
        messages.push_back(systemMessage);

        /* Add user message */
        json userMessage;
        userMessage["role"]    = "user";
        userMessage["content"] = prompt.toStdString();
        messages.push_back(userMessage);

        payload["messages"]    = messages;
        payload["temperature"] = temperature;
        if (jsonMode) {
            payload["response_format"] = {{"type", "json_object"}};
        }
        break;
    }
    case GROQ: {
        /* Set model from stored value or use default */
        payload["model"] = aiModel.isEmpty() ? "mixtral-8x7b-32768" : aiModel.toStdString();

        json messages = json::array();

        /* Add system message */
        json systemMessage;
        systemMessage["role"]    = "system";
        systemMessage["content"] = systemPrompt.toStdString();
        messages.push_back(systemMessage);

        /* Add user message */
        json userMessage;
        userMessage["role"]    = "user";
        userMessage["content"] = prompt.toStdString();
        messages.push_back(userMessage);

        payload["messages"]    = messages;
        payload["temperature"] = temperature;

        if (jsonMode) {
            payload["response_format"] = {{"type", "json_object"}};
        }
        break;
    }
    case CLAUDE: {
        /* Set model from stored value or use default */
        payload["model"] = aiModel.isEmpty() ? "claude-3-5-sonnet-20241022" : aiModel.toStdString();

        payload["max_tokens"] = 4096;
        payload["system"]     = systemPrompt.toStdString();

        json messages = json::array();

        /* Add user message (Claude only needs the user message, with system in a separate field) */
        json userMessage;
        userMessage["role"]    = "user";
        userMessage["content"] = prompt.toStdString();
        messages.push_back(userMessage);

        payload["messages"] = messages;

        /* JSON mode is handled by modifying the system prompt if needed */
        if (jsonMode && !systemPrompt.isEmpty()) {
            payload["system"] = (systemPrompt + " Respond in JSON format only.").toStdString();
        } else if (jsonMode) {
            payload["system"] = "Respond in JSON format only.";
        }

        break;
    }
    case OLLAMA: {
        /* Set model from stored value or use default */
        payload["model"] = aiModel.isEmpty() ? "llama3" : aiModel.toStdString();

        /* Format prompt by combining system prompt and user prompt */
        QString combinedPrompt;
        if (!systemPrompt.isEmpty()) {
            combinedPrompt = systemPrompt + "\n\n" + prompt;
        } else {
            combinedPrompt = prompt;
        }

        /* Add instruction for JSON output if needed */
        if (jsonMode) {
            combinedPrompt += "\n\nRespond in JSON format only.";
        }

        payload["prompt"] = combinedPrompt.toStdString();
        payload["stream"] = false;

        break;
    }
    }

    return payload;
}

LLMResponse QLLMService::parseResponse(QNetworkReply *reply) const
{
    LLMResponse response;

    if (reply->error() == QNetworkReply::NoError) {
        QByteArray responseData = reply->readAll();

        try {
            json jsonResponse = json::parse(responseData.toStdString());
            response.success  = true;
            response.jsonData = jsonResponse;

            /* Parse content based on different providers */
            switch (provider) {
            case DEEPSEEK:
            case OPENAI:
            case GROQ: {
                /* First try standard OpenAI-compatible format */
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
                /* Handle any non-standard but valid JSON format */
                else if (!jsonResponse.empty()) {
                    /* For custom JSON structures, return the complete formatted JSON */
                    response.content = QString::fromStdString(jsonResponse.dump(2));
                }
                break;
            }
            case CLAUDE: {
                if (jsonResponse.contains("content") && jsonResponse["content"].is_array()
                    && !jsonResponse["content"].empty()) {
                    /* Get the first content item */
                    auto firstContent = jsonResponse["content"][0];

                    /* Extract the text field */
                    if (firstContent.contains("text")) {
                        response.content = QString::fromStdString(
                            firstContent["text"].get<std::string>());
                    } else if (
                        firstContent.contains("type")
                        && firstContent["type"].get<std::string>() == "text") {
                        response.content = QString::fromStdString(
                            firstContent["text"].get<std::string>());
                    }
                }
                break;
            }
            case OLLAMA: {
                if (jsonResponse.contains("response")) {
                    response.content = QString::fromStdString(
                        jsonResponse["response"].get<std::string>());
                }
                break;
            }
            }

            // If we couldn't parse the content with specific provider rules,
            // just convert the entire JSON to a string
            if (response.content.isEmpty()) {
                try {
                    response.content = QString::fromStdString(jsonResponse.dump(2));
                } catch (const std::exception &e) {
                    response.success      = false;
                    response.errorMessage = QString("Failed to extract content: %1").arg(e.what());
                    qWarning() << "Failed to extract content from LLM response:" << e.what();
                }
            }
        } catch (const json::parse_error &e) {
            response.success      = false;
            response.errorMessage = QString("JSON parse error: %1").arg(e.what());
            qWarning() << "JSON parse error:" << e.what();
            qWarning() << "Raw response:" << responseData;
        }
    } else {
        response.success      = false;
        response.errorMessage = reply->errorString();
        QByteArray errorData  = reply->readAll();
        qWarning() << "LLM API request failed:" << reply->errorString();
        qWarning() << "Error response:" << errorData;
    }

    return response;
}

void QLLMService::setupNetworkProxy()
{
    /* Skip if no config or network manager */
    if (!config || !networkManager) {
        return;
    }

    /* Get proxy type, default is "system" */
    QString proxyType = config->getValue("proxy_type", "system").toLower();

    QNetworkProxy proxy;

    if (proxyType == "none") {
        /* No proxy */
        proxy.setType(QNetworkProxy::NoProxy);
    } else if (proxyType == "default") {
        /* Use application-defined proxy */
        proxy.setType(QNetworkProxy::DefaultProxy);
    } else if (proxyType == "socks5") {
        /* Use SOCKS5 proxy */
        proxy.setType(QNetworkProxy::Socks5Proxy);
        proxy.setHostName(config->getValue("proxy_host", "127.0.0.1"));
        proxy.setPort(config->getValue("proxy_port", "1080").toUInt());

        /* Set authentication if provided */
        if (config->hasKey("proxy_user")) {
            proxy.setUser(config->getValue("proxy_user"));
            if (config->hasKey("proxy_password")) {
                proxy.setPassword(config->getValue("proxy_password"));
            }
        }
    } else if (proxyType == "http") {
        /* Use HTTP proxy */
        proxy.setType(QNetworkProxy::HttpProxy);
        proxy.setHostName(config->getValue("proxy_host", "127.0.0.1"));
        proxy.setPort(config->getValue("proxy_port", "8080").toUInt());

        /* Set authentication if provided */
        if (config->hasKey("proxy_user")) {
            proxy.setUser(config->getValue("proxy_user"));
            if (config->hasKey("proxy_password")) {
                proxy.setPassword(config->getValue("proxy_password"));
            }
        }
    } else {
        /* Default to system proxy settings */
        QNetworkProxyFactory::setUseSystemConfiguration(true);
        networkManager->setProxy(QNetworkProxy::DefaultProxy);
        return;
    }

    /* Apply proxy settings to network manager */
    networkManager->setProxy(proxy);
}
