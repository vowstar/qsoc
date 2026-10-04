// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QLLMDIAGNOSTICS_H
#define QLLMDIAGNOSTICS_H

#include <memory>
#include <nlohmann/json.hpp>
#include <QString>

class QLLMDiagnostics
{
    struct State;

public:
    enum class Kind { Text, Chat, Stream };
    enum class Outcome { Completed, Failed, Cancelled, TimedOut, Superseded, Destroyed };

    class Request
    {
    public:
        ~Request();
        void posted();
        void firstByte();
        void usage(const nlohmann::json &value, bool separateCache = false);
        void finish(Outcome outcome);

    private:
        friend class QLLMDiagnostics;
        struct Data;
        explicit Request(std::shared_ptr<State> state, Kind kind);
        std::unique_ptr<Data> data_;
    };
    using RequestPtr = std::shared_ptr<Request>;

    QLLMDiagnostics();
    RequestPtr     begin(Kind kind, const QString &route, const nlohmann::json &payload);
    void           setEnabled(bool enabled);
    nlohmann::json snapshot() const;

private:
    bool                   enabled_ = true;
    std::shared_ptr<State> state_;
};

#endif // QLLMDIAGNOSTICS_H
