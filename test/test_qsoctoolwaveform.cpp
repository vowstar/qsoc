// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsocagentremote.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsocsshsession.h"
#include "agent/tool/qsoctoolpath.h"
#include "agent/tool/qsoctoolwaveform.h"
#include "agent/waveform/qsocvcdcheckpoints.h"
#include "fstapi.h"
#include "qsoc_test.h"
#include "qsoc_test_sshd.h"
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
#include "qsoc_test_sftp_peer.h"
#endif

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <array>
#include <stop_token>
#include <thread>
#include <vector>

namespace {

QByteArray waveform()
{
    return QByteArray(
        "$timescale 1 ns $end\n$scope module top $end\n"
        "$var wire 1 ! clk $end\n$var wire 1 ! clk_alias $end\n"
        "$var wire 4 \" data [3:0] $end\n$var wire 1 # rst_n $end\n"
        "$upscope $end\n$enddefinitions $end\n$dumpvars\n"
        "0! b0000 \" 0#\n$end\n"
        "#5 1!\n#10 0! 1# b10xz \"\n"
        "$comment a comment between events $end\n"
        "#15 1!\n#20 b1010 \"\n#25 0!\n");
}

QString writeFile(const QTemporaryDir &dir, const QString &name, const QByteArray &bytes)
{
    const QString path = dir.filePath(name);
    QFile         file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size())
        qFatal("Cannot create waveform input");
    return path;
}

json read(QSocToolWaveformRead &tool, const json &arguments)
{
    const QString result = tool.execute(arguments);
    if (result.startsWith("Error:"))
        qFatal("%s", qPrintable(result));
    return json::parse(result.toStdString());
}

QString nativeFst(const QTemporaryDir &directory, bool embeddedHierarchy = true)
{
    const auto path    = directory.filePath("sample.fst");
    const auto encoded = QFile::encodeName(path);
    auto       writer  = fstWriterCreate(encoded.constData(), embeddedHierarchy ? 1 : 0);
    if (!writer)
        qFatal("Cannot create FST fixture");
    fstWriterSetTimescale(writer, -9);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "top", nullptr);
    const auto clock = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "clk", 0);
    fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "clk_alias", clock);
    const auto data
        = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 4, "data [3:0]", 0);
    const auto reset = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "rst_n", 0);
    fstWriterSetUpscope(writer);
    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChange(writer, clock, "0");
    fstWriterEmitValueChange(writer, data, "0000");
    fstWriterEmitValueChange(writer, reset, "0");
    fstWriterEmitTimeChange(writer, 5);
    fstWriterEmitValueChange(writer, clock, "1");
    fstWriterEmitTimeChange(writer, 10);
    fstWriterEmitValueChange(writer, clock, "0");
    fstWriterEmitValueChange(writer, reset, "1");
    fstWriterEmitValueChange(writer, data, "10xz");
    fstWriterEmitTimeChange(writer, 15);
    fstWriterEmitValueChange(writer, clock, "1");
    fstWriterEmitTimeChange(writer, 20);
    fstWriterEmitValueChange(writer, data, "1010");
    fstWriterEmitTimeChange(writer, 25);
    fstWriterEmitValueChange(writer, clock, "0");
    if (fstWriterClose2(writer))
        qFatal("Cannot close FST fixture");
    return path;
}

QString sharedTickFst(const QTemporaryDir &directory, bool finalShared, bool embedded)
{
    const auto path    = directory.filePath("shared.fst");
    const auto encoded = QFile::encodeName(path);
    auto      *writer  = fstWriterCreate(encoded.constData(), embedded ? 1 : 0);
    if (!writer)
        qFatal("Cannot create shared-tick FST");
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
    if (fstWriterClose2(writer))
        qFatal("Cannot close shared-tick FST");
    return path;
}

void wrapFst(const QString &path)
{
    QFile file(path);
    QVERIFY(file.open(QIODevice::ReadWrite));
    const auto plain = file.readAll();
    z_stream   stream{};
    QCOMPARE(
        deflateInit2(&stream, Z_DEFAULT_COMPRESSION, Z_DEFLATED, 31, 8, Z_DEFAULT_STRATEGY), Z_OK);
    QByteArray compressed(deflateBound(&stream, plain.size()), '\0');
    stream.next_in   = reinterpret_cast<Bytef *>(const_cast<char *>(plain.constData()));
    stream.avail_in  = plain.size();
    stream.next_out  = reinterpret_cast<Bytef *>(compressed.data());
    stream.avail_out = compressed.size();
    QCOMPARE(deflate(&stream, Z_FINISH), Z_STREAM_END);
    compressed.resize(stream.total_out);
    QCOMPARE(deflateEnd(&stream), Z_OK);
    QByteArray output(1, char(FST_BL_ZWRAPPER));
    for (quint64 value : {quint64(16 + compressed.size()), quint64(plain.size())}) {
        for (int shift = 56; shift >= 0; shift -= 8)
            output.append(char(value >> shift));
    }
    output += compressed;
    QVERIFY(file.resize(0));
    QVERIFY(file.seek(0));
    QCOMPARE(file.write(output), qint64(output.size()));
}

#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
using RemoteWaveformFixture = QSocTestSftpPeer;
#else
using RemoteWaveformFixture = QSocTestSshd;
#endif

QString remoteDirectory(const RemoteWaveformFixture &fixture)
{
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    return fixture.canonicalDir();
#else
    return fixture.workDir();
#endif
}

QString remoteFilePath(const RemoteWaveformFixture &fixture, const QString &native)
{
    return remoteDirectory(fixture) + QLatin1Char('/')
           + QDir(fixture.workDir()).relativeFilePath(native);
}

bool bindRemote(
    const RemoteWaveformFixture &fixture, QSocRemoteConnection &connection, QString *error)
{
    AgentRemoteState state;
    state.session      = new QSocSshSession;
    state.sftp         = new QSocSftpClient(*state.session);
    const auto cleanup = qScopeGuard([&] { discardAgentRemoteState(&state); });
    if (state.session->connectTo(fixture.hostConfig(), error) != QSocSshSession::ConnectStatus::Ok)
        return false;
    state.endpointIdentity = state.session->hostKeyIdentity();
    state.targetKey        = QStringLiteral("loopback");
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    connection.setHostProbe([](QSocSshSession *, const QString &) {
        QSocMachine host;
#ifdef Q_OS_WIN
        host.kind = QSocMachine::Kind::Windows;
#else
        host.kind = QSocMachine::Kind::Posix;
#endif
        return host;
    });
#endif
    return prepareAgentRemoteWorkspace(remoteDirectory(fixture), &state, error)
           && connection.adopt(std::move(state));
}

struct WideVcd
{
    QString path;
    quint64 checkpointTime = 0;
};

WideVcd wideVcd(const QTemporaryDir &directory)
{
    WideVcd fixture{directory.filePath("wide-events.vcd"), 0};
    QFile   file(fixture.path);
    if (!file.open(QIODevice::WriteOnly))
        qFatal("Cannot create generated wide VCD");
    file.write(
        "$timescale 1 ns $end\n$scope module top $end\n"
        "$var wire 1 ! clk $end\n$var wire 1 ! clk_alias $end\n"
        "$var wire 65535 \" payload [65534:0] $end\n"
        "$upscope $end\n$enddefinitions $end\n");
    for (quint64 tick = 0; tick < 280; ++tick) {
        const auto       time  = QByteArray::number(tick);
        const QByteArray value = tick & 1U ? "1" : "0";
        file.write('#' + time + ' ' + value + "!\n");
        if (!fixture.checkpointTime && file.pos() >= 16 * 1024 * 1024)
            fixture.checkpointTime = tick;
        QByteArray bits(65535, '0');
        quint32    state = static_cast<quint32>(tick + 1);
        for (char &bit : bits) {
            state = state * 1664525U + 1013904223U;
            bit   = state >> 31 ? '1' : '0';
        }
        file.write('b' + bits + " \"\n#" + time + ' ');
        if (!fixture.checkpointTime && file.pos() >= 16 * 1024 * 1024)
            fixture.checkpointTime = tick;
        file.write((tick & 1U ? QByteArray("0!") : QByteArray("1!")) + ' ' + value + "!\n");
    }
    if (file.error() != QFileDevice::NoError || !file.flush())
        qFatal("Cannot write generated wide VCD");
    return fixture;
}

json collect(QSocToolWaveformRead &tool, json arguments)
{
    auto    result = read(tool, arguments);
    json    events = result["events"];
    quint64 bytes  = result["scan_bytes"].get<quint64>();
    int     pages  = 0;
    while (result.contains("next_cursor")) {
        if (++pages > 200)
            qFatal("Generated waveform query did not finish");
        arguments = {{"file_path", arguments["file_path"]}, {"cursor", result["next_cursor"]}};
        result    = read(tool, arguments);
        bytes += result["scan_bytes"].get<quint64>();
        for (const auto &event : result["events"])
            events.push_back(event);
    }
    result["events"]           = std::move(events);
    result["total_scan_bytes"] = bytes;
    return result;
}

class TestQSocToolWaveform : public QObject
{
    Q_OBJECT

private slots:
    void checkpointStorageBoundsAndStrictTime()
    {
        QSocVcdCheckpoints exact(1, 4, 64);
        exact.record(5, 10, {{"!", "0"}});
        exact.record(5, 20, {{"!", "1"}});
        exact.record(6, 30, {{"!", "0"}});
        QVERIFY(!exact.before(5));
        QVERIFY(exact.before(6));
        QCOMPARE(exact.before(6)->offset, quint64(20));
        QCOMPARE(exact.before(6)->values.value("!"), QByteArray("1"));
        QSocVcdCheckpoints bounded(1, 4, 16);
        for (quint64 tick = 1; tick < 1000; ++tick) {
            bounded.record(tick, tick * 8, {{"!", QByteArray::number(tick)}});
            QVERIFY(bounded.size() <= 4);
            QVERIFY(bounded.payloadBytes() <= 16);
            if (const auto checkpoint = bounded.before(tick)) {
                QVERIFY(checkpoint->time < tick);
                QCOMPARE(checkpoint->values.value("!"), QByteArray::number(checkpoint->time));
            }
        }
        const auto count = bounded.size();
        bounded.record(2000, 100000, {{"!", QByteArray(17, '1')}});
        QCOMPARE(bounded.size(), count);
    }

    void warmVcdRestoresSelectedStateAndSameTickEvents()
    {
        QTemporaryDir directory;
        const auto    fixture = wideVcd(directory);
        QVERIFY(fixture.checkpointTime > 0);
        QSocToolWaveformRead tool;
        const auto           request = [&](quint64 start, quint64 end) {
            return json{
                {"file_path", fixture.path.toStdString()},
                {"signals", {"top.clk*"}},
                {"start_time", start},
                {"end_time", end}};
        };
        const auto first = collect(tool, request(270, 272));
        QVERIFY(first["total_scan_bytes"].get<quint64>() > 16 * 1024 * 1024);
        const auto equalTick
            = collect(tool, request(fixture.checkpointTime, fixture.checkpointTime));
        QCOMPARE(equalTick["events"].size(), std::size_t(6));
        QCOMPARE(
            equalTick["values_at_start"]["top.clk"], json(fixture.checkpointTime & 1U ? "1" : "0"));
        const auto           warm = collect(tool, request(271, 272));
        QSocToolWaveformRead coldTool;
        const auto           cold = collect(coldTool, request(271, 272));
        for (const auto *field :
             {"signals",
              "timescale",
              "events",
              "values_at_start",
              "window_complete",
              "values_at_start_complete"})
            QCOMPARE(warm[field], cold[field]);
        QCOMPARE(warm["events"].size(), std::size_t(12));
        QCOMPARE(warm["values_at_start"]["top.clk_alias"], json("1"));
        QVERIFY(warm["total_scan_bytes"].get<quint64>() < 2 * 1024 * 1024);
        QVERIFY(cold["total_scan_bytes"].get<quint64>() > 16 * 1024 * 1024);
        auto *expiry = tool.findChild<QTimer *>(QString(), Qt::FindDirectChildrenOnly);
        QVERIFY(expiry);
        expiry->start(1);
        QTest::qWait(20);
        const auto expired = collect(tool, request(271, 272));
        QVERIFY(expired["total_scan_bytes"].get<quint64>() > 16 * 1024 * 1024);
    }

    void completedVcdCacheOwnershipAndCancellation()
    {
        QTemporaryDir        directory;
        const auto           path = writeFile(directory, "cached.vcd", waveform());
        QSocToolWaveformRead tool;
        QSocToolRegistry     registry;
        registry.registerTool(&tool);
        QObject    owner;
        QObject    sibling;
        const json request
            = {{"file_path", path.toStdString()},
               {"signals", {"top.clk*"}},
               {"start_time", 7},
               {"end_time", 17}};
        const auto run = [&](QObject *scope, std::stop_token token = {}) {
            const auto text
                = registry.executeTool("read_waveform", request, scope, {}, {}, {}, token);
            if (text.startsWith("Error:"))
                qFatal("%s", qPrintable(text));
            return json::parse(text.toStdString());
        };
        const auto first = run(&owner);
        const auto warm  = run(&owner);
        QVERIFY(warm["scan_bytes"].get<quint64>() < first["scan_bytes"].get<quint64>());
        const auto other = run(&sibling);
        QCOMPARE(other["scan_bytes"], first["scan_bytes"]);
        run(&sibling);
        registry.abortCalls(&sibling);
        QCOMPARE(run(&sibling)["scan_bytes"], first["scan_bytes"]);
        std::stop_source stop;
        run(&sibling, stop.get_token());
        stop.request_stop();
        QCOMPARE(run(&sibling)["scan_bytes"], first["scan_bytes"]);
        QCoreApplication::processEvents();
        QVERIFY(run(&sibling)["scan_bytes"].get<quint64>() < first["scan_bytes"].get<quint64>());
        auto transient = std::make_unique<QObject>();
        run(transient.get());
        transient.reset();
        QCOMPARE(run(&owner)["scan_bytes"], first["scan_bytes"]);
    }

    void completedVcdDoesNotReviveConsumedCursor()
    {
        QTemporaryDir        directory;
        const auto           path = writeFile(directory, "cursor.vcd", waveform());
        QSocToolWaveformRead tool;
        const auto           first = read(
            tool, {{"file_path", path.toStdString()}, {"signals", {"top.clk*"}}, {"max_events", 1}});
        const json continuation
            = {{"file_path", path.toStdString()}, {"cursor", first["next_cursor"]}};
        const auto done = collect(tool, continuation);
        QVERIFY(done["window_complete"].get<bool>());
        QVERIFY(tool.execute(continuation).contains("cursor_expired"));
    }

    void appendedOrTruncatedVcdDiscardsCheckpointState()
    {
        QTemporaryDir        directory;
        const auto           path = writeFile(directory, "growing.vcd", waveform());
        QSocToolWaveformRead tool;
        const auto           first = read(
            tool, {{"file_path", path.toStdString()}, {"signals", {"top.clk*"}}, {"max_events", 1}});
        auto changed = waveform();
        changed.replace("0! b0000", "1! b0000");
        changed += "#30 1!\n";
        writeFile(directory, "growing.vcd", changed);
        QVERIFY(tool.execute({{"file_path", path.toStdString()}, {"cursor", first["next_cursor"]}})
                    .contains("source_changed"));
        const json request  = {{"file_path", path.toStdString()}, {"signals", {"top.clk*"}}};
        const auto appended = collect(tool, request);
        QCOMPARE(appended["values_at_start"]["top.clk"], json("1"));
        QCOMPARE(appended["events"].back()["time"], json(30));
        writeFile(directory, "growing.vcd", waveform().left(waveform().indexOf("#20")));
        const auto truncated = collect(tool, request);
        QCOMPARE(truncated["values_at_start"]["top.clk"], json("0"));
        QCOMPARE(truncated["events"].back()["time"], json(15));
    }

    void nativeFstWindowAndAliases_data()
    {
        QTest::addColumn<bool>("wrapped");
        QTest::addColumn<bool>("externalHierarchy");
        QTest::newRow("native") << false << false;
        QTest::newRow("whole-file-gzip") << true << false;
        QTest::newRow("companion") << false << true;
        QTest::newRow("wrapped-companion") << true << true;
    }

    void nativeFstWindowAndAliases()
    {
        QFETCH(bool, wrapped);
        QFETCH(bool, externalHierarchy);
        QTemporaryDir directory;
        const auto    path = nativeFst(directory, !externalHierarchy);
        if (wrapped)
            wrapFst(path);
        QSocToolWaveformRead tool;
        const auto           listing = read(tool, {{"file_path", path.toStdString()}});
        QCOMPARE(listing["format"], json("fst"));
        QCOMPARE(listing["total_signals"], json(4));
        QCOMPARE(listing["signals"][2]["name"], json("top.data[3:0]"));
        const auto window = read(
            tool,
            {{"file_path", path.toStdString()},
             {"signals", {"top.clk", "top.rst_n", "top.data"}},
             {"start_time", 7},
             {"end_time", 17}});
        QCOMPARE(window["timescale"], json("1ns"));
        QCOMPARE(window["values_at_start"]["top.clk"], json("1"));
        QCOMPARE(window["values_at_start"]["top.rst_n"], json("0"));
        QCOMPARE(window["values_at_start"]["top.data[3:0]"], json("0000"));
        QCOMPARE(window["events"].size(), std::size_t(4));
        QVERIFY(window["window_complete"].get<bool>());
        QVERIFY(window["values_at_start_complete"].get<bool>());
        for (const auto &event : window["events"]) {
            QVERIFY(event["time"].get<int>() >= 7);
            QVERIFY(event["time"].get<int>() <= 17);
            if (event["signal"] == "top.data[3:0]")
                QCOMPARE(event["value"], json("10xz"));
        }
        const json overlapRequest
            = {{"file_path", path.toStdString()},
               {"signals", {"top.clk", "top.rst_n", "top.data"}},
               {"start_time", 8},
               {"end_time", 17}};
        const auto warm = read(tool, overlapRequest);
        QVERIFY(warm["cache"]["reader_reused"].get<bool>());
        QVERIFY(warm["scan_bytes"].get<quint64>() < window["scan_bytes"].get<quint64>());
        if (wrapped)
            QCOMPARE(warm["cache"]["wrapper_cold_rewinds"], json("0"));
        QSocToolWaveformRead cold;
        const auto           reference = read(cold, overlapRequest);
        QVERIFY(!reference["cache"]["reader_reused"].get<bool>());
        for (const auto *field :
             {"signals", "timescale", "values_at_start", "events", "window_complete"})
            QCOMPARE(warm[field], reference[field]);
        auto page = read(
            tool, {{"file_path", path.toStdString()}, {"signals", {"top.clk*"}}, {"max_events", 1}});
        json events = page["events"];
        while (page.contains("next_cursor")) {
            page = read(
                tool,
                {{"file_path", path.toStdString()},
                 {"cursor", page["next_cursor"]},
                 {"max_events", 1}});
            for (const auto &event : page["events"])
                events.push_back(event);
        }
        QCOMPARE(events.size(), std::size_t(10));
        QCOMPARE(page["values_at_start"]["top.clk"], json("0"));
        QCOMPARE(page["values_at_start"]["top.clk_alias"], json("0"));
        QVERIFY(page["window_complete"].get<bool>());
    }

    void completedFstCacheOwnershipAndCancellation_data() { nativeFstWindowAndAliases_data(); }

    void completedFstCacheOwnershipAndCancellation()
    {
        QFETCH(bool, wrapped);
        QFETCH(bool, externalHierarchy);
        QTemporaryDir directory;
        const auto    path = nativeFst(directory, !externalHierarchy);
        if (wrapped)
            wrapFst(path);
        QSocToolWaveformRead tool;
        QSocToolRegistry     registry;
        registry.registerTool(&tool);
        QObject    owner;
        QObject    sibling;
        const json request
            = {{"file_path", path.toStdString()},
               {"signals", {"top.clk*"}},
               {"start_time", 7},
               {"end_time", 17}};
        const auto run = [&](QObject *scope, std::stop_token token = {}) {
            const auto text
                = registry.executeTool("read_waveform", request, scope, {}, {}, {}, token);
            if (text.startsWith("Error:"))
                qFatal("%s", qPrintable(text));
            return json::parse(text.toStdString());
        };
        QVERIFY(!run(&owner)["cache"]["reader_reused"].get<bool>());
        QVERIFY(run(&owner)["cache"]["reader_reused"].get<bool>());
        QVERIFY(!run(&sibling)["cache"]["reader_reused"].get<bool>());
        registry.abortCalls(&sibling);
        QVERIFY(!run(&sibling)["cache"]["reader_reused"].get<bool>());
        std::stop_source stop;
        run(&sibling, stop.get_token());
        stop.request_stop();
        QVERIFY(!run(&sibling)["cache"]["reader_reused"].get<bool>());
        QCoreApplication::processEvents();
        QVERIFY(run(&sibling)["cache"]["reader_reused"].get<bool>());
        auto *expiry = tool.findChild<QTimer *>();
        QVERIFY(expiry);
        expiry->start(1);
        QTest::qWait(10);
        QVERIFY(!run(&sibling)["cache"]["reader_reused"].get<bool>());
        auto transient = std::make_unique<QObject>();
        run(transient.get());
        transient.reset();
        QVERIFY(!run(&owner)["cache"]["reader_reused"].get<bool>());
    }

    void sharedFstTickAcrossSections_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("sftp");
        for (const auto &kind : {"native", "wrapped", "companion", "wrapped-companion"})
            for (bool sftp : {false, true})
                QTest::newRow(qPrintable(QString("%1-sftp%2").arg(kind).arg(sftp)))
                    << QString::fromLatin1(kind) << sftp;
    }

    void sharedFstTickAcrossSections()
    {
        QFETCH(QString, kind);
        QFETCH(bool, sftp);
        RemoteWaveformFixture fixture;
        QSocRemoteConnection  connection;
        if (sftp) {
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
            if (!fixture.start(true) && fixture.missing())
                QSOC_TEST_MISSING_DEPENDENCY(fixture.error());
            QVERIFY2(fixture.error().isEmpty(), qPrintable(fixture.error()));
#else
            fixture.setExtraConfig({QStringLiteral("ForceCommand internal-sftp -R")});
            fixture.start();
            QSOC_REQUIRE_SSHD(fixture);
#endif
            QString error;
            QVERIFY2(bindRemote(fixture, connection, &error), qPrintable(error));
        }
        QTemporaryDir directory(
            sftp ? fixture.workDir() + "/shared-XXXXXX"
                 : QDir::tempPath() + "/test_qsoc_shared-XXXXXX");
        QVERIFY(directory.isValid());
        for (bool finalShared : {false, true}) {
            const auto path = sharedTickFst(directory, finalShared, !kind.contains("companion"));
            if (kind.contains("wrapped"))
                wrapFst(path);
            const auto requested = sftp ? QDir(fixture.workDir()).relativeFilePath(path) : path;
            auto      *remote    = sftp ? &connection : nullptr;
            QSocToolWaveformRead warm(nullptr, nullptr, remote);
            const auto           request = [&](quint64 start) {
                return json{
                    {"file_path", requested.toStdString()},
                    {"signals", {"top.clk*", "top.bus"}},
                    {"start_time", start},
                    {"end_time", start == 6 ? 8 : start}};
            };
            const auto first = read(warm, request(6));
            QCOMPARE(first["values_at_start"]["top.clk"], json("1"));
            QCOMPARE(first["values_at_start"]["top.bus[3:0]"], json("0001"));
            const auto after = read(warm, request(11));
            QVERIFY(after["cache"]["reader_reused"].get<bool>());
            if (finalShared) {
                QCOMPARE(after["values_at_start"]["top.clk"], json("1"));
                QCOMPARE(after["values_at_start"]["top.bus[3:0]"], json("0111"));
                QVERIFY(after["events"].empty());
            } else {
                QCOMPARE(after["values_at_start"]["top.clk"], json("0"));
                QCOMPARE(after["values_at_start"]["top.bus[3:0]"], json("1000"));
                QCOMPARE(after["events"].size(), std::size_t(3));
                for (const auto &event : after["events"]) {
                    QCOMPARE(event["time"], json(11));
                    if (event["signal"] != "top.bus[3:0]")
                        QCOMPARE(event["edge"], json("falling"));
                }
            }
            const auto tick = read(warm, request(10));
            QVERIFY(tick["cache"]["reader_reused"].get<bool>());
            QCOMPARE(tick["values_at_start"]["top.clk"], json("1"));
            QCOMPARE(tick["values_at_start"]["top.bus[3:0]"], json("0111"));
            QCOMPARE(tick["events"].size(), std::size_t(18));
            for (const auto &name : {"top.clk", "top.clk_alias", "top.bus[3:0]"}) {
                json values = json::array();
                for (const auto &event : tick["events"]) {
                    QCOMPARE(event["time"], json(10));
                    if (event["signal"] == name)
                        values.push_back(event["value"]);
                }
                const json expected = QString::fromLatin1(name).contains("bus")
                                          ? json({"0010", "0011", "0100", "0101", "0110", "0111"})
                                          : json({"0", "1", "0", "1", "0", "1"});
                QCOMPARE(values, expected);
            }
            QSocToolWaveformRead cold(nullptr, nullptr, remote);
            const auto           coldAfter = read(cold, request(11));
            QVERIFY(!coldAfter["cache"]["reader_reused"].get<bool>());
            cold.abort();
            const auto coldTick = read(cold, request(10));
            QVERIFY(!coldTick["cache"]["reader_reused"].get<bool>());
            for (const auto *field :
                 {"values_at_start", "events", "window_complete", "values_at_start_complete"}) {
                QCOMPARE(after[field], coldAfter[field]);
                QCOMPARE(tick[field], coldTick[field]);
            }
        }
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        if (sftp)
            QCOMPARE(fixture.execRequests(), 0);
#endif
    }

    void localCompanionVersionAndEmbeddedPrecedence()
    {
        for (bool external : {false, true}) {
            QTemporaryDir directory;
            const auto    path = nativeFst(directory, !external);
            if (!external)
                writeFile(directory, "sample.fst.hier", "unused hierarchy");
            QSocToolWaveformRead tool;
            auto                 first = read(
                tool,
                {{"file_path", path.toStdString()}, {"signals", {"top.clk*"}}, {"max_events", 1}});
            QFile hierarchy(path + ".hier");
            QVERIFY(hierarchy.open(QIODevice::Append));
            QCOMPARE(hierarchy.write("changed"), qint64(7));
            hierarchy.close();
            const auto next = tool.execute(
                {{"file_path", path.toStdString()},
                 {"cursor", first["next_cursor"]},
                 {"max_events", 2000}});
            if (external) {
                QVERIFY2(next.contains("source_changed"), qPrintable(next));
                QVERIFY(tool.execute({{"file_path", path.toStdString()},
                                      {"cursor", first["next_cursor"]}})
                            .contains("cursor_expired"));
            } else {
                QVERIFY2(!next.startsWith("Error:"), qPrintable(next));
                QVERIFY(json::parse(next.toStdString())["window_complete"].get<bool>());
            }
        }
    }

    void localCompanionFailures()
    {
        for (bool missing : {false, true}) {
            QTemporaryDir directory;
            const auto    path = nativeFst(directory, false);
            if (missing)
                QVERIFY(QFile::remove(path + ".hier"));
            else
                writeFile(directory, "sample.fst.hier", "invalid hierarchy");
            QSocToolWaveformRead tool;
            const auto           result = tool.execute({{"file_path", path.toStdString()}});
            QVERIFY2(result.startsWith("Error:"), qPrintable(result));
            QVERIFY2(result.contains(missing ? "hierarchy_missing" : "status: 5"), qPrintable(result));
        }
    }

    void remoteWaveforms_data()
    {
        QTest::addColumn<QString>("kind");
        QTest::addColumn<bool>("readOnly");
        for (const auto &kind : {"vcd", "native", "wrapped", "companion", "wrapped-companion"}) {
            QTest::newRow(kind) << QString::fromLatin1(kind) << true;
            QTest::newRow(qPrintable(QString::fromLatin1(kind) + "-writable"))
                << QString::fromLatin1(kind) << false;
        }
    }

    void remoteWaveforms()
    {
        QFETCH(QString, kind);
        QFETCH(bool, readOnly);
        RemoteWaveformFixture fixture;
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        if (!fixture.start(readOnly) && fixture.missing())
            QSOC_TEST_MISSING_DEPENDENCY(fixture.error());
        QVERIFY2(fixture.error().isEmpty(), qPrintable(fixture.error()));
#else
        fixture.setExtraConfig(
            {readOnly ? QStringLiteral("ForceCommand internal-sftp -R")
                      : QStringLiteral("ForceCommand internal-sftp")});
        fixture.start();
        QSOC_REQUIRE_SSHD(fixture);
#endif
        QTemporaryDir directory(fixture.workDir() + "/wave-XXXXXX");
        QVERIFY(directory.isValid());
        const auto path = kind == "vcd" ? writeFile(directory, "sample.vcd", waveform())
                                        : nativeFst(directory, !kind.contains("companion"));
        if (kind.contains("wrapped"))
            wrapFst(path);
        QSocRemoteConnection connection;
        QString              error;
        QVERIFY2(bindRemote(fixture, connection, &error), qPrintable(error));
        QCOMPARE(connection.workspaceReadOnly(), readOnly);
        QCOMPARE(QFileInfo::exists(fixture.workDir() + "/.qsoc-agent"), !readOnly);
        QSocToolWaveformRead local;
        QSocToolWaveformRead remote(nullptr, nullptr, &connection);
        QVERIFY(remote.isReadOnly());
        const auto relative = QDir(fixture.workDir()).relativeFilePath(path).toStdString();
        const auto listing  = read(remote, {{"file_path", relative}});
        QCOMPARE(listing["total_signals"], json(4));
        QCOMPARE(listing["signals"][2]["name"], json("top.data[3:0]"));
        const json query
            = {{"signals", {"top.clk", "top.rst_n", "top.data"}},
               {"start_time", 7},
               {"end_time", 17}};
        auto localRequest          = query;
        localRequest["file_path"]  = path.toStdString();
        auto remoteRequest         = query;
        remoteRequest["file_path"] = relative;
        const auto expected        = read(local, localRequest);
        const auto window          = read(remote, remoteRequest);
        for (const auto *field :
             {"signals",
              "timescale",
              "values_at_start",
              "events",
              "window_complete",
              "values_at_start_complete"})
            QCOMPARE(window[field], expected[field]);
        QCOMPARE(window["values_at_start"]["top.clk"], json("1"));
        QCOMPARE(window["values_at_start"]["top.rst_n"], json("0"));
        QCOMPARE(window["values_at_start"]["top.data[3:0]"], json("0000"));
        QCOMPARE(window["events"].size(), std::size_t(4));
        QCOMPARE(window["events"][0]["time"], json(10));
        bool busChange = false;
        for (const auto &event : window["events"]) {
            if (event["signal"] == "top.data[3:0]") {
                QCOMPARE(event["value"], json("10xz"));
                busChange = true;
            }
        }
        QVERIFY(busChange);
        if (kind != "vcd") {
            auto overlap          = remoteRequest;
            overlap["start_time"] = 8;
            const auto warm       = read(remote, overlap);
            QVERIFY(warm["cache"]["reader_reused"].get<bool>());
            QVERIFY(warm["scan_bytes"].get<quint64>() < window["scan_bytes"].get<quint64>());
            if (kind.contains("wrapped"))
                QCOMPARE(warm["cache"]["wrapper_cold_rewinds"], json("0"));
            QSocToolWaveformRead cold;
            overlap["file_path"] = path.toStdString();
            const auto reference = read(cold, overlap);
            for (const auto *field :
                 {"signals", "timescale", "values_at_start", "events", "window_complete"})
                QCOMPARE(warm[field], reference[field]);
        }
        auto page
            = read(remote, {{"file_path", relative}, {"signals", {"top.clk*"}}, {"max_events", 1}});
        json events = page["events"];
        while (page.contains("next_cursor")) {
            page = read(
                remote,
                {{"file_path", relative}, {"cursor", page["next_cursor"]}, {"max_events", 1}});
            for (const auto &event : page["events"])
                events.push_back(event);
        }
        QCOMPARE(events.size(), std::size_t(10));
        constexpr std::array<quint64, 5> expectedTimes{0, 5, 10, 15, 25};
        for (std::size_t index = 0; index < events.size(); ++index) {
            QCOMPARE(events[index]["time"], json(expectedTimes[index / 2]));
            QCOMPARE(events[index]["value"], json(index / 2 % 2 == 0 ? "0" : "1"));
        }
        auto localPage = read(
            local,
            {{"file_path", path.toStdString()}, {"signals", {"top.clk*"}}, {"max_events", 1}});
        json localEvents = localPage["events"];
        while (localPage.contains("next_cursor")) {
            localPage = read(
                local,
                {{"file_path", path.toStdString()},
                 {"cursor", localPage["next_cursor"]},
                 {"max_events", 1}});
            for (const auto &event : localPage["events"])
                localEvents.push_back(event);
        }
        QCOMPARE(events, localEvents);
        QVERIFY(page["window_complete"].get<bool>());
        QCOMPARE(page["values_at_start"]["top.clk_alias"], json("0"));
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        QCOMPARE(fixture.execRequests(), 0);
#endif
    }

    void completedRemoteCachesReleaseHandles_data()
    {
        QTest::addColumn<bool>("fst");
        QTest::addColumn<bool>("wrapped");
        QTest::newRow("nine-vcd") << false << false;
        QTest::newRow("nine-native-and-companion") << true << false;
        QTest::newRow("nine-wrapped-and-companion") << true << true;
    }

    void completedRemoteCachesReleaseHandles()
    {
        QFETCH(bool, fst);
        QFETCH(bool, wrapped);
        RemoteWaveformFixture fixture;
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        if (!fixture.start(true) && fixture.missing())
            QSOC_TEST_MISSING_DEPENDENCY(fixture.error());
        QVERIFY2(fixture.error().isEmpty(), qPrintable(fixture.error()));
#else
        fixture.setExtraConfig({QStringLiteral("ForceCommand internal-sftp -R")});
        fixture.start();
        QSOC_REQUIRE_SSHD(fixture);
#endif
        QSocRemoteConnection connection;
        QString              error;
        QVERIFY2(bindRemote(fixture, connection, &error), qPrintable(error));
        std::vector<std::unique_ptr<QTemporaryDir>>        directories;
        std::vector<std::unique_ptr<QSocToolWaveformRead>> tools;
        std::vector<json>                                  requests;
        std::vector<json>                                  cold;
        QString                                            firstPath;
        QString                                            firstRemotePath;
        for (int index = 0; index < 9; ++index) {
            auto directory = std::make_unique<QTemporaryDir>(fixture.workDir() + "/idle-XXXXXX");
            QVERIFY(directory->isValid());
            const auto path = fst ? nativeFst(*directory, index % 2 != 0)
                                  : writeFile(*directory, "events.vcd", waveform());
            if (wrapped)
                wrapFst(path);
            if (index == 0) {
                firstPath       = path;
                firstRemotePath = remoteFilePath(fixture, path);
            }
            const auto relative = QDir(fixture.workDir()).relativeFilePath(path).toStdString();
            auto       tool = std::make_unique<QSocToolWaveformRead>(nullptr, nullptr, &connection);
            const json request
                = {{"file_path", relative},
                   {"signals", {"top.clk*"}},
                   {"start_time", 7},
                   {"end_time", 17}};
            const auto result = read(*tool, request);
            QVERIFY(result["window_complete"].get<bool>());
            QCOMPARE(result["values_at_start"]["top.clk"], json("1"));
            QCOMPARE(result["events"].size(), std::size_t(4));
            const auto warm = read(*tool, request);
            if (fst) {
                QVERIFY(!result["cache"]["reader_reused"].get<bool>());
                QVERIFY(warm["cache"]["reader_reused"].get<bool>());
            } else {
                QVERIFY(warm["scan_bytes"].get<quint64>() < result["scan_bytes"].get<quint64>());
            }
            directories.push_back(std::move(directory));
            tools.push_back(std::move(tool));
            requests.push_back(request);
            cold.push_back(result);
        }
        std::vector<std::unique_ptr<QSocSftpClient::ReadFile>> probes;
        for (int index = 0; index < 6; ++index) {
            auto handle = connection.sftp()->openReadFile(firstRemotePath, &error);
            QVERIFY2(handle, qPrintable(error));
            probes.push_back(std::move(handle));
        }
        probes.clear();
        const auto retained = read(*tools.back(), requests.back());
        if (fst)
            QVERIFY(retained["cache"]["reader_reused"].get<bool>());
        else
            QVERIFY(
                retained["scan_bytes"].get<quint64>() < cold.back()["scan_bytes"].get<quint64>());
        const auto reclaimed = read(*tools.front(), requests.front());
        if (fst)
            QVERIFY(!reclaimed["cache"]["reader_reused"].get<bool>());
        else
            QCOMPARE(reclaimed["scan_bytes"], cold.front()["scan_bytes"]);
        QSocToolWaveformRead active(nullptr, nullptr, &connection);
        const json           activeRequest
            = {{"file_path", requests.front()["file_path"]},
               {"signals", {"top.clk*"}},
               {"max_events", 1}};
        const auto first = read(active, activeRequest);
        QVERIFY(first.contains("next_cursor"));
        for (std::size_t index = 0; index < tools.size(); ++index)
            QVERIFY(read(*tools[index], requests[index])["window_complete"].get<bool>());
        const json continuation
            = {{"file_path", activeRequest["file_path"]}, {"cursor", first["next_cursor"]}};
        const auto rest = collect(active, continuation);
        QCOMPARE(first["events"].size() + rest["events"].size(), std::size_t(10));
        QVERIFY(rest["window_complete"].get<bool>());
        QVERIFY(active.execute(continuation).contains("cursor_expired"));
        if (fst) {
            QSocToolWaveformRead localOne;
            QSocToolWaveformRead localTwo;
            auto                 localRequest = requests.front();
            localRequest["file_path"]         = firstPath.toStdString();
            QVERIFY(read(localOne, localRequest)["window_complete"].get<bool>());
            QVERIFY(read(localTwo, localRequest)["window_complete"].get<bool>());
            for (int index = 0; index < 8; ++index) {
                auto handle = connection.sftp()->openReadFile(firstRemotePath, &error);
                QVERIFY2(handle, qPrintable(error));
                probes.push_back(std::move(handle));
            }
            probes.clear();
            const auto reopened = read(active, activeRequest);
            QVERIFY(reopened.contains("next_cursor"));
            active.abort();
        }
        QVERIFY(!QFileInfo::exists(fixture.workDir() + "/.qsoc-agent"));
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        QCOMPARE(fixture.execRequests(), 0);
#endif
    }

    void remoteSourceLifecycle_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::addColumn<QString>("kind");
        for (const auto &kind : {"native", "wrapped", "companion", "wrapped-companion"}) {
            for (const auto &mode :
                 {"main-change", "hierarchy-change", "embedded-sidecar", "close", "adopt", "cancel"}) {
                const bool companion = QString::fromLatin1(kind).contains("companion");
                if ((QString::fromLatin1(mode) == "hierarchy-change" && !companion)
                    || (QString::fromLatin1(mode) == "embedded-sidecar" && companion))
                    continue;
                QTest::newRow(qPrintable(QString("%1-%2").arg(kind, mode)))
                    << QString::fromLatin1(mode) << QString::fromLatin1(kind);
            }
        }
    }

    void remoteSourceLifecycle()
    {
        QFETCH(QString, mode);
        QFETCH(QString, kind);
        RemoteWaveformFixture fixture;
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        if (!fixture.start(true) && fixture.missing())
            QSOC_TEST_MISSING_DEPENDENCY(fixture.error());
        QVERIFY2(fixture.error().isEmpty(), qPrintable(fixture.error()));
#else
        fixture.setExtraConfig({QStringLiteral("ForceCommand internal-sftp -R")});
        fixture.start();
        QSOC_REQUIRE_SSHD(fixture);
#endif
        QTemporaryDir directory(fixture.workDir() + "/lifecycle-XXXXXX");
        QVERIFY(directory.isValid());
        const auto path = nativeFst(directory, !kind.contains("companion"));
        if (kind.contains("wrapped"))
            wrapFst(path);
        const auto remotePath = remoteFilePath(fixture, path);
        if (mode == "embedded-sidecar")
            writeFile(directory, "sample.fst.hier", "unused");
        QSocRemoteConnection connection;
        QString              error;
        QVERIFY2(bindRemote(fixture, connection, &error), qPrintable(error));
        QSocToolWaveformRead tool(nullptr, nullptr, &connection);
        QSocToolRegistry     registry;
        registry.registerTool(&tool);
        QObject    owner;
        const auto request = json{
            {"file_path", remotePath.toStdString()}, {"signals", {"top.clk*"}}, {"max_events", 1}};
        const auto firstText = registry.executeTool("read_waveform", request, &owner);
        QVERIFY2(!firstText.startsWith("Error:"), qPrintable(firstText));
        const auto first = json::parse(firstText.toStdString());
        if (mode.endsWith("change") || mode == "embedded-sidecar") {
            QFile changed(path + (mode == "main-change" ? "" : ".hier"));
            QVERIFY(changed.open(QIODevice::Append));
            QCOMPARE(changed.write("changed"), qint64(7));
        } else if (mode == "close") {
            connection.sftp()->close();
        } else if (mode == "adopt") {
            QVERIFY2(bindRemote(fixture, connection, &error), qPrintable(error));
        } else {
            registry.abortCalls(&owner);
        }
        const json nextRequest
            = {{"file_path", remotePath.toStdString()},
               {"cursor", first["next_cursor"]},
               {"max_events", 2000}};
        const auto next = registry.executeTool("read_waveform", nextRequest, &owner);
        if (mode == "embedded-sidecar") {
            QVERIFY2(!next.startsWith("Error:"), qPrintable(next));
            QVERIFY(json::parse(next.toStdString())["window_complete"].get<bool>());
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
            QCOMPARE(fixture.execRequests(), 0);
#endif
            return;
        }
        const auto expected = mode.endsWith("change") ? "source_changed"
                              : mode == "cancel"      ? "cursor_expired"
                                                      : "source_expired";
        QVERIFY2(next.contains(expected), qPrintable(next));
        QVERIFY(
            registry.executeTool("read_waveform", nextRequest, &owner).contains("cursor_expired"));
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
        QCOMPARE(fixture.execRequests(), 0);
#endif
    }

    void listHierarchicalSignals()
    {
        QTemporaryDir        dir;
        const auto           path = writeFile(dir, "sample.vcd", waveform());
        QSocToolWaveformRead tool;
        QVERIFY(tool.isReadOnly());
        const auto result = read(tool, {{"file_path", path.toStdString()}});
        QCOMPARE(result["timescale"], json("1ns"));
        QCOMPARE(result["total_signals"], json(4));
        QCOMPARE(result["signals"][2]["name"], json("top.data[3:0]"));
        QCOMPARE(result["signals"][2]["width"], json(4));
        QVERIFY(!result["truncated"].get<bool>());
    }

    void valuesAndEdgesInWindow()
    {
        QTemporaryDir        dir;
        const auto           path = writeFile(dir, "sample.vcd", waveform());
        QSocToolWaveformRead tool;
        const auto           result = read(
            tool,
            {{"file_path", path.toStdString()},
             {"signals", {"top.clk", "top.rst_n", "top.data"}},
             {"start_time", 7},
             {"end_time", 17}});
        QCOMPARE(result["values_at_start"]["top.clk"], json("1"));
        QCOMPARE(result["values_at_start"]["top.rst_n"], json("0"));
        QCOMPARE(result["values_at_start"]["top.data[3:0]"], json("0000"));
        QCOMPARE(result["events"].size(), std::size_t(4));
        QCOMPARE(result["events"][0]["time"], json(10));
        QCOMPARE(result["events"][0]["edge"], json("falling"));
        QCOMPARE(result["events"][1]["edge"], json("rising"));
        QCOMPARE(result["events"][2]["value"], json("10xz"));
        QCOMPARE(result["events"][3]["time"], json(15));
        QVERIFY(result["window_complete"].get<bool>());
    }

    void aliasesAndSameTickChanges()
    {
        QTemporaryDir dir;
        auto          data = waveform();
        data += "#30 1! 0! 1!\n";
        const auto           path = writeFile(dir, "sample.vcd", data);
        QSocToolWaveformRead tool;
        const auto           result = read(
            tool,
            {{"file_path", path.toStdString()},
             {"signals", {"top.clk*"}},
             {"start_time", 30},
             {"end_time", 30}});
        QCOMPARE(result["signals"].size(), std::size_t(2));
        QCOMPARE(result["events"].size(), std::size_t(6));
        QCOMPARE(result["events"][2]["edge"], json("falling"));
        QCOMPARE(result["events"][5]["signal"], json("top.clk_alias"));
    }

    void eventLimitIsExplicit()
    {
        QTemporaryDir        dir;
        const auto           path = writeFile(dir, "sample.vcd", waveform());
        QSocToolWaveformRead tool;
        const auto           result = read(
            tool, {{"file_path", path.toStdString()}, {"signals", {"top.clk"}}, {"max_events", 1}});
        QCOMPARE(result["events"].size(), std::size_t(1));
        QVERIFY(result["truncated"].get<bool>());
        QVERIFY(!result["window_complete"].get<bool>());
    }

    void continuationPreservesEveryAliasAtTheSameTick()
    {
        QTemporaryDir dir;
        QByteArray    data(
            "$var wire 1 ! bit $end\n$var wire 1 ! alias $end\n"
            "$enddefinitions $end\n");
        json expected = json::array();
        bool previous = false;
        for (int index = 0; index < 9; ++index) {
            const bool value = index % 2 != 0;
            const int  tick  = index / 3;
            data += '#' + QByteArray::number(tick) + ' ' + (value ? "1!\n" : "0!\n");
            const char *edge = index == 0 ? "change" : previous ? "falling" : "rising";
            for (const char *name : {"bit", "alias"})
                expected.push_back(
                    {{"time", tick}, {"signal", name}, {"value", value ? "1" : "0"}, {"edge", edge}});
            previous = value;
        }
        const auto           path = writeFile(dir, "same_tick.vcd", data);
        QSocToolWaveformRead tool;
        json arguments = {{"file_path", path.toStdString()}, {"signals", {"*"}}, {"max_events", 1}};
        json observed  = json::array();
        json last;
        for (int page = 0; page < 24; ++page) {
            last = read(tool, arguments);
            if (page == 1)
                QVERIFY(tool.execute(arguments).contains("cursor_expired"));
            for (const auto &event : last["events"])
                observed.push_back(event);
            if (page == 0)
                QVERIFY(!last["values_at_start_complete"].get<bool>());
            if (last["window_complete"].get<bool>())
                break;
            QCOMPARE(last["limit_reason"], json("events"));
            arguments = {{"file_path", path.toStdString()}, {"cursor", last["next_cursor"]}};
        }
        QCOMPARE(observed, expected);
        QVERIFY(last["window_complete"].get<bool>());
        QVERIFY(last["values_at_start_complete"].get<bool>());
        QCOMPARE(last["values_at_start"]["bit"], json("0"));
        QCOMPARE(last["values_at_start"]["alias"], json("0"));
    }

    void continuationKeepsWindowAndRejectsChangedSource()
    {
        QTemporaryDir        dir;
        const auto           path = writeFile(dir, "sample.vcd", waveform());
        QSocToolWaveformRead tool;
        const auto           first = read(
            tool,
            {{"file_path", path.toStdString()},
             {"signals", {"top.clk"}},
             {"start_time", 7},
             {"end_time", 17},
             {"max_events", 1}});
        const json resume   = {{"file_path", path.toStdString()}, {"cursor", first["next_cursor"]}};
        json       wrong    = resume;
        wrong["start_time"] = 8;
        QVERIFY(tool.execute(wrong).contains("parameters do not match"));
        const auto second = read(tool, resume);
        QCOMPARE(second["events"].size(), std::size_t(1));
        QCOMPARE(second["events"][0]["time"], json(15));
        QCOMPARE(second["events"][0]["edge"], json("rising"));
        QCOMPARE(second["values_at_start"]["top.clk"], json("1"));
        QVERIFY(second["window_complete"].get<bool>());
        QVERIFY(tool.execute(resume).contains("cursor_expired"));

        const auto changed = read(
            tool, {{"file_path", path.toStdString()}, {"signals", {"top.clk"}}, {"max_events", 1}});
        QFile append(path);
        QVERIFY(append.open(QIODevice::Append));
        QCOMPARE(append.write("#40 1!\n"), qint64(7));
        append.close();
        QVERIFY(tool.execute({{"file_path", path.toStdString()}, {"cursor", changed["next_cursor"]}})
                    .contains("source_changed"));
    }

    void newQueryAndOwnerTeardownExpireContinuation()
    {
        QTemporaryDir        dir;
        const auto           path = writeFile(dir, "sample.vcd", waveform());
        QSocToolWaveformRead tool;
        const auto           first = read(
            tool, {{"file_path", path.toStdString()}, {"signals", {"top.clk"}}, {"max_events", 1}});
        read(tool, {{"file_path", path.toStdString()}});
        QVERIFY(tool.execute({{"file_path", path.toStdString()}, {"cursor", first["next_cursor"]}})
                    .contains("cursor_expired"));

        QSocToolRegistry registry;
        registry.registerTool(&tool);
        auto       owner = std::make_unique<QObject>();
        const auto text  = registry.executeTool(
            "read_waveform",
            {{"file_path", path.toStdString()}, {"signals", {"top.clk"}}, {"max_events", 1}},
            owner.get());
        const auto owned = json::parse(text.toStdString());
        QObject    otherOwner;
        QVERIFY(registry
                    .executeTool(
                        "read_waveform",
                        {{"file_path", path.toStdString()}, {"cursor", owned["next_cursor"]}},
                        &otherOwner)
                    .contains("different source or owner"));
        owner.reset();
        QVERIFY(tool.execute({{"file_path", path.toStdString()}, {"cursor", owned["next_cursor"]}})
                    .contains("cursor_expired"));
        const auto cancelled = read(
            tool, {{"file_path", path.toStdString()}, {"signals", {"top.clk"}}, {"max_events", 1}});
        registry.abortAll();
        QVERIFY(
            tool.execute({{"file_path", path.toStdString()}, {"cursor", cancelled["next_cursor"]}})
                .contains("cursor_expired"));
    }

    void outputLimitCanContinueWithoutDroppingWideEvents()
    {
        QTemporaryDir dir;
        QByteArray    data("$var wire 65535 ! bus $end\n$enddefinitions $end\n");
        QByteArray    bits(65535, '0');
        for (int tick = 0; tick < 24; ++tick) {
            bits[0] = tick % 2 == 0 ? '0' : '1';
            data += '#' + QByteArray::number(tick) + " b" + bits + " !\n";
        }
        const auto           path = writeFile(dir, "wide_pages.vcd", data);
        QSocToolWaveformRead tool;
        json                 arguments
            = {{"file_path", path.toStdString()}, {"signals", {"bus"}}, {"max_events", 2000}};
        int  expectedTick = 0;
        bool complete     = false;
        for (int page = 0; page < 4; ++page) {
            const QString text = tool.execute(arguments);
            QVERIFY2(!text.startsWith("Error:"), qPrintable(text));
            QVERIFY(text.toUtf8().size() <= 1024 * 1024);
            const auto result = json::parse(text.toStdString());
            for (const auto &event : result["events"]) {
                QCOMPARE(event["time"], json(expectedTick));
                QCOMPARE(event["value"].get<std::string>().front(), expectedTick % 2 == 0 ? '0' : '1');
                ++expectedTick;
            }
            complete = result["window_complete"].get<bool>();
            if (complete)
                break;
            QCOMPARE(result["limit_reason"], json("output"));
            arguments = {{"file_path", path.toStdString()}, {"cursor", result["next_cursor"]}};
        }
        QCOMPARE(expectedTick, 24);
        QVERIFY(complete);
    }

    void siblingOwnerCannotReplaceOrCancelPausedQuery()
    {
        QTemporaryDir        dir;
        const auto           path = writeFile(dir, "owners.vcd", waveform());
        QSocToolWaveformRead tool;
        QSocToolRegistry     registry;
        registry.registerTool(&tool);
        QObject    firstOwner;
        QObject    sibling;
        const json request
            = {{"file_path", path.toStdString()}, {"signals", {"top.clk"}}, {"max_events", 1}};
        const auto first = json::parse(
            registry.executeTool("read_waveform", request, &firstOwner).toStdString());
        QVERIFY(registry.executeTool("read_waveform", request, &sibling).contains("busy"));
        registry.abortCalls(&sibling);
        const auto next = json::parse(
            registry
                .executeTool(
                    "read_waveform",
                    {{"file_path", path.toStdString()}, {"cursor", first["next_cursor"]}},
                    &firstOwner)
                .toStdString());
        QCOMPARE(next["events"][0]["time"], json(5));
        registry.abortCalls(&firstOwner);
        QVERIFY(registry
                    .executeTool(
                        "read_waveform",
                        {{"file_path", path.toStdString()}, {"cursor", next["next_cursor"]}},
                        &firstOwner)
                    .contains("cursor_expired"));
        QVERIFY(!registry.executeTool("read_waveform", request, &sibling).startsWith("Error:"));
    }

    void parentRunStopExpiresPausedQueryWithoutCancellingItsSuccessor()
    {
        QTemporaryDir        dir;
        const auto           path = writeFile(dir, "stop.vcd", waveform());
        QSocToolWaveformRead tool;
        QSocToolRegistry     registry;
        registry.registerTool(&tool);
        QObject          owner;
        std::stop_source stop;
        const json       request
            = {{"file_path", path.toStdString()}, {"signals", {"top.clk"}}, {"max_events", 1}};
        const auto first = json::parse(
            registry.executeTool("read_waveform", request, &owner, {}, {}, {}, stop.get_token())
                .toStdString());
        std::thread stopper([&] { stop.request_stop(); });
        stopper.join();
        QVERIFY(registry
                    .executeTool(
                        "read_waveform",
                        {{"file_path", path.toStdString()}, {"cursor", first["next_cursor"]}},
                        &owner)
                    .contains("cursor_expired"));
        const auto successor = json::parse(
            registry.executeTool("read_waveform", request, &owner).toStdString());
        QCoreApplication::processEvents();
        const auto resumed = json::parse(
            registry
                .executeTool(
                    "read_waveform",
                    {{"file_path", path.toStdString()}, {"cursor", successor["next_cursor"]}},
                    &owner)
                .toStdString());
        QCOMPARE(resumed["events"][0]["time"], json(5));
    }

    void sameSizePathReplacementCannotReuseTheOpenedFile()
    {
        QTemporaryDir        dir;
        const auto           path     = writeFile(dir, "original.vcd", waveform());
        const auto           modified = QFileInfo(path).lastModified();
        QSocToolWaveformRead tool;
        const auto           first = read(
            tool, {{"file_path", path.toStdString()}, {"signals", {"top.clk"}}, {"max_events", 1}});
        auto bytes = waveform();
        bytes.replace("0! b0000", "1! b0000");
        const auto replacement = writeFile(dir, "replacement.vcd", bytes);
        QFile      other(replacement);
        QVERIFY(other.open(QIODevice::ReadWrite));
        QVERIFY(other.setFileTime(modified, QFileDevice::FileModificationTime));
        other.close();
        QCOMPARE(QFileInfo(path).size(), QFileInfo(replacement).size());
        QFile      original(path);
        const auto previous = dir.filePath("previous.vcd");
        const bool renamed  = original.rename(previous);
#ifdef Q_OS_WIN
        if (!renamed) {
            QCOMPARE(original.error(), QFileDevice::RenameError);
            const auto resumed = read(tool, {{"file_path", path.toStdString()}, {
                                                 "cursor",
                                                 first["next_cursor"]
                                             }});
            QCOMPARE(resumed["events"][0]["time"], json(5));
            QCOMPARE(resumed["events"][0]["value"], json("1"));
            tool.abort();
            QVERIFY(original.rename(previous));
            QVERIFY(QFile::rename(replacement, path));
            QVERIFY(tool.execute({{"file_path", path.toStdString()},
                                  { "cursor",
                                    resumed["next_cursor"] }})
                        .contains("cursor_expired"));
            const auto fresh = read(
                tool,
                {{"file_path", path.toStdString()}, {"signals", {"top.clk"}}, {"max_events", 1}});
            QCOMPARE(fresh["events"][0]["time"], json(0));
            QCOMPARE(fresh["events"][0]["value"], json("1"));
            return;
        }
#endif
        QVERIFY(renamed);
        QVERIFY(QFile::rename(replacement, path));
        QVERIFY(tool.execute({{"file_path", path.toStdString()}, {"cursor", first["next_cursor"]}})
                    .contains("source_changed"));
        const auto fresh = read(
            tool, {{"file_path", path.toStdString()}, {"signals", {"top.clk"}}, {"max_events", 1}});
        QCOMPARE(fresh["events"][0]["value"], json("1"));
    }

    void signalListPagination()
    {
        QTemporaryDir dir;
        QByteArray    data("$scope module top $end\n");
        for (int i = 0; i < 240; ++i) {
            data += "$var wire 1 id" + QByteArray::number(i) + " signal" + QByteArray::number(i)
                    + " $end\n";
        }
        data += "$upscope $end\n$enddefinitions $end\n";
        const auto           path = writeFile(dir, "many.vcd", data);
        QSocToolWaveformRead tool;
        const auto           first = read(tool, {{"file_path", path.toStdString()}});
        QCOMPARE(first["signals"].size(), std::size_t(200));
        QCOMPARE(first["next_signal_offset"], json(200));
        const auto second = read(tool, {{"file_path", path.toStdString()}, {"signal_offset", 200}});
        QCOMPARE(second["signals"].size(), std::size_t(40));
        QCOMPARE(second["signals"][0]["name"], json("top.signal200"));
        QVERIFY(!second["truncated"].get<bool>());
    }

    void invalidArguments()
    {
        QTemporaryDir        dir;
        const auto           path = writeFile(dir, "sample.vcd", waveform());
        QSocToolWaveformRead tool;
        for (const json &extra :
             {json{{"start_time", -1}},
              json{{"start_time", "7"}},
              json{{"start_time", 20}, {"end_time", 10}},
              json{{"max_events", 0}},
              json{{"max_events", 2001}},
              json{{"signals", json::array()}},
              json{{"signals", {"absent"}}}}) {
            json arguments         = extra;
            arguments["file_path"] = path.toStdString();
            QVERIFY2(tool.execute(arguments).startsWith("Error:"), arguments.dump().c_str());
        }
    }

    void rejectsMalformedAndOversizedTokens()
    {
        QTemporaryDir        dir;
        QSocToolWaveformRead tool;
        const auto           missing = writeFile(dir, "missing.vcd", "$scope module top $end\n");
        QVERIFY(tool.execute({{"file_path", missing.toStdString()}}).contains("enddefinitions"));
        const auto decreasing = writeFile(dir, "bad.vcd", waveform() + "#24 1!\n");
        QVERIFY(tool.execute({{"file_path", decreasing.toStdString()}, {"signals", {"top.clk"}}})
                    .contains("decreasing"));
        const auto huge = writeFile(dir, "huge.vcd", waveform() + QByteArray(70000, 'x'));
        QVERIFY(tool.execute({{"file_path", huge.toStdString()}, {"signals", {"top.clk"}}})
                    .contains("64 KiB"));
    }

    void resolvesWorkingDirectory()
    {
        QTemporaryDir dir;
        writeFile(dir, "sample.vcd", waveform());
        QSocPathContext paths;
        paths.setWorkingDir(dir.path());
        QSocToolWaveformRead tool(nullptr, &paths);
        QCOMPARE(read(tool, {{"file_path", "sample.vcd"}})["total_signals"], json(4));
    }

    void wideBusOutputHasByteLimit()
    {
        QTemporaryDir dir;
        QByteArray    data("$var wire 65535 ! bus $end\n$enddefinitions $end\n");
        QByteArray    bits(65535, '0');
        for (int tick = 0; tick < 24; ++tick) {
            bits[0] = tick % 2 == 0 ? '0' : '1';
            data += '#' + QByteArray::number(tick) + " b" + bits + " !\n";
        }
        const auto           path = writeFile(dir, "wide.vcd", data);
        QSocToolWaveformRead tool;
        const auto           text = tool.execute(
            {{"file_path", path.toStdString()}, {"signals", {"bus"}}, {"max_events", 2000}});
        const auto result = json::parse(text.toStdString());
        QVERIFY(text.toUtf8().size() <= 1024 * 1024);
        QVERIFY(result["truncated"].get<bool>());
        QVERIFY(!result["window_complete"].get<bool>());
        QVERIFY(result["events"].size() < std::size_t(24));
        for (const auto &event : result["events"])
            QCOMPARE(event["edge"], json("change"));
    }

    void onlyDigitalBitSignalsHaveEdges()
    {
        QTemporaryDir dir;
        const auto    path = writeFile(
            dir,
            "aliases.vcd",
            QByteArray(
                "$var wire 1 ! bit $end\n$var wire 4 ! bus_alias $end\n"
                "$var real 1 # analog $end\n$enddefinitions $end\n"
                "0! r0 #\n#1 1! r1 #\n"));
        QSocToolWaveformRead tool;
        const auto           result
            = read(tool, {{"file_path", path.toStdString()}, {"signals", {"*"}}, {"start_time", 1}});
        QCOMPARE(result["events"].size(), std::size_t(3));
        QCOMPARE(result["events"][0]["edge"], json("rising"));
        QCOMPARE(result["events"][1]["edge"], json("change"));
        QCOMPARE(result["events"][2]["edge"], json("change"));
        const auto bad = writeFile(
            dir,
            "invalid.vcd",
            QByteArray("$var wire 4 ! bus $end\n$enddefinitions $end\nb120x !\n"));
        QVERIFY(tool.execute({{"file_path", bad.toStdString()}, {"signals", {"bus"}}})
                    .contains("Invalid VCD value"));
    }

    void indexedSignalNamesMatchLiterally()
    {
        QTemporaryDir dir;
        const auto    path = writeFile(
            dir,
            "indexed.vcd",
            QByteArray(
                "$scope module top $end\n$var wire 1 ! data[3] $end\n"
                "$var wire 1 @ data3 $end\n"
                "$upscope $end\n$enddefinitions $end\n0! 0@\n#5 1! 1@\n"));
        QSocToolWaveformRead tool;
        const auto           result
            = read(tool, {{"file_path", path.toStdString()}, {"signals", {"top.data[3]"}}});
        QCOMPARE(result["signals"][0]["name"], json("top.data[3]"));
        QCOMPARE(result["signals"].size(), std::size_t(1));
        QCOMPARE(result["events"].size(), std::size_t(2));
        QCOMPARE(result["events"][1]["edge"], json("rising"));
    }

    void longListedNamesCanBeQueriedExactly()
    {
        QTemporaryDir    dir;
        const QByteArray reference(400, 'a');
        const auto       path = writeFile(
            dir,
            "long.vcd",
            QByteArray("$scope module top $end\n$var wire 1 ! ") + reference
                + " $end\n$upscope $end\n$enddefinitions $end\n0!\n#5 1!\n");
        QSocToolWaveformRead tool;
        const auto           listed = read(tool, {{"file_path", path.toStdString()}});
        const auto           name   = listed["signals"][0]["name"];
        const auto           queried
            = read(tool, {{"file_path", path.toStdString()}, {"signals", json::array({name})}});
        QCOMPARE(queried["signals"].size(), std::size_t(1));
        QCOMPARE(queried["events"].size(), std::size_t(2));
        QVERIFY(tool.execute({{"file_path", path.toStdString()},
                              {"signals", {name.get<std::string>() + "*"}}})
                    .contains("256 bytes"));
    }

    void separateBitDeclarationsKeepTheirIndices()
    {
        QTemporaryDir dir;
        const auto    path = writeFile(
            dir,
            "bits.vcd",
            QByteArray(
                "$scope module top $end\n$var wire 1 ! data [0] $end\n"
                "$var wire 1 @ data [ 1 ] $end\n$upscope $end\n"
                "$enddefinitions $end\n0! 1@\n#5 1! 0@\n"));
        QSocToolWaveformRead tool;
        const auto           listed = read(tool, {{"file_path", path.toStdString()}});
        QCOMPARE(listed["signals"][0]["name"], json("top.data[0]"));
        QCOMPARE(listed["signals"][1]["name"], json("top.data[1]"));
        const auto queried = read(
            tool, {{"file_path", path.toStdString()}, {"signals", {"top.data[0]", "top.data[1]"}}});
        QCOMPARE(queried["values_at_start"].size(), std::size_t(2));
        QCOMPARE(queried["values_at_start"]["top.data[0]"], json("0"));
        QCOMPARE(queried["values_at_start"]["top.data[1]"], json("1"));
        QCOMPARE(queried["events"].size(), std::size_t(4));
        QCOMPARE(queried["events"][2]["signal"], json("top.data[0]"));
        QCOMPARE(queried["events"][3]["signal"], json("top.data[1]"));
        QVERIFY(tool.execute({{"file_path", path.toStdString()}, {"signals", {"top.data"}}})
                    .contains("ambiguous"));
    }

    void vectorSlicesKeepTheirRanges()
    {
        QTemporaryDir dir;
        const auto    path = writeFile(
            dir,
            "slices.vcd",
            QByteArray(
                "$scope module top $end\n$var wire 4 ! data [3:0] $end\n"
                "$var wire 4 @ data [ 7 : 4 ] $end\n$upscope $end\n"
                "$enddefinitions $end\nb0000 ! b1111 @\n#5 b0011 ! b1100 @\n"));
        QSocToolWaveformRead tool;
        const auto           listed = read(tool, {{"file_path", path.toStdString()}});
        QCOMPARE(listed["signals"][0]["name"], json("top.data[3:0]"));
        QCOMPARE(listed["signals"][1]["name"], json("top.data[7:4]"));
        const auto queried = read(
            tool,
            {{"file_path", path.toStdString()}, {"signals", {"top.data[3:0]", "top.data[7:4]"}}});
        QCOMPARE(queried["values_at_start"]["top.data[3:0]"], json("0000"));
        QCOMPARE(queried["values_at_start"]["top.data[7:4]"], json("1111"));
        QCOMPARE(queried["events"][2]["signal"], json("top.data[3:0]"));
        QCOMPARE(queried["events"][3]["signal"], json("top.data[7:4]"));
        QVERIFY(tool.execute({{"file_path", path.toStdString()}, {"signals", {"top.data"}}})
                    .contains("ambiguous"));
        const auto globbed
            = read(tool, {{"file_path", path.toStdString()}, {"signals", {"top.data*"}}});
        QCOMPARE(globbed["signals"].size(), std::size_t(2));
    }

    void remoteRegistryCannotFallbackToLocalFiles()
    {
        QSocToolRegistry     local;
        QSocToolWaveformRead tool;
        local.registerTool(&tool);
        QSocToolRegistry remote;
        remote.setFallback(&local);
        QVERIFY(QSocToolRegistry::isWorkspaceBound(tool.getName()));
        QVERIFY(remote.getTool(tool.getName()) == nullptr);
    }
};

} // namespace

QSOC_TEST_MAIN(TestQSocToolWaveform)
#include "test_qsoctoolwaveform.moc"
