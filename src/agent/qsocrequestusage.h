// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCREQUESTUSAGE_H
#define QSOCREQUESTUSAGE_H

#include "common/qsoctokenizer.h"

#include <nlohmann/json.hpp>
#include <optional>
#include <QString>

struct QSocRequestSnapshot
{
    nlohmann::json      messages = nlohmann::json::array();
    nlohmann::json      tools    = nlohmann::json::array();
    QString             route;
    QString             effort;
    qint64              imageTokens = 5000;
    QSocTokenizer::Mode counter     = QSocTokenizer::Mode::O200k;
};

/**
 * @brief Request size split into its server reported and locally counted parts.
 */
struct QSocTokenEstimate
{
    qint64 reported = 0;   /* Server reported anchor */
    qint64 counted  = 0;   /* Local count on top of the anchor */
    double margin   = 1.0; /* Upper bound factor for the local part */

    qint64 point() const;
    qint64 upper() const;
    bool   approximate() const { return counted > 0; }
};

struct QSocObservedUsage
{
    qint64  inputTokens              = 0;
    qint64  outputTokens             = 0;
    qint64  cachedTokens             = 0;
    qint64  cacheEligibleInputTokens = 0;
    quint64 requests                 = 0;
    quint64 cacheReportedRequests    = 0;
    quint64 outputReportedRequests   = 0;
};

/**
 * @brief A server count that disagreed with the usage its request reported.
 */
struct QSocCountMismatch
{
    qint64 counted  = 0;
    qint64 reported = 0;
};

class QSocRequestUsage
{
public:
    static qint64 estimateHistory(
        const nlohmann::json &messages, QSocTokenizer::Mode counter = QSocTokenizer::Mode::O200k);
    static qint64 estimateText(
        const QString &text, QSocTokenizer::Mode counter = QSocTokenizer::Mode::O200k);
    static qint64 estimateMessages(
        const nlohmann::json &messages,
        qint64                imageTokens = 5000,
        QSocTokenizer::Mode   counter     = QSocTokenizer::Mode::O200k);
    static qint64              estimateRequest(const QSocRequestSnapshot &request);
    static QSocTokenizer::Mode counterFor(const QString &tokenizer);
    static double              margin(QSocTokenizer::Mode counter);
    QSocTokenEstimate          estimate(const QSocRequestSnapshot &request) const;
    qint64                     estimateNext(const QSocRequestSnapshot &request) const;
    quint64                    begin(QSocRequestSnapshot request);
    bool                       complete(quint64 generation, const nlohmann::json &usage);
    void                       discardPending();
    void                       invalidateAnchor();
    /** @brief Anchor on a server count, checked when the same request completes. */
    void                             recordCount(const QSocRequestSnapshot &request, qint64 tokens);
    std::optional<QSocCountMismatch> takeCountMismatch();
    QSocObservedUsage                observed() const { return observed_; }

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
    quint64                          generation_ = 0;
    std::optional<Pending>           pending_;
    std::optional<Anchor>            anchor_;
    std::optional<Anchor>            count_;
    std::optional<QSocCountMismatch> mismatch_;
    QSocObservedUsage                observed_;
};

#endif // QSOCREQUESTUSAGE_H
