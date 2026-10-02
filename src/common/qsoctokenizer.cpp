// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoctokenizer.h"

#include <functional>
#include <limits>
#include <tuple>
#include <QCryptographicHash>
#include <QFile>
#include <QHash>
#include <QMutex>
#include <QRegularExpression>
#include <queue>

namespace {

constexpr int none = std::numeric_limits<int>::max();

/* tiktoken's expected_hash for o200k_base. */
constexpr char o200kSha256[] = "446a9538cb6c348e3516120d7c08b09f57c36495e2acfffe59a5bf8b0cfb1a2d";

struct Table
{
    QByteArray                 bytes;
    std::vector<qsizetype>     offsets;
    QHash<QByteArrayView, int> ranks;
    QRegularExpression         pattern;
    bool                       valid = false;

    QByteArrayView token(int rank) const
    {
        const auto index = static_cast<size_t>(rank);
        return QByteArrayView(bytes).sliced(offsets[index], offsets[index + 1] - offsets[index]);
    }
};

Table load()
{
    Table table;
    QFile file(QStringLiteral(":/tokenizer/o200k_base.tiktoken"));
    if (!file.open(QIODevice::ReadOnly)) {
        return table;
    }
    const QByteArray source = file.readAll();
    if (QCryptographicHash::hash(source, QCryptographicHash::Sha256).toHex() != o200kSha256) {
        return table;
    }
    table.bytes.reserve(source.size() / 2);
    table.offsets.push_back(0);
    for (qsizetype begin = 0; begin < source.size();) {
        qsizetype end = source.indexOf('\n', begin);
        if (end < 0) {
            end = source.size();
        }
        const QByteArrayView line = QByteArrayView(source).sliced(begin, end - begin);
        begin                     = end + 1;
        if (line.isEmpty()) {
            continue;
        }
        const qsizetype space = line.indexOf(' ');
        bool            ok    = false;
        const qsizetype rank  = space > 0 ? line.sliced(space + 1).toLongLong(&ok) : -1;
        const auto      token = QByteArray::fromBase64Encoding(
            line.first(qMax<qsizetype>(space, 0)).toByteArray(),
            QByteArray::AbortOnBase64DecodingErrors);
        if (!ok || rank != qsizetype(table.offsets.size()) - 1 || !token
            || token.decoded.isEmpty()) {
            return {};
        }
        table.bytes.append(token.decoded);
        table.offsets.push_back(table.bytes.size());
    }
    const int size = int(table.offsets.size()) - 1;
    table.ranks.reserve(size);
    for (int rank = 0; rank < size; ++rank) {
        table.ranks.insert(table.token(rank), rank);
    }
    if (table.ranks.size() != size) {
        return {};
    }
    /* tiktoken's o200k_base pat_str. */
    table.pattern = QRegularExpression(
        QStringLiteral(
            "[^\\r\\n\\p{L}\\p{N}]?[\\p{Lu}\\p{Lt}\\p{Lm}\\p{Lo}\\p{M}]*[\\p{Ll}\\p{Lm}\\p{Lo}"
            "\\p{M}]+(?i:'s|'t|'re|'ve|'m|'ll|'d)?"
            "|[^\\r\\n\\p{L}\\p{N}]?[\\p{Lu}\\p{Lt}\\p{Lm}\\p{Lo}\\p{M}]+[\\p{Ll}\\p{Lm}\\p{Lo}"
            "\\p{M}]*(?i:'s|'t|'re|'ve|'m|'ll|'d)?"
            "|\\p{N}{1,3}"
            "| ?[^\\s\\p{L}\\p{N}]+[\\r\\n/]*"
            "|\\s*[\\r\\n]+"
            "|\\s+(?!\\S)"
            "|\\s+"),
        QRegularExpression::UseUnicodePropertiesOption);
    table.pattern.optimize();
    table.valid = table.pattern.isValid();
    return table;
}

const Table &table()
{
    static const Table instance = load();
    return instance;
}

/* Merge the lowest ranked adjacent pair until none is left, leftmost first. */
void mergePairs(const Table &table, QByteArrayView piece, std::vector<int> &out)
{
    const qsizetype        size = piece.size();
    std::vector<qsizetype> next(static_cast<size_t>(size));
    std::vector<qsizetype> prev(static_cast<size_t>(size));
    std::vector<quint32>   stamp(static_cast<size_t>(size), 0);
    for (qsizetype i = 0; i < size; ++i) {
        next[size_t(i)] = i + 1;
        prev[size_t(i)] = i - 1;
    }
    const auto pairRank = [&](qsizetype i) {
        const qsizetype second = next[size_t(i)];
        if (second >= size) {
            return none;
        }
        return table.ranks.value(piece.sliced(i, next[size_t(second)] - i), none);
    };
    using Item = std::tuple<int, qsizetype, quint32>;
    std::priority_queue<Item, std::vector<Item>, std::greater<>> heap;
    for (qsizetype i = 0; i + 1 < size; ++i) {
        if (const int rank = pairRank(i); rank != none) {
            heap.emplace(rank, i, 0);
        }
    }
    while (!heap.empty()) {
        const auto [rank, i, version] = heap.top();
        heap.pop();
        if (version != stamp[size_t(i)] || next[size_t(i)] >= size) {
            continue;
        }
        const qsizetype second = next[size_t(i)];
        next[size_t(i)]        = next[size_t(second)];
        if (next[size_t(i)] < size) {
            prev[size_t(next[size_t(i)])] = i;
        }
        stamp[size_t(second)] = std::numeric_limits<quint32>::max();
        ++stamp[size_t(i)];
        if (const int merged = pairRank(i); merged != none) {
            heap.emplace(merged, i, stamp[size_t(i)]);
        }
        if (const qsizetype before = prev[size_t(i)]; before >= 0) {
            ++stamp[size_t(before)];
            if (const int merged = pairRank(before); merged != none) {
                heap.emplace(merged, before, stamp[size_t(before)]);
            }
        }
    }
    for (qsizetype i = 0; i < size; i = next[size_t(i)]) {
        out.push_back(table.ranks.value(piece.sliced(i, next[size_t(i)] - i)));
    }
}

void bytePairEncode(const Table &table, QByteArrayView piece, std::vector<int> &out)
{
    const auto direct = table.ranks.constFind(piece);
    if (direct != table.ranks.cend()) {
        out.push_back(*direct);
        return;
    }
    mergePairs(table, piece, out);
}

/* Lone surrogates encode as U+FFFD; the length in UTF-16 units is unchanged. */
QString wellFormed(const QString &text)
{
    QString   result = text;
    qsizetype i      = 0;
    while (i < result.size()) {
        const QChar unit = result.at(i);
        if (unit.isHighSurrogate() && i + 1 < result.size() && result.at(i + 1).isLowSurrogate()) {
            i += 2;
            continue;
        }
        if (unit.isSurrogate()) {
            result[i] = QChar::ReplacementCharacter;
        }
        ++i;
    }
    return result;
}

qint64 utf8Bytes(QStringView text)
{
    qint64 bytes = 0;
    for (qsizetype i = 0; i < text.size(); ++i) {
        const char16_t unit = text[i].unicode();
        if (unit < 0x80) {
            bytes += 1;
        } else if (unit < 0x800) {
            bytes += 2;
        } else if (QChar::isHighSurrogate(unit) && i + 1 < text.size() && text[i + 1].isLowSurrogate()) {
            bytes += 4;
            ++i;
        } else {
            bytes += 3;
        }
    }
    return bytes;
}

/* Calls visit(start, length, tokens) for each piece; stops when it returns false. */
template<typename Visit>
void forEachPiece(const Table &table, const QString &text, Visit visit)
{
    const QString    subject = wellFormed(text);
    std::vector<int> tokens;
    auto             matches = table.pattern.globalMatch(subject);
    while (matches.hasNext()) {
        const auto match = matches.next();
        tokens.clear();
        bytePairEncode(
            table,
            QStringView(subject).sliced(match.capturedStart(), match.capturedLength()).toUtf8(),
            tokens);
        if (!visit(match.capturedStart(), match.capturedLength(), tokens)) {
            return;
        }
    }
}

qint64 uncachedCount(const QString &text, QSocTokenizer::Mode mode)
{
    const Table &tokens = table();
    if (mode == QSocTokenizer::Mode::Bytes || !tokens.valid) {
        return (utf8Bytes(text) + 3) / 4;
    }
    qint64 total = 0;
    forEachPiece(tokens, text, [&total](qsizetype, qsizetype, const std::vector<int> &piece) {
        total += qint64(piece.size());
        return true;
    });
    return total;
}

/* Index in text of the cut that keeps target tokens of the pieces. */
qsizetype cutAt(const QString &text, qint64 target, QSocTokenizer::Mode mode)
{
    const Table &tokens = table();
    if (mode == QSocTokenizer::Mode::Bytes || !tokens.valid) {
        qint64    bytes = 0;
        qsizetype i     = 0;
        while (i < text.size()) {
            const qsizetype width = text.at(i).isHighSurrogate() && i + 1 < text.size()
                                            && text.at(i + 1).isLowSurrogate()
                                        ? 2
                                        : 1;
            bytes += utf8Bytes(QStringView(text).sliced(i, width));
            if (bytes > target * 4) {
                break;
            }
            i += width;
        }
        return i;
    }
    qint64    used = 0;
    qsizetype cut  = text.size();
    forEachPiece(tokens, text, [&](qsizetype start, qsizetype length, const std::vector<int> &piece) {
        if (used + qint64(piece.size()) <= target) {
            used += qint64(piece.size());
            return true;
        }
        qint64 keep = 0;
        for (qint64 k = 0; k < target - used; ++k) {
            keep += tokens.token(piece[size_t(k)]).size();
        }
        const QStringView source = QStringView(text).sliced(start, length);
        qsizetype         units  = 0;
        while (units < source.size()) {
            const qsizetype width = source[units].isHighSurrogate() && units + 1 < source.size()
                                            && source[units + 1].isLowSurrogate()
                                        ? 2
                                        : 1;
            keep -= utf8Bytes(source.sliced(units, width));
            if (keep < 0) {
                break;
            }
            units += width;
        }
        cut = start + units;
        return false;
    });
    return cut;
}

struct CountCache
{
    QMutex                    mutex;
    QHash<QByteArray, qint64> counts;
};

CountCache &countCache()
{
    static CountCache cache;
    return cache;
}

QByteArray cacheKey(const QString &text, QSocTokenizer::Mode mode)
{
    QByteArray key;
    key.append(char(mode));
    key.append(QByteArray::number(text.size()));
    key.append(':');
    key.append(QByteArray::number(quint64(qHash(text, size_t(0x9e3779b97f4a7c15ULL)))));
    key.append(':');
    key.append(QByteArray::number(quint64(qHash(text, size_t(0x51ed270b2c8f1a3dULL)))));
    return key;
}

} // namespace

qint64 QSocTokenizer::count(const QString &text, Mode mode)
{
    /* Short texts cost less to count than to look up. */
    if (text.size() < 256) {
        return uncachedCount(text, mode);
    }
    const QByteArray key   = cacheKey(text, mode);
    CountCache      &cache = countCache();
    {
        const QMutexLocker locker(&cache.mutex);
        const auto         found = cache.counts.constFind(key);
        if (found != cache.counts.cend()) {
            return *found;
        }
    }
    const qint64       result = uncachedCount(text, mode);
    const QMutexLocker locker(&cache.mutex);
    if (cache.counts.size() >= 8192) {
        cache.counts.clear();
    }
    cache.counts.insert(key, result);
    return result;
}

QString QSocTokenizer::truncate(const QString &text, qint64 maxTokens, Mode mode)
{
    if (maxTokens <= 0) {
        return {};
    }
    if (count(text, mode) <= maxTokens) {
        return text;
    }
    /* A cut can retokenize across its edge, so verify and retry. */
    qint64 target = maxTokens;
    for (int attempt = 0; attempt < 4 && target > 0; ++attempt) {
        const QString prefix = text.left(cutAt(text, target, mode));
        const qint64  used   = count(prefix, mode);
        if (used <= maxTokens) {
            return prefix;
        }
        target -= used - maxTokens;
    }
    qsizetype low  = 0;
    qsizetype high = text.size();
    while (low < high) {
        const qsizetype middle = low + (high - low + 1) / 2;
        if (count(text.left(middle), mode) <= maxTokens) {
            low = middle;
        } else {
            high = middle - 1;
        }
    }
    if (low > 0 && low < text.size() && text.at(low - 1).isHighSurrogate()) {
        --low;
    }
    return text.left(low);
}

std::vector<int> QSocTokenizer::encode(const QString &text)
{
    std::vector<int> result;
    const Table     &tokens = table();
    if (!tokens.valid) {
        return result;
    }
    forEachPiece(tokens, text, [&result](qsizetype, qsizetype, const std::vector<int> &piece) {
        result.insert(result.end(), piece.begin(), piece.end());
        return true;
    });
    return result;
}

std::vector<int> QSocTokenizer::encodePiece(QByteArrayView piece)
{
    std::vector<int> result;
    const Table     &tokens = table();
    if (tokens.valid && !piece.isEmpty()) {
        mergePairs(tokens, piece, result);
    }
    return result;
}

bool QSocTokenizer::available()
{
    return table().valid;
}

int QSocTokenizer::vocabularySize()
{
    return table().valid ? int(table().offsets.size()) - 1 : 0;
}

QByteArray QSocTokenizer::tokenBytes(int rank)
{
    const Table &tokens = table();
    if (!tokens.valid || rank < 0 || rank >= vocabularySize()) {
        return {};
    }
    return tokens.token(rank).toByteArray();
}
