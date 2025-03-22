#include "common/qllmservice.h"

#include <fstream>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSettings>
#include <QUrlQuery>

#include <yaml-cpp/yaml.h>

/* Constructor and Destructor */

QLLMService::QLLMService(QObject *parent, QSocConfig *config)
    : QObject(parent)
    , config(config)
    , provider(DEEPSEEK) /* Default provider */
{
    networkManager = new QNetworkAccessManager(this);

    /* Load settings from config */
    loadConfigSettings();
}

QLLMService::~QLLMService()
{
    /* QNetworkAccessManager is a QObject child class and will be cleaned up automatically */
}

/* Configuration related methods */

void QLLMService::setConfig(QSocConfig *config)
{
    this->config = config;

    /* Reload settings from new config */
    loadConfigSettings();
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
    QJsonDocument payload = buildRequestPayload(prompt, systemPrompt, temperature, jsonMode);

    /* Send request and wait for response */
    QEventLoop     loop;
    QNetworkReply *reply = networkManager->post(request, payload.toJson());

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
    QJsonDocument payload = buildRequestPayload(prompt, systemPrompt, temperature, jsonMode);

    /* Send asynchronous request */
    QNetworkReply *reply = networkManager->post(request, payload.toJson());

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
    QJsonDocument jsonDoc = QJsonDocument::fromJson(content.toUtf8());
    if (jsonDoc.isObject()) {
        QJsonObject obj = jsonDoc.object();
        for (auto it = obj.begin(); it != obj.end(); ++it) {
            if (it.value().isString()) {
                mappings[it.key()] = it.value().toString();
            }
        }
        return mappings;
    }

    /* Method 2: Extract JSON object from text */
    QRegularExpression      jsonRegex("\\{[^\\{\\}]*\\}");
    QRegularExpressionMatch match = jsonRegex.match(content);

    if (match.hasMatch()) {
        QString       jsonString = match.captured(0);
        QJsonDocument mappingDoc = QJsonDocument::fromJson(jsonString.toUtf8());

        if (mappingDoc.isObject()) {
            QJsonObject mappingObj = mappingDoc.object();
            for (auto it = mappingObj.begin(); it != mappingObj.end(); ++it) {
                if (it.value().isString()) {
                    mappings[it.key()] = it.value().toString();
                }
            }
            return mappings;
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
        return QUrl("https://api.deepseek.com/v1/chat/completions");
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

QJsonDocument QLLMService::buildRequestPayload(
    const QString &prompt, const QString &systemPrompt, double temperature, bool jsonMode) const
{
    QJsonObject payload;

    switch (provider) {
    case DEEPSEEK: {
        /* Set model from stored value or use default */
        payload["model"] = aiModel.isEmpty() ? "deepseek-chat" : aiModel;

        /* Create messages array with system and user messages */
        QJsonArray messages;

        /* Add system message */
        QJsonObject systemMessage;
        systemMessage["role"]    = "system";
        systemMessage["content"] = systemPrompt;
        messages.append(systemMessage);

        /* Add user message */
        QJsonObject userMessage;
        userMessage["role"]    = "user";
        userMessage["content"] = prompt;
        messages.append(userMessage);

        payload["messages"]    = messages;
        payload["temperature"] = temperature;

        if (jsonMode) {
            payload["response_format"] = QJsonObject{{"type", "json_object"}};
        }
        break;
    }
    case OPENAI: {
        /* Set model from stored value or use default */
        payload["model"] = aiModel.isEmpty() ? "gpt-4o-mini" : aiModel;

        QJsonArray messages;

        /* Add system message */
        QJsonObject systemMessage;
        systemMessage["role"]    = "system";
        systemMessage["content"] = systemPrompt;
        messages.append(systemMessage);

        /* Add user message */
        QJsonObject userMessage;
        userMessage["role"]    = "user";
        userMessage["content"] = prompt;
        messages.append(userMessage);

        payload["messages"]    = messages;
        payload["temperature"] = temperature;
        if (jsonMode) {
            payload["response_format"] = QJsonObject{{"type", "json_object"}};
        }
        break;
    }
    case GROQ: {
        /* Set model from stored value or use default */
        payload["model"] = aiModel.isEmpty() ? "mixtral-8x7b-32768" : aiModel;

        QJsonArray messages;

        /* Add system message */
        QJsonObject systemMessage;
        systemMessage["role"]    = "system";
        systemMessage["content"] = systemPrompt;
        messages.append(systemMessage);

        /* Add user message */
        QJsonObject userMessage;
        userMessage["role"]    = "user";
        userMessage["content"] = prompt;
        messages.append(userMessage);

        payload["messages"]    = messages;
        payload["temperature"] = temperature;

        if (jsonMode) {
            payload["response_format"] = QJsonObject{{"type", "json_object"}};
        }
        break;
    }
    case CLAUDE: {
        /* Set model from stored value or use default */
        payload["model"] = aiModel.isEmpty() ? "claude-3-5-sonnet-20241022" : aiModel;

        payload["max_tokens"] = 4096;
        payload["system"]     = systemPrompt;

        QJsonArray messages;

        /* Add user message (Claude only needs the user message, with system in a separate field) */
        QJsonObject userMessage;
        userMessage["role"]    = "user";
        userMessage["content"] = prompt;
        messages.append(userMessage);

        payload["messages"] = messages;

        /* JSON mode is handled by modifying the system prompt if needed */
        if (jsonMode && !systemPrompt.isEmpty()) {
            payload["system"] = systemPrompt + " Respond in JSON format only.";
        } else if (jsonMode) {
            payload["system"] = "Respond in JSON format only.";
        }

        break;
    }
    case OLLAMA: {
        /* Set model from stored value or use default */
        payload["model"] = aiModel.isEmpty() ? "llama3" : aiModel;

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

        payload["prompt"] = combinedPrompt;
        payload["stream"] = false;

        break;
    }
    }

    return QJsonDocument(payload);
}

LLMResponse QLLMService::parseResponse(QNetworkReply *reply) const
{
    LLMResponse response;

    if (reply->error() == QNetworkReply::NoError) {
        QByteArray    responseData = reply->readAll();
        QJsonDocument jsonResponse = QJsonDocument::fromJson(responseData);
        response.success           = true;
        response.jsonDoc           = jsonResponse;

        /* Parse content based on different providers */
        switch (provider) {
        case DEEPSEEK:
        case OPENAI:
        case GROQ: {
            if (jsonResponse.isObject() && jsonResponse.object().contains("choices")) {
                QJsonArray choices = jsonResponse.object()["choices"].toArray();
                if (!choices.isEmpty() && choices[0].isObject()) {
                    QJsonObject choice = choices[0].toObject();
                    if (choice.contains("message") && choice["message"].isObject()) {
                        QJsonObject message = choice["message"].toObject();
                        if (message.contains("content") && message["content"].isString()) {
                            response.content = message["content"].toString();
                        }
                    }
                }
            }
            break;
        }
        case CLAUDE: {
            if (jsonResponse.isObject() && jsonResponse.object().contains("content")) {
                QJsonArray contentArray = jsonResponse.object()["content"].toArray();
                if (!contentArray.isEmpty()) {
                    /* Get the first content item */
                    QJsonObject firstContent = contentArray.first().toObject();

                    /* Extract the text field as in the Rust implementation */
                    if (firstContent.contains("text")) {
                        response.content = firstContent["text"].toString();
                    } else if (firstContent.contains("type") && firstContent["type"].toString() == "text") {
                        response.content = firstContent["text"].toString();
                    }
                }
            }
            break;
        }
        case OLLAMA: {
            if (jsonResponse.isObject() && jsonResponse.object().contains("response")) {
                QString responseText = jsonResponse.object()["response"].toString();
                response.content     = responseText;
            }
            break;
        }
        }

        if (response.content.isEmpty()) {
            response.success      = false;
            response.errorMessage = "Could not extract content from response";
            qWarning() << "Failed to extract content from LLM response:" << jsonResponse.toJson();
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
