// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2025 Huang Rui <vowstar@gmail.com>

#ifndef QSOCTOOLDOC_H
#define QSOCTOOLDOC_H

#include "agent/qsoctool.h"

#include <QMap>

/**
 * @brief Tool to query QSoC documentation
 */
class QSocToolDocQuery : public QSocTool
{
    Q_OBJECT

public:
    explicit QSocToolDocQuery(QObject *parent = nullptr);
    ~QSocToolDocQuery() override;

    QString getName() const override;
    QString getDescription() const override;
    json    getParametersSchema() const override;
    QString execute(const json &arguments) override;

private:
    /**
     * @brief Get list of available documentation topics
     * @return QStringList List of topic names
     */
    QStringList getAvailableTopics() const;

    /**
     * @brief Read documentation content from resource
     * @param resourcePath Path to the resource file
     * @return QString Content of the documentation
     */
    QString readDocumentation(const QString &resourcePath) const;

    /**
     * @brief Strip Typst markup from content
     * @param content Raw Typst content
     * @return QString Cleaned text content
     */
    QString stripTypstMarkup(const QString &content) const;

    /* Mapping from topic names to resource paths */
    QMap<QString, QString> topicMap_;
};

#endif // QSOCTOOLDOC_H
