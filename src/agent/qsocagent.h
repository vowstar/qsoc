// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCAGENT_H
#define QSOCAGENT_H

#include "agent/qsocagentconfig.h"
#include "agent/qsoctool.h"
#include "common/qllmservice.h"

#include <nlohmann/json.hpp>
#include <QObject>
#include <QString>

using json = nlohmann::json;

/**
 * @brief The QSocAgent class provides an AI agent for SoC design automation
 * @details Implements an agent loop that interacts with an LLM using tool calling
 *          to perform various SoC design tasks. The agent maintains conversation
 *          history and handles tool execution automatically.
 */
class QSocAgent : public QObject
{
    Q_OBJECT

public:
    /**
     * @brief Constructor
     * @param parent Parent QObject
     * @param llmService LLM service for API calls
     * @param toolRegistry Registry of available tools
     * @param config Agent configuration
     */
    explicit QSocAgent(
        QObject          *parent       = nullptr,
        QLLMService      *llmService   = nullptr,
        QSocToolRegistry *toolRegistry = nullptr,
        QSocAgentConfig   config       = QSocAgentConfig());

    /**
     * @brief Destructor
     */
    ~QSocAgent() override;

    /**
     * @brief Run the agent with a user query
     * @param userQuery The user's input query
     * @return The agent's final response
     */
    QString run(const QString &userQuery);

    /**
     * @brief Clear the conversation history
     */
    void clearHistory();

    /**
     * @brief Set the LLM service
     * @param llmService Pointer to the LLM service
     */
    void setLLMService(QLLMService *llmService);

    /**
     * @brief Set the tool registry
     * @param toolRegistry Pointer to the tool registry
     */
    void setToolRegistry(QSocToolRegistry *toolRegistry);

    /**
     * @brief Set the agent configuration
     * @param config Agent configuration
     */
    void setConfig(const QSocAgentConfig &config);

    /**
     * @brief Get the current configuration
     * @return Current agent configuration
     */
    QSocAgentConfig getConfig() const;

    /**
     * @brief Get the conversation history
     * @return JSON array of messages
     */
    json getMessages() const;

signals:
    /**
     * @brief Signal emitted when a tool is called
     * @param toolName Name of the tool being called
     * @param arguments Arguments passed to the tool
     */
    void toolCalled(const QString &toolName, const QString &arguments);

    /**
     * @brief Signal emitted when a tool returns a result
     * @param toolName Name of the tool
     * @param result Result from the tool
     */
    void toolResult(const QString &toolName, const QString &result);

    /**
     * @brief Signal emitted for verbose output
     * @param message The verbose message
     */
    void verboseOutput(const QString &message);

private:
    QLLMService      *llmService_   = nullptr;
    QSocToolRegistry *toolRegistry_ = nullptr;
    QSocAgentConfig   config_;
    json              messages_;

    /**
     * @brief Process a single iteration of the agent loop
     * @return true if the agent completed (no more tool calls), false otherwise
     */
    bool processIteration();

    /**
     * @brief Handle tool calls from the LLM response
     * @param toolCalls JSON array of tool calls
     */
    void handleToolCalls(const json &toolCalls);

    /**
     * @brief Add a message to the conversation history
     * @param role The role (user, assistant, system)
     * @param content The message content
     */
    void addMessage(const QString &role, const QString &content);

    /**
     * @brief Add a tool result message to the conversation history
     * @param toolCallId The ID of the tool call
     * @param content The tool result content
     */
    void addToolMessage(const QString &toolCallId, const QString &content);

    /**
     * @brief Compress history if needed based on token count
     */
    void compressHistoryIfNeeded();

    /**
     * @brief Estimate the number of tokens in a text
     * @param text The text to estimate
     * @return Estimated token count (approximately 4 characters per token)
     */
    int estimateTokens(const QString &text) const;

    /**
     * @brief Estimate the total tokens in the message history
     * @return Estimated token count for all messages
     */
    int estimateMessagesTokens() const;
};

#endif // QSOCAGENT_H
