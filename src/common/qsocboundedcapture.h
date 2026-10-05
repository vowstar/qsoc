// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCBOUNDEDCAPTURE_H
#define QSOCBOUNDEDCAPTURE_H

#include <QByteArray>
#include <QByteArrayView>
#include <QString>

/**
 * @brief Byte stream kept as its head and a rolling tail in bounded memory.
 * @details A stream longer than the limit keeps its first and last bytes with
 *          one marker() between them. bytes() stays within the limit, marker
 *          included, whenever the limit can hold the marker, and every cut
 *          lands on a UTF-8 character boundary.
 */
class QSocBoundedCapture
{
public:
    static constexpr qint64 kDefaultLimit = qint64{4} * 1024 * 1024;

    explicit QSocBoundedCapture(qint64 limitBytes = kDefaultLimit);

    void   append(QByteArrayView data);
    qint64 totalBytes() const { return total_; }
    bool   isElided() const { return total_ > limit_; }

    /** @brief Head, marker and tail, or the whole stream when it fits. */
    QByteArray bytes() const;
    QString    text() const { return QString::fromUtf8(bytes()); }

    /** @brief Last @p maxBytes bytes kept, starting on a character boundary. */
    QByteArray tailBytes(qint64 maxBytes) const;

    static QString marker(qint64 omittedBytes);
    static bool    isElided(const QString &text);

    /** @brief @p text cut to @p limitBytes of UTF-8 the same way. */
    static QString bound(const QString &text, qint64 limitBytes);

private:
    qint64     limit_;
    qint64     headLimit_;
    qint64     tailLimit_;
    qint64     total_ = 0;
    QByteArray head_;
    QByteArray tail_;
};

#endif // QSOCBOUNDEDCAPTURE_H
