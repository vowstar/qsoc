// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qllmdiagnostics.h"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <QElapsedTimer>
#include <QMessageAuthenticationCode>
#include <QUuid>
#include <QVector>

using json = nlohmann::json;

namespace {
constexpr qint64    maximum         = (std::numeric_limits<qint64>::max)();
constexpr qsizetype maximumMessages = 4096;
constexpr qsizetype maximumBytes    = 8 * 1024 * 1024;

void add(qint64 &total, qint64 value = 1)
{
    total = value > maximum - total ? maximum : total + value;
}

enum class Availability { Missing, Invalid, Reported };
struct Count
{
    Availability availability = Availability::Missing;
    qint64       value        = 0;
};

Count count(const json &object, const char *key)
{
    if (!object.is_object())
        return {Availability::Invalid, 0};
    const auto entry = object.find(key);
    if (entry == object.end() || entry->is_null())
        return {};
    if (!entry->is_number_integer())
        return {Availability::Invalid, 0};
    if (entry->is_number_unsigned()) {
        const auto value = entry->get<quint64>();
        if (value > static_cast<quint64>(maximum))
            return {Availability::Invalid, 0};
        return {Availability::Reported, static_cast<qint64>(value)};
    }
    const auto value = entry->get<qint64>();
    return value < 0 ? Count{Availability::Invalid, 0} : Count{Availability::Reported, value};
}

Count nestedCount(const json &usage, const char *container, const char *field)
{
    const auto entry = usage.find(container);
    if (entry == usage.end() || entry->is_null())
        return {};
    return count(*entry, field);
}

struct Usage
{
    Count input;
    Count output;
    Count read;
    Count write;
};

Usage parseUsage(const json &value, bool separateCache)
{
    if (value.is_null())
        return {};
    if (!value.is_object()) {
        const Count invalid{Availability::Invalid, 0};
        return {invalid, invalid, invalid, invalid};
    }
    Usage result;
    result.input = count(value, value.contains("prompt_tokens") ? "prompt_tokens" : "input_tokens");
    result.output
        = count(value, value.contains("completion_tokens") ? "completion_tokens" : "output_tokens");
    if (separateCache) {
        result.read  = count(value, "cache_read_input_tokens");
        result.write = count(value, "cache_creation_input_tokens");
        if (!value.contains("prompt_tokens")
            && result.input.availability == Availability::Reported) {
            for (const Count part : {result.read, result.write}) {
                if (part.availability != Availability::Reported
                    || part.value > maximum - result.input.value) {
                    result.input
                        = {part.availability == Availability::Missing ? Availability::Missing
                                                                      : Availability::Invalid,
                           0};
                    break;
                }
                result.input.value += part.value;
            }
        }
    } else {
        const char *container = value.contains("prompt_tokens_details") ? "prompt_tokens_details"
                                                                        : "input_tokens_details";
        result.read           = nestedCount(value, container, "cached_tokens");
        result.write          = nestedCount(value, container, "cache_write_tokens");
    }
    if (result.input.availability == Availability::Reported) {
        if (result.read.availability == Availability::Reported
            && result.read.value > result.input.value)
            result.read = {Availability::Invalid, 0};
        if (result.write.availability == Availability::Reported
            && result.write.value > result.input.value)
            result.write = {Availability::Invalid, 0};
    }
    return result;
}

struct Totals
{
    qint64 reported = 0;
    qint64 missing  = 0;
    qint64 invalid  = 0;
    qint64 total    = 0;
    void   record(Count value)
    {
        if (value.availability == Availability::Reported) {
            add(reported);
            add(total, value.value);
        } else if (value.availability == Availability::Invalid) {
            add(invalid);
        } else {
            add(missing);
        }
    }
    json snapshot() const
    {
        return {
            {"reported", reported},
            {"missing", missing},
            {"invalid", invalid},
            {"tokens", reported > 0 ? json(total) : json(nullptr)}};
    }
};

struct Prefix
{
    QByteArray          route;
    QByteArray          options;
    QByteArray          model;
    QByteArray          tools;
    QByteArray          system;
    QVector<QByteArray> messages;
};

QByteArray digest(const QByteArray &data, const QByteArray &key)
{
    return QMessageAuthenticationCode::hash(data, key, QCryptographicHash::Sha256);
}

bool spend(qsizetype amount, qsizetype &remaining)
{
    if (amount > remaining)
        return false;
    remaining -= amount;
    return true;
}

bool boundedString(const std::string &text, qsizetype &remaining)
{
    if (!spend(2, remaining) || text.size() > static_cast<std::size_t>(remaining))
        return false;
    for (const unsigned char character : text) {
        const qsizetype width = character == '"' || character == '\\' ? 2
                                : character < 0x20                    ? 6
                                                                      : 1;
        if (!spend(width, remaining))
            return false;
    }
    return true;
}

bool boundedValue(const json &value, qsizetype &remaining, qsizetype &nodes, int depth = 0)
{
    if (depth > 64 || !spend(1, nodes))
        return false;
    if (value.is_string())
        return boundedString(value.get_ref<const std::string &>(), remaining);
    if (value.is_primitive())
        return !value.is_binary() && !value.is_discarded() && spend(32, remaining);
    if (value.size() > static_cast<std::size_t>(nodes) || !spend(2, remaining))
        return false;
    for (auto it = value.begin(); it != value.end(); ++it) {
        if (!spend(2, remaining) || (value.is_object() && !boundedString(it.key(), remaining))
            || !boundedValue(it.value(), remaining, nodes, depth + 1))
            return false;
    }
    return true;
}

const json &field(const json &object, const char *key)
{
    static const json absent;
    const auto        found = object.find(key);
    return found == object.end() ? absent : *found;
}

std::optional<Prefix> prefix(const QString &route, const json &payload, const QByteArray &key)
{
    const auto messages = payload.find("messages");
    if (messages == payload.end() || !messages->is_array()
        || messages->size() > static_cast<std::size_t>(maximumMessages)
        || route.size() > maximumBytes / 4)
        return std::nullopt;
    qsizetype remaining = maximumBytes;
    qsizetype nodes     = 65536;
    if (!boundedValue(payload, remaining, nodes))
        return std::nullopt;

    Prefix result;
    result.route    = digest(route.toUtf8(), key);
    const auto hash = [&key](const json &part) {
        return digest(QByteArray::fromStdString(part.dump()), key);
    };
    result.model = hash(field(payload, "model"));
    result.tools = hash(field(payload, "tools"));
    QMessageAuthenticationCode optionsHash(QCryptographicHash::Sha256, key);
    for (auto it = payload.begin(); it != payload.end(); ++it) {
        if (it.key() != "messages" && it.key() != "system" && it.key() != "tools"
            && it.key() != "model") {
            optionsHash.addData(digest(QByteArray::fromStdString(it.key()), key));
            optionsHash.addData(hash(it.value()));
        }
    }
    result.options = optionsHash.result();
    QMessageAuthenticationCode systemHash(QCryptographicHash::Sha256, key);
    systemHash.addData(hash(field(payload, "system")));
    for (const json &message : *messages) {
        const QByteArray messageHash = hash(message);
        result.messages.append(messageHash);
        if (message.is_object()) {
            const auto &role = field(message, "role");
            if (role == "system" || role == "developer")
                systemHash.addData(messageHash);
        }
    }
    result.system = systemHash.result();
    return result;
}

const char *change(const std::optional<Prefix> &previous, const std::optional<Prefix> &current)
{
    if (!current)
        return "unobserved";
    if (!previous)
        return "first_observation";
    if (previous->route != current->route)
        return "endpoint_changed";
    if (previous->model != current->model)
        return "model_changed";
    if (previous->options != current->options)
        return "options_changed";
    if (previous->tools != current->tools)
        return "tools_changed";
    if (previous->system != current->system)
        return "system_changed";
    if (previous->messages.size() > current->messages.size()
        || !std::equal(
            previous->messages.begin(), previous->messages.end(), current->messages.begin()))
        return "history_changed";
    return previous->messages.size() == current->messages.size() ? "unchanged" : "history_appended";
}

const char *outcomeName(QLLMDiagnostics::Outcome outcome)
{
    switch (outcome) {
    case QLLMDiagnostics::Outcome::Completed:
        return "completed";
    case QLLMDiagnostics::Outcome::Failed:
        return "failed";
    case QLLMDiagnostics::Outcome::Cancelled:
        return "cancelled";
    case QLLMDiagnostics::Outcome::TimedOut:
        return "timed_out";
    case QLLMDiagnostics::Outcome::Superseded:
        return "superseded";
    case QLLMDiagnostics::Outcome::Destroyed:
        return "destroyed";
    }
    return "failed";
}

struct Bucket
{
    qint64                started          = 0;
    qint64                finished         = 0;
    qint64                attempts         = 0;
    qint64                partial          = 0;
    qint64                elapsedMs        = 0;
    qint64                maxElapsedMs     = 0;
    qint64                firstByteReports = 0;
    qint64                firstByteMs      = 0;
    Totals                input;
    Totals                output;
    Totals                read;
    Totals                write;
    qint64                ratioReports = 0;
    qint64                ratioInput   = 0;
    qint64                ratioRead    = 0;
    json                  outcomes     = json::object();
    json                  changes      = json::object();
    std::optional<Prefix> previous;

    json snapshot() const
    {
        return {
            {"service_calls", started},
            {"finished_calls", finished},
            {"in_flight_calls", started - finished},
            {"network_attempts", attempts},
            {"outcomes", outcomes},
            {"prefix_changes", changes},
            {"input", input.snapshot()},
            {"output", output.snapshot()},
            {"cache_read", read.snapshot()},
            {"cache_write", write.snapshot()},
            {"partial_usage_reports", partial},
            {"cache_ratio",
             {{"reported_calls", ratioReports},
              {"input_tokens", ratioInput},
              {"read_tokens", ratioRead},
              {"fraction",
               ratioInput > 0 ? json(double(ratioRead) / double(ratioInput)) : json(nullptr)}}},
            {"latency_ms",
             {{"reported_calls", finished},
              {"total", elapsedMs},
              {"maximum", maxElapsedMs},
              {"first_byte_reports", firstByteReports},
              {"first_byte_total", firstByteReports > 0 ? json(firstByteMs) : json(nullptr)}}}};
    }
};

void increment(json &object, const char *key)
{
    qint64 value = object.value(key, qint64(0));
    add(value);
    object[key] = value;
}
} // namespace

struct QLLMDiagnostics::State
{
    QByteArray            key = QUuid::createUuid().toRfc4122();
    std::array<Bucket, 3> buckets;
};

struct QLLMDiagnostics::Request::Data
{
    std::shared_ptr<State> state;
    Kind                   kind;
    QElapsedTimer          timer;
    Usage                  observed;
    bool                   terminal = false;
    bool                   posted   = false;
    std::optional<qint64>  firstByte;
};

QLLMDiagnostics::Request::Request(std::shared_ptr<State> state, Kind kind)
    : data_(std::make_unique<Data>())
{
    data_->state = std::move(state);
    data_->kind  = kind;
    data_->timer.start();
}

QLLMDiagnostics::Request::~Request()
{
    finish(Outcome::Destroyed);
}

void QLLMDiagnostics::Request::posted()
{
    if (!data_->terminal && !data_->posted) {
        data_->posted = true;
        add(data_->state->buckets[static_cast<std::size_t>(data_->kind)].attempts);
    }
}

void QLLMDiagnostics::Request::firstByte()
{
    if (!data_->terminal && !data_->firstByte)
        data_->firstByte = data_->timer.elapsed();
}

void QLLMDiagnostics::Request::usage(const json &value, bool separateCache)
{
    if (!data_->terminal)
        data_->observed = parseUsage(value, separateCache);
}

void QLLMDiagnostics::Request::finish(Outcome outcome)
{
    if (data_->terminal)
        return;
    data_->terminal = true;
    Bucket &bucket  = data_->state->buckets[static_cast<std::size_t>(data_->kind)];
    add(bucket.finished);
    increment(bucket.outcomes, outcomeName(outcome));
    const qint64 elapsed = data_->timer.elapsed();
    add(bucket.elapsedMs, elapsed);
    bucket.maxElapsedMs = std::max(bucket.maxElapsedMs, elapsed);
    if (data_->firstByte) {
        add(bucket.firstByteReports);
        add(bucket.firstByteMs, *data_->firstByte);
    }
    const Usage &observed = data_->observed;
    bucket.input.record(observed.input);
    bucket.output.record(observed.output);
    bucket.read.record(observed.read);
    bucket.write.record(observed.write);
    if (outcome != Outcome::Completed
        && (observed.input.availability == Availability::Reported
            || observed.output.availability == Availability::Reported))
        add(bucket.partial);
    if (observed.input.availability == Availability::Reported
        && observed.read.availability == Availability::Reported) {
        add(bucket.ratioReports);
        add(bucket.ratioInput, observed.input.value);
        add(bucket.ratioRead, observed.read.value);
    }
}

QLLMDiagnostics::QLLMDiagnostics()
    : state_(std::make_shared<State>())
{}

QLLMDiagnostics::RequestPtr QLLMDiagnostics::begin(
    Kind kind, const QString &route, const json &payload)
{
    if (!enabled_)
        return {};
    Bucket &bucket  = state_->buckets[static_cast<std::size_t>(kind)];
    auto    current = prefix(route, payload, state_->key);
    increment(bucket.changes, change(bucket.previous, current));
    bucket.previous = std::move(current);
    add(bucket.started);
    return RequestPtr(new Request(state_, kind));
}

void QLLMDiagnostics::setEnabled(bool enabled)
{
    if (enabled_ == enabled)
        return;
    enabled_ = enabled;
    state_   = std::make_shared<State>();
}

json QLLMDiagnostics::snapshot() const
{
    return {
        {"schema_version", 1},
        {"scope", "llm_service"},
        {"enabled", enabled_},
        {"text", state_->buckets[0].snapshot()},
        {"chat", state_->buckets[1].snapshot()},
        {"stream", state_->buckets[2].snapshot()}};
}
