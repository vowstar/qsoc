// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qllmanthropic.h"

#include <algorithm>
#include <limits>
#include <set>

namespace QLLMAnthropic {

namespace {

constexpr const char *thinkingFormat = "anthropic-claude-v1";

/* Same wording the agent uses when it repairs an interrupted batch. */
constexpr const char *missingResult = "Not executed because the tool batch was interrupted.";

std::string stringField(const json &object, const char *key)
{
    const auto value = object.find(key);
    return value != object.end() && value->is_string() ? value->get<std::string>() : std::string();
}

std::int64_t countField(const json &object, const char *key)
{
    const auto value = object.find(key);
    return value != object.end() && value->is_number_integer() ? value->get<std::int64_t>() : 0;
}

json textBlock(const std::string &text)
{
    return {{"type", "text"}, {"text", text}};
}

/* data:<media>;base64,<payload> becomes a base64 source, anything else a URL. */
json imageBlock(const std::string &url)
{
    static const std::string marker = ";base64,";
    if (url.rfind("data:", 0) == 0) {
        const auto split = url.find(marker);
        if (split != std::string::npos) {
            return {
                {"type", "image"},
                {"source",
                 {{"type", "base64"},
                  {"media_type", url.substr(5, split - 5)},
                  {"data", url.substr(split + marker.size())}}}};
        }
    }
    return {{"type", "image"}, {"source", {{"type", "url"}, {"url", url}}}};
}

/* User or tool content as Messages blocks; empty text is dropped. */
json contentBlocks(const json &content)
{
    json blocks = json::array();
    if (content.is_string()) {
        if (!content.get_ref<const std::string &>().empty()) {
            blocks.push_back(textBlock(content.get<std::string>()));
        }
        return blocks;
    }
    if (!content.is_array()) {
        return blocks;
    }
    for (const json &part : content) {
        if (!part.is_object()) {
            continue;
        }
        const std::string type = stringField(part, "type");
        if (type == "text") {
            const std::string text = stringField(part, "text");
            if (!text.empty()) {
                blocks.push_back(textBlock(text));
            }
        } else if (type == "image_url" && part.contains("image_url")) {
            const json       &image = part["image_url"];
            const std::string url   = image.is_string() ? image.get<std::string>()
                                                        : stringField(image, "url");
            if (!url.empty()) {
                blocks.push_back(imageBlock(url));
            }
        }
    }
    return blocks;
}

std::string contentText(const json &content)
{
    std::string text;
    for (const json &block : contentBlocks(content)) {
        if (block["type"] == "text") {
            text += (text.empty() ? "" : "\n") + block["text"].get<std::string>();
        }
    }
    return text;
}

/* Signed thinking and redacted thinking from an earlier Messages reply. */
json thinkingBlocks(const json &message)
{
    json       blocks  = json::array();
    const auto details = message.find("reasoning_details");
    if (details == message.end() || !details->is_array()) {
        return blocks;
    }
    for (const json &detail : *details) {
        if (!detail.is_object() || stringField(detail, "format") != thinkingFormat) {
            continue;
        }
        const std::string type = stringField(detail, "type");
        if (type == "reasoning.text" && !stringField(detail, "signature").empty()) {
            blocks.push_back(
                {{"type", "thinking"},
                 {"thinking", stringField(detail, "text")},
                 {"signature", stringField(detail, "signature")}});
        } else if (type == "reasoning.encrypted" && !stringField(detail, "data").empty()) {
            blocks.push_back({{"type", "redacted_thinking"}, {"data", stringField(detail, "data")}});
        }
    }
    return blocks;
}

json assistantBlocks(const json &message, std::vector<std::string> *toolIds)
{
    json       blocks = thinkingBlocks(message);
    const auto text   = message.find("content");
    if (text != message.end()) {
        for (const json &block : contentBlocks(*text)) {
            blocks.push_back(block);
        }
    }
    const auto calls = message.find("tool_calls");
    if (calls == message.end() || !calls->is_array()) {
        return blocks;
    }
    for (const json &call : *calls) {
        if (!call.is_object() || !call.contains("function") || !call["function"].is_object()) {
            continue;
        }
        const std::string id = stringField(call, "id");
        json              input;
        try {
            input = json::parse(stringField(call["function"], "arguments"));
        } catch (const json::exception &) {
            input = json::object();
        }
        if (!input.is_object()) {
            input = json::object();
        }
        blocks.push_back(
            {{"type", "tool_use"},
             {"id", id},
             {"name", stringField(call["function"], "name")},
             {"input", input}});
        toolIds->push_back(id);
    }
    return blocks;
}

json toolResult(const std::string &id, const json &content)
{
    json blocks = contentBlocks(content);
    json value  = blocks.empty() ? json("") : blocks;
    if (blocks.size() == 1 && blocks[0]["type"] == "text") {
        value = blocks[0]["text"];
    }
    return {{"type", "tool_result"}, {"tool_use_id", id}, {"content", value}};
}

/* Consecutive turns of one role become one message, as Messages requires. */
class Turns
{
public:
    void append(const char *role, const json &blocks)
    {
        if (blocks.empty()) {
            return;
        }
        if (!turns_.empty() && turns_.back()["role"] == role) {
            for (const json &block : blocks) {
                turns_.back()["content"].push_back(block);
            }
            return;
        }
        turns_.push_back({{"role", role}, {"content", blocks}});
    }

    /* Every tool_use needs a tool_result before the conversation moves on. */
    void closeToolBatch()
    {
        json missing = json::array();
        for (const std::string &id : pending_) {
            missing.push_back(toolResult(id, missingResult));
        }
        pending_.clear();
        append("user", missing);
    }

    void openToolBatch(const std::vector<std::string> &ids)
    {
        pending_.assign(ids.begin(), ids.end());
    }

    bool answer(const std::string &id)
    {
        const auto found = std::find(pending_.begin(), pending_.end(), id);
        if (found == pending_.end()) {
            return false;
        }
        pending_.erase(found);
        return true;
    }

    json &messages() { return turns_; }

private:
    json                     turns_ = json::array();
    std::vector<std::string> pending_;
};

/* Breakpoint on the last block that accepts one. */
void markCache(json &blocks)
{
    for (auto block = blocks.rbegin(); block != blocks.rend(); ++block) {
        const std::string type = stringField(*block, "type");
        if (type != "thinking" && type != "redacted_thinking") {
            (*block)["cache_control"] = {{"type", "ephemeral"}};
            return;
        }
    }
}

std::int64_t imageBytes(const json &value, int *images)
{
    std::int64_t bytes = 0;
    if (value.is_object()) {
        if (stringField(value, "type") == "image") {
            ++*images;
            const auto source = value.find("source");
            if (source != value.end() && source->is_object()) {
                bytes += static_cast<std::int64_t>(stringField(*source, "data").size());
            }
            return bytes;
        }
        for (const auto &item : value.items()) {
            bytes += imageBytes(item.value(), images);
        }
    } else if (value.is_array()) {
        for (const json &item : value) {
            bytes += imageBytes(item, images);
        }
    }
    return bytes;
}

} // namespace

int defaultMaxTokens(const json &body, const LLMModelConfig &endpoint)
{
    int                images = 0;
    const std::int64_t media  = imageBytes(body, &images);
    const std::int64_t input  = static_cast<std::int64_t>(body.dump().size()) - media
                                + static_cast<std::int64_t>(images)
                                      * std::max(endpoint.imageMaxTokens, 0);
    const std::int64_t room   = static_cast<std::int64_t>(endpoint.contextTokens) - input;
    return static_cast<int>(std::clamp<std::int64_t>(room, 1024, std::numeric_limits<int>::max()));
}

json buildRequest(
    const json           &messages,
    const json           &tools,
    const LLMModelConfig &endpoint,
    const RequestOptions &options)
{
    json  system = json::array();
    Turns turns;
    bool  leading = true;
    for (const json &message : messages) {
        if (!message.is_object()) {
            continue;
        }
        const std::string role    = stringField(message, "role");
        const json        content = message.value("content", json());
        if (role == "system" || role == "developer") {
            const std::string text = contentText(content);
            if (text.empty()) {
                continue;
            }
            if (leading) {
                system.push_back(textBlock(text));
            } else {
                turns.closeToolBatch();
                turns.append("user", json::array({textBlock(text)}));
            }
            continue;
        }
        leading = false;
        if (role == "tool") {
            const std::string id = stringField(message, "tool_call_id");
            if (turns.answer(id)) {
                turns.append("user", json::array({toolResult(id, content)}));
                continue;
            }
            turns.closeToolBatch();
            const std::string text = contentText(content);
            turns.append("user", json::array({textBlock("Result of tool call " + id + ":\n" + text)}));
            continue;
        }
        turns.closeToolBatch();
        if (role == "assistant") {
            std::vector<std::string> ids;
            turns.append("assistant", assistantBlocks(message, &ids));
            turns.openToolBatch(ids);
        } else {
            turns.append("user", contentBlocks(content));
        }
    }
    turns.closeToolBatch();

    if (options.jsonMode) {
        system.push_back(textBlock("Reply with one JSON object and nothing else."));
    }
    if (!system.empty()) {
        markCache(system);
    }
    json &wire = turns.messages();
    if (!wire.empty()) {
        markCache(wire.back()["content"]);
    }

    json body = {{"model", endpoint.model.toStdString()}, {"messages", wire}};
    if (!system.empty()) {
        body["system"] = system;
    }
    if (tools.is_array() && !tools.empty()) {
        json converted = json::array();
        for (const json &tool : tools) {
            const json function = tool.value("function", json::object());
            json       schema   = function.value("parameters", json());
            if (!schema.is_object()) {
                schema = {{"type", "object"}, {"properties", json::object()}};
            }
            json entry = {{"name", stringField(function, "name")}, {"input_schema", schema}};
            if (!stringField(function, "description").empty()) {
                entry["description"] = stringField(function, "description");
            }
            converted.push_back(std::move(entry));
        }
        body["tools"] = converted;
    }
    if (options.effort.isEmpty()) {
        body["temperature"] = options.temperature;
    } else {
        body["thinking"]      = {{"type", "adaptive"}};
        body["output_config"] = {{"effort", options.effort.toStdString()}};
    }
    body["stream"]     = options.stream;
    body["max_tokens"] = endpoint.maxOutputTokens > 0 ? endpoint.maxOutputTokens
                                                      : defaultMaxTokens(body, endpoint);
    return body;
}

std::string toFinishReason(const std::string &stopReason)
{
    if (stopReason == "tool_use") {
        return "tool_calls";
    }
    if (stopReason == "max_tokens" || stopReason == "model_context_window_exceeded") {
        return "length";
    }
    if (stopReason == "refusal" || stopReason == "sensitive") {
        return "content_filter";
    }
    return "stop";
}

json toChatUsage(const json &usage)
{
    if (!usage.is_object() || !usage.contains("input_tokens")) {
        return json::object();
    }
    const std::int64_t read   = countField(usage, "cache_read_input_tokens");
    const std::int64_t prompt = countField(usage, "input_tokens") + read
                                + countField(usage, "cache_creation_input_tokens");
    const std::int64_t output = countField(usage, "output_tokens");
    return {
        {"prompt_tokens", prompt},
        {"completion_tokens", output},
        {"total_tokens", prompt + output},
        {"prompt_tokens_details", {{"cached_tokens", read}}}};
}

QString errorText(const json &error)
{
    static const std::map<std::string, int> statuses
        = {{"invalid_request_error", 400},
           {"authentication_error", 401},
           {"permission_error", 403},
           {"not_found_error", 404},
           {"request_too_large", 413},
           {"rate_limit_error", 429},
           {"api_error", 500},
           {"overloaded_error", 529}};
    if (!error.is_object()) {
        return error.is_string() ? QString::fromStdString(error.get<std::string>())
                                 : QStringLiteral("LLM provider returned an error");
    }
    QString message = QString::fromStdString(stringField(error, "message")).trimmed();
    if (message.isEmpty()) {
        message = QStringLiteral("LLM provider returned an error");
    }
    const auto status = statuses.find(stringField(error, "type"));
    return status == statuses.end() ? message
                                    : QStringLiteral("[HTTP %1] ").arg(status->second) + message;
}

json toChatResponse(const json &reply)
{
    if (!reply.is_object()) {
        return {{"error", "Invalid response from LLM"}};
    }
    if (stringField(reply, "type") == "error" || reply.contains("error")) {
        return {{"error", errorText(reply.value("error", json())).toStdString()}};
    }
    StreamDecoder        decoder;
    StreamDecoder::Delta delta;
    QString              error;
    decoder.feed(
        {{"type", "message_start"}, {"message", {{"usage", reply.value("usage", json())}}}},
        &delta,
        &error);
    const json content = reply.value("content", json::array());
    for (std::size_t index = 0; content.is_array() && index < content.size(); ++index) {
        const auto position = static_cast<std::int64_t>(index);
        decoder.feed(
            {{"type", "content_block_start"},
             {"index", position},
             {"content_block", content[index]}},
            &delta,
            &error);
        decoder.feed({{"type", "content_block_stop"}, {"index", position}}, &delta, &error);
    }
    decoder.feed(
        {{"type", "message_delta"},
         {"delta", {{"stop_reason", reply.value("stop_reason", json())}}}},
        &delta,
        &error);
    return decoder.response();
}

StreamDecoder::Status StreamDecoder::feed(const json &event, Delta *delta, QString *error)
{
    *delta = Delta();
    if (!event.is_object()) {
        return Status::Malformed;
    }
    const std::string type = stringField(event, "type");
    if (type == "error" || (type.empty() && event.contains("error"))) {
        *error = errorText(event.value("error", json()));
        return Status::Error;
    }
    if (type == "message_start") {
        started_         = true;
        const json usage = event.value("message", json::object()).value("usage", json());
        if (usage.is_object()) {
            usage_.update(usage);
        }
        return Status::More;
    }
    if (type == "message_delta") {
        const json change = event.value("delta", json::object());
        if (change.is_object() && change.contains("stop_reason")
            && change["stop_reason"].is_string()) {
            stopReason_ = change["stop_reason"].get<std::string>();
        }
        const json usage = event.value("usage", json());
        if (usage.is_object()) {
            usage_.update(usage);
        }
        return Status::More;
    }
    if (type == "message_stop") {
        return Status::Done;
    }
    if (type != "content_block_start" && type != "content_block_delta"
        && type != "content_block_stop") {
        return Status::More;
    }

    const auto index = event.find("index");
    if (index == event.end() || !index->is_number_integer()) {
        return Status::Malformed;
    }
    const std::int64_t position = index->get<std::int64_t>();
    started_                    = true;

    if (type == "content_block_start") {
        const json source = event.value("content_block", json());
        if (!source.is_object()) {
            return Status::Malformed;
        }
        Block block;
        block.type      = stringField(source, "type");
        block.text      = stringField(source, block.type == "thinking" ? "thinking" : "text");
        block.signature = stringField(source, "signature");
        block.data      = stringField(source, "data");
        block.id        = stringField(source, "id");
        block.name      = stringField(source, "name");
        if (source.contains("input") && source["input"].is_object()) {
            block.initialInput = source["input"];
        }
        if (block.type == "tool_use" && (block.id.empty() || block.name.empty())) {
            return Status::Malformed;
        }
        if (block.type == "text") {
            delta->text = QString::fromStdString(block.text);
        } else if (block.type == "thinking") {
            delta->reasoning = QString::fromStdString(block.text);
        } else if (block.type == "tool_use") {
            delta->tool     = true;
            delta->toolId   = QString::fromStdString(block.id);
            delta->toolName = QString::fromStdString(block.name);
        }
        blocks_[position] = std::move(block);
        return Status::More;
    }

    const auto found = blocks_.find(position);
    if (found == blocks_.end()) {
        return Status::Malformed;
    }
    Block &block = found->second;
    if (type == "content_block_stop") {
        if (block.type == "tool_use" && block.input.empty()) {
            block.input = block.initialInput.dump();
        }
        return Status::More;
    }

    const json change = event.value("delta", json());
    if (!change.is_object()) {
        return Status::Malformed;
    }
    const std::string kind = stringField(change, "type");
    if (kind == "text_delta") {
        const std::string text = stringField(change, "text");
        block.text += text;
        delta->text = QString::fromStdString(text);
    } else if (kind == "thinking_delta") {
        const std::string text = stringField(change, "thinking");
        block.text += text;
        delta->reasoning = QString::fromStdString(text);
    } else if (kind == "signature_delta") {
        block.signature += stringField(change, "signature");
    } else if (kind == "input_json_delta") {
        block.input += stringField(change, "partial_json");
        delta->tool          = true;
        delta->toolId        = QString::fromStdString(block.id);
        delta->toolName      = QString::fromStdString(block.name);
        delta->toolArguments = QString::fromStdString(block.input);
    }
    return Status::More;
}

json StreamDecoder::response() const
{
    std::string text;
    std::string thinking;
    json        details = json::array();
    json        calls   = json::array();
    for (const auto &[position, block] : blocks_) {
        if (block.type == "text") {
            text += block.text;
        } else if (block.type == "thinking") {
            thinking += block.text;
            details.push_back(
                {{"type", "reasoning.text"},
                 {"text", block.text},
                 {"signature", block.signature},
                 {"format", thinkingFormat}});
        } else if (block.type == "redacted_thinking") {
            details.push_back(
                {{"type", "reasoning.encrypted"}, {"data", block.data}, {"format", thinkingFormat}});
        } else if (block.type == "tool_use") {
            calls.push_back(
                {{"id", block.id},
                 {"type", "function"},
                 {"function",
                  {{"name", block.name},
                   {"arguments", block.input.empty() ? block.initialInput.dump() : block.input}}}});
        }
    }

    json message = {{"role", "assistant"}};
    if (!text.empty() || calls.empty()) {
        message["content"] = text;
    } else {
        message["content"] = nullptr;
    }
    if (!calls.empty()) {
        message["tool_calls"] = calls;
    }
    if (!thinking.empty()) {
        message["reasoning_content"] = thinking;
    }
    if (!details.empty()) {
        message["reasoning_details"] = details;
    }

    json choice = {{"index", 0}, {"message", message}};
    if (!stopReason_.empty()) {
        choice["finish_reason"] = toFinishReason(stopReason_);
    }
    json       response = {{"choices", json::array({choice})}};
    const json usage    = toChatUsage(usage_);
    if (!usage.empty()) {
        response["usage"] = usage;
    }
    return response;
}

} // namespace QLLMAnthropic
