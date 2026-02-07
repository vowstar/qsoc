// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QLLMSERVICE_H
#define QLLMSERVICE_H

#include "common/qsocconfig.h"

#include <functional>
#include <nlohmann/json.hpp>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QObject>
#include <QString>
#include <QUrl>

using json = nlohmann::json;

/**
 * @brief LLM endpoint configuration
 * @details Holds configuration for a single LLM API endpoint
 */
struct LLMEndpoint
{
    QString name;            /* Endpoint name for identification */
    QUrl    url;             /* API endpoint URL */
    QString key;             /* API key (optional for local services) */
    QString model;           /* Model name to use */
    int     timeout = 30000; /* Request timeout in milliseconds */
};

/**
 * @brief The LLMResponse struct
 * @details This struct holds the result of an LLM request
 */
struct LLMResponse
{
    bool    success;      /* Whether the request was successful */
    QString content;      /* Text content returned by the LLM */
    json    jsonData;     /* Parsed JSON response if available */
    QString errorMessage; /* Error message if the request failed */
};

/**
 * @brief Fallback strategy for multiple endpoints
 */
enum class LLMFallbackStrategy : std::uint8_t {
    Sequential, /* Try endpoints in order */
    Random,     /* Try endpoints in random order */
    RoundRobin  /* Rotate through endpoints */
};

/**
 * @brief The QLLMService class provides a unified interface for LLM API services
 * @details This class handles API communication using OpenAI Chat Completions format.
 *          All providers (OpenAI, DeepSeek, Groq, Claude, Ollama) support this format.
 */
class QLLMService : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Constructor for QLLMService
     * @param parent Parent object
     * @param config Configuration manager, can be nullptr
     */
    explicit QLLMService(QObject *parent = nullptr, QSocConfig *config = nullptr);

    /**
     * @brief Destructor for QLLMService
     */
    ~QLLMService() override;

public slots:
    /* Configuration */

    /**
     * @brief Set the configuration manager
     * @param config Configuration manager, can be nullptr
     */
    void setConfig(QSocConfig *config);

    /**
     * @brief Get the configuration manager
     * @return Pointer to the current configuration manager
     */
    QSocConfig *getConfig();

    /* Endpoint management */

    /**
     * @brief Add an endpoint to the service
     * @param endpoint Endpoint configuration to add
     */
    void addEndpoint(const LLMEndpoint &endpoint);

    /**
     * @brief Clear all endpoints
     */
    void clearEndpoints();

    /**
     * @brief Get the number of configured endpoints
     * @return Number of endpoints
     */
    int endpointCount() const;

    /**
     * @brief Check if at least one endpoint is configured
     * @return True if at least one endpoint is available
     */
    bool hasEndpoint() const;

    /**
     * @brief Set the fallback strategy
     * @param strategy Strategy to use when an endpoint fails
     */
    void setFallbackStrategy(LLMFallbackStrategy strategy);

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
        const QString                            &prompt,
        const std::function<void(LLMResponse &)> &callback,
        const QString                            &systemPrompt
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

    /**
     * @brief Send chat completion with tool definitions (Agent mode)
     * @details Sends a request using the OpenAI Chat Completions format with
     *          tool/function calling support. Returns the full JSON response
     *          which may contain tool_calls that need to be processed.
     * @param messages Conversation history in OpenAI format
     * @param tools Tool definitions in OpenAI format (optional)
     * @param temperature Temperature parameter (0.0-1.0)
     * @return Full JSON response from the LLM (may contain tool_calls)
     */
    json sendChatCompletion(
        const json &messages, const json &tools = json::array(), double temperature = 0.2);

    /**
     * @brief Send streaming chat completion with tool definitions (Agent mode)
     * @details Sends a request using SSE streaming. Emits signals for each chunk.
     *          Connect to streamChunk, streamToolCall, streamComplete, streamError signals.
     * @param messages Conversation history in OpenAI format
     * @param tools Tool definitions in OpenAI format (optional)
     * @param temperature Temperature parameter (0.0-1.0)
     */
    void sendChatCompletionStream(
        const json &messages, const json &tools = json::array(), double temperature = 0.2);

signals:
    /**
     * @brief Signal emitted when a text chunk is received during streaming
     * @param chunk The text content chunk
     */
    void streamChunk(const QString &chunk);

    /**
     * @brief Signal emitted when a tool call is detected during streaming
     * @param id Tool call ID
     * @param name Function name
     * @param arguments JSON arguments (may be partial during streaming)
     */
    void streamToolCall(const QString &id, const QString &name, const QString &arguments);

    /**
     * @brief Signal emitted when streaming is complete
     * @param response The complete response JSON
     */
    void streamComplete(const json &response);

    /**
     * @brief Signal emitted when an error occurs during streaming
     * @param error Error message
     */
    void streamError(const QString &error);

private:
    QNetworkAccessManager *networkManager = nullptr;
    QSocConfig            *config         = nullptr;
    QList<LLMEndpoint>     endpoints;
    int                    currentEndpoint  = 0;
    LLMFallbackStrategy    fallbackStrategy = LLMFallbackStrategy::Sequential;

    /**
     * @brief Load configuration settings from config
     */
    void loadConfigSettings();

    /**
     * @brief Set up network proxy based on configuration settings
     */
    void setupNetworkProxy();

    /**
     * @brief Select an endpoint based on fallback strategy
     * @return Selected endpoint, or empty endpoint if none available
     */
    LLMEndpoint selectEndpoint();

    /**
     * @brief Advance to next endpoint for fallback
     */
    void advanceEndpoint();

    /**
     * @brief Prepare network request for an endpoint
     * @param endpoint Endpoint to prepare request for
     * @return Configured network request
     */
    QNetworkRequest prepareRequest(const LLMEndpoint &endpoint) const;

    /**
     * @brief Build the request payload (OpenAI Chat Completions format)
     * @param prompt User prompt content
     * @param systemPrompt System prompt content
     * @param temperature Temperature parameter
     * @param jsonMode Whether to request JSON format output
     * @param model Model name to use
     * @return JSON payload for the request
     */
    json buildRequestPayload(
        const QString &prompt,
        const QString &systemPrompt,
        double         temperature,
        bool           jsonMode,
        const QString &model) const;

    /**
     * @brief Parse the API response (OpenAI Chat Completions format)
     * @param reply Network response
     * @return Parsed LLM response struct
     */
    LLMResponse parseResponse(QNetworkReply *reply) const;

    /**
     * @brief Send request to a specific endpoint
     * @param endpoint Endpoint to use
     * @param prompt User prompt
     * @param systemPrompt System prompt
     * @param temperature Temperature
     * @param jsonMode JSON mode flag
     * @return Response from the endpoint
     */
    LLMResponse sendRequestToEndpoint(
        const LLMEndpoint &endpoint,
        const QString     &prompt,
        const QString     &systemPrompt,
        double             temperature,
        bool               jsonMode);

    /**
     * @brief Parse SSE data line and extract JSON
     * @param line SSE data line (without "data: " prefix)
     * @param accumulatedContent Accumulated content for building complete response
     * @param accumulatedToolCalls Accumulated tool calls (indexed by tool call index)
     * @return True if stream is complete ([DONE] received)
     */
    bool parseStreamLine(
        const QString &line, QString &accumulatedContent, QMap<int, json> &accumulatedToolCalls);

    /**
     * @brief Build complete response from accumulated streaming data
     * @param content Accumulated content
     * @param toolCalls Accumulated tool calls
     * @return Complete response in standard format
     */
    json buildStreamResponse(const QString &content, const QMap<int, json> &toolCalls) const;

    /* Current streaming state */
    QNetworkReply  *currentStreamReply = nullptr;
    QString         streamBuffer;
    QString         streamAccumulatedContent;
    QMap<int, json> streamAccumulatedToolCalls;
    bool            streamCompleted = false;
};

#endif // QLLMSERVICE_H
