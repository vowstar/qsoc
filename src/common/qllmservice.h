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

    /**
     * @brief Destructor for QLLMService
     */
    ~QLLMService();

public slots:
    /* Configuration related methods */

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

    /* Provider related methods */

    /**
     * @brief Set the provider to use for API requests
     * @param provider Provider to use
     */
    void setProvider(Provider provider);

    /**
     * @brief Get the current provider used for API requests
     * @return Current provider
     */
    Provider getProvider() const;

    /**
     * @brief Get provider name as string
     * @param provider LLM provider
     * @return Provider name as string
     */
    QString getProviderName(Provider provider) const;

    /* API key related methods */

    /**
     * @brief Check if an API key is configured
     * @return Whether the API key is configured
     */
    bool isApiKeyConfigured() const;

    /**
     * @brief Get the API key
     * @return API key for the current provider
     */
    QString getApiKey() const;

    /**
     * @brief Manually set an API key
     * @param apiKey API key to set
     */
    void setApiKey(const QString &apiKey);

    /* API endpoint related methods */

    /**
     * @brief Get the API endpoint URL
     * @return API endpoint URL for the current provider
     */
    QUrl getApiEndpoint() const;

    /* LLM request methods */

    /**
     * @brief Send a synchronous request to an LLM
     * @param prompt User prompt content
     * @param systemPrompt System prompt to guide AI role and behavior
     * @param temperature Temperature parameter (0.0-1.0)
     * @param jsonMode Whether to request JSON format output from the LLM
     * @return LLM response result
     */
    LLMResponse sendRequest(
        const QString &prompt,
        const QString &systemPrompt
        = "You are a helpful assistant that provides accurate and informative responses.",
        double temperature = 0.2,
        bool   jsonMode    = false);

    /**
     * @brief Send an asynchronous request to an LLM
     * @param prompt User prompt content
     * @param callback Callback function to handle the response
     * @param systemPrompt System prompt to guide AI role and behavior
     * @param temperature Temperature parameter (0.0-1.0)
     * @param jsonMode Whether to request JSON format output from the LLM
     */
    void sendRequestAsync(
        const QString                     &prompt,
        std::function<void(LLMResponse &)> callback,
        const QString                     &systemPrompt
        = "You are a helpful assistant that provides accurate and informative responses.",
        double temperature = 0.2,
        bool   jsonMode    = false);

    /* Utility methods */

    /**
     * @brief Extract key-value pairs from a JSON response
     * @param response LLM response
     * @return Extracted key-value mapping
     */
    static QMap<QString, QString> extractMappingsFromResponse(const LLMResponse &response);

private:
    QNetworkAccessManager *networkManager = nullptr;
    QSocConfig            *config         = nullptr;
    Provider               provider       = DEEPSEEK;
    QString                apiKey;
    QUrl                   apiUrl;
    QString                aiModel;

    /**
     * @brief Load configuration settings from config
     * @details Loads provider, apiKey, apiUrl, aiModel according to priority rules
     */
    void loadConfigSettings();

    /**
     * @brief Set up network proxy based on configuration settings
     * @details Configures the network proxy for the networkManager based on
     *          settings from the configuration (proxy_type, proxy_host, etc.)
     */
    void setupNetworkProxy();

    /**
     * @brief Get the default API endpoint URL for a provider
     * @param provider LLM provider
     * @return Default API endpoint URL
     */
    QUrl getDefaultApiEndpoint(Provider provider) const;

    /**
     * @brief Get current provider from configuration
     * @return Provider to use
     */
    Provider getCurrentProvider() const;

    /**
     * @brief Prepare network request for the current provider
     * @return Configured network request
     */
    QNetworkRequest prepareRequest() const;

    /**
     * @brief Build the request payload
     * @param prompt User prompt content
     * @param systemPrompt System prompt content
     * @param temperature Temperature parameter
     * @param jsonMode Whether to request JSON format output
     * @return JSON document for the request payload
     */
    QJsonDocument buildRequestPayload(
        const QString &prompt, const QString &systemPrompt, double temperature, bool jsonMode) const;

    /**
     * @brief Parse the API response
     * @param reply Network response
     * @return Parsed LLM response struct
     */
    LLMResponse parseResponse(QNetworkReply *reply) const;
};

#endif // QLLMSERVICE_H
