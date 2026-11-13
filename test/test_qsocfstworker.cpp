// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/waveform/qsocfstclient.h"
#include "common/qsocresourceusage.h"
#include "fstapi.h"
#include "fstinput.h"
#include "qsoc_test.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QProcess>
#include <QRegularExpression>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>
#include <QtTest>

#include <array>
#include <cstdio>
#include <cstring>
#include <limits>

#ifdef Q_OS_WIN
#include <qt_windows.h>

#include <io.h>
#include <winioctl.h>
#endif

namespace {
QByteArray bits(unsigned value)
{
    return QByteArray::number(value & 15U, 2).rightJustified(4, '0');
}

bool makeFixture(
    const QString &path,
    int            pack,
    bool           embeddedHierarchy = true,
    bool           noisy             = false,
    unsigned       ticks             = 2048)
{
    const auto encoded = QFile::encodeName(path);
    auto       writer  = fstWriterCreate(encoded.constData(), embeddedHierarchy ? 1 : 0);
    if (!writer)
        return false;
    fstWriterSetPackType(writer, static_cast<fstWriterPackType>(pack));
    fstWriterSetTimescale(writer, -9);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "top", nullptr);
    const auto clock = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "clk", 0);
    fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "clk_alias", clock);
    const auto bus = fstWriterCreateVar(writer, FST_VT_VCD_REG, FST_VD_IMPLICIT, 4, "data [3:0]", 0);
    const auto noise
        = noisy
              ? fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 256, "noise [255:0]", 0)
              : fstHandle(0);
    fstWriterSetUpscope(writer);
    quint32    state = 17;
    QByteArray noiseValue(256, '0');
    for (unsigned time = 0; time < ticks; ++time) {
        fstWriterEmitTimeChange(writer, time);
        fstWriterEmitValueChange(writer, clock, time & 1U ? "1" : "0");
        const auto value = bits(time * 7U + 3U);
        fstWriterEmitValueChange(writer, bus, value.constData());
        if (noisy) {
            for (char &bit : noiseValue) {
                state = state * 1664525U + 1013904223U;
                bit   = state >> 31 ? '1' : '0';
            }
            fstWriterEmitValueChange(writer, noise, noiseValue.constData());
        }
        if (time && time % 512 == 0)
            fstWriterFlushContext(writer);
    }
    return fstWriterClose2(writer) == 0;
}

bool makeSharedTickFixture(const QString &path, bool finalShared, bool embedded)
{
    const auto encoded = QFile::encodeName(path);
    auto      *writer  = fstWriterCreate(encoded.constData(), embedded ? 1 : 0);
    if (!writer)
        return false;
    fstWriterSetTimescale(writer, -9);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "top", nullptr);
    const auto clock = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "clk", 0);
    fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "clk_alias", clock);
    const auto bus = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 4, "bus [3:0]", 0);
    fstWriterSetUpscope(writer);
    const auto emitValues = [&](quint64 time, const char *bit, const char *value) {
        fstWriterEmitTimeChange(writer, time);
        fstWriterEmitValueChange(writer, clock, bit);
        fstWriterEmitValueChange(writer, bus, value);
    };
    emitValues(0, "0", "0000");
    emitValues(1, "1", "0001");
    emitValues(10, "0", "0010");
    fstWriterFlushContext(writer);
    emitValues(10, "1", "0011");
    emitValues(10, "0", "0100");
    fstWriterFlushContext(writer);
    emitValues(10, "1", "0101");
    emitValues(10, "0", "0110");
    fstWriterFlushContext(writer);
    emitValues(10, "1", "0111");
    if (!finalShared) {
        emitValues(11, "0", "1000");
        emitValues(20, "1", "1001");
    }
    return fstWriterClose2(writer) == 0;
}

bool wrapFixture(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadWrite))
        return false;
    const auto plain = file.readAll();
    z_stream   stream{};
    if (deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY) != Z_OK)
        return false;
    QByteArray compressed(deflateBound(&stream, plain.size()), '\0');
    stream.next_in   = reinterpret_cast<Bytef *>(const_cast<char *>(plain.constData()));
    stream.avail_in  = plain.size();
    stream.next_out  = reinterpret_cast<Bytef *>(compressed.data());
    stream.avail_out = compressed.size();
    const int result = deflate(&stream, Z_FINISH);
    compressed.resize(stream.total_out);
    deflateEnd(&stream);
    if (result != Z_STREAM_END)
        return false;
    QByteArray output(1, char(FST_BL_ZWRAPPER));
    for (quint64 value : {quint64(16 + compressed.size()), quint64(plain.size())}) {
        for (int shift = 56; shift >= 0; shift -= 8)
            output.append(char(value >> shift));
    }
    output += compressed;
    return file.resize(0) && file.seek(0) && file.write(output) == output.size();
}

class Test : public QObject
{
    Q_OBJECT
private slots:
    void nativeOracle_data();
    void nativeOracle();
    void smallSourceBudget_data();
    void smallSourceBudget();
    void oversizedSourceReply();
    void wrappedSourceRanges();
    void metadataReadBudget();
    void callbackSectionBounds_data();
    void callbackSectionBounds();
    void pauseResume();
    void cancelledPause();
    void malformedTraversal();
    void malformedCompressedTime();
    void malformedHierarchy();
    void valuesAfterEnd();
    void sameTickChanges();
    void realValues_data();
    void realValues();
    void workerEnvironment();
    void wrapperPreparationPause();
    void wrapperPreparationPause_data();
    void corruptOuterWrapper();
    void keepaliveDoesNotAdvance();
    void companionFailures_data();
    void companionFailures();
    void companionPause_data();
    void companionPause();
    void filenameHierarchyCompatibility();
    void embeddedHierarchyPreferred();
    void sparseFileOffsetsAboveFourGiB();
    void completedWorkersReclaimIdle();
    void sectionDirectoryCompactsWithoutPrefixReads();
    void pausedWorkersKeepCapacity();
    void idleReclaimRunsOnOwnerThread();
    void finishedOwnerThreadRetiresWorkers();
    void sharedTickAcrossSections_data();
    void sharedTickAcrossSections();
};

QSocFstClient::ReadAt source(QFile &file, int shortRead = 0)
{
    return [&file, shortRead](quint64 offset, quint32 count, QString *error) {
        if (!file.seek(static_cast<qint64>(offset))) {
            *error = QStringLiteral("Fixture seek failed");
            return QByteArray{};
        }
        auto bytes = file.read(shortRead ? qMin<int>(count, shortRead) : count);
        if (file.error() != QFileDevice::NoError)
            *error = QStringLiteral("Fixture read failed");
        return bytes;
    };
}

bool select(QSocFstClient &client, quint64 start, quint64 end, QString *error)
{
    return client.select(
        {{"handles", QJsonArray{1, 2}},
         {"start", QString::number(start)},
         {"end", QString::number(end)}},
        error);
}

void Test::nativeOracle_data()
{
    QTest::addColumn<int>("pack");
    QTest::addColumn<int>("shortRead");
    QTest::addColumn<bool>("outerWrapper");
    QTest::addColumn<bool>("externalHierarchy");
    QTest::newRow("gzip-zlib") << int(FST_WR_PT_ZLIB) << 0 << false << false;
    QTest::newRow("gzip-fastlz-short-read") << int(FST_WR_PT_FASTLZ) << 37 << false << false;
    QTest::newRow("lz4") << int(FST_WR_PT_LZ4) << 0 << false << false;
    QTest::newRow("wrapper-zlib") << int(FST_WR_PT_ZLIB) << 0 << true << false;
    QTest::newRow("wrapper-one-byte-source") << int(FST_WR_PT_ZLIB) << 1 << true << false;
    QTest::newRow("wrapper-seven-byte-source") << int(FST_WR_PT_ZLIB) << 7 << true << false;
    QTest::newRow("wrapper-fastlz-short-read") << int(FST_WR_PT_FASTLZ) << 37 << true << false;
    QTest::newRow("wrapper-lz4") << int(FST_WR_PT_LZ4) << 0 << true << false;
    QTest::newRow("companion-zlib") << int(FST_WR_PT_ZLIB) << 0 << false << true;
    QTest::newRow("companion-fastlz-short-read") << int(FST_WR_PT_FASTLZ) << 37 << false << true;
    QTest::newRow("companion-lz4") << int(FST_WR_PT_LZ4) << 0 << false << true;
    QTest::newRow("companion-wrapper") << int(FST_WR_PT_ZLIB) << 0 << true << true;
}

void Test::nativeOracle()
{
    QFETCH(int, pack);
    QFETCH(int, shortRead);
    QFETCH(bool, outerWrapper);
    QFETCH(bool, externalHierarchy);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("fixture.fst");
    QVERIFY(makeFixture(path, pack, !externalHierarchy));
    if (outerWrapper)
        QVERIFY(wrapFixture(path));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QFile                                   hierarchy(path + ".hier");
    std::optional<QSocFstClient::Companion> companion;
    if (externalHierarchy) {
        QVERIFY(hierarchy.open(QIODevice::ReadOnly));
        companion
            = QSocFstClient::Companion{quint64(hierarchy.size()), source(hierarchy, shortRead)};
    }
    auto          cancelled = std::make_shared<std::atomic_bool>(false);
    QSocFstClient client;
    QString       error;
    QVERIFY2(
        client.start(
            file.size(),
            source(file, shortRead),
            cancelled,
            &error,
            QSOC_WAVE_WORKER_PATH,
            std::move(companion)),
        qPrintable(error));
    client.beginQuantum(30000);
    QHash<quint64, QHash<int, QString>> events;
    QHash<int, QString>                 initial;
    QStringList                         names;
    bool                                complete = false;
    while (!complete) {
        QJsonObject message;
        QCOMPARE(client.next(&message, &error), QSocFstClient::Next::Message);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto type = message.value("type").toString();
        if (type == "signal")
            names.append(message.value("name").toString());
        else if (type == "hierarchy_end")
            QVERIFY2(select(client, 777, 790, &error), qPrintable(error));
        else if (type == "initial")
            initial.insert(message.value("handle").toInt(), message.value("value").toString());
        else if (type == "event") {
            const auto time = message.value("time").toString().toULongLong();
            if (time >= 777)
                events[time][message.value("handle").toInt()] = message.value("value").toString();
        } else if (type == "complete")
            complete = true;
    }
    QCOMPARE(names, (QStringList{"top.clk", "top.clk_alias", "top.data [3:0]"}));
    QCOMPARE(initial.value(1), "0");
    QCOMPARE(initial.value(2), QString::fromLatin1(bits(776 * 7 + 3)));
    QCOMPARE(events.size(), 14);
    for (quint64 time = 777; time <= 790; ++time) {
        QCOMPARE(events.value(time).value(1), time & 1U ? "1" : "0");
        QCOMPARE(events.value(time).value(2), QString::fromLatin1(bits(time * 7 + 3)));
    }
    const auto pid = client.processId();
    QVERIFY(client.reusable());
    cancelled->store(true);
    auto newCancellation = std::make_shared<std::atomic_bool>(false);
    client.beginQuantum(30000);
    QVERIFY(client.select(
        {{"handles", QJsonArray{1, 2}}, {"start", "780"}, {"end", "793"}}, &error, newCancellation));
    events.clear();
    initial.clear();
    for (;;) {
        QJsonObject message;
        QCOMPARE(client.next(&message, &error), QSocFstClient::Next::Message);
        const auto type = message.value("type").toString();
        QVERIFY(type != "header" && type != "signal" && type != "hierarchy_end");
        if (type == "initial")
            initial.insert(message.value("handle").toInt(), message.value("value").toString());
        else if (type == "event") {
            const auto time = message.value("time").toString().toULongLong();
            if (time >= 780)
                events[time][message.value("handle").toInt()] = message.value("value").toString();
        } else if (type == "complete") {
            QVERIFY(message.value("cache").toObject().value("reader_reused").toBool());
            if (outerWrapper)
                QCOMPARE(
                    message.value("cache").toObject().value("wrapper_cold_rewinds").toString(), "0");
            break;
        }
    }
    QCOMPARE(client.processId(), pid);
    QCOMPARE(initial.value(1), "1");
    QCOMPARE(initial.value(2), QString::fromLatin1(bits(779 * 7 + 3)));
    QCOMPARE(events.size(), 14);
    for (quint64 time = 780; time <= 793; ++time) {
        QCOMPARE(events.value(time).value(1), time & 1U ? "1" : "0");
        QCOMPARE(events.value(time).value(2), QString::fromLatin1(bits(time * 7 + 3)));
    }
}

void Test::smallSourceBudget_data()
{
    QTest::addColumn<bool>("outerWrapper");
    QTest::addColumn<bool>("externalHierarchy");
    QTest::newRow("native") << false << false;
    QTest::newRow("wrapped") << true << false;
    QTest::newRow("hierarchy") << false << true;
    QTest::newRow("wrapped-hierarchy") << true << true;
}

void Test::smallSourceBudget()
{
    QFETCH(bool, outerWrapper);
    QFETCH(bool, externalHierarchy);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("budget.fst");
    QVERIFY(makeFixture(path, FST_WR_PT_ZLIB, !externalHierarchy, true));
    if (outerWrapper)
        QVERIFY(wrapFixture(path));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(file.size() > 4096);
    quint64    sourceBytes    = 0;
    quint64    mainReads      = 0;
    quint64    hierarchyReads = 0;
    quint32    maximumIssued  = 0;
    const auto mainReader     = source(file);
    const auto readMain       = [&](quint64 offset, quint32 count, QString *error) {
        maximumIssued    = qMax(maximumIssued, count);
        const auto bytes = mainReader(offset, count, error);
        sourceBytes += bytes.size();
        ++mainReads;
        return bytes;
    };
    QFile                                   hierarchy(path + ".hier");
    std::optional<QSocFstClient::Companion> companion;
    if (externalHierarchy) {
        QVERIFY(hierarchy.open(QIODevice::ReadOnly));
        const auto hierarchyReader = source(hierarchy);
        companion                  = QSocFstClient::Companion{
            quint64(hierarchy.size()),
            [&, hierarchyReader](quint64 offset, quint32 count, QString *error) {
                maximumIssued    = qMax(maximumIssued, count);
                const auto bytes = hierarchyReader(offset, count, error);
                sourceBytes += bytes.size();
                ++hierarchyReads;
                return bytes;
            }};
    }
    auto          cancelled = std::make_shared<std::atomic_bool>(false);
    QSocFstClient client;
    QString       error;
    QVERIFY2(
        client.start(
            file.size(), readMain, cancelled, &error, QSOC_WAVE_WORKER_PATH, std::move(companion)),
        qPrintable(error));
    const auto pid = client.processId();
    QVERIFY(pid > 0);
    QCOMPARE(sourceBytes, quint64(0));
    client.beginQuantum(5000, 0);
    QJsonObject pausedMessage;
    QCOMPARE(client.next(&pausedMessage, &error), QSocFstClient::Next::Paused);
    QCOMPARE(client.limitReason(), "scan_bytes");
    QCOMPARE(sourceBytes, quint64(0));
    QHash<quint64, QHash<int, QString>> events;
    QHash<int, QString>                 initial;
    QStringList                         names;
    bool                                complete   = false;
    int                                 pauses     = 0;
    int                                 eventCount = 0;
    for (int page = 0; page < 512 && !complete; ++page) {
        const auto before = sourceBytes;
        client.beginQuantum(5000, 4096);
        bool paused = false;
        for (int messages = 0; messages < 10000 && !complete; ++messages) {
            QJsonObject message;
            const auto  next = client.next(&message, &error);
            QCOMPARE(client.processId(), pid);
            QVERIFY2(next != QSocFstClient::Next::Failed, qPrintable(error));
            QVERIFY2(error.isEmpty(), qPrintable(error));
            // next() invokes the source callback, which updates sourceBytes.
            // cppcheck-suppress duplicateExpression
            QCOMPARE(client.scanBytes(), sourceBytes - before);
            QVERIFY(client.scanBytes() <= 4096);
            QVERIFY(maximumIssued <= 4096);
            if (next == QSocFstClient::Next::Paused) {
                if (client.limitReason() == "scan_bytes")
                    // cppcheck-suppress knownConditionTrueFalse
                    QVERIFY2(sourceBytes > before, "Byte quantum made no source progress");
                paused = true;
                ++pauses;
                break;
            }
            QCOMPARE(next, QSocFstClient::Next::Message);
            const auto type = message.value("type").toString();
            if (type == "signal") {
                names.append(message.value("name").toString());
            } else if (type == "hierarchy_end") {
                QVERIFY2(select(client, 777, 790, &error), qPrintable(error));
            } else if (type == "initial") {
                const int handle = message.value("handle").toInt();
                QVERIFY(!initial.contains(handle));
                initial.insert(handle, message.value("value").toString());
            } else if (type == "event") {
                const auto time   = message.value("time").toString().toULongLong();
                const int  handle = message.value("handle").toInt();
                QVERIFY(time <= 790);
                if (time < 777)
                    continue;
                QVERIFY(handle == 1 || handle == 2);
                QVERIFY(!events[time].contains(handle));
                events[time][handle] = message.value("value").toString();
                ++eventCount;
            } else if (type == "complete") {
                complete = true;
            }
        }
        QVERIFY2(paused || complete, "Worker message bound exceeded");
    }
    QVERIFY2(complete, "Source budget did not complete within the page bound");
    QVERIFY(pauses > 0);
    QVERIFY(mainReads > 0);
    QCOMPARE(hierarchyReads > 0, externalHierarchy);
    QCOMPARE(names, (QStringList{"top.clk", "top.clk_alias", "top.data [3:0]", "top.noise [255:0]"}));
    QCOMPARE(initial.size(), 2);
    QCOMPARE(initial.value(1), "0");
    QCOMPARE(initial.value(2), QString::fromLatin1(bits(776 * 7 + 3)));
    QCOMPARE(events.size(), 14);
    QCOMPARE(eventCount, 28);
    for (quint64 time = 777; time <= 790; ++time) {
        QCOMPARE(events.value(time).size(), 2);
        QCOMPARE(events.value(time).value(1), time & 1U ? "1" : "0");
        QCOMPARE(events.value(time).value(2), QString::fromLatin1(bits(time * 7 + 3)));
    }
    QCOMPARE(client.processId(), pid);
    QVERIFY(client.reusable());
}

void Test::oversizedSourceReply()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("oversized.fst");
    QVERIFY(makeFixture(path, FST_WR_PT_ZLIB, true, true));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(file.size() > 4096);
    const auto reader    = source(file);
    quint32    issued    = 0;
    const auto malicious = [&](quint64 offset, quint32 count, QString *error) {
        if (count > 1) {
            issued = count;
            return QByteArray(count + 1, '\0');
        }
        return reader(offset, count, error);
    };
    auto          cancelled = std::make_shared<std::atomic_bool>(false);
    QSocFstClient client;
    QString       error;
    QVERIFY2(
        client.start(file.size(), malicious, cancelled, &error, QSOC_WAVE_WORKER_PATH),
        qPrintable(error));
    client.beginQuantum(5000, 4096);
    auto next = QSocFstClient::Next::Message;
    for (int messages = 0; messages < 32 && next != QSocFstClient::Next::Failed; ++messages) {
        QJsonObject message;
        next = client.next(&message, &error);
        QVERIFY(next != QSocFstClient::Next::Paused);
    }
    QCOMPARE(next, QSocFstClient::Next::Failed);
    QCOMPARE(issued, quint32(4095));
    QVERIFY(quint64(issued) + 1 <= qMin<quint64>(65536, file.size()));
    QCOMPARE(client.scanBytes(), quint64(1));
    QVERIFY2(error.contains("invalid short read"), qPrintable(error));
}

void Test::wrappedSourceRanges()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("ranges.fst");
    QVERIFY(makeFixture(path, FST_WR_PT_ZLIB, true, true, 65536));
    QVERIFY(wrapFixture(path));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(file.size() > 128 * 1024);
    const auto reader        = source(file);
    quint32    maximumIssued = 0;
    const auto readAt        = [&](quint64 offset, quint32 count, QString *error) {
        maximumIssued = qMax(maximumIssued, count);
        return reader(offset, count, error);
    };
    auto          cancelled = std::make_shared<std::atomic_bool>(false);
    QSocFstClient client;
    QString       error;
    QVERIFY2(
        client.start(file.size(), readAt, cancelled, &error, QSOC_WAVE_WORKER_PATH),
        qPrintable(error));
    client.beginQuantum(30000);
    bool ready = false;
    for (int messages = 0; messages < 1000 && !ready; ++messages) {
        QJsonObject message;
        QCOMPARE(client.next(&message, &error), QSocFstClient::Next::Message);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        ready = message.value("type") == "hierarchy_end";
    }
    QVERIFY(ready);
    QCOMPARE(maximumIssued, quint32(128 * 1024));
}

void Test::metadataReadBudget()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("metadata.fst");
    QVERIFY(makeFixture(path, FST_WR_PT_ZLIB, true, true, 65536));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(file.size() > 1024 * 1024);
    const auto reader    = source(file);
    quint64    bytesRead = 0;
    const auto readAt    = [&](quint64 offset, quint32 count, QString *error) {
        const auto bytes = reader(offset, count, error);
        bytesRead += quint64(bytes.size());
        return bytes;
    };
    auto          cancelled = std::make_shared<std::atomic_bool>(false);
    QSocFstClient client;
    QString       error;
    QVERIFY2(
        client.start(file.size(), readAt, cancelled, &error, QSOC_WAVE_WORKER_PATH),
        qPrintable(error));
    client.beginQuantum(30000);
    QStringList names;
    for (;;) {
        QJsonObject message;
        QCOMPARE(client.next(&message, &error), QSocFstClient::Next::Message);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        if (message.value("type") == "signal")
            names.append(message.value("name").toString());
        if (message.value("type") == "hierarchy_end")
            break;
    }
    QCOMPARE(names, (QStringList{"top.clk", "top.clk_alias", "top.data [3:0]", "top.noise [255:0]"}));
    qInfo() << "metadata source bytes" << bytesRead << "encoded bytes" << file.size();
    QVERIFY(bytesRead < quint64(file.size()) / 3);
    client.cancel();
}

void Test::callbackSectionBounds_data()
{
    QTest::addColumn<QString>("mutation");
    QTest::addColumn<int>("expectedStatus");
    QTest::newRow("valid") << QString("valid") << int(INPUT_OK);
    QTest::newRow("unknown-tail") << QString("unknown-tail") << int(INPUT_OK);
    QTest::newRow("incomplete-header") << QString("incomplete-header") << int(INPUT_OK);
    QTest::newRow("unknown-first") << QString("unknown-first") << int(INPUT_MALFORMED);
    QTest::newRow("zero-first") << QString("zero-first") << int(INPUT_MALFORMED);
    QTest::newRow("short-first") << QString("short-first") << int(INPUT_MALFORMED);
    QTest::newRow("oversize-first") << QString("oversize-first") << int(INPUT_MALFORMED);
    QTest::newRow("overflow-first") << QString("overflow-first") << int(INPUT_MALFORMED);
    QTest::newRow("zero-tail") << QString("zero-tail") << int(INPUT_MALFORMED);
    QTest::newRow("oversize-tail") << QString("oversize-tail") << int(INPUT_MALFORMED);
    QTest::newRow("truncated-payload") << QString("truncated-payload") << int(INPUT_MALFORMED);
    QTest::newRow("incomplete-zero-tail")
        << QString("incomplete-zero-tail") << int(INPUT_MALFORMED);
    QTest::newRow("incomplete-short-tail") << QString("incomplete-short-tail") << int(INPUT_EOF);
    QTest::newRow("wrapper") << QString("wrapper") << int(INPUT_UNSUPPORTED);
    QTest::newRow("wrapper-zero") << QString("wrapper-zero") << int(INPUT_MALFORMED);
    for (int bytes = 1; bytes < 9; ++bytes) {
        const auto name = QByteArray("tail-") + QByteArray::number(bytes);
        QTest::newRow(name.constData()) << QString::fromLatin1(name) << int(INPUT_EOF);
    }
}

void Test::callbackSectionBounds()
{
    QFETCH(QString, mutation);
    QFETCH(int, expectedStatus);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("bounds.fst");
    QVERIFY(makeFixture(path, FST_WR_PT_ZLIB));
    if (mutation.startsWith("wrapper"))
        QVERIFY(wrapFixture(path));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto bytes = file.readAll();
    file.close();
    const auto length = [&](qsizetype offset, quint64 value) {
        for (int index = 0; index < 8; ++index)
            bytes[offset + 1 + index] = char(value >> (56 - index * 8));
    };
    if (mutation.startsWith("incomplete"))
        bytes.replace(17, 8, QByteArray(8, '\0'));
    if (mutation == "unknown-first")
        bytes[0] = char(127);
    else if (mutation == "zero-first" || mutation == "wrapper-zero")
        length(0, 0);
    else if (mutation == "short-first")
        length(0, 7);
    else if (mutation == "oversize-first")
        length(0, bytes.size());
    else if (mutation == "overflow-first")
        length(0, std::numeric_limits<quint64>::max());
    else if (mutation == "truncated-payload")
        bytes.chop(1);
    else if (mutation.endsWith("tail") || mutation.startsWith("tail-")) {
        const auto offset = bytes.size();
        bytes.append(bytes.left(9));
        bytes[offset] = char(127);
        length(offset, mutation.endsWith("zero-tail") ? 0 : mutation == "oversize-tail" ? 9 : 8);
        if (mutation.startsWith("tail-"))
            bytes.chop(9 - mutation.mid(5).toInt());
        else if (mutation == "incomplete-short-tail")
            bytes.chop(1);
    }
    QSocFstInput input{};
    input.abi_version = 1;
    input.struct_size = sizeof(input);
    input.user        = &bytes;
    input.read_at =
        [](void *user, uint64_t offset, void *destination, uint32_t count, uint32_t *received) {
            const auto &source = *static_cast<QByteArray *>(user);
            const auto  chunk  = source.mid(offset, count);
            std::memcpy(destination, chunk.constData(), chunk.size());
            *received = static_cast<uint32_t>(chunk.size());
            return int(INPUT_OK);
        };
    input.size = [](void *user, uint64_t *size) {
        *size = static_cast<QByteArray *>(user)->size();
        return int(INPUT_OK);
    };
    input.temporary_file = [](void *, uint64_t size) {
        return size <= QSocFstClient::temporaryLimit ? std::tmpfile() : nullptr;
    };
    input.hierarchy_limit = QSocFstClient::temporaryLimit;
    int        status     = INPUT_OK;
    auto      *reader     = fstReaderOpenWithInput(&input, &status);
    const auto close      = qScopeGuard([&] { fstReaderClose(reader); });
    QCOMPARE(status, expectedStatus);
    QCOMPARE(reader != nullptr, expectedStatus == INPUT_OK);
}

void Test::sectionDirectoryCompactsWithoutPrefixReads()
{
    QTemporaryDir directory;
    const auto    path    = directory.filePath("sections.fst");
    const auto    encoded = QFile::encodeName(path);
    auto         *writer  = fstWriterCreate(encoded.constData(), 1);
    QVERIFY(writer);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "top", nullptr);
    const auto clock = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "clk", 0);
    fstWriterSetUpscope(writer);
    for (quint64 tick = 0; tick < 100000; ++tick) {
        fstWriterEmitTimeChange(writer, tick);
        fstWriterEmitValueChange(writer, clock, tick & 1U ? "1" : "0");
        fstWriterFlushContext(writer);
    }
    QCOMPARE(fstWriterClose2(writer), 0);
    struct Input
    {
        QFile   file;
        quint64 minimum = std::numeric_limits<quint64>::max();
    } input;
    input.file.setFileName(path);
    QVERIFY(input.file.open(QIODevice::ReadOnly));
    QSocFstInput callbacks{};
    callbacks.abi_version = 1;
    callbacks.struct_size = sizeof(callbacks);
    callbacks.user        = &input;
    callbacks.read_at =
        [](void *user, uint64_t offset, void *data, uint32_t count, uint32_t *received) {
            auto &source   = *static_cast<Input *>(user);
            source.minimum = qMin<quint64>(source.minimum, offset);
            if (!source.file.seek(offset))
                return int(INPUT_IO_ERROR);
            const auto bytes = source.file.read(static_cast<char *>(data), count);
            if (bytes < 0)
                return int(INPUT_IO_ERROR);
            *received = static_cast<uint32_t>(bytes);
            return int(INPUT_OK);
        };
    callbacks.size = [](void *user, uint64_t *size) {
        *size = static_cast<Input *>(user)->file.size();
        return int(INPUT_OK);
    };
    callbacks.temporary_file = [](void *, uint64_t size) {
        return size <= QSocFstClient::temporaryLimit ? std::tmpfile() : nullptr;
    };
    callbacks.hierarchy_limit = QSocFstClient::temporaryLimit;
    int   status              = 0;
    auto *reader              = fstReaderOpenWithInput(&callbacks, &status);
    QVERIFY(reader);
    const auto close = qScopeGuard([&] { fstReaderClose(reader); });
    QCOMPARE(status, int(INPUT_OK));
    while (fstReaderIterateHier(reader)) {
    }
    QVERIFY(fstReaderGetValueChangeSectionCount(reader) > 44000);
    fstReaderSetFacProcessMaskAll(reader);
    struct Events
    {
        quint64                    start = 0;
        QHash<quint64, QByteArray> values;
    } events;
    for (quint64 start : {90000ULL, 90005ULL, 10000ULL, 50000ULL}) {
        events.start = start;
        events.values.clear();
        input.minimum = std::numeric_limits<quint64>::max();
        QVERIFY(fstReaderInputSectionOffset(reader, start - 1) > 0);
        char value[128]{};
        QVERIFY(fstReaderGetValueFromHandleAtTime(reader, start - 1, 1, value));
        QCOMPARE(value[0], (start - 1) & 1U ? '1' : '0');
        fstReaderSetLimitTimeRange(reader, start, start + 19);
        QVERIFY(fstReaderIterBlocks(
            reader,
            [](void *user, uint64_t time, fstHandle handle, const unsigned char *value) {
                auto &received = *static_cast<Events *>(user);
                if (handle == 1 && time >= received.start && time <= received.start + 19)
                    received.values.insert(time, QByteArray(reinterpret_cast<const char *>(value)));
            },
            &events,
            nullptr));
        QCOMPARE(fstReaderInputStatus(reader), int(INPUT_OK));
        QCOMPARE(events.values.size(), 20);
        for (quint64 tick = start; tick < start + 20; ++tick)
            QCOMPARE(events.values.value(tick), tick & 1U ? QByteArray("1") : QByteArray("0"));
        QVERIFY(input.minimum > 0);
    }
    fstReaderClose(reader);
    reader = nullptr;
    input.file.close();
    QVERIFY(input.file.open(QIODevice::ReadWrite));
    quint64 offset   = 0;
    int     sections = 0;
    while (offset + 25 <= quint64(input.file.size())) {
        QVERIFY(input.file.seek(offset));
        const auto header = input.file.read(9);
        QCOMPARE(header.size(), 9);
        quint64 length = 0;
        for (int index = 1; index < 9; ++index)
            length = (length << 8) | quint8(header[index]);
        const auto kind = quint8(header[0]);
        if ((kind == FST_BL_VCDATA || kind == FST_BL_VCDATA_DYN_ALIAS
             || kind == FST_BL_VCDATA_DYN_ALIAS2)
            && ++sections == 2) {
            QVERIFY(input.file.seek(offset + 17));
            QCOMPARE(input.file.write(QByteArray(8, '\0')), 8);
            break;
        }
        QVERIFY(length && length < quint64(input.file.size()));
        offset += 1 + length;
    }
    QCOMPARE(sections, 2);
    input.file.close();
    QVERIFY(input.file.open(QIODevice::ReadOnly));
    reader = fstReaderOpenWithInput(&callbacks, &status);
    QVERIFY(reader);
    QCOMPARE(status, int(INPUT_OK));
    QCOMPARE(fstReaderInputSectionOffset(reader, 90000), uint64_t(0));
    fstReaderSetFacProcessMaskAll(reader);
    events.start = 90000;
    events.values.clear();
    fstReaderSetLimitTimeRange(reader, 90000, 90019);
    QVERIFY(fstReaderIterBlocks(
        reader,
        [](void *user, uint64_t time, fstHandle handle, const unsigned char *value) {
            auto &received = *static_cast<Events *>(user);
            if (handle == 1 && time >= received.start && time <= received.start + 19)
                received.values.insert(time, QByteArray(reinterpret_cast<const char *>(value)));
        },
        &events,
        nullptr));
    QCOMPARE(fstReaderInputStatus(reader), int(INPUT_OK));
    QCOMPARE(events.values.size(), 20);
    for (quint64 tick = 90000; tick < 90020; ++tick)
        QCOMPARE(events.values.value(tick), tick & 1U ? QByteArray("1") : QByteArray("0"));
}

void Test::completedWorkersReclaimIdle()
{
    QTemporaryDir directory;
    const auto    path = directory.filePath("fixture.fst");
    QVERIFY(makeFixture(path, FST_WR_PT_ZLIB));
    std::array<QFile, 3>              files;
    std::array<QSocFstClient, 3>      clients;
    std::array<std::weak_ptr<int>, 3> lifetimes;
    std::array<int, 3>                evictions{};
    QString                           privateDirectory;
    QString                           error;
    for (unsigned index = 0; index < clients.size(); ++index) {
        files[index].setFileName(path);
        QVERIFY(files[index].open(QIODevice::ReadOnly));
        auto lifetime    = std::make_shared<int>(index);
        lifetimes[index] = lifetime;
        const auto read  = source(files[index]);
        clients[index].setEvictionHandler([&, index] { ++evictions[index]; });
        QVERIFY2(
            clients[index].start(
                files[index].size(),
                [read, lifetime](quint64 offset, quint32 count, QString *error) {
                    (void) lifetime;
                    return read(offset, count, error);
                },
                std::make_shared<std::atomic_bool>(false),
                &error,
                QSOC_WAVE_WORKER_PATH),
            qPrintable(error));
        lifetime.reset();
#ifdef Q_OS_LINUX
        if (index == 0)
            privateDirectory = QFileInfo(
                                   QStringLiteral("/proc/%1/cwd").arg(clients[index].processId()))
                                   .symLinkTarget();
#endif
        clients[index].beginQuantum(30000);
        for (;;) {
            QJsonObject message;
            QCOMPARE(clients[index].next(&message, &error), QSocFstClient::Next::Message);
            if (message.value("type") == "hierarchy_end")
                QVERIFY(select(clients[index], 777, 790, &error));
            if (message.value("type") == "complete")
                break;
        }
        QVERIFY(clients[index].reusable());
    }
    QCOMPARE(clients[0].processId(), 0);
    QVERIFY(!clients[0].reusable());
    QVERIFY(clients[1].reusable());
    QVERIFY(clients[2].reusable());
    QCOMPARE(evictions[0], 1);
    QCOMPARE(evictions[1], 0);
    QCOMPARE(evictions[2], 0);
    QVERIFY(lifetimes[0].expired());
    QVERIFY(!lifetimes[1].expired());
    QVERIFY(!lifetimes[2].expired());
#ifdef Q_OS_LINUX
    QVERIFY(!privateDirectory.isEmpty());
    QVERIFY(!QFileInfo::exists(privateDirectory));
#endif
}

void Test::idleReclaimRunsOnOwnerThread()
{
    QTemporaryDir directory;
    const auto    path = directory.filePath("threaded.fst");
    QVERIFY(makeFixture(path, FST_WR_PT_ZLIB));
    QThread thread;
    QObject context;
    auto   *originalThread = context.thread();
    context.moveToThread(&thread);
    thread.start();
    std::array<std::unique_ptr<QFile>, 2>         files;
    std::array<std::unique_ptr<QSocFstClient>, 2> clients;
    const auto                                    cleanup  = qScopeGuard([&] {
        QMetaObject::invokeMethod(
            &context,
            [&] {
                for (auto &client : clients)
                    client.reset();
                for (auto &file : files)
                    file.reset();
                context.moveToThread(originalThread);
            },
            Qt::BlockingQueuedConnection);
        thread.quit();
        thread.wait();
    });
    bool                                          prepared = true;
    QString                                       error;
    QMetaObject::invokeMethod(
        &context,
        [&] {
            for (unsigned index = 0; index < clients.size() && prepared; ++index) {
                files[index]   = std::make_unique<QFile>(path);
                clients[index] = std::make_unique<QSocFstClient>();
                prepared       = files[index]->open(QIODevice::ReadOnly)
                                 && clients[index]->start(
                                     files[index]->size(),
                                     source(*files[index]),
                                     std::make_shared<std::atomic_bool>(false),
                                     &error,
                                     QSOC_WAVE_WORKER_PATH);
                if (!prepared)
                    break;
                clients[index]->beginQuantum(30000);
                for (;;) {
                    QJsonObject message;
                    if (clients[index]->next(&message, &error) != QSocFstClient::Next::Message) {
                        prepared = false;
                        break;
                    }
                    if (message.value("type") == "hierarchy_end"
                        && !select(*clients[index], 777, 790, &error)) {
                        prepared = false;
                        break;
                    }
                    if (message.value("type") == "complete")
                        break;
                }
            }
        },
        Qt::BlockingQueuedConnection);
    QVERIFY2(prepared, qPrintable(error));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QSocFstClient client;
    QVERIFY2(
        client.start(
            file.size(),
            source(file),
            std::make_shared<std::atomic_bool>(false),
            &error,
            QSOC_WAVE_WORKER_PATH),
        qPrintable(error));
    bool reclaimed = false;
    QMetaObject::invokeMethod(
        &context,
        [&] {
            reclaimed = clients[0]->processId() == 0 && !clients[0]->reusable()
                        && clients[1]->reusable();
        },
        Qt::BlockingQueuedConnection);
    QVERIFY(reclaimed);
}

void Test::finishedOwnerThreadRetiresWorkers()
{
    QTest::failOnWarning(QRegularExpression(QStringLiteral(".*")));
    QTemporaryDir directory;
    const auto    path = directory.filePath("ended-owner.fst");
    QVERIFY(makeFixture(path, FST_WR_PT_ZLIB));
    QThread thread;
    QObject context;
    auto   *originalThread = context.thread();
    context.moveToThread(&thread);
    std::array<std::unique_ptr<QFile>, 2>            files;
    std::array<std::unique_ptr<QSocFstClient>, 2>    clients;
    std::array<std::shared_ptr<std::atomic_bool>, 2> cancelled;
    QObject::connect(
        &thread,
        &QThread::finished,
        &context,
        [&] {
            for (auto &file : files) {
                if (file) {
                    file->close();
                    file->moveToThread(originalThread);
                }
            }
            context.moveToThread(originalThread);
        },
        Qt::DirectConnection);
    thread.start();
    const auto cleanup  = qScopeGuard([&] {
        thread.quit();
        if (!thread.wait(15000))
            qFatal("FST owner thread cleanup timed out");
        for (auto &client : clients)
            client.reset();
        for (auto &file : files)
            file.reset();
    });
    bool       prepared = true;
    QString    error;
    QMetaObject::invokeMethod(
        &context,
        [&] {
            for (unsigned index = 0; index < clients.size() && prepared; ++index) {
                files[index]     = std::make_unique<QFile>(path);
                clients[index]   = std::make_unique<QSocFstClient>();
                cancelled[index] = std::make_shared<std::atomic_bool>(false);
                prepared         = clients[index]->onOwnerThread()
                                   && files[index]->open(QIODevice::ReadOnly)
                                   && clients[index]->start(
                                       files[index]->size(),
                                       source(*files[index]),
                                       cancelled[index],
                                       &error,
                                       QSOC_WAVE_WORKER_PATH);
                if (!prepared)
                    break;
                clients[index]->beginQuantum(30000);
                for (;;) {
                    QJsonObject message;
                    if (clients[index]->next(&message, &error) != QSocFstClient::Next::Message) {
                        prepared = false;
                        break;
                    }
                    if (message.value("type") == "hierarchy_end"
                        && !select(*clients[index], 777, 790, &error)) {
                        prepared = false;
                        break;
                    }
                    if (message.value("type") == "complete")
                        break;
                }
            }
        },
        Qt::BlockingQueuedConnection);
    QVERIFY2(prepared, qPrintable(error));
    thread.quit();
    QVERIFY(thread.wait(15000));
    for (unsigned index = 0; index < clients.size(); ++index) {
        QVERIFY(cancelled[index]->load());
        QVERIFY(clients[index]->onOwnerThread());
        QCOMPARE(clients[index]->processId(), 0);
        QVERIFY(!clients[index]->reusable());
    }
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QSocFstClient replacement;
    QVERIFY2(
        replacement.start(
            file.size(),
            source(file),
            std::make_shared<std::atomic_bool>(false),
            &error,
            QSOC_WAVE_WORKER_PATH),
        qPrintable(error));
    QVERIFY(replacement.processId() > 0);
}

void Test::pausedWorkersKeepCapacity()
{
    QTemporaryDir directory;
    const auto    path = directory.filePath("fixture.fst");
    QVERIFY(makeFixture(path, FST_WR_PT_ZLIB));
    std::array<QFile, 3>         files;
    std::array<QSocFstClient, 3> clients;
    QString                      error;
    for (unsigned index = 0; index < files.size(); ++index) {
        files[index].setFileName(path);
        QVERIFY(files[index].open(QIODevice::ReadOnly));
        const bool started = clients[index].start(
            files[index].size(),
            source(files[index]),
            std::make_shared<std::atomic_bool>(false),
            &error,
            QSOC_WAVE_WORKER_PATH);
        if (index == 2) {
            QVERIFY(!started);
            QVERIFY(error.contains("busy"));
            clients[0].cancel();
            QCOMPARE(clients[0].processId(), 0);
            error.clear();
            QVERIFY2(
                clients[index].start(
                    files[index].size(),
                    source(files[index]),
                    std::make_shared<std::atomic_bool>(false),
                    &error,
                    QSOC_WAVE_WORKER_PATH),
                qPrintable(error));
            QVERIFY(clients[index].processId() > 0);
            break;
        }
        QVERIFY2(started, qPrintable(error));
        clients[index].beginQuantum(1);
        QTest::qWait(2);
        QJsonObject message;
        QCOMPARE(clients[index].next(&message, &error), QSocFstClient::Next::Paused);
    }
}

void Test::pauseResume()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("fixture.fst");
    QVERIFY(makeFixture(path, FST_WR_PT_ZLIB));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto          cancelled = std::make_shared<std::atomic_bool>(false);
    QSocFstClient client;
    QString       error;
    QVERIFY2(
        client.start(file.size(), source(file), cancelled, &error, QSOC_WAVE_WORKER_PATH),
        qPrintable(error));
    const auto pid = client.processId();
    client.beginQuantum(30000);
    int  events = 0;
    bool paused = false;
    while (true) {
        QJsonObject message;
        const auto  next = client.next(&message, &error);
        if (next == QSocFstClient::Next::Paused) {
            QVERIFY(!paused);
            paused               = true;
            const auto cpuBefore = QSocResourceUsage::process(pid).value("cpu_time_ns");
            QTest::qWait(5050);
            QCOMPARE(client.processId(), pid);
            const auto cpuAfter = QSocResourceUsage::process(pid).value("cpu_time_ns");
            if (cpuBefore.isDouble() && cpuAfter.isDouble())
                QVERIFY(cpuAfter.toInteger() - cpuBefore.toInteger() <= 100000000);
            client.beginQuantum(30000);
            continue;
        }
        QCOMPARE(next, QSocFstClient::Next::Message);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto type = message.value("type").toString();
        if (type == "hierarchy_end")
            QVERIFY(select(client, 0, 2047, &error));
        else if (type == "event") {
            ++events;
            if (events == 3) {
                client.beginQuantum(1);
                QTest::qWait(2);
            }
        } else if (type == "complete")
            break;
    }
    QVERIFY(paused);
    QCOMPARE(events, 4096);
}

void Test::cancelledPause()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("fixture.fst");
    QVERIFY(makeFixture(path, FST_WR_PT_ZLIB));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto          cancelled = std::make_shared<std::atomic_bool>(false);
    QSocFstClient client;
    QString       error;
    QVERIFY(client.start(file.size(), source(file), cancelled, &error, QSOC_WAVE_WORKER_PATH));
    client.beginQuantum(1);
    QTest::qWait(2);
    QJsonObject message;
    QCOMPARE(client.next(&message, &error), QSocFstClient::Next::Paused);
    client.cancel();
    QCOMPARE(client.processId(), 0);
    QCOMPARE(client.next(&message, &error), QSocFstClient::Next::Failed);
    QVERIFY(error.contains("cancelled"));
}

quint64 uint64At(const QByteArray &bytes, qsizetype offset)
{
    quint64 value = 0;
    for (int index = 0; index < 8; ++index)
        value = (value << 8) | quint8(bytes.at(offset + index));
    return value;
}

QByteArray corruptFixture(const QString &path, bool timeTable)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    auto bytes = file.readAll();
    for (qsizetype block = 0; block + 9 <= bytes.size();) {
        const auto length = uint64At(bytes, block + 1);
        const auto type   = quint8(bytes.at(block));
        if (type == FST_BL_VCDATA || type == FST_BL_VCDATA_DYN_ALIAS
            || type == FST_BL_VCDATA_DYN_ALIAS2) {
            if (timeTable) {
                const auto compressed = uint64At(bytes, block + 1 + length - 16);
                if (compressed < 1 || compressed > length - 24)
                    return {};
                bytes[block + 1 + length - 24 - compressed] ^= 0x7f;
            } else {
                for (int index = 0; index < 8; ++index)
                    bytes[block + 25 + index] = char(0xff);
            }
            return bytes;
        }
        block += 1 + length;
    }
    return {};
}

void checkCorrupt(bool timeTable, bool outerWrapper = false)
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("fixture.fst");
    QVERIFY(makeFixture(path, FST_WR_PT_ZLIB));
    QByteArray bytes;
    if (outerWrapper) {
        QVERIFY(wrapFixture(path));
        QFile input(path);
        QVERIFY(input.open(QIODevice::ReadOnly));
        bytes = input.readAll();
        bytes[bytes.size() - 8] ^= 1;
    } else {
        bytes = corruptFixture(path, timeTable);
    }
    QVERIFY(!bytes.isEmpty());
    auto          cancelled = std::make_shared<std::atomic_bool>(false);
    QSocFstClient client;
    QString       error;
    auto          readAt = [&bytes](quint64 offset, quint32 count, QString *) {
        return bytes.mid(offset, count);
    };
    QVERIFY2(
        client.start(bytes.size(), readAt, cancelled, &error, QSOC_WAVE_WORKER_PATH),
        qPrintable(error));
    client.beginQuantum(30000);
    bool failed = false;
    while (!failed) {
        QJsonObject message;
        const auto  result = client.next(&message, &error);
        if (result == QSocFstClient::Next::Failed) {
            failed = true;
            break;
        }
        QCOMPARE(result, QSocFstClient::Next::Message);
        QVERIFY(message.value("type") != "complete");
        if (message.value("type") == "hierarchy_end")
            QVERIFY(select(client, 0, 2047, &error));
    }
    QVERIFY(failed);
    QVERIFY2(
        timeTable && !outerWrapper ? error.contains("255") : error.contains("status: 5"),
        qPrintable(error));
}

void Test::malformedTraversal()
{
    checkCorrupt(false);
}
void Test::malformedCompressedTime()
{
    checkCorrupt(true);
}

void Test::corruptOuterWrapper()
{
    checkCorrupt(false, true);
}

void Test::keepaliveDoesNotAdvance()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("heartbeat.fst");
    QVERIFY(makeFixture(path, FST_WR_PT_ZLIB));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto          cancelled = std::make_shared<std::atomic_bool>(false);
    QSocFstClient client;
    QString       error;
    QVERIFY(client.start(file.size(), source(file), cancelled, &error, QSOC_WAVE_WORKER_PATH));
    client.beginQuantum();
    QJsonObject message;
    QCOMPARE(client.next(&message, &error), QSocFstClient::Next::Message);
    QCOMPARE(message.value("type").toString(), "header");
    const auto pid = client.processId();
    for (int index = 0; index < 100; ++index)
        client.beginQuantum();
    QTest::qWait(50);
    QCOMPARE(client.processId(), pid);
    QCOMPARE(client.next(&message, &error), QSocFstClient::Next::Message);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(message.value("type").toString(), "signal");
}

void Test::sparseFileOffsetsAboveFourGiB()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto original = directory.filePath("original.fst");
    QVERIFY(makeFixture(original, FST_WR_PT_ZLIB));
    QFile base(original);
    QVERIFY(base.open(QIODevice::ReadOnly));
    const auto bytes = base.readAll();
    QVERIFY(bytes.size() > 9);
    quint64 section = 0;
    for (int index = 1; index < 9; ++index)
        section = (section << 8) | static_cast<unsigned char>(bytes.at(index));
    const auto headerSize = section + 1;
    QVERIFY(headerSize < quint64(bytes.size()));
    auto          header = bytes.left(static_cast<qsizetype>(headerSize));
    const quint64 gap    = (quint64(1) << 32) + 65536;
    for (int index = 0; index < 8; ++index)
        header[1 + index] = static_cast<char>((section + gap) >> (56 - index * 8));
    QFile sparse(directory.filePath("sparse-offset.fst"));
    QVERIFY(sparse.open(QIODevice::ReadWrite));
#ifdef Q_OS_WIN
    DWORD returned;
    if (!DeviceIoControl(
            reinterpret_cast<HANDLE>(_get_osfhandle(sparse.handle())),
            FSCTL_SET_SPARSE,
            nullptr,
            0,
            nullptr,
            0,
            &returned,
            nullptr))
        QSKIP("The fixture filesystem does not support sparse files");
#endif
    // The header extension tests offsets; the real event records remain small.
    QCOMPARE(sparse.write(header), qint64(header.size()));
    QVERIFY(sparse.seek(static_cast<qint64>(headerSize + gap)));
    const auto tail = bytes.mid(static_cast<qsizetype>(headerSize));
    QCOMPARE(sparse.write(tail), qint64(tail.size()));
    QVERIFY(sparse.flush());
    QCOMPARE(sparse.size(), qint64(bytes.size()) + qint64(gap));
    const auto actualRead    = source(sparse);
    quint64    largestOffset = 0;
    quint64    transferred   = 0;
    const auto readAt        = [&](quint64 offset, quint32 count, QString *error) {
        largestOffset = qMax(largestOffset, offset);
        auto data     = actualRead(offset, count, error);
        transferred += data.size();
        return data;
    };
    auto          cancelled = std::make_shared<std::atomic_bool>(false);
    QSocFstClient client;
    QString       error;
    QVERIFY2(
        client.start(sparse.size(), readAt, cancelled, &error, QSOC_WAVE_WORKER_PATH),
        qPrintable(error));
    client.beginQuantum(30000);
    QHash<quint64, QHash<int, QString>> events;
    QHash<int, QString>                 initial;
    while (true) {
        QJsonObject message;
        QCOMPARE(client.next(&message, &error), QSocFstClient::Next::Message);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto type = message.value("type").toString();
        if (type == "hierarchy_end")
            QVERIFY(select(client, 777, 790, &error));
        else if (type == "initial")
            initial[message.value("handle").toInt()] = message.value("value").toString();
        else if (type == "event") {
            const auto time = message.value("time").toString().toULongLong();
            if (time >= 777)
                events[time][message.value("handle").toInt()] = message.value("value").toString();
        } else if (type == "complete")
            break;
    }
    QCOMPARE(initial.value(1), "0");
    QCOMPARE(initial.value(2), QString::fromLatin1(bits(776 * 7 + 3)));
    QCOMPARE(events.size(), 14);
    for (quint64 time = 777; time <= 790; ++time) {
        QCOMPARE(events.value(time).value(1), time & 1U ? "1" : "0");
        QCOMPARE(events.value(time).value(2), QString::fromLatin1(bits(time * 7 + 3)));
    }
    QVERIFY(largestOffset > std::numeric_limits<quint32>::max());
    QVERIFY(transferred < 1024 * 1024);
}

void Test::filenameHierarchyCompatibility()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    for (bool embedded : {false, true}) {
        const auto path = directory.filePath(embedded ? "embedded.fst" : "external.fst");
        QVERIFY(makeFixture(path, FST_WR_PT_ZLIB, embedded));
        const auto encoded = QFile::encodeName(path);
        auto      *reader  = fstReaderOpen(encoded.constData());
        QVERIFY(reader);
        const auto  close = qScopeGuard([&] { fstReaderClose(reader); });
        QStringList names;
        while (const auto hierarchy = fstReaderIterateHier(reader)) {
            if (hierarchy->htyp == FST_HT_VAR)
                names.append(QString::fromLatin1(hierarchy->u.var.name));
        }
        QCOMPARE(names, (QStringList{"clk", "clk_alias", "data [3:0]"}));
    }
}

void Test::embeddedHierarchyPreferred()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("embedded.fst");
    QVERIFY(makeFixture(path, FST_WR_PT_ZLIB));
    QFile main(path);
    QVERIFY(main.open(QIODevice::ReadOnly));
    int reads = 0;
    QSocFstClient::Companion
         companion{QSocFstClient::temporaryLimit + 1, [&](quint64, quint32, QString *error) {
                      ++reads;
                      *error = "Unused companion was read";
                      return QByteArray{};
                   }};
    auto cancelled = std::make_shared<std::atomic_bool>(false);
    QSocFstClient client;
    QString       error;
    QVERIFY2(
        client.start(
            main.size(), source(main), cancelled, &error, QSOC_WAVE_WORKER_PATH, std::move(companion)),
        qPrintable(error));
    client.beginQuantum(30000);
    int names = 0;
    while (true) {
        QJsonObject message;
        QCOMPARE(client.next(&message, &error), QSocFstClient::Next::Message);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto type = message.value("type").toString();
        if (type == "signal")
            ++names;
        else if (type == "hierarchy_end")
            QVERIFY(select(client, 777, 790, &error));
        else if (type == "complete")
            break;
    }
    QCOMPARE(names, 3);
    QCOMPARE(reads, 0);
}

void Test::companionFailures_data()
{
    QTest::addColumn<QString>("mode");
    QTest::addColumn<QString>("expected");
    QTest::newRow("missing") << QString("missing") << QString("hierarchy_missing");
    QTest::newRow("malformed") << QString("malformed") << QString("status: 5");
    QTest::newRow("empty") << QString("empty") << QString("status: 5");
    QTest::newRow("quota") << QString("quota") << QString("status: 4");
    QTest::newRow("changed-source-error") << QString("changed") << QString("source_changed");
    QTest::newRow("truncated-source") << QString("shortened") << QString("short read");
    QTest::newRow("read-error") << QString("read-error") << QString("companion read failed");
}

void Test::companionFailures()
{
    QFETCH(QString, mode);
    QFETCH(QString, expected);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("companion.fst");
    QVERIFY(makeFixture(path, FST_WR_PT_ZLIB, false));
    QFile main(path);
    QFile hierarchy(path + ".hier");
    QVERIFY(main.open(QIODevice::ReadOnly));
    QVERIFY(hierarchy.open(QIODevice::ReadOnly));
    auto    bytes = hierarchy.readAll();
    quint64 size  = bytes.size();
    if (mode == "malformed") {
        bytes.chop(1);
        size = bytes.size();
    } else if (mode == "shortened") {
        bytes.chop(1);
    } else if (mode == "empty") {
        bytes.clear();
        size = 0;
    } else if (mode == "quota") {
        size = QSocFstClient::temporaryLimit + 1;
    }
    auto                                    cancelled = std::make_shared<std::atomic_bool>(false);
    QSocFstClient                           client;
    QString                                 error;
    std::optional<QSocFstClient::Companion> companion;
    if (mode != "missing") {
        companion
            = QSocFstClient::Companion{size, [&](quint64 offset, quint32 count, QString *readError) {
                                           if (mode == "changed") {
                                               *readError
                                                   = "source_changed: hierarchy companion changed";
                                               return QByteArray{};
                                           }
                                           if (mode == "read-error") {
                                               *readError = "companion read failed";
                                               return QByteArray{};
                                           }
                                           return bytes.mid(offset, qMin<quint32>(count, 7));
                                       }};
    }
    QVERIFY2(
        client.start(
            main.size(), source(main), cancelled, &error, QSOC_WAVE_WORKER_PATH, std::move(companion)),
        qPrintable(error));
    client.beginQuantum();
    while (true) {
        QJsonObject message;
        const auto  next = client.next(&message, &error);
        if (next == QSocFstClient::Next::Failed)
            break;
        QCOMPARE(next, QSocFstClient::Next::Message);
        QVERIFY(message.value("type") != "complete");
        if (message.value("type") == "hierarchy_end")
            QVERIFY(select(client, 0, 2047, &error));
    }
    QVERIFY2(error.contains(expected), qPrintable(error));
}

void Test::companionPause_data()
{
    QTest::addColumn<bool>("outerWrapper");
    QTest::addColumn<bool>("cancelDuringPause");
    QTest::newRow("native-resume") << false << false;
    QTest::newRow("wrapper-cancel") << true << true;
}

void Test::companionPause()
{
    QFETCH(bool, outerWrapper);
    QFETCH(bool, cancelDuringPause);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("paused-companion.fst");
    QVERIFY(makeFixture(path, FST_WR_PT_ZLIB, false));
    if (outerWrapper)
        QVERIFY(wrapFixture(path));
    QFile main(path);
    QFile hierarchy(path + ".hier");
    QVERIFY(main.open(QIODevice::ReadOnly));
    QVERIFY(hierarchy.open(QIODevice::ReadOnly));
    auto          cancelled = std::make_shared<std::atomic_bool>(false);
    QSocFstClient client;
    bool          armed          = false;
    quint64       hierarchyReads = 0;
    const auto    hierarchyRead  = source(hierarchy, 7);
    QSocFstClient::Companion
        companion{quint64(hierarchy.size()), [&](quint64 offset, quint32 count, QString *error) {
                      ++hierarchyReads;
                      const auto bytes = hierarchyRead(offset, count, error);
                      if (!armed) {
                          armed = true;
                          client.beginQuantum(1);
                          QTest::qSleep(2);
                      }
                      return bytes;
                  }};
    QString error;
    QVERIFY2(
        client.start(
            main.size(), source(main), cancelled, &error, QSOC_WAVE_WORKER_PATH, std::move(companion)),
        qPrintable(error));
    const auto pid = client.processId();
    client.beginQuantum(30000);
    bool                                paused       = false;
    quint64                             readsAtPause = 0;
    QHash<quint64, QHash<int, QString>> events;
    while (true) {
        QJsonObject message;
        const auto  next = client.next(&message, &error);
        if (next == QSocFstClient::Next::Paused) {
            QVERIFY(armed);
            QVERIFY(!paused);
            paused       = true;
            readsAtPause = hierarchyReads;
            QVERIFY(readsAtPause > 0);
            const auto cpuBefore = QSocResourceUsage::process(pid).value("cpu_time_ns");
            QTest::qWait(5050);
            QCOMPARE(client.processId(), pid);
            const auto cpuAfter = QSocResourceUsage::process(pid).value("cpu_time_ns");
            if (cpuBefore.isDouble() && cpuAfter.isDouble())
                QVERIFY(cpuAfter.toInteger() - cpuBefore.toInteger() <= 100000000);
            if (cancelDuringPause) {
                client.cancel();
                QCOMPARE(client.processId(), 0);
                QCOMPARE(client.next(&message, &error), QSocFstClient::Next::Failed);
                QVERIFY(error.contains("cancelled"));
                return;
            }
            client.beginQuantum(30000);
            continue;
        }
        QCOMPARE(next, QSocFstClient::Next::Message);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto type = message.value("type").toString();
        if (type == "hierarchy_end")
            QVERIFY(select(client, 777, 790, &error));
        else if (type == "event") {
            const auto time = message.value("time").toString().toULongLong();
            if (time >= 777)
                events[time][message.value("handle").toInt()] = message.value("value").toString();
        } else if (type == "complete")
            break;
    }
    QVERIFY(paused);
    QVERIFY(hierarchyReads > readsAtPause);
    QCOMPARE(events.size(), 14);
    for (quint64 time = 777; time <= 790; ++time) {
        QCOMPARE(events.value(time).value(1), time & 1U ? "1" : "0");
        QCOMPARE(events.value(time).value(2), QString::fromLatin1(bits(time * 7 + 3)));
    }
}

void Test::wrapperPreparationPause_data()
{
    QTest::addColumn<bool>("cancelDuringPause");
    QTest::newRow("resume") << false;
    QTest::newRow("cancel") << true;
}

void Test::wrapperPreparationPause()
{
    QFETCH(bool, cancelDuringPause);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path    = directory.filePath("preparing.fst");
    const auto encoded = QFile::encodeName(path);
    auto       writer  = fstWriterCreate(encoded.constData(), 1);
    QVERIFY(writer);
    fstWriterSetPackType(writer, FST_WR_PT_LZ4);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "top", nullptr);
    const auto handle = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 32, "data", 0);
    fstWriterSetUpscope(writer);
    uint32_t random = 0x12345678U;
    for (unsigned time = 0; time < 262144; ++time) {
        random = random * 1664525U + 1013904223U;
        char value[33];
        for (unsigned bit = 0; bit < 32; ++bit)
            value[bit] = random & (1U << (31 - bit)) ? '1' : '0';
        value[32] = 0;
        fstWriterEmitTimeChange(writer, time);
        fstWriterEmitValueChange(writer, handle, value);
        if (time && time % 2048 == 0)
            fstWriterFlushContext(writer);
    }
    QCOMPARE(fstWriterClose2(writer), 0);
    QVERIFY(wrapFixture(path));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QVERIFY(file.size() > 128 * 1024);
    auto          cancelled = std::make_shared<std::atomic_bool>(false);
    QSocFstClient client;
    bool          armed   = false;
    quint64       reads   = 0;
    const auto    rawRead = source(file);
    const auto    readAt  = [&](quint64 offset, quint32 count, QString *error) {
        ++reads;
        const auto bytes = rawRead(offset, count, error);
        if (!armed && offset >= 65536) {
            armed = true;
            client.beginQuantum(1);
            QTest::qSleep(2);
        }
        return bytes;
    };
    QString error;
    QVERIFY2(
        client.start(file.size(), readAt, cancelled, &error, QSOC_WAVE_WORKER_PATH),
        qPrintable(error));
    const auto pid = client.processId();
    client.beginQuantum(30000);
    bool                    sawHeader   = false;
    bool                    paused      = false;
    quint64                 pausedReads = 0;
    QHash<quint64, QString> events;
    QString                 initial;
    while (true) {
        QJsonObject message;
        const auto  next = client.next(&message, &error);
        if (next == QSocFstClient::Next::Paused) {
            QVERIFY(armed);
            QVERIFY(!paused);
            QVERIFY(!sawHeader);
            paused               = true;
            pausedReads          = reads;
            const auto cpuBefore = QSocResourceUsage::process(pid).value("cpu_time_ns");
            QTest::qWait(5050);
            QCOMPARE(client.processId(), pid);
            QCOMPARE(reads, pausedReads);
            const auto cpuAfter = QSocResourceUsage::process(pid).value("cpu_time_ns");
            if (cpuBefore.isDouble() && cpuAfter.isDouble())
                QVERIFY(cpuAfter.toInteger() - cpuBefore.toInteger() <= 100000000);
            if (cancelDuringPause) {
                QElapsedTimer cleanup;
                cleanup.start();
                client.cancel();
                QCOMPARE(client.processId(), 0);
                QCOMPARE(client.next(&message, &error), QSocFstClient::Next::Failed);
                QVERIFY(error.contains("cancelled"));
                QVERIFY(cleanup.elapsed() < 2500);
                qInfo() << "wrapper paused cancellation ms" << cleanup.elapsed();
                return;
            }
            client.beginQuantum(30000);
            continue;
        }
        QCOMPARE(next, QSocFstClient::Next::Message);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto type = message.value("type").toString();
        if (type == "header")
            sawHeader = true;
        else if (type == "hierarchy_end")
            QVERIFY(client.select(
                {{"handles", QJsonArray{1}}, {"start", "200000"}, {"end", "200010"}}, &error));
        else if (type == "initial")
            initial = message.value("value").toString();
        else if (type == "event") {
            const auto time = message.value("time").toString().toULongLong();
            if (time >= 200000)
                events[time] = message.value("value").toString();
        } else if (type == "complete")
            break;
    }
    QVERIFY(paused);
    QVERIFY(reads > pausedReads);
    QCOMPARE(events.size(), 11);
    random = 0x12345678U;
    for (unsigned time = 0; time <= 200010; ++time) {
        random = random * 1664525U + 1013904223U;
        if (time < 199999)
            continue;
        const auto value = QString::fromLatin1(
            QByteArray::number(random, 2).rightJustified(32, '0'));
        if (time == 199999)
            QCOMPARE(initial, value);
        if (time >= 200000)
            QCOMPARE(events.value(time), value);
    }
}

void Test::valuesAfterEnd()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("fixture.fst");
    QVERIFY(makeFixture(path, FST_WR_PT_ZLIB));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto          cancelled = std::make_shared<std::atomic_bool>(false);
    QSocFstClient client;
    QString       error;
    QVERIFY(client.start(file.size(), source(file), cancelled, &error, QSOC_WAVE_WORKER_PATH));
    client.beginQuantum(30000);
    QHash<int, QString> initial;
    while (true) {
        QJsonObject message;
        QCOMPARE(client.next(&message, &error), QSocFstClient::Next::Message);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto type = message.value("type").toString();
        if (type == "hierarchy_end")
            QVERIFY(select(client, 3000, 3100, &error));
        else if (type == "initial")
            initial[message.value("handle").toInt()] = message.value("value").toString();
        else if (type == "complete")
            break;
        else
            QVERIFY(type != "event");
    }
    QCOMPARE(initial.value(1), "1");
    QCOMPARE(initial.value(2), QString::fromLatin1(bits(2047 * 7 + 3)));
}

void Test::malformedHierarchy()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path = directory.filePath("fixture.fst");
    QVERIFY(makeFixture(path, FST_WR_PT_ZLIB));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto       bytes = file.readAll();
    QByteArray hierarchy;
    hierarchy.append(char(FST_ST_VCD_SCOPE));
    hierarchy.append(char(FST_ST_VCD_MODULE));
    hierarchy.append("unterminated");
    z_stream stream{};
    QCOMPARE(
        deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY), Z_OK);
    QByteArray compressed(128, '\0');
    stream.next_in   = reinterpret_cast<Bytef *>(hierarchy.data());
    stream.avail_in  = hierarchy.size();
    stream.next_out  = reinterpret_cast<Bytef *>(compressed.data());
    stream.avail_out = compressed.size();
    QCOMPARE(deflate(&stream, Z_FINISH), Z_STREAM_END);
    compressed.resize(stream.total_out);
    QCOMPARE(deflateEnd(&stream), Z_OK);
    QByteArray replacement(1, char(FST_BL_HIER));
    for (quint64 value : {quint64(16 + compressed.size()), quint64(hierarchy.size())}) {
        for (int shift = 56; shift >= 0; shift -= 8)
            replacement.append(char(value >> shift));
    }
    replacement += compressed;
    bool replaced = false;
    for (qsizetype offset = 0; offset + 9 <= bytes.size();) {
        const auto length = uint64At(bytes, offset + 1);
        if (quint8(bytes.at(offset)) == FST_BL_HIER) {
            bytes.replace(offset, 1 + length, replacement);
            replaced = true;
            break;
        }
        offset += 1 + length;
    }
    QVERIFY(replaced);
    auto          cancelled = std::make_shared<std::atomic_bool>(false);
    QSocFstClient client;
    QString       error;
    QVERIFY(client.start(
        bytes.size(),
        [&bytes](quint64 offset, quint32 count, QString *) { return bytes.mid(offset, count); },
        cancelled,
        &error,
        QSOC_WAVE_WORKER_PATH));
    client.beginQuantum(5000);
    QElapsedTimer elapsed;
    elapsed.start();
    while (true) {
        QJsonObject message;
        const auto  next = client.next(&message, &error);
        if (next == QSocFstClient::Next::Failed)
            break;
        QCOMPARE(next, QSocFstClient::Next::Message);
        QVERIFY(message.value("type") != "complete");
    }
    QVERIFY2(error.contains("status: 5"), qPrintable(error));
    QVERIFY(elapsed.elapsed() < 5000);
}

void Test::sameTickChanges()
{
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path    = directory.filePath("same-tick.fst");
    const auto encoded = QFile::encodeName(path);
    auto       writer  = fstWriterCreate(encoded.constData(), 1);
    QVERIFY(writer);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "top", nullptr);
    const auto clock = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "clk", 0);
    fstWriterSetUpscope(writer);
    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChange(writer, clock, "0");
    fstWriterEmitTimeChange(writer, 5);
    for (const char *value : {"1", "0", "1"})
        fstWriterEmitValueChange(writer, clock, value);
    fstWriterEmitTimeChange(writer, 10);
    fstWriterEmitValueChange(writer, clock, "0");
    QCOMPARE(fstWriterClose2(writer), 0);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto          cancelled = std::make_shared<std::atomic_bool>(false);
    QSocFstClient client;
    QString       error;
    QVERIFY(client.start(file.size(), source(file), cancelled, &error, QSOC_WAVE_WORKER_PATH));
    client.beginQuantum(5000);
    QStringList values;
    while (true) {
        QJsonObject message;
        QCOMPARE(client.next(&message, &error), QSocFstClient::Next::Message);
        QVERIFY2(error.isEmpty(), qPrintable(error));
        const auto type = message.value("type").toString();
        if (type == "hierarchy_end")
            QVERIFY(
                client.select({{"handles", QJsonArray{1}}, {"start", "5"}, {"end", "5"}}, &error));
        else if (type == "event" && message.value("time") == "5")
            values.append(message.value("value").toString());
        else if (type == "complete")
            break;
    }
    QCOMPARE(values, (QStringList{"1", "0", "1"}));
}

void Test::sharedTickAcrossSections_data()
{
    QTest::addColumn<bool>("wrapped");
    QTest::addColumn<bool>("external");
    QTest::addColumn<bool>("finalShared");
    for (bool wrapped : {false, true})
        for (bool external : {false, true})
            for (bool finalShared : {false, true})
                QTest::newRow(qPrintable(QString("wrapper%1-hierarchy%2-final%3")
                                             .arg(wrapped)
                                             .arg(external)
                                             .arg(finalShared)))
                    << wrapped << external << finalShared;
}

void Test::sharedTickAcrossSections()
{
    QFETCH(bool, wrapped);
    QFETCH(bool, external);
    QFETCH(bool, finalShared);
    QTemporaryDir directory;
    const auto    path = directory.filePath("shared.fst");
    QVERIFY(makeSharedTickFixture(path, finalShared, !external));
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto encoded  = file.readAll();
    int        sections = 0;
    for (quint64 offset = 0; offset + 9 <= quint64(encoded.size());) {
        const auto kind   = quint8(encoded[offset]);
        const auto length = uint64At(encoded, offset + 1);
        QVERIFY(length && length <= quint64(encoded.size()) - offset - 1);
        if (kind == FST_BL_VCDATA || kind == FST_BL_VCDATA_DYN_ALIAS
            || kind == FST_BL_VCDATA_DYN_ALIAS2)
            ++sections;
        offset += 1 + length;
    }
    QCOMPARE(sections, 4);
    file.close();
    if (wrapped)
        QVERIFY(wrapFixture(path));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QFile                                   hierarchy(path + ".hier");
    std::optional<QSocFstClient::Companion> companion;
    if (external) {
        QVERIFY(hierarchy.open(QIODevice::ReadOnly));
        companion = QSocFstClient::Companion{quint64(hierarchy.size()), source(hierarchy)};
    }
    QString       error;
    QSocFstClient client;
    QVERIFY(client.start(
        file.size(),
        source(file),
        std::make_shared<std::atomic_bool>(false),
        &error,
        QSOC_WAVE_WORKER_PATH,
        companion));
    client.beginQuantum(30000);
    for (;;) {
        QJsonObject message;
        QCOMPARE(client.next(&message, &error), QSocFstClient::Next::Message);
        if (message.value("type") == "hierarchy_end")
            QVERIFY(select(client, 6, 8, &error));
        if (message.value("type") == "complete")
            break;
    }
    client.beginQuantum(30000);
    QVERIFY(select(client, 11, 11, &error));
    QHash<int, QString> initial;
    for (;;) {
        QJsonObject message;
        QCOMPARE(client.next(&message, &error), QSocFstClient::Next::Message);
        if (message.value("type") == "initial")
            initial[message.value("handle").toInt()] = message.value("value").toString();
        if (message.value("type") == "complete")
            break;
    }
    QCOMPARE(initial.value(1), "1");
    QCOMPARE(initial.value(2), "0111");
    client.beginQuantum(30000);
    QVERIFY(select(client, 10, 10, &error));
    QStringList clocks, buses;
    for (;;) {
        QJsonObject message;
        QCOMPARE(client.next(&message, &error), QSocFstClient::Next::Message);
        if (message.value("type") == "event" && message.value("time") == "10") {
            auto &values = message.value("handle").toInt() == 1 ? clocks : buses;
            values.append(message.value("value").toString());
        }
        if (message.value("type") == "complete")
            break;
    }
    QCOMPARE(clocks, (QStringList{"0", "1", "0", "1", "0", "1"}));
    QCOMPARE(buses, (QStringList{"0010", "0011", "0100", "0101", "0110", "0111"}));
}

void Test::realValues_data()
{
    QTest::addColumn<int>("nonfiniteTime");
    QTest::newRow("finite") << -1;
    QTest::newRow("nan-initial") << 0;
    QTest::newRow("nan-event") << 5;
}

void Test::realValues()
{
    QFETCH(int, nonfiniteTime);
    QTemporaryDir directory;
    QVERIFY(directory.isValid());
    const auto path    = directory.filePath("real.fst");
    const auto encoded = QFile::encodeName(path);
    auto       writer  = fstWriterCreate(encoded.constData(), 1);
    QVERIFY(writer);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "top", nullptr);
    const auto handle = fstWriterCreateVar(writer, FST_VT_VCD_REAL, FST_VD_IMPLICIT, 64, "value", 0);
    fstWriterSetUpscope(writer);
    for (int time : {0, 5}) {
        const double value = time == nonfiniteTime ? std::numeric_limits<double>::quiet_NaN()
                             : time == 0           ? 1.25
                                                   : -2.5;
        fstWriterEmitTimeChange(writer, time);
        fstWriterEmitValueChange(writer, handle, &value);
    }
    QCOMPARE(fstWriterClose2(writer), 0);
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadOnly));
    auto          cancelled = std::make_shared<std::atomic_bool>(false);
    QSocFstClient client;
    QString       error;
    QVERIFY(client.start(file.size(), source(file), cancelled, &error, QSOC_WAVE_WORKER_PATH));
    client.beginQuantum(5000);
    QString initial;
    QString last;
    bool    complete = false;
    while (true) {
        QJsonObject message;
        const auto  next = client.next(&message, &error);
        if (next == QSocFstClient::Next::Failed)
            break;
        QCOMPARE(next, QSocFstClient::Next::Message);
        const auto type = message.value("type").toString();
        if (type == "hierarchy_end")
            QVERIFY(
                client.select({{"handles", QJsonArray{1}}, {"start", "2"}, {"end", "5"}}, &error));
        else if (type == "initial")
            initial = message.value("value").toString();
        else if (type == "event" && message.value("time") == "5")
            last = message.value("value").toString();
        else if (type == "complete") {
            complete = true;
            break;
        }
    }
    QVERIFY2(complete == (nonfiniteTime < 0), qPrintable(error));
    if (complete) {
        QCOMPARE(initial, "1.25");
        QCOMPARE(last, "-2.5");
    } else {
        QVERIFY2(error.contains("nonfinite_real"), qPrintable(error));
    }
}
void Test::workerEnvironment()
{
    constexpr auto probe = "QSOC_FST_ENV_CREDENTIAL_PROBE";
    QVERIFY(!qEnvironmentVariableIsSet(probe));
    QVERIFY(qputenv(probe, QUuid::createUuid().toByteArray(QUuid::Id128)));
    const auto clearProbe = qScopeGuard([probe] { qunsetenv(probe); });
    QProcess   unfiltered;
    unfiltered.setProgram(QSOC_WAVE_ENV_PROBE_PATH);
    unfiltered.setArguments({"--socket", "unused", "--owner-pid", "1"});
    unfiltered.start();
    QVERIFY(unfiltered.waitForFinished(2000));
    QCOMPARE(unfiltered.exitCode(), 21);
    auto          cancelled = std::make_shared<std::atomic_bool>(false);
    QSocFstClient client;
    QString       error;
    QVERIFY2(
        client.start(
            1,
            [](quint64, quint32, QString *) { return QByteArray{}; },
            cancelled,
            &error,
            QSOC_WAVE_ENV_PROBE_PATH),
        qPrintable(error));
    client.beginQuantum();
    QJsonObject result;
    QCOMPARE(client.next(&result, &error), QSocFstClient::Next::Message);
    QVERIFY2(error.isEmpty(), qPrintable(error));
    QCOMPARE(result.value("type").toString(), "complete");
    QVERIFY(qEnvironmentVariableIsSet(probe));
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocfstworker.moc"
