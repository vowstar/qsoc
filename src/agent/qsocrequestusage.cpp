// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocrequestusage.h"
#include "common/qsoctokenizer.h"

#include <cmath>
#include <limits>
#include <utility>

using json = nlohmann::json;

namespace {
constexpr qint64 maximum = std::numeric_limits<qint64>::max();

qint64 add(qint64 left, qint64 right)
{
    return right > maximum - left ? maximum : left + right;
}

std::optional<qint64> count(const json &object, const char *key)
{
    const auto it = object.find(key);
    if (it == object.end() || !it->is_number_integer()) {
        return std::nullopt;
    }
    if (it->is_number_unsigned()) {
        const auto value = it->get<quint64>();
        return value <= static_cast<quint64>(maximum) ? std::optional<qint64>(value) : std::nullopt;
    }
    const auto value = it->get<qint64>();
    return value >= 0 ? std::optional<qint64>(value) : std::nullopt;
}

qint64 textField(const json &object, const char *key, QSocTokenizer::Mode counter)
{
    const auto value = object.find(key);
    return value != object.end() && value->is_string()
               ? QSocRequestUsage::estimateText(
                     QString::fromStdString(value->get<std::string>()), counter)
               : 0;
}

qint64 messageTokens(
    const json &message, qint64 imageTokens, QSocTokenizer::Mode counter, bool history = false)
{
    qint64 total = 10;
    if (!message.is_object()) {
        return total;
    }
    qint64     images  = 0;
    const auto content = message.find("content");
    if (content != message.end() && content->is_array()) {
        for (const auto &part : *content) {
            if (!part.is_object()) {
                continue;
            }
            total           = add(total, textField(part, "text", counter));
            const auto type = part.find("type");
            if (type != part.end() && *type == "image_url") {
                images = add(images, qMax<qint64>(1, imageTokens));
            }
        }
    } else {
        total = add(total, textField(message, "content", counter));
    }
    const auto savedImageTokens = history ? count(message, "_img_tokens") : std::nullopt;
    total                       = add(
        total, images > 0 && savedImageTokens && *savedImageTokens > 0 ? *savedImageTokens : images);
    for (const char *field :
         {"reasoning_content", "reasoning", "reasoning_text", "name", "tool_call_id"}) {
        total = add(total, textField(message, field, counter));
    }
    for (const char *field : {"tool_calls", "reasoning_details"}) {
        const auto value = message.find(field);
        if (value != message.end() && !value->is_null()) {
            total = add(
                total,
                QSocRequestUsage::estimateText(QString::fromStdString(value->dump()), counter));
        }
    }
    return total;
}
bool sameRequest(const QSocRequestSnapshot &left, const QSocRequestSnapshot &right)
{
    return left.route == right.route && left.effort == right.effort
           && left.imageTokens == right.imageTokens && left.tools.dump() == right.tools.dump()
           && left.messages.dump() == right.messages.dump();
}
} // namespace

qint64 QSocTokenEstimate::point() const
{
    return add(reported, counted);
}

qint64 QSocTokenEstimate::upper() const
{
    const double scaled = std::ceil(double(counted) * margin);
    return add(reported, scaled >= double(maximum) ? maximum : qint64(scaled));
}

qint64 QSocRequestUsage::estimateText(const QString &text, QSocTokenizer::Mode counter)
{
    return QSocTokenizer::count(text, counter);
}

qint64 QSocRequestUsage::estimateMessages(
    const json &messages, qint64 imageTokens, QSocTokenizer::Mode counter)
{
    qint64 total = 0;
    if (messages.is_array()) {
        for (const auto &message : messages) {
            total = add(total, messageTokens(message, imageTokens, counter));
        }
    }
    return total;
}

qint64 QSocRequestUsage::estimateHistory(const json &messages, QSocTokenizer::Mode counter)
{
    qint64 total = 0;
    if (messages.is_array()) {
        for (const auto &message : messages) {
            total = add(total, messageTokens(message, 5000, counter, true));
        }
    }
    return total;
}

qint64 QSocRequestUsage::estimateRequest(const QSocRequestSnapshot &request)
{
    qint64 total = estimateMessages(request.messages, request.imageTokens, request.counter);
    if (!request.tools.empty()) {
        total
            = add(total, estimateText(QString::fromStdString(request.tools.dump()), request.counter));
    }
    return total;
}

QSocTokenizer::Mode QSocRequestUsage::counterFor(const QString &tokenizer)
{
    return tokenizer == QStringLiteral("bytes") ? QSocTokenizer::Mode::Bytes
                                                : QSocTokenizer::Mode::O200k;
}

double QSocRequestUsage::margin(QSocTokenizer::Mode counter)
{
    return counter == QSocTokenizer::Mode::Bytes || !QSocTokenizer::available() ? 1.5 : 1.3;
}

QSocTokenEstimate QSocRequestUsage::estimate(const QSocRequestSnapshot &request) const
{
    const auto local = [&request] {
        return QSocTokenEstimate{0, estimateRequest(request), margin(request.counter)};
    };
    if (!anchor_ || anchor_->request.route != request.route
        || anchor_->request.effort != request.effort
        || anchor_->request.tools.dump() != request.tools.dump()
        || anchor_->request.imageTokens != request.imageTokens
        || !anchor_->request.messages.is_array() || !request.messages.is_array()
        || anchor_->request.messages.size() > request.messages.size()) {
        return local();
    }
    const auto &prefix = anchor_->request.messages;
    for (json::size_type i = 0; i < prefix.size(); ++i) {
        if (prefix[i].dump() != request.messages[i].dump()) {
            return local();
        }
    }
    QSocTokenEstimate result{anchor_->inputTokens, 0, margin(request.counter)};
    for (json::size_type i = prefix.size(); i < request.messages.size(); ++i) {
        result.counted = add(
            result.counted,
            messageTokens(request.messages[i], request.imageTokens, request.counter));
    }
    return result;
}

qint64 QSocRequestUsage::estimateNext(const QSocRequestSnapshot &request) const
{
    return estimate(request).point();
}

quint64 QSocRequestUsage::begin(QSocRequestSnapshot request)
{
    ++generation_;
    pending_ = Pending{generation_, std::move(request)};
    return generation_;
}

bool QSocRequestUsage::complete(quint64 generation, const json &usage)
{
    if (!pending_ || pending_->generation != generation) {
        return false;
    }
    auto request = std::move(pending_->request);
    pending_.reset();
    const auto counted = std::exchange(count_, std::nullopt);
    if (!usage.is_object()) {
        return false;
    }
    /* Split cache accounting does not establish the full request input count. */
    if (!usage.contains("prompt_tokens")
        && (usage.contains("cache_read_input_tokens")
            || usage.contains("cache_creation_input_tokens"))) {
        return false;
    }
    const auto input
        = count(usage, usage.contains("prompt_tokens") ? "prompt_tokens" : "input_tokens");
    const auto output
        = count(usage, usage.contains("completion_tokens") ? "completion_tokens" : "output_tokens");
    if (!input || (usage.contains("completion_tokens") && !output)
        || (usage.contains("output_tokens") && !output)) {
        return false;
    }
    if (*input > maximum - observed_.inputTokens
        || output.value_or(0) > maximum - observed_.outputTokens) {
        return false;
    }
    std::optional<qint64> cached;
    const auto            details = usage.find("prompt_tokens_details");
    if (details != usage.end() && details->is_object()) {
        cached = count(*details, "cached_tokens");
    }
    if (!cached && usage.contains("cache_read_input_tokens")) {
        cached = count(usage, "cache_read_input_tokens");
    }
    if (cached && *cached > *input) {
        cached.reset();
    }
    /* A server count must match what the same request reports. */
    if (counted && sameRequest(counted->request, request)
        && qAbs(counted->inputTokens - *input) > *input * 3 / 100 + 64) {
        mismatch_ = QSocCountMismatch{counted->inputTokens, *input};
    }
    observed_.inputTokens += *input;
    observed_.outputTokens += output.value_or(0);
    ++observed_.requests;
    if (output) {
        ++observed_.outputReportedRequests;
    }
    if (cached) {
        observed_.cachedTokens += *cached;
        observed_.cacheEligibleInputTokens += *input;
        ++observed_.cacheReportedRequests;
    }
    if (*input > 0) {
        anchor_ = Anchor{std::move(request), *input};
    } else {
        anchor_.reset();
    }
    return true;
}

void QSocRequestUsage::discardPending()
{
    pending_.reset();
}

void QSocRequestUsage::invalidateAnchor()
{
    anchor_.reset();
    count_.reset();
}

void QSocRequestUsage::recordCount(const QSocRequestSnapshot &request, qint64 tokens)
{
    anchor_ = Anchor{request, tokens};
    count_  = Anchor{request, tokens};
}

std::optional<QSocCountMismatch> QSocRequestUsage::takeCountMismatch()
{
    return std::exchange(mismatch_, std::nullopt);
}
