// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLWEB_H
#define QSOCTOOLWEB_H

#include "agent/qsoctool.h"
#include "common/qllmservice.h"
#include "common/qsocconfig.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QPointer>

/**
 * @brief Tool to search the web via SearXNG
 */
class QSocToolWebSearch : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolWebSearch(QObject *parent = nullptr, QSocConfig *config = nullptr);
    ~QSocToolWebSearch() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    void    abort() override;

private:
    QSocConfig            *config         = nullptr;
    QNetworkAccessManager *networkManager = nullptr;
    QNetworkReply         *currentReply   = nullptr;
    QEventLoop            *currentLoop    = nullptr;

    void setupProxy();
};

/**
 * @brief Tool to fetch content from a URL
 */
class QSocToolWebFetch : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolWebFetch(
        QObject *parent = nullptr, QSocConfig *config = nullptr, QLLMService *llm = nullptr);
    ~QSocToolWebFetch() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    void    abort() override;

    static QString htmlToMarkdown(const QString &html);

    /**
     * @brief Sentinel that wraps a JSON image-attachment payload in tool
     *        results so the agent main loop can lift it into the next
     *        request as an OpenAI-compat image_url block.
     * @details The byte sequence is intentionally unusual (control char
     *          \x01 plus an XML-style element name) so a model that
     *          paraphrases tool output cannot accidentally inject one.
     *          The agent strips the marker before showing results to the
     *          user; only the human-readable summary line remains.
     */
    static const char *attachmentMarkerOpen();
    static const char *attachmentMarkerClose();

    /**
     * @brief Decide what to emit for an already-fetched image response.
     * @details Pure function over (url, mime, bytes, model-config): no
     *          network I/O. Public so unit tests can drive every branch
     *          (capability gate, budget overflow, resize) without a
     *          live HTTP server, and to keep the http-only validator in
     *          execute() from blocking test fixtures. Production callers
     *          go through execute() which fetches first, then dispatches
     *          here for image MIME types.
     */
    QString handleImageResponse(
        const QString &sourceUrl, const QString &contentType, const QByteArray &body);

private:
    QSocConfig            *config = nullptr;
    QPointer<QLLMService>  llmService;
    QNetworkAccessManager *networkManager = nullptr;
    QNetworkReply         *currentReply   = nullptr;
    QEventLoop            *currentLoop    = nullptr;

    void setupProxy();
};

#endif // QSOCTOOLWEB_H
