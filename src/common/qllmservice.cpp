#include "common/qllmservice.h"

#include <fstream>
#include <QDebug>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QNetworkRequest>
#include <QProcessEnvironment>
#include <QRegularExpression>
#include <QSettings>
#include <QUrlQuery>

#include <yaml-cpp/yaml.h>

QLLMService::QLLMService(QObject *parent)
    : QObject(parent)
{
    networkManager = new QNetworkAccessManager(this);
    loadApiKeys();
}

QLLMService::~QLLMService()
{
    /* QNetworkAccessManager is a QObject child class and will be cleaned up automatically */
}

void QLLMService::loadApiKeys()
{
    /* Load API keys from environment variables */
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

    /* Try to load from environment variables */
    apiKeys[DEEPSEEK] = env.value("DEEPSEEK_API_KEY", "");
    apiKeys[OPENAI]   = env.value("OPENAI_API_KEY", "");
    apiKeys[GROQ]     = env.value("GROQ_API_KEY", "");
    apiKeys[CLAUDE]   = env.value("CLAUDE_API_KEY", "");
    apiKeys[OLLAMA]   = env.value("OLLAMA_API_KEY", "");

    /* If not found in environment variables, try to load from config file */
    QString configPath = QDir::home().absoluteFilePath(".config/socstudio/config.yaml");
    QDir    configDir  = QFileInfo(configPath).dir();

    /* Create config directory if it doesn't exist */
    if (!configDir.exists()) {
        configDir.mkpath(".");
    }

    YAML::Node config;
    try {
        if (QFile::exists(configPath)) {
            config = YAML::LoadFile(configPath.toStdString());
        } else {
            /* Create default config with commented API keys */
            std::ofstream fout(configPath.toStdString());
            fout << "# SoC Studio API Configuration\n";
            fout << "api:\n";
            fout << "  # Deepseek API key - Get from https://platform.deepseek.com\n";
            fout << "  #deepseek: your_deepseek_api_key\n";
            fout << "  # OpenAI API key - Get from https://platform.openai.com\n";
            fout << "  #openai: your_openai_api_key\n";
            fout << "  # Groq API key - Get from https://console.groq.com\n";
            fout << "  #groq: your_groq_api_key\n";
            fout << "  # Anthropic Claude API key - Get from https://console.anthropic.com\n";
            fout << "  #claude: your_claude_api_key\n";
            fout << "  # Ollama API key - Local deployment\n";
            fout << "  #ollama: your_ollama_api_key\n";
            fout.close();

            config = YAML::LoadFile(configPath.toStdString());
        }

        /* Try to load API keys from YAML if they're not already set */
        if (config["api"]) {
            if (apiKeys[DEEPSEEK].isEmpty() && config["api"]["deepseek"]) {
                apiKeys[DEEPSEEK] = QString::fromStdString(
                    config["api"]["deepseek"].as<std::string>());
            }
            if (apiKeys[OPENAI].isEmpty() && config["api"]["openai"]) {
                apiKeys[OPENAI] = QString::fromStdString(config["api"]["openai"].as<std::string>());
            }
            if (apiKeys[GROQ].isEmpty() && config["api"]["groq"]) {
                apiKeys[GROQ] = QString::fromStdString(config["api"]["groq"].as<std::string>());
            }
            if (apiKeys[CLAUDE].isEmpty() && config["api"]["claude"]) {
                apiKeys[CLAUDE] = QString::fromStdString(config["api"]["claude"].as<std::string>());
            }
            if (apiKeys[OLLAMA].isEmpty() && config["api"]["ollama"]) {
                apiKeys[OLLAMA] = QString::fromStdString(config["api"]["ollama"].as<std::string>());
            }
        }

    } catch (const YAML::Exception &e) {
        qWarning() << "Failed to load/save config:" << e.what();
    }
}

bool QLLMService::isApiKeyConfigured(Provider provider) const
{
    return apiKeys.contains(provider) && !apiKeys[provider].isEmpty();
}

void QLLMService::setApiKey(Provider provider, const QString &apiKey)
{
    apiKeys[provider] = apiKey;

    /* Save API key to config file */
    QString configPath = QDir::home().absoluteFilePath(".config/socstudio/config.yaml");

    YAML::Node config;
    try {
        if (QFile::exists(configPath)) {
            config = YAML::LoadFile(configPath.toStdString());
        }

        /* Create api section if it doesn't exist */
        if (!config["api"]) {
            config["api"] = YAML::Node(YAML::NodeType::Map);
        }

        /* Update the API key */
        switch (provider) {
        case DEEPSEEK:
            config["api"]["deepseek"] = apiKey.toStdString();
            break;
        case OPENAI:
            config["api"]["openai"] = apiKey.toStdString();
            break;
        case GROQ:
            config["api"]["groq"] = apiKey.toStdString();
            break;
        case CLAUDE:
            config["api"]["claude"] = apiKey.toStdString();
            break;
        case OLLAMA:
            config["api"]["ollama"] = apiKey.toStdString();
            break;
        }

        /* Save the updated config */
        std::ofstream fout(configPath.toStdString());
        fout << config;
        fout.close();

    } catch (const YAML::Exception &e) {
        qWarning() << "Failed to save API key to config:" << e.what();
    }
}

QUrl QLLMService::getApiEndpoint(Provider provider) const
{
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

    switch (provider) {
    case DEEPSEEK: {
        payload["model"] = "deepseek-chat"; /* default model */

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
        payload["temperature"] = 0.7; /* Match the temperature from the Rust code */

        if (jsonMode) {
            payload["response_format"] = QJsonObject{{"type", "json_object"}};
        }
        break;
    }
    case OPENAI: {
        payload["model"] = "gpt-4o-mini";
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
        payload["model"] = "mixtral-8x7b-32768"; /* Updated to match Rust code's default model */
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

        payload["messages"] = messages;

        /* Temperature is not set in the Rust code, so we'll only set it if needed */
        if (temperature != 0.2) { /* Only set if not default */
            payload["temperature"] = temperature;
        }

        if (jsonMode) {
            payload["response_format"] = QJsonObject{{"type", "json_object"}};
        }
        break;
    }
    case CLAUDE: {
        payload["model"] = "claude-3-5-sonnet-20241022"; /* Updated to newer model from Rust code */
        payload["max_tokens"] = 4096;                    /* Updated to match Rust code */
        payload["system"]     = systemPrompt;            /* Set system prompt */

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
        payload["model"] = "llama3"; /* Default model */

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

LLMResponse QLLMService::sendRequest(
    Provider       provider,
    const QString &prompt,
    const QString &systemPrompt,
    double         temperature,
    bool           jsonMode)
{
    /* Check if API key is configured */
    if (!isApiKeyConfigured(provider)) {
        LLMResponse response;
        response.success      = false;
        response.errorMessage = QString("API key for provider %1 is not configured").arg(provider);
        return response;
    }

    /* Prepare request */
    QNetworkRequest request(getApiEndpoint(provider));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    /* Set authentication headers based on different providers */
    switch (provider) {
    case DEEPSEEK:
    case OPENAI:
    case GROQ:
        request.setRawHeader("Authorization", QString("Bearer %1").arg(apiKeys[provider]).toUtf8());
        break;
    case CLAUDE:
        request.setRawHeader("x-api-key", apiKeys[provider].toUtf8());
        request.setRawHeader("anthropic-version", "2023-06-01");
        request.setRawHeader("Content-Type", "application/json");
        break;
    case OLLAMA:
        /* Ollama typically doesn't need authentication when running locally */
        break;
    }

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
    /* Check if API key is configured */
    if (!isApiKeyConfigured(provider)) {
        LLMResponse response;
        response.success      = false;
        response.errorMessage = QString("API key for provider %1 is not configured").arg(provider);
        callback(response);
        return;
    }

    /* Prepare request */
    QNetworkRequest request(getApiEndpoint(provider));
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    /* Set authentication headers based on different providers */
    switch (provider) {
    case DEEPSEEK:
    case OPENAI:
    case GROQ:
        request.setRawHeader("Authorization", QString("Bearer %1").arg(apiKeys[provider]).toUtf8());
        break;
    case CLAUDE:
        request.setRawHeader("x-api-key", apiKeys[provider].toUtf8());
        request.setRawHeader("anthropic-version", "2023-06-01");
        request.setRawHeader("Content-Type", "application/json");
        break;
    case OLLAMA:
        /* Ollama typically doesn't need authentication when running locally */
        break;
    }

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
