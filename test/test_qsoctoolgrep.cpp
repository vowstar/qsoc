// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocworkspacefs.h"
#include "agent/tool/qsoctoolgrep.h"
#include "qsoc_test.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtTest>

#include <functional>
#include <utility>

namespace {

bool writeFile(const QTemporaryDir &dir, const QString &path, const QByteArray &bytes)
{
    const QString absolute = dir.filePath(path);
    if (!QDir().mkpath(QFileInfo(absolute).absolutePath()))
        return false;
    QFile file(absolute);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

json search(QSocToolGrep *tool, const json &arguments)
{
    const QString output = tool->execute(arguments);
    const json    result = json::parse(output.toStdString(), nullptr, false);
    return result.is_discarded() ? json({{"error", output.toStdString()}}) : result;
}

class MemoryFs final : public QSocWorkspaceFs
{
public:
    QMap<QString, QByteArray> files;
    std::function<void()>     onRead;
    int                       unboundedReads    = 0;
    int                       boundedReads      = 0;
    int                       syntheticEntries  = 0;
    int                       unboundedLists    = 0;
    int                       enumeratedEntries = 0;
    QString                   root() const override { return QStringLiteral("/workspace"); }
    Result                    read(const QString &, QByteArray *, QString *) override
    {
        ++unboundedReads;
        return Result::Failed;
    }
    Result readLimited(const QString &path, qint64 maxBytes, QByteArray *bytes, QString *) override
    {
        ++boundedReads;
        if (!files.contains(path)) {
            if (syntheticEntries > 0 && path.startsWith(QStringLiteral("file_"))) {
                bytes->clear();
                return Result::Ok;
            }
            return Result::Absent;
        }
        if (files[path].size() > maxBytes)
            return Result::Failed;
        *bytes              = files[path];
        const auto callback = std::exchange(onRead, {});
        if (callback)
            callback();
        return Result::Ok;
    }
    Result list(const QString &, QList<Entry> *, QString *) override
    {
        ++unboundedLists;
        return Result::Failed;
    }
    Result listLimited(
        const QString &path,
        int            maxEntries,
        QList<Entry>  *entries,
        bool          *truncated,
        QString *) override
    {
        entries->clear();
        if (path != ".")
            return Result::Absent;
        *truncated = files.size() + syntheticEntries > maxEntries;
        for (auto it = files.cbegin(); it != files.cend() && entries->size() < maxEntries; ++it)
            entries->append({it.key(), true, it->size()});
        for (int i = 0; i < syntheticEntries && entries->size() < maxEntries; ++i)
            entries->append({QStringLiteral("file_%1.txt").arg(i), true, 0});
        enumeratedEntries += static_cast<int>(entries->size());
        return Result::Ok;
    }
    Result stat(const QString &path, Entry *entry, QString *) override
    {
        if (path == ".") {
            *entry = {".", false, -1};
            return Result::Ok;
        }
        if (!files.contains(path))
            return Result::Absent;
        *entry = {path, true, files[path].size()};
        return Result::Ok;
    }
    bool write(const QString &, const QByteArray &, QString *) override { return false; }
};

class Test : public QObject
{
    Q_OBJECT

private slots:
    void pathsLineNumbersAndContext()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeFile(dir, "rtl/clock.sv", "module clock;\r\nwire clk;\r\nendmodule\r\n"));
        QSocLocalWorkspaceFs fs([&] { return dir.path(); });
        QSocToolGrep         tool(nullptr, &fs);
        QVERIFY(tool.isReadOnly());
        const json result
            = search(&tool, {{"pattern", "clk"}, {"context", 1}, {"glob", "rtl/*.sv"}});
        QVERIFY2(!result.contains("error"), result.dump().c_str());
        QCOMPARE(result["matches"].size(), std::size_t(1));
        const auto &match = result["matches"][0];
        QCOMPARE(match["path"], json("rtl/clock.sv"));
        QCOMPARE(match["line"], json(2));
        QCOMPARE(match["text"], json("wire clk;"));
        QCOMPARE(match["before"][0]["line"], json(1));
        QCOMPARE(match["after"][0]["text"], json("endmodule"));
        QCOMPARE(result["files_searched"], json(1));
        QCOMPARE(result["truncated"], json(false));
    }

    void literalCaseAndHiddenFiles()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        QVERIFY(writeFile(dir, "clock.sv", "CLK[0]\nclk0\n"));
        QVERIFY(writeFile(dir, ".hidden/clock.sv", "clk[0]\n"));
        QVERIFY(writeFile(dir, "notes.txt", "clk[0]\n"));
        QSocLocalWorkspaceFs fs([&] { return dir.path(); });
        QSocToolGrep         tool(nullptr, &fs);
        json                 args
            = {{"pattern", "clk[0]"}, {"literal", true}, {"ignore_case", true}, {"glob", "*.sv"}};
        auto result = search(&tool, args);
        QCOMPARE(result["matches"].size(), std::size_t(1));
        QCOMPARE(result["matches"][0]["text"], json("CLK[0]"));
        args["include_hidden"] = true;
        result                 = search(&tool, args);
        QCOMPARE(result["matches"].size(), std::size_t(2));
        args["literal"] = false;
        result          = search(&tool, args);
        QCOMPARE(result["matches"].size(), std::size_t(1));
        QCOMPARE(result["matches"][0]["text"], json("clk0"));
    }

    void invalidArguments_data()
    {
        QTest::addColumn<QString>("arguments");
        QTest::newRow("missing-pattern") << QStringLiteral("{}");
        QTest::newRow("bad-regex") << QStringLiteral("{\"pattern\":\"[\"}");
        QTest::newRow("outside") << QStringLiteral(R"({"pattern":"clk","path":"../other"})");
        QTest::newRow("absolute") << QStringLiteral(R"({"pattern":"clk","path":"/other"})");
        QTest::newRow("bad-type") << QStringLiteral(R"({"pattern":"clk","context":"many"})");
        QTest::newRow("large-context") << QStringLiteral(R"({"pattern":"clk","context":6})");
        QTest::newRow("zero-limit") << QStringLiteral(R"({"pattern":"clk","max_results":0})");
    }

    void invalidArguments()
    {
        QFETCH(QString, arguments);
        MemoryFs     fs;
        QSocToolGrep tool(nullptr, &fs);
        QVERIFY(tool.execute(json::parse(arguments.toStdString())).startsWith("Error:"));
        QCOMPARE(fs.boundedReads, 0);
    }

    void boundedBackendAndBindingSwitch()
    {
        MemoryFs first;
        first.files["first.sv"] = "first clock\n";
        QSocToolGrep tool(nullptr, &first);
        auto         result = search(&tool, {{"pattern", "clock"}});
        QCOMPARE(result["matches"][0]["path"], json("first.sv"));
        QCOMPARE(first.boundedReads, 1);
        QCOMPARE(first.unboundedReads, 0);
        MemoryFs second;
        second.files["second.sv"] = "second clock\n";
        tool.setFilesystem(&second);
        result = search(&tool, {{"pattern", "clock"}});
        QCOMPARE(result["matches"][0]["path"], json("second.sv"));
        QCOMPARE(first.boundedReads, 1);
        QCOMPARE(second.boundedReads, 1);
        QCOMPARE(second.unboundedReads, 0);
    }

    void bindingChangedDuringReadDiscardsOldMatches()
    {
        MemoryFs first;
        MemoryFs second;
        first.files["first.sv"]   = "old clock\n";
        second.files["second.sv"] = "new clock\n";
        QCOMPARE(first.identity(), second.identity());
        QSocToolGrep tool(nullptr, &first);
        int          callbacks = 0;
        first.onRead           = [&] {
            ++callbacks;
            tool.setFilesystem(&second);
        };
        QCOMPARE(
            tool.execute({{"pattern", "clock"}}),
            QStringLiteral("Error: workspace changed during search"));
        QCOMPARE(callbacks, 1);
        QCOMPARE(first.boundedReads, 1);
        QCOMPARE(second.boundedReads, 0);
        const auto result = search(&tool, {{"pattern", "clock"}});
        QCOMPARE(result["matches"].size(), std::size_t(1));
        QCOMPARE(result["matches"][0]["path"], json("second.sv"));
        QCOMPARE(result["matches"][0]["text"], json("new clock"));
        QCOMPARE(callbacks, 1);
        QCOMPARE(second.boundedReads, 1);
    }

    void resultAndScanLimits()
    {
        MemoryFs fs;
        fs.files["clock.sv"] = "clk\nclk\nclk\n";
        QSocToolGrep tool(nullptr, &fs);
        auto         result = search(&tool, {{"pattern", "clk"}, {"max_results", 1}});
        QCOMPARE(result["matches"].size(), std::size_t(1));
        QCOMPARE(result["truncated"], json(true));
        fs.syntheticEntries  = 100000000;
        fs.enumeratedEntries = 0;
        result               = search(&tool, {{"pattern", "clk"}, {"glob", "*.none"}});
        QCOMPARE(result["truncated"], json(true));
        QVERIFY(result["entries_scanned"].get<int>() <= 10000);
        QVERIFY(fs.enumeratedEntries <= 10000);
        QCOMPARE(fs.unboundedLists, 0);
        fs.syntheticEntries = 600;
        result              = search(&tool, {{"pattern", "clk"}});
        QCOMPARE(result["truncated"], json(true));
        QCOMPARE(result["files_searched"], json(512));
    }

    void largeAndBinaryFilesAreReported()
    {
        MemoryFs fs;
        fs.files["large.sv"]   = QByteArray(2 * 1024 * 1024 + 1, 'x');
        fs.files["binary.dat"] = QByteArray(16, '\0') + "clk";
        fs.files["text.sv"]    = "clk\n";
        QSocToolGrep tool(nullptr, &fs);
        const auto   result = search(&tool, {{"pattern", "clk"}});
        QCOMPARE(result["matches"].size(), std::size_t(1));
        QCOMPARE(result["matches"][0]["path"], json("text.sv"));
        QCOMPARE(result["skipped_files"], json(2));
        QCOMPARE(result["truncated"], json(true));
        QCOMPARE(fs.boundedReads, 2);
    }

    void longLinesAndOutputAreBounded()
    {
        MemoryFs fs;
        fs.files["clock.sv"] = QByteArray(64 * 1024 + 1, 'x') + "\nclk\n";
        QSocToolGrep tool(nullptr, &fs);
        auto         result = search(&tool, {{"pattern", "clk"}});
        QCOMPARE(result["matches"].size(), std::size_t(1));
        QCOMPARE(result["matches"][0]["line"], json(2));
        QCOMPARE(result["skipped_lines"], json(1));
        fs.files["clock.sv"].clear();
        for (int i = 0; i < 100; ++i)
            fs.files["clock.sv"] += "clk " + QByteArray(600, 'x') + '\n';
        const QString output = tool.execute(
            {{"pattern", "clk"}, {"context", 5}, {"max_results", 500}});
        result = json::parse(output.toStdString());
        QVERIFY(output.toUtf8().size() < 128 * 1024);
        QCOMPARE(result["truncated"], json(true));
        QCOMPARE(result["matches"][0]["text_truncated"], json(true));
    }

    void boundedDirectoryEnumeration()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        for (int i = 0; i < 40; ++i)
            QVERIFY(writeFile(dir, QStringLiteral("file_%1.sv").arg(i), "clk\n"));
        QSocLocalWorkspaceFs          fs([&] { return dir.path(); });
        QList<QSocWorkspaceFs::Entry> entries;
        QString                       error;
        bool                          truncated = false;
        QCOMPARE(fs.listLimited(".", 7, &entries, &truncated, &error), QSocWorkspaceFs::Result::Ok);
        QCOMPARE(entries.size(), qsizetype(7));
        QVERIFY(truncated);
        QCOMPARE(fs.listLimited(".", 40, &entries, &truncated, &error), QSocWorkspaceFs::Result::Ok);
        QCOMPARE(entries.size(), qsizetype(40));
        QVERIFY(!truncated);
        QCOMPARE(fs.listLimited(".", 0, &entries, &truncated, &error), QSocWorkspaceFs::Result::Ok);
        QVERIFY(entries.isEmpty());
        QVERIFY(truncated);
        QCOMPARE(fs.list(".", &entries, &error), QSocWorkspaceFs::Result::Ok);
        QCOMPARE(entries.size(), qsizetype(40));
    }

    void boundedReadAndSymlinkDirectories()
    {
        QTemporaryDir dir;
        QTemporaryDir outside;
        QVERIFY(dir.isValid());
        QVERIFY(outside.isValid());
        QVERIFY(writeFile(dir, "clock.sv", "clk\n"));
        QSocLocalWorkspaceFs fs([&] { return dir.path(); });
        QByteArray           bytes;
        QString              error;
        QCOMPARE(fs.readLimited("clock.sv", 3, &bytes, &error), QSocWorkspaceFs::Result::Failed);
        QVERIFY(bytes.isEmpty());
        QCOMPARE(fs.readLimited("clock.sv", 4, &bytes, &error), QSocWorkspaceFs::Result::Ok);
        QCOMPARE(bytes, QByteArray("clk\n"));
#ifdef Q_OS_UNIX
        QVERIFY(writeFile(outside, "outside.sv", "outside-only\n"));
        QVERIFY(QFile::link(outside.filePath("outside.sv"), dir.filePath("outside.sv")));
        QCOMPARE(fs.readLimited("outside.sv", 1024, &bytes, &error), QSocWorkspaceFs::Result::Failed);
        QVERIFY(bytes.isEmpty());
        QVERIFY(QFile::link(dir.path(), dir.filePath("loop")));
        QSocToolGrep tool(nullptr, &fs);
        const auto   result = search(&tool, {{"pattern", "clk"}});
        QCOMPARE(result["matches"].size(), std::size_t(1));
        QVERIFY(result["entries_scanned"].get<int>() < 10);
#endif
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsoctoolgrep.moc"
