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

QLLMService::QLLMService(QObject *parent, QSocConfig *config)
    : QObject(parent)
    , config(config)
{
    networkManager = new QNetworkAccessManager(this);
}

QLLMService::~QLLMService()
{
    /* QNetworkAccessManager is a QObject child class and will be cleaned up automatically */
}

void QLLMService::setConfig(QSocConfig *config)
{
    this->config = config;
}

QSocConfig *QLLMService::getConfig()
{
    return config;
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

QString QLLMService::getApiKey(Provider provider) const
{
    /* Check config */
    if (!config) {
        return QString();
    }

    QString providerName = getProviderName(provider);

    /* Priority 1: Global key when ai_provider matches current provider */
    if (config->hasKey("api_key") && config->hasKey("ai_provider")) {
        QString configProvider = config->getValue("ai_provider").toLower();
        if (configProvider == providerName) {
            return config->getValue("api_key");
        }
    }

    /* Priority 2: Global api_key regardless of provider */
    if (config->hasKey("api_key")) {
        return config->getValue("api_key");
    }

    /* Priority 3: Provider-specific key in nested configuration */
    QString providerSpecificKey = providerName + ".api_key";
    if (config->hasKey(providerSpecificKey)) {
        return config->getValue(providerSpecificKey);
    }

    return QString();
}

bool QLLMService::isApiKeyConfigured(Provider provider) const
{
    return !getApiKey(provider).isEmpty();
}

void QLLMService::setApiKey(Provider provider, const QString &apiKey)
{
    /* If config is available, save to it */
    if (config) {
        /* Use modern nested format */
        QString providerName        = getProviderName(provider);
        QString providerSpecificKey = providerName + ".api_key";
        config->setValue(providerSpecificKey, apiKey);
    }
}

QUrl QLLMService::getApiEndpoint(Provider provider) const
{
    if (!config) {
        /* Use default endpoints if config not available */
        return getDefaultApiEndpoint(provider);
    }

    QString providerName = getProviderName(provider);

    /* Priority 1: Global URL when ai_provider matches current provider */
    if (config->hasKey("api_url") && !config->getValue("api_url").isEmpty()
        && config->hasKey("ai_provider")
        && config->getValue("ai_provider").toLower() == providerName) {
        return QUrl(config->getValue("api_url"));
    }

    /* Priority 2: Global URL regardless of provider */
    if (config->hasKey("api_url") && !config->getValue("api_url").isEmpty()) {
        return QUrl(config->getValue("api_url"));
    }

    /* Priority 3: Provider-specific URL in nested configuration */
    QString providerSpecificUrl = providerName + ".api_url";
    if (config->hasKey(providerSpecificUrl) && !config->getValue(providerSpecificUrl).isEmpty()) {
        return QUrl(config->getValue(providerSpecificUrl));
    }

    /* Fall back to default endpoints */
    return getDefaultApiEndpoint(provider);
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

QJsonDocument QLLMService::buildRequestPayload(
    Provider       provider,
    const QString &prompt,
    const QString &systemPrompt,
    double         temperature,
    bool           jsonMode) const
{
    QJsonObject payload;
    QString     model;
    QString     providerName = getProviderName(provider);

    /* Get model from config with proper priority */
    if (config) {
        /* Priority 1: Global model when ai_provider matches current provider */
        if (config->hasKey("ai_model") && config->hasKey("ai_provider")
            && config->getValue("ai_provider").toLower() == providerName) {
            model = config->getValue("ai_model");
        }
        /* Priority 2: Global model regardless of provider */
        else if (config->hasKey("ai_model")) {
            model = config->getValue("ai_model");
        }
        /* Priority 3 (lowest): Provider-specific model in nested configuration */
        else {
            QString providerSpecificModel = providerName + ".ai_model";
            if (config->hasKey(providerSpecificModel)) {
                model = config->getValue(providerSpecificModel);
            }
        }
    }

    switch (provider) {
    case DEEPSEEK: {
        /* Set model from config or use default */
        payload["model"] = model.isEmpty() ? "deepseek-chat" : model;

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
        /* Set model from config or use default */
        payload["model"] = model.isEmpty() ? "gpt-4o-mini" : model;

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
        /* Set model from config or use default */
        payload["model"] = model.isEmpty() ? "mixtral-8x7b-32768" : model;

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
        /* Set model from config or use default */
        payload["model"] = model.isEmpty() ? "claude-3-5-sonnet-20241022" : model;

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
        /* Set model from config or use default */
        payload["model"] = model.isEmpty() ? "llama3" : model;

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

QLLMService::Provider QLLMService::getCurrentProvider(QLLMService::Provider defaultProvider) const
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

    return defaultProvider;
}

QNetworkRequest QLLMService::prepareRequest(QLLMService::Provider provider) const
{
    QNetworkRequest request(getApiEndpoint(provider));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    /* Set authentication headers based on different providers */
    switch (provider) {
    case DEEPSEEK:
    case OPENAI:
    case GROQ:
        request.setRawHeader("Authorization", QString("Bearer %1").arg(getApiKey(provider)).toUtf8());
        break;
    case CLAUDE:
        request.setRawHeader("x-api-key", getApiKey(provider).toUtf8());
        request.setRawHeader("anthropic-version", "2023-06-01");
        request.setRawHeader("Content-Type", "application/json");
        break;
    case OLLAMA:
        /* Ollama typically doesn't need authentication when running locally */
        break;
    }

    return request;
}

LLMResponse QLLMService::sendRequest(
    Provider       provider,
    const QString &prompt,
    const QString &systemPrompt,
    double         temperature,
    bool           jsonMode)
{
    /* Get provider based on config if available */
    provider = getCurrentProvider(provider);

    /* Check if API key is configured */
    if (!isApiKeyConfigured(provider)) {
        LLMResponse response;
        response.success = false;
        response.errorMessage
            = QString("API key for provider %1 is not configured").arg(getProviderName(provider));
        return response;
    }

    /* Prepare request */
    QNetworkRequest request = prepareRequest(provider);

    /* Build request payload */
    QJsonDocument payload
        = buildRequestPayload(provider, prompt, systemPrompt, temperature, jsonMode);

    /* Send request and wait for response */
    QEventLoop     loop;
    QNetworkReply *reply = networkManager->post(request, payload.toJson());

    QObject::connect(reply, SIGNAL(finished()), &loop, SLOT(quit()));
    loop.exec();

    /* Parse response */
    LLMResponse response = parseResponse(provider, reply);
    reply->deleteLater();

    return response;
}

void QLLMService::sendRequestAsync(
    Provider                                 provider,
    const QString                           &prompt,
    std::function<void(const LLMResponse &)> callback,
    const QString                           &systemPrompt,
    double                                   temperature,
    bool                                     jsonMode)
{
    /* Get provider based on config if available */
    provider = getCurrentProvider(provider);

    /* Check if API key is configured */
    if (!isApiKeyConfigured(provider)) {
        LLMResponse response;
        response.success = false;
        response.errorMessage
            = QString("API key for provider %1 is not configured").arg(getProviderName(provider));
        callback(response);
        return;
    }

    /* Prepare request */
    QNetworkRequest request = prepareRequest(provider);

    /* Build request payload */
    QJsonDocument payload
        = buildRequestPayload(provider, prompt, systemPrompt, temperature, jsonMode);

    /* Send asynchronous request */
    QNetworkReply *reply = networkManager->post(request, payload.toJson());

    /* Connect finished signal to handler function */
    QObject::connect(reply, &QNetworkReply::finished, [this, provider, reply, callback]() {
        LLMResponse response = parseResponse(provider, reply);
        reply->deleteLater();
        callback(response);
    });
}

LLMResponse QLLMService::parseResponse(Provider provider, QNetworkReply *reply) const
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
