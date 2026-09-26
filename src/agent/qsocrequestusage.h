// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCREQUESTUSAGE_H
#define QSOCREQUESTUSAGE_H

#include <nlohmann/json.hpp>
#include <optional>
#include <QString>

struct QSocRequestSnapshot
{
    nlohmann::json messages = nlohmann::json::array();
    nlohmann::json tools    = nlohmann::json::array();
    QString        route;
    QString        effort;
    qint64         imageTokens = 5000;
};

struct QSocObservedUsage
{
    qint64  inputTokens              = 0;
    qint64  outputTokens             = 0;
    qint64  cachedTokens             = 0;
    qint64  cacheEligibleInputTokens = 0;
    quint64 requests                 = 0;
    quint64 cacheReportedRequests    = 0;
};

class QSocRequestUsage
{
public:
    static qint64     estimateHistory(const nlohmann::json &messages);
    static qint64     estimateText(const QString &text);
    static qint64     estimateMessages(const nlohmann::json &messages, qint64 imageTokens = 5000);
    static qint64     estimateRequest(const QSocRequestSnapshot &request);
    qint64            estimateNext(const QSocRequestSnapshot &request) const;
    quint64           begin(QSocRequestSnapshot request);
    bool              complete(quint64 generation, const nlohmann::json &usage);
    void              discardPending();
    void              invalidateAnchor();
    QSocObservedUsage observed() const { return observed_; }

private:
    struct Pending
    {
        quint64             generation;
        QSocRequestSnapshot request;
    };
    struct Anchor
    {
        QSocRequestSnapshot request;
        qint64              inputTokens;
    };
    quint64                generation_ = 0;
    std::optional<Pending> pending_;
    std::optional<Anchor>  anchor_;
    QSocObservedUsage      observed_;
};

#endif // QSOCREQUESTUSAGE_H
