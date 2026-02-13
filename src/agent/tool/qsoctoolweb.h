// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLWEB_H
#define QSOCTOOLWEB_H

#include "agent/qsoctool.h"
#include "common/qsocconfig.h"

#include <QEventLoop>
#include <QNetworkAccessManager>
#include <QNetworkReply>

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
    explicit QSocToolWebFetch(QObject *parent = nullptr, QSocConfig *config = nullptr);
    ~QSocToolWebFetch() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;
    void    abort() override;

    static QString htmlToText(const QString &html);

private:
    QSocConfig            *config         = nullptr;
    QNetworkAccessManager *networkManager = nullptr;
    QNetworkReply         *currentReply   = nullptr;
    QEventLoop            *currentLoop    = nullptr;

    void setupProxy();
};

#endif // QSOCTOOLWEB_H
