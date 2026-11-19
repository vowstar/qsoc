// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qllmresponses.h"

#include <stdexcept>
#include <QCryptographicHash>

namespace QLLMResponses {

namespace {

constexpr const char *replayField = "responses_replay";

json inputContent(const json &content)
{
    if (!content.is_array())
        return content;
    json parts = json::array();
    for (const auto &part : content) {
        const auto &type = part.at("type");
        if (type == "text") {
            parts.push_back({{"type", "input_text"}, {"text", part.at("text")}});
        } else if (type == "image_url") {
            const auto &image = part.at("image_url");
            json        value
                = {{"type", "input_image"},
                   {"image_url", image.is_object() ? image.at("url") : image}};
            if (image.contains("detail"))
                value["detail"] = image.at("detail");
            parts.push_back(std::move(value));
        } else {
            throw std::invalid_argument("Unsupported Responses input content");
        }
    }
    return parts;
}

json assistantMessage(const json &items)
{
    if (!items.is_array())
        throw std::invalid_argument("Missing Responses output items");
    json        message = {{"role", "assistant"}, {"content", ""}};
    std::string text;
    std::string summary;
    for (const auto &item : items) {
        const auto &type = item.at("type");
        if (type == "message" && item.at("role") == "assistant") {
            if (!item.at("content").is_array())
                throw std::invalid_argument("Invalid Responses message content");
            for (const auto &part : item.at("content")) {
                const auto &kind = part.at("type");
                if (kind == "output_text")
                    text += part.at("text").get_ref<const std::string &>();
                else if (kind == "refusal")
                    text += part.at("refusal").get_ref<const std::string &>();
                else
                    throw std::invalid_argument("Unsupported Responses output content");
            }
        } else if (type == "function_call") {
            if (item.contains("status") && item.at("status") != "completed")
                throw std::invalid_argument("Incomplete Responses function call");
            message["tool_calls"].push_back(
                {{"type", "function"},
                 {"id", item.at("call_id")},
                 {"function", {{"name", item.at("name")}, {"arguments", item.at("arguments")}}}});
        } else if (type == "reasoning") {
            const auto &parts = item.contains("content") && item.at("content").is_array()
                                        && !item.at("content").empty()
                                    ? item.at("content")
                                    : item.value("summary", json::array());
            for (const auto &part : parts)
                summary += part.at("text").get_ref<const std::string &>();
        } else {
            throw std::invalid_argument("Unsupported Responses output item");
        }
    }
    message["content"] = text.empty() && message.contains("tool_calls") ? json(nullptr)
                                                                        : json(text);
    if (!summary.empty())
        message["reasoning_content"] = std::move(summary);
    return message;
}

bool replay(json &input, const json &message, const std::string &route)
{
    const auto saved = message.find(replayField);
    if (saved == message.end() || saved->value("route", "") != route)
        return false;
    const auto &items    = saved->at("output");
    const json  original = assistantMessage(items);
    if (original.at("content") != message.at("content")
        || original.value("tool_calls", json::array()) != message.value("tool_calls", json::array()))
        return false;
    input.insert(input.end(), items.begin(), items.end());
    return true;
}

json inputItems(const json &messages, const std::string &route)
{
    json input = json::array();
    for (const auto &message : messages) {
        const auto &role    = message.at("role");
        const json  content = message.value("content", json(nullptr));
        if (role == "tool") {
            input.push_back(
                {{"type", "function_call_output"},
                 {"call_id", message.at("tool_call_id")},
                 {"output", inputContent(content)}});
            continue;
        }
        if (role == "assistant" && replay(input, message, route))
            continue;
        if (!content.is_null() && (content != "" || !message.contains("tool_calls")))
            input.push_back({{"role", role}, {"content", inputContent(content)}});
        if (role == "assistant" && message.contains("tool_calls")) {
            for (const auto &call : message.at("tool_calls")) {
                const auto &function = call.at("function");
                input.push_back(
                    {{"type", "function_call"},
                     {"call_id", call.at("id")},
                     {"name", function.at("name")},
                     {"arguments", function.at("arguments")}});
            }
        }
    }
    return input;
}

json chatUsage(const json &usage)
{
    json result = json::object();
    for (const auto &[wire, chat] :
         {std::pair{"input_tokens", "prompt_tokens"},
          {"output_tokens", "completion_tokens"},
          {"input_tokens_details", "prompt_tokens_details"},
          {"output_tokens_details", "completion_tokens_details"},
          {"total_tokens", "total_tokens"}})
        if (usage.contains(wire))
            result[chat] = usage.at(wire);
    return result;
}

} // namespace

QString replayRoute(const LLMModelConfig &endpoint)
{
    return QString::fromLatin1(
        QCryptographicHash::hash(
            endpoint.url.toUtf8() + '\0' + endpoint.model.toUtf8(), QCryptographicHash::Sha256)
            .toHex());
}

void removeReplay(json &messages)
{
    for (auto &message : messages)
        if (message.is_object())
            message.erase(replayField);
}

json buildRequest(
    const json           &messages,
    const json           &tools,
    const LLMModelConfig &endpoint,
    double                temperature,
    const QString        &effort,
    bool                  stream,
    bool                  jsonMode)
{
    try {
        json body
            = {{"model", endpoint.model.toStdString()},
               {"input", inputItems(messages, replayRoute(endpoint).toStdString())},
               {"stream", stream},
               {"store", false}};
        if (endpoint.reasoning)
            body["include"] = json::array({"reasoning.encrypted_content"});
        else
            body["temperature"] = temperature;
        if (!effort.isEmpty())
            body["reasoning"] = {{"effort", effort.toStdString()}};
        if (endpoint.maxOutputTokens > 0)
            body["max_output_tokens"] = endpoint.maxOutputTokens;
        if (jsonMode)
            body["text"] = {{"format", {{"type", "json_object"}}}};
        for (const auto &tool : tools) {
            if (tool.at("type") != "function")
                return {{"error", "Unsupported Responses tool type"}};
            json function    = tool.at("function");
            function["type"] = "function";
            if (!function.contains("strict"))
                function["strict"] = false;
            body["tools"].push_back(std::move(function));
        }
        return body;
    } catch (const std::exception &error) {
        return {{"error", error.what()}};
    }
}

json toChatResponse(const json &response, const QString &route)
{
    try {
        if (response.contains("error") && !response.at("error").is_null())
            return {{"error", response.at("error").value("message", "Responses request failed")}};
        if (response.at("status") != "completed") {
            const auto details = response.value("incomplete_details", json());
            return {
                {"error",
                 "Responses request did not complete: "
                     + (details.is_object() ? details.value("reason", "incomplete")
                                            : response.at("status").get<std::string>())}};
        }
        const auto &items = response.at("output");
        if (items.empty())
            return {{"error", "Empty Responses output"}};
        json message         = assistantMessage(items);
        message[replayField] = {{"route", route.toStdString()}, {"output", items}};
        const auto reason    = message.contains("tool_calls") ? "tool_calls" : "stop";
        json       result    = {
            {"choices",
             json::array({{{"message", std::move(message)}, {"finish_reason", reason}}})}};
        if (response.contains("usage"))
            result["usage"] = chatUsage(response.at("usage"));
        return result;
    } catch (const std::exception &error) {
        return {{"error", error.what()}};
    }
}

} // namespace QLLMResponses
