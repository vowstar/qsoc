// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QLLMANTHROPIC_H
#define QLLMANTHROPIC_H

#include "common/qllmservice.h"

#include <cstdint>
#include <map>
#include <nlohmann/json.hpp>
#include <string>
#include <QString>

/**
 * @brief Anthropic Messages codec around the OpenAI chat form.
 * @details History, sessions and compaction keep the OpenAI Chat
 *          Completions shape. These functions translate one request
 *          into a Messages body and one Messages reply back into a chat
 *          completion, so the rest of qsoc never sees the second format.
 *          Thinking blocks travel in the assistant message as
 *          `reasoning_details` entries with `format: anthropic-claude-v1`.
 */
namespace QLLMAnthropic {

/** @brief Value of the anthropic-version header sent on every request. */
inline constexpr const char *apiVersion = "2023-06-01";

/** @brief Per-request options that are not part of the conversation. */
struct RequestOptions
{
    double  temperature = 0.2;
    QString effort; /* Empty: no thinking fields are sent */
    bool    stream   = false;
    bool    jsonMode = false;
};

/**
 * @brief Build a Messages request body.
 * @param messages OpenAI chat messages (system, user, assistant, tool).
 * @param tools OpenAI function tools.
 * @param endpoint Model entry: wire name, context and output limits.
 * @param options Sampling, effort, streaming and JSON mode.
 * @return Messages API request body.
 */
json buildRequest(
    const json           &messages,
    const json           &tools,
    const LLMModelConfig &endpoint,
    const RequestOptions &options);

/**
 * @brief Output cap sent when the entry sets no max_output_tokens.
 * @details The context window minus an upper bound of the input tokens:
 *          one token per byte of the request body, with each image
 *          counted at the entry's image_max_tokens instead of its bytes.
 */
int defaultMaxTokens(const json &body, const LLMModelConfig &endpoint);

/** @brief Convert a non-streaming Messages reply into a chat completion. */
json toChatResponse(const json &reply);

/** @brief Usage in the OpenAI shape; prompt_tokens includes cache reads and writes. */
json toChatUsage(const json &usage);

/** @brief Map a Messages stop_reason to an OpenAI finish_reason. */
std::string toFinishReason(const std::string &stopReason);

/**
 * @brief Text for an Anthropic error object.
 * @details Prefixed with `[HTTP <status>] ` for the error types whose
 *          status the API documents, so retry classification matches
 *          the HTTP path.
 */
QString errorText(const json &error);

/**
 * @brief Incremental decoder for a streamed Messages reply.
 */
class StreamDecoder
{
public:
    enum class Status : std::uint8_t { More, Done, Error, Malformed };

    /** @brief What one event adds for the listeners. */
    struct Delta
    {
        QString text;
        QString reasoning;
        bool    tool = false;
        QString toolId;
        QString toolName;
        QString toolArguments;
    };

    /**
     * @brief Consume one event.
     * @param event Parsed `data:` payload.
     * @param delta Receives the visible increment.
     * @param error Receives the provider message on Status::Error.
     */
    Status feed(const json &event, Delta *delta, QString *error);

    /** @brief True once message_start or a content block arrived. */
    bool started() const { return started_; }

    /** @brief The reply so far as a chat completion. */
    json response() const;

private:
    struct Block
    {
        std::string type;
        std::string text;
        std::string signature;
        std::string data;
        std::string id;
        std::string name;
        std::string input;
        json        initialInput = json::object();
    };

    std::map<std::int64_t, Block> blocks_;
    json                          usage_ = json::object();
    std::string                   stopReason_;
    bool                          started_ = false;
};

} // namespace QLLMAnthropic

#endif // QLLMANTHROPIC_H
