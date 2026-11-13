// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCVCDCHECKPOINTS_H
#define QSOCVCDCHECKPOINTS_H

#include <QByteArray>
#include <QHash>
#include <QList>

#include <limits>
#include <utility>

class QSocVcdCheckpoints
{
public:
    using Values = QHash<QByteArray, QByteArray>;
    struct Entry
    {
        quint64 time   = 0;
        quint64 offset = 0;
        Values  values;
        quint64 payload = 0;
    };

    explicit QSocVcdCheckpoints(
        quint64   stride  = 16ULL * 1024 * 1024,
        qsizetype entries = 256,
        quint64   payload = 8ULL * 1024 * 1024)
        : stride_(qMax<quint64>(1, stride))
        , entryLimit_(qMax<qsizetype>(1, entries))
        , payloadLimit_(payload)
    {}

    void record(quint64 time, quint64 offset, const Values &values)
    {
        if (values.size() > 32 || offset < lastOffset_ || offset - lastOffset_ < stride_
            || (!entries_.isEmpty() && time < entries_.last().time))
            return;
        quint64 payload = 0;
        for (auto it = values.cbegin(); it != values.cend(); ++it) {
            const auto bytes = quint64(it.key().size()) + quint64(it.value().size());
            if (bytes > payloadLimit_ - payload)
                return;
            payload += bytes;
        }
        while (entries_.size() >= entryLimit_ || payload > payloadLimit_ - payload_) {
            compact();
            if (entries_.size() == 1 && (entryLimit_ == 1 || payload > payloadLimit_ - payload_)) {
                entries_.clear();
                payload_    = 0;
                lastOffset_ = 0;
            }
            if (offset < lastOffset_ || offset - lastOffset_ < stride_)
                return;
        }
        entries_.append({time, offset, values, payload});
        payload_ += payload;
        lastOffset_ = offset;
    }

    const Entry *before(quint64 start) const
    {
        for (auto it = entries_.crbegin(); it != entries_.crend(); ++it) {
            if (it->time < start)
                return &*it;
        }
        return nullptr;
    }

    quint64   payloadBytes() const { return payload_; }
    qsizetype size() const { return entries_.size(); }

private:
    void compact()
    {
        qsizetype destination = 0;
        payload_              = 0;
        for (qsizetype source = 0; source < entries_.size(); source += 2) {
            if (destination != source)
                entries_[destination] = std::move(entries_[source]);
            payload_ += entries_[destination].payload;
            ++destination;
        }
        entries_.resize(destination);
        lastOffset_ = entries_.isEmpty() ? 0 : entries_.last().offset;
        stride_     = stride_ > std::numeric_limits<quint64>::max() / 2
                          ? std::numeric_limits<quint64>::max()
                          : stride_ * 2;
    }

    QList<Entry> entries_;
    quint64      stride_;
    qsizetype    entryLimit_;
    quint64      payloadLimit_;
    quint64      payload_    = 0;
    quint64      lastOffset_ = 0;
};

#endif
