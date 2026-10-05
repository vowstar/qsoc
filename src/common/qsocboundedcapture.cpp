// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocboundedcapture.h"

#include <QRegularExpression>

#include <algorithm>

namespace {

bool continuation(const QByteArray &data, qint64 index)
{
    return index < data.size() && (static_cast<unsigned char>(data[index]) & 0xc0) == 0x80;
}

/* Longest prefix of at most @p limit bytes that ends on a character boundary. */
qint64 headEnd(const QByteArray &data, qint64 limit)
{
    qint64 end = std::clamp(limit, qint64(0), qint64(data.size()));
    while (end > 0 && continuation(data, end))
        --end;
    return end;
}

/* Start of the longest suffix of at most @p limit bytes on a character boundary. */
qint64 tailStart(const QByteArray &data, qint64 limit)
{
    qint64 start = data.size() - std::clamp(limit, qint64(0), qint64(data.size()));
    while (continuation(data, start))
        ++start;
    return start;
}

} // namespace

QSocBoundedCapture::QSocBoundedCapture(qint64 limitBytes)
    : limit_(std::max(qint64(0), limitBytes))
    , headLimit_(limit_ / 2)
    , tailLimit_(limit_ - headLimit_)
{}

void QSocBoundedCapture::append(QByteArrayView data)
{
    total_ += data.size();
    const qint64 toHead = std::min(qint64(data.size()), headLimit_ - qint64(head_.size()));
    if (toHead > 0) {
        head_.append(data.first(toHead));
        data = data.sliced(toHead);
    }
    if (data.isEmpty())
        return;
    tail_.append(data);
    /* Trim in bulk so each byte is moved a bounded number of times. */
    if (tail_.size() > 2 * tailLimit_)
        tail_.remove(0, tail_.size() - tailLimit_);
}

QByteArray QSocBoundedCapture::bytes() const
{
    if (!isElided())
        return head_ + tail_;
    /* The marker for the whole stream is never shorter than the final one. */
    const qint64 room     = std::max(qint64(0), limit_ - qint64(marker(total_).toUtf8().size()));
    const qint64 headKeep = headEnd(head_, room / 2);
    const qint64 tailFrom = tailStart(tail_, room - headKeep);
    const qint64 omitted  = total_ - headKeep - (tail_.size() - tailFrom);
    return head_.left(headKeep) + marker(omitted).toUtf8() + tail_.mid(tailFrom);
}

QByteArray QSocBoundedCapture::tailBytes(qint64 maxBytes) const
{
    QByteArray kept = tail_;
    if (!isElided() && tail_.size() < maxBytes)
        kept = head_.right(maxBytes - tail_.size()) + tail_;
    return kept.mid(tailStart(kept, maxBytes));
}

QString QSocBoundedCapture::marker(qint64 omittedBytes)
{
    return QStringLiteral("\n[... %1 bytes omitted ...]\n").arg(omittedBytes);
}

bool QSocBoundedCapture::isElided(const QString &text)
{
    static const QRegularExpression pattern(
        QStringLiteral("\\n\\[\\.\\.\\. \\d+ bytes omitted \\.\\.\\.\\]\\n"));
    return text.contains(pattern);
}

QString QSocBoundedCapture::bound(const QString &text, qint64 limitBytes)
{
    const QByteArray data = text.toUtf8();
    if (data.size() <= limitBytes)
        return text;
    QSocBoundedCapture capture(limitBytes);
    capture.append(data);
    return capture.text();
}
