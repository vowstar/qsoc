#ifndef QLLMSERVICE_H
#define QLLMSERVICE_H

#include "common/qsocconfig.h"

#include <functional>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>

/**
 * @brief The LLMResponse struct.
 * @details This struct holds the result of an LLM request.
 */
struct LLMResponse
{
    bool          success;      /* Whether the request was successful */
    QString       content;      /* Text content returned by the LLM */
    QJsonDocument jsonDoc;      /* Parsed JSON response if available */
    QString       errorMessage; /* Error message if the request failed */
};

/**
 * @brief The QLLMService class provides a general interface for LLM API services
 * @details This class handles API communication with various LLM providers
 */
class QLLMService : public QObject
{
    Q_OBJECT

public:
    enum Provider {
        DEEPSEEK, /* Default provider */
        OPENAI,
        GROQ,
        CLAUDE,
        OLLAMA
        /* More providers can be added */
    };

    /**
     * @brief Constructor for QLLMService
     * @param parent Parent object
     * @param config Configuration manager, can be nullptr
     */
    explicit QLLMService(QObject *parent = nullptr, QSocConfig *config = nullptr);
    ~QLLMService();

    /**
     * @brief Set the configuration manager
     * @details Sets the configuration manager and reloads API keys.
     * @param config Configuration manager, can be nullptr
     */
    void setConfig(QSocConfig *config);

    /**
     * @brief Get the configuration manager
     * @details Retrieves the currently assigned configuration manager.
     * @return QSocConfig* Pointer to the current configuration manager.
     */
    QSocConfig *getConfig();

    /**
     * @brief Send a synchronous request to an LLM
     * @param provider LLM provider
     * @param prompt User prompt content
     * @param systemPrompt System prompt to guide AI role and behavior
     * @param temperature Temperature parameter (0.0-1.0)
     * @param jsonMode Whether to request JSON format output from the LLM
     * @return LLM response result
     */
    LLMResponse sendRequest(
        Provider       provider,
        const QString &prompt,
        const QString &systemPrompt
        = "You are a helpful assistant that provides accurate and informative responses.",
        double temperature = 0.2,
        bool   jsonMode    = false);

    /**
     * @brief Send an asynchronous request to an LLM
     * @param provider LLM provider
     * @param prompt User prompt content
     * @param callback Callback function to handle the response
     * @param systemPrompt System prompt to guide AI role and behavior
     * @param temperature Temperature parameter (0.0-1.0)
     * @param jsonMode Whether to request JSON format output from the LLM
     */
    void sendRequestAsync(
        Provider                                 provider,
        const QString                           &prompt,
        std::function<void(const LLMResponse &)> callback,
        const QString                           &systemPrompt
        = "You are a helpful assistant that provides accurate and informative responses.",
        double temperature = 0.2,
        bool   jsonMode    = false);

    /**
     * @brief Check if an API key is configured for the given provider
     * @param provider LLM provider
     * @return Whether the API key is configured
     */
    bool isApiKeyConfigured(Provider provider) const;

    /**
     * @brief Manually set an API key
     * @param provider LLM provider
     * @param apiKey API key
     */
    void setApiKey(Provider provider, const QString &apiKey);

    /**
     * @brief Extract key-value pairs from a JSON response
     * @param response LLM response
     * @return Extracted key-value mapping
     */
    static QMap<QString, QString> extractMappingsFromResponse(const LLMResponse &response);

private:
    QNetworkAccessManager *networkManager;
    QSocConfig            *config;

    /**
     * @brief Get the API key for the given provider
     * @param provider LLM provider
     * @return API key for the provider
     */
    QString getApiKey(Provider provider) const;

    /**
     * @brief Get provider name as string
     * @param provider LLM provider
     * @return Provider name as string
     */
    QString getProviderName(Provider provider) const;

    /**
     * @brief Get the API endpoint URL for a provider
     * @param provider LLM provider
     * @return API endpoint URL
     */
    QUrl getApiEndpoint(Provider provider) const;

    /**
     * @brief Get the default API endpoint URL for a provider
     * @param provider LLM provider
     * @return Default API endpoint URL
     */
    QUrl getDefaultApiEndpoint(Provider provider) const;

    /**
     * @brief Build the request payload
     * @param provider LLM provider
     * @param prompt User prompt content
     * @param systemPrompt System prompt content
     * @param temperature Temperature parameter
     * @param jsonMode Whether to request JSON format output
     * @return JSON document for the request payload
     */
    QJsonDocument buildRequestPayload(
        Provider       provider,
        const QString &prompt,
        const QString &systemPrompt,
        double         temperature,
        bool           jsonMode) const;

    /**
     * @brief Parse the API response
     * @param provider LLM provider
     * @param reply Network response
     * @return Parsed LLM response struct
     */
    LLMResponse parseResponse(Provider provider, QNetworkReply *reply) const;

    /**
     * @brief Get current provider from configuration
     * @param defaultProvider Default provider to use if not specified in config
     * @return Provider to use
     */
    Provider getCurrentProvider(Provider defaultProvider) const;

    /**
     * @brief Prepare network request for given provider
     * @param provider LLM provider
     * @return Configured network request
     */
    QNetworkRequest prepareRequest(Provider provider) const;
};

#endif // QLLMSERVICE_H
