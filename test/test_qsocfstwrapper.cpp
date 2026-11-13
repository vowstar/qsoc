// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/waveform/qsocfstwrapper.h"
#include "qsoc_test.h"

#include <QByteArray>
#include <QtTest>

#include <algorithm>
#include <cstring>

namespace {
QByteArray content()
{
    QByteArray pattern(8192, '\0');
    uint32_t   state = 0x12345678U;
    for (char &byte : pattern) {
        state = state * 1664525U + 1013904223U;
        byte  = char(state >> 24);
    }
    return pattern.repeated(1024);
}

QByteArray wrapped(const QByteArray &plain)
{
    z_stream stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return {};
    QByteArray compressed(deflateBound(&stream, plain.size()), '\0');
    stream.next_in   = reinterpret_cast<Bytef *>(const_cast<char *>(plain.constData()));
    stream.avail_in  = plain.size();
    stream.next_out  = reinterpret_cast<Bytef *>(compressed.data());
    stream.avail_out = compressed.size();
    const int result = deflate(&stream, Z_FINISH);
    compressed.resize(stream.total_out);
    deflateEnd(&stream);
    if (result != Z_STREAM_END)
        return {};
    QByteArray output(1, char(FST_BL_ZWRAPPER));
    for (uint64_t value : {uint64_t(16 + compressed.size()), uint64_t(plain.size())}) {
        for (int shift = 56; shift >= 0; shift -= 8)
            output.append(char(value >> shift));
    }
    return output + compressed;
}

class Bits
{
public:
    void put(uint32_t value, unsigned width)
    {
        hold |= value << count;
        count += width;
        while (count >= 8) {
            bytes.append(char(hold));
            hold >>= 8;
            count -= 8;
        }
    }

    static uint32_t reverse(uint32_t value, unsigned width)
    {
        uint32_t result = 0;
        for (unsigned bit = 0; bit < width; ++bit)
            result = (result << 1) | ((value >> bit) & 1U);
        return result;
    }

    void symbol(unsigned value)
    {
        const unsigned width = value <= 143 ? 8 : value <= 255 ? 9 : value <= 279 ? 7 : 8;
        const unsigned code  = value <= 143   ? 0x30 + value
                               : value <= 255 ? 0x190 + value - 144
                               : value <= 279 ? value - 256
                                              : 0xc0 + value - 280;
        put(reverse(code, width), width);
    }

    QByteArray finish()
    {
        if (count)
            bytes.append(char(hold));
        return bytes;
    }

private:
    QByteArray bytes;
    uint32_t   hold  = 0;
    unsigned   count = 0;
};

QByteArray singleBlock(const QByteArray &plain)
{
    static constexpr unsigned bases[] = {3,  4,  5,  6,   7,   8,   9,   10,  11, 13,
                                         15, 17, 19, 23,  27,  31,  35,  43,  51, 59,
                                         67, 83, 99, 115, 131, 163, 195, 227, 258};
    static constexpr unsigned extra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2,
                                         2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    Bits                      bits;
    bits.put(1, 1);
    bits.put(1, 2);
    for (qsizetype index = 0; index < 8192; ++index)
        bits.symbol(quint8(plain.at(index)));
    qsizetype position = 8192;
    while (position < plain.size()) {
        const auto length = unsigned(qMin<qsizetype>(258, plain.size() - position));
        if (length < 3) {
            bits.symbol(quint8(plain.at(position++)));
            continue;
        }
        unsigned code = 28;
        while (bases[code] > length)
            --code;
        bits.symbol(257 + code);
        bits.put(length - bases[code], extra[code]);
        bits.put(Bits::reverse(25, 5), 5);
        bits.put(8192 - 6145, 11);
        position += length;
    }
    bits.symbol(256);
    QByteArray     gzip = wrapped({}).mid(17, 10) + bits.finish();
    const uint32_t checksum
        = crc32(0, reinterpret_cast<const Bytef *>(plain.constData()), plain.size());
    for (uint32_t value : {checksum, uint32_t(plain.size())}) {
        for (unsigned shift = 0; shift < 32; shift += 8)
            gzip.append(char(value >> shift));
    }
    QByteArray result(1, char(FST_BL_ZWRAPPER));
    for (uint64_t value : {uint64_t(16 + gzip.size()), uint64_t(plain.size())}) {
        for (int shift = 56; shift >= 0; shift -= 8)
            result.append(char(value >> shift));
    }
    return result + gzip;
}

struct Source
{
    QSocFstInput input()
    {
        return QSocFstInput{
            1,
            sizeof(QSocFstInput),
            this,
            [](void     *user,
               uint64_t  offset,
               void     *destination,
               uint32_t  requested,
               uint32_t *received) {
                auto &source = *static_cast<Source *>(user);
                *received    = 0;
                if (source.failAfter && source.reads >= source.failAfter)
                    return int(INPUT_IO_ERROR);
                if (offset >= uint64_t(source.bytes.size()))
                    return int(INPUT_EOF);
                const auto count = uint32_t(
                    std::min<uint64_t>(
                        {requested,
                         source.shortRead ? source.shortRead : requested,
                         uint64_t(source.bytes.size()) - offset}));
                std::memcpy(destination, source.bytes.constData() + offset, count);
                *received = count;
                ++source.reads;
                return int(INPUT_OK);
            },
            [](void *user, uint64_t *size) {
                *size = static_cast<Source *>(user)->bytes.size();
                return int(INPUT_OK);
            },
            [](void *user) { return static_cast<Source *>(user)->cancelled ? 1 : 0; },
            [](void *, uint64_t) {},
            nullptr,
            64ULL * 1024 * 1024};
    }

    QByteArray bytes;
    uint64_t   reads     = 0;
    uint64_t   failAfter = 0;
    uint32_t   shortRead = 0;
    bool       cancelled = false;
};

class Test : public QObject
{
    Q_OBJECT
private slots:
    void randomRanges_data();
    void randomRanges();
    void corruptCrc();
    void wrongSize();
    void cancelled();
    void sourceFailure();
    void warmReadKeepsInflaters();
    void pinnedWindowSurvivesCompletedValidation();
    void checkpointBudget();
    void largeSourceCheckpoints_data();
    void largeSourceCheckpoints();
    void resourceBounds_data();
    void resourceBounds();
};

void Test::randomRanges_data()
{
    QTest::addColumn<unsigned>("shortRead");
    QTest::addColumn<bool>("oneBlock");
    QTest::newRow("full-ranges") << 0U << false;
    QTest::newRow("37-byte-source-ranges") << 37U << false;
    QTest::newRow("single-block-8192-distance") << 0U << true;
}

void Test::randomRanges()
{
    QFETCH(unsigned, shortRead);
    QFETCH(bool, oneBlock);
    const auto plain = content();
    Source     source{oneBlock ? singleBlock(plain) : wrapped(plain)};
    QVERIFY(!source.bytes.isEmpty());
    source.shortRead = shortRead;
    QSocFstWrapper::Limits limits;
    limits.checkpointCount  = 4;
    limits.checkpointStride = 65536;
    limits.memoryBytes      = 2 * 1024 * 1024;
    QSocFstWrapper wrapper(source.input(), limits);
    QCOMPARE(wrapper.status(), int(INPUT_OK));
    auto input = wrapper.input();
    for (uint64_t offset :
         {0ULL,
          131109ULL,
          65701ULL,
          7340155ULL,
          7274496ULL,
          7340163ULL,
          17ULL,
          3145777ULL,
          8380000ULL}) {
        QByteArray result(4096, '\0');
        uint32_t   received = 0;
        QCOMPARE(
            input.read_at(input.user, offset, result.data(), result.size(), &received),
            int(INPUT_OK));
        QCOMPARE(received, uint32_t(result.size()));
        QCOMPARE(result, plain.mid(offset, result.size()));
        const auto stats = wrapper.statistics();
        QVERIFY(stats.checkpoints <= limits.checkpointCount);
        QVERIFY(stats.memoryBytes <= limits.memoryBytes);
        QVERIFY(stats.peakMemoryBytes <= limits.memoryBytes);
    }
    QByteArray large(128 * 1024, '\0');
    uint32_t   received = 0;
    QCOMPARE(input.read_at(input.user, 0, large.data(), large.size(), &received), int(INPUT_OK));
    QCOMPARE(received, uint32_t(large.size()));
    QCOMPARE(large, plain.left(large.size()));
    QCOMPARE(
        input.read_at(input.user, 0, large.data(), large.size() + 1, &received), int(INPUT_RANGE));
    QCOMPARE(received, uint32_t(0));
    QCOMPARE(wrapper.finish(), int(INPUT_OK));
    const auto stats = wrapper.statistics();
    QVERIFY(stats.validated);
    QVERIFY(stats.checkpointCopies > 4);
    QVERIFY(stats.checkpointHits > 0);
    QVERIFY(stats.copiedPendingBytes > 0);
    QVERIFY(stats.copiesInsideBlock > 0);
    if (oneBlock) {
        QVERIFY(stats.restoredPendingBytes > 0);
        QVERIFY(stats.restoresInsideBlock > 0);
    }
    QVERIFY(stats.peakMemoryBytes <= limits.memoryBytes);
    qInfo() << "wrapper bytes" << plain.size() << "source" << stats.sourceBytes << "inflated"
            << stats.inflatedBytes << "managed peak" << stats.peakMemoryBytes << "hits"
            << stats.checkpointHits << "rewinds" << stats.coldRewinds << "restored pending"
            << stats.restoredPendingBytes << "mid-block restores" << stats.restoresInsideBlock;
}

void Test::warmReadKeepsInflaters()
{
    const auto     plain = content();
    Source         source{singleBlock(plain)};
    QSocFstWrapper wrapper(source.input());
    auto           input = wrapper.input();
    QByteArray     value(4096, '\0');
    uint32_t       received = 0;
    QCOMPARE(
        input.read_at(input.user, 4 * 1024 * 1024 + 17, value.data(), value.size(), &received),
        int(INPUT_OK));
    QCOMPARE(value, plain.mid(4 * 1024 * 1024 + 17, value.size()));
    const auto before = wrapper.statistics();
    QCOMPARE(
        input.read_at(input.user, 4 * 1024 * 1024 + 2048, value.data(), value.size(), &received),
        int(INPUT_OK));
    QCOMPARE(value, plain.mid(4 * 1024 * 1024 + 2048, value.size()));
    const auto after = wrapper.statistics();
    QCOMPARE(after.coldRewinds, before.coldRewinds);
    QVERIFY(after.inflatedBytes - before.inflatedBytes <= 4096);
    QVERIFY(after.sourceBytes - before.sourceBytes <= 65536);
    QCOMPARE(wrapper.finish(), int(INPUT_OK));
}

void Test::pinnedWindowSurvivesCompletedValidation()
{
    const auto     plain = content();
    Source         source{singleBlock(plain)};
    QSocFstWrapper wrapper(source.input());
    QCOMPARE(wrapper.finish(), int(INPUT_OK));
    QVERIFY(wrapper.statistics().validated);
    QCOMPARE(wrapper.pin(4 * 1024 * 1024), int(INPUT_OK));
    auto       input = wrapper.input();
    QByteArray value(4096, '\0');
    uint32_t   received = 0;
    QCOMPARE(
        input.read_at(input.user, 6 * 1024 * 1024, value.data(), value.size(), &received),
        int(INPUT_OK));
    QCOMPARE(value, plain.mid(6 * 1024 * 1024, value.size()));
    const auto before = wrapper.statistics();
    QCOMPARE(wrapper.pin(4 * 1024 * 1024 + 2048), int(INPUT_OK));
    QCOMPARE(
        input.read_at(input.user, 4 * 1024 * 1024 + 2048, value.data(), value.size(), &received),
        int(INPUT_OK));
    QCOMPARE(value, plain.mid(4 * 1024 * 1024 + 2048, value.size()));
    QCOMPARE(wrapper.finish(), int(INPUT_OK));
    const auto after = wrapper.statistics();
    QCOMPARE(after.coldRewinds, before.coldRewinds);
    QVERIFY(after.inflatedBytes - before.inflatedBytes <= 8192);
    QVERIFY(after.checkpointHits > before.checkpointHits);
    QVERIFY(after.checkpoints <= 16);
    QVERIFY(after.peakMemoryBytes <= 8 * 1024 * 1024);
    const auto validated = wrapper.statistics();
    QCOMPARE(wrapper.finish(), int(INPUT_OK));
    QCOMPARE(wrapper.statistics().inflatedBytes, validated.inflatedBytes);
    QCOMPARE(wrapper.statistics().sourceBytes, validated.sourceBytes);
}

void Test::checkpointBudget()
{
    const auto             plain = content();
    Source                 source{singleBlock(plain)};
    QSocFstWrapper::Limits limits;
    limits.checkpointStride = 65536;
    QSocFstWrapper wrapper(source.input(), limits);
    QCOMPARE(wrapper.finish(), int(INPUT_OK));
    QCOMPARE(wrapper.statistics().checkpoints, limits.checkpointCount);
    const uint64_t offset = 4 * 1024 * 1024 + 17;
    QCOMPARE(wrapper.pin(offset), int(INPUT_OK));
    auto       input = wrapper.input();
    QByteArray result(4096, '\0');
    uint32_t   received = 0;
    QCOMPARE(
        input.read_at(input.user, offset, result.data(), result.size(), &received), int(INPUT_OK));
    QCOMPARE(received, uint32_t(result.size()));
    QCOMPARE(result, plain.mid(offset, result.size()));
    const auto stats = wrapper.statistics();
    QVERIFY(stats.checkpoints <= limits.checkpointCount);
    QVERIFY(stats.peakMemoryBytes <= limits.memoryBytes);
    qInfo() << "managed peak" << stats.peakMemoryBytes << "limit" << limits.memoryBytes
            << "checkpoints" << stats.checkpoints;
}

void Test::largeSourceCheckpoints_data()
{
    QTest::addColumn<unsigned>("members");
    QTest::newRow("expanded-over-4-GiB") << 513U;
    QTest::newRow("window-over-4-GiB") << 1025U;
}

void Test::largeSourceCheckpoints()
{
    QFETCH(unsigned, members);
    const auto plain  = content();
    const auto member = wrapped(plain).mid(17);
    QVERIFY(!member.isEmpty());
    const auto expanded = uint64_t(plain.size()) * members;
    QVERIFY(expanded > UINT32_MAX);
    QByteArray bytes(1, char(FST_BL_ZWRAPPER));
    for (uint64_t value : {16 + uint64_t(member.size()) * members, expanded}) {
        for (int shift = 56; shift >= 0; shift -= 8)
            bytes.append(char(value >> shift));
    }
    bytes += member.repeated(members);
    QVERIFY(bytes.size() < 64 * 1024 * 1024);
    Source         source{bytes};
    QSocFstWrapper wrapper(source.input());
    QCOMPARE(wrapper.status(), int(INPUT_OK));
    QCOMPARE(wrapper.finish(), int(INPUT_OK));
    const auto before = wrapper.statistics();
    QVERIFY(before.validated);
    const auto offset = expanded * 9 / 10;
    QCOMPARE(wrapper.pin(offset), int(INPUT_OK));
    auto       input = wrapper.input();
    QByteArray result(4096, '\0');
    uint32_t   received = 0;
    QCOMPARE(
        input.read_at(input.user, offset, result.data(), result.size(), &received), int(INPUT_OK));
    QCOMPARE(received, uint32_t(result.size()));
    QCOMPARE(result, plain.mid(offset % uint64_t(plain.size()), result.size()));
    const auto after = wrapper.statistics();
    qInfo() << "expanded" << expanded << "encoded" << bytes.size() << "window" << offset
            << "replayed" << after.inflatedBytes - before.inflatedBytes << "source"
            << after.sourceBytes - before.sourceBytes << "managed peak" << after.peakMemoryBytes;
    QVERIFY(after.inflatedBytes - before.inflatedBytes < expanded / 8);
    QCOMPARE(after.coldRewinds, before.coldRewinds);
    QVERIFY(after.checkpoints <= 16);
    QVERIFY(after.peakMemoryBytes <= 8 * 1024 * 1024);
}

void Test::resourceBounds_data()
{
    QTest::addColumn<unsigned>("checkpoints");
    QTest::addColumn<quint64>("memoryBytes");
    QTest::addColumn<int>("status");
    QTest::newRow("no-checkpoints") << 0U << quint64(8 * 1024 * 1024) << int(INPUT_OK);
    QTest::newRow("excess-checkpoints") << 17U << quint64(8 * 1024 * 1024) << int(INPUT_RANGE);
    QTest::newRow("insufficient-memory") << 16U << quint64(128 * 1024) << int(INPUT_RANGE);
}

void Test::resourceBounds()
{
    QFETCH(unsigned, checkpoints);
    QFETCH(quint64, memoryBytes);
    QFETCH(int, status);
    const auto             plain = content();
    Source                 source{wrapped(plain)};
    QSocFstWrapper::Limits limits;
    limits.checkpointCount = checkpoints;
    limits.memoryBytes     = memoryBytes;
    QSocFstWrapper wrapper(source.input(), limits);
    QCOMPARE(wrapper.status(), status);
    QCOMPARE(wrapper.finish(), status);
    QCOMPARE(wrapper.pin(0), status);
    auto       input = wrapper.input();
    QByteArray result(32, '\0');
    uint32_t   received = 0;
    QCOMPARE(input.read_at(input.user, 0, result.data(), result.size(), &received), status);
    if (status == INPUT_OK) {
        QCOMPARE(received, uint32_t(result.size()));
        QCOMPARE(result, plain.left(result.size()));
    } else {
        QCOMPARE(received, uint32_t(0));
    }
    const auto stats = wrapper.statistics();
    QVERIFY(stats.checkpoints <= 16);
    QVERIFY(stats.peakMemoryBytes <= limits.memoryBytes);
}

void Test::corruptCrc()
{
    Source source{wrapped(content())};
    source.bytes[source.bytes.size() - 8] ^= 1;
    QSocFstWrapper wrapper(source.input());
    QCOMPARE(wrapper.finish(), int(INPUT_MALFORMED));
    QVERIFY(!wrapper.statistics().validated);
}

void Test::wrongSize()
{
    Source source{wrapped(content())};
    source.bytes[16] ^= 1;
    QSocFstWrapper wrapper(source.input());
    QCOMPARE(wrapper.finish(), int(INPUT_MALFORMED));
    QVERIFY(!wrapper.statistics().validated);
}

void Test::cancelled()
{
    Source         source{wrapped(content())};
    QSocFstWrapper wrapper(source.input());
    source.cancelled = true;
    QCOMPARE(wrapper.finish(), int(INPUT_CANCELLED));
    QVERIFY(!wrapper.statistics().validated);
}

void Test::sourceFailure()
{
    Source source{wrapped(content())};
    source.shortRead = 37;
    QSocFstWrapper wrapper(source.input());
    source.failAfter = source.reads + 2;
    QCOMPARE(wrapper.finish(), int(INPUT_IO_ERROR));
    QVERIFY(!wrapper.statistics().validated);
}
} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocfstwrapper.moc"
