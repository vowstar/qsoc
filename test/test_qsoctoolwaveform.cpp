// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/tool/qsoctoolpath.h"
#include "agent/tool/qsoctoolwaveform.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

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

class TestQSocToolWaveform : public QObject
{
    Q_OBJECT

private slots:
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
