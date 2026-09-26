// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocagentconfig.h"
#include "agent/qsoccontextrestore.h"
#include "agent/qsocfilereadstate.h"
#include "agent/qsoctool.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <QSignalSpy>
#include <QtCore>
#include <QtTest>

using json = nlohmann::json;

namespace {

class Test : public QObject
{
    Q_OBJECT

    /* Deterministic ascii-ish estimator: ~4 chars per token. */
    static int estimate(const QString &text) { return (static_cast<int>(text.size()) / 4) + 1; }

    static QSocContextRestoreBuilder::Inputs baseInputs()
    {
        QSocContextRestoreBuilder::Inputs in;
        in.estimateTokens    = &Test::estimate;
        in.maxFiles          = QSocContextRestoreBuilder::kMaxFilesToRestore;
        in.fileBudget        = QSocContextRestoreBuilder::kFileTokenBudget;
        in.maxTokensPerFile  = QSocContextRestoreBuilder::kMaxTokensPerFile;
        in.maxTokensPerSkill = QSocContextRestoreBuilder::kMaxTokensPerSkill;
        in.skillsBudget      = QSocContextRestoreBuilder::kSkillsTokenBudget;
        return in;
    }

private slots:
    void recentFileCalls_data()
    {
        QTest::addColumn<QString>("name");
        QTest::addColumn<bool>("invoke");
        for (const auto &name : {"read_file", "write_file", "edit_file"}) {
            QTest::newRow(name) << QString::fromLatin1(name) << false;
            QTest::newRow((QByteArray("invoke-") + name).constData())
                << QString::fromLatin1(name) << true;
        }
    }

    void recentFileCalls()
    {
        QFETCH(QString, name);
        QFETCH(bool, invoke);
        json arguments = {{"file_path", "retained.txt"}};
        if (invoke) {
            arguments
                = {{"name", name.toStdString()},
                   {"schema_version", "historical"},
                   {"arguments_json", arguments.dump()}};
            name = QStringLiteral("tool_invoke");
        }
        const json history = json::array(
            {{{"role", "assistant"},
              {"tool_calls",
               json::array(
                   {{{"function",
                      {{"name", name.toStdString()}, {"arguments", arguments.dump()}}}}})}}});
        QCOMPARE(
            QSocContextRestoreBuilder::recentFilePaths(history),
            QSet<QString>{QStringLiteral("retained.txt")});
        auto inputs           = baseInputs();
        inputs.candidatePaths = {QStringLiteral("retained.txt"), QStringLiteral("restore.txt")};
        inputs.excludedPaths  = QSocContextRestoreBuilder::recentFilePaths(history);
        QStringList reads;
        inputs.readFile = [&](const QString &path) -> std::optional<QString> {
            reads.append(path);
            return QStringLiteral("content");
        };
        const auto restored = QSocContextRestoreBuilder::build(inputs);
        QCOMPARE(reads, QStringList{QStringLiteral("restore.txt")});
        QCOMPARE(restored.readPaths(), reads);
    }

    void malformedRecentCalls_data()
    {
        QTest::addColumn<int>("kind");
        for (int kind = 0; kind < 7; ++kind)
            QTest::newRow(QByteArray::number(kind).constData()) << kind;
    }

    void malformedRecentCalls()
    {
        QFETCH(int, kind);
        QString name = QStringLiteral("tool_invoke");
        json    arguments
            = {{"name", "read_file"},
               {"schema_version", "historical"},
               {"arguments_json", json({{"file_path", "ignored.txt"}}).dump()}};
        if (kind == 0)
            arguments.erase("schema_version");
        else if (kind == 1)
            arguments["arguments_json"] = "[1,2]";
        else if (kind == 2)
            arguments["name"] = "tool_invoke";
        else if (kind == 3)
            name = "unknown_wrapper";
        else if (kind == 4)
            arguments["arguments_json"]
                = json({{"file_path", QString(600000, QLatin1Char('x')).toStdString()}}).dump();
        else if (kind == 5) {
            json nested = {{"file_path", "ignored.txt"}};
            for (int depth = 0; depth < 65; ++depth)
                nested = {{"nested", nested}};
            arguments["arguments_json"] = nested.dump();
        } else
            arguments["arguments_json"] = json({{"file_path", 42}}).dump();
        const json history = json::array(
            {{{"tool_calls",
               json::array(
                   {{{"function",
                      {{"name", name.toStdString()}, {"arguments", arguments.dump()}}}}})}}});
        QVERIFY(QSocContextRestoreBuilder::recentFilePaths(history).isEmpty());
    }

    void testTotalBudgetLimitsAllSources()
    {
        auto inputs            = baseInputs();
        inputs.totalBudget     = 160;
        inputs.candidatePaths  = {QStringLiteral("large.txt")};
        inputs.readFileBounded = [](const QString &) {
            return QSocContextRestoreBuilder::FileRead{true, true, QStringLiteral("prefix")};
        };
        inputs.skillNames = {QStringLiteral("short")};
        inputs.readSkill  = [](const QString &) -> std::optional<QString> {
            return QStringLiteral("skill body");
        };
        for (int index = 0; index < 100; ++index) {
            inputs.agents.append(
                {QString::number(index), QStringLiteral("task"), QString(100, QLatin1Char('x'))});
        }
        const auto result = QSocContextRestoreBuilder::build(inputs);
        QCOMPARE(result.files.size(), 1);
        QCOMPARE(result.files.front().mode, QSocContextRestore::Mode::Referenced);
        QVERIFY(!result.files.front().attachmentText.contains(QStringLiteral("prefix")));
        QVERIFY(result.agents.size() < inputs.agents.size());
        const auto messages = QSocContextRestoreBuilder::toMessages(result);
        qint64     tokens   = 0;
        for (const auto &message : messages) {
            tokens += estimate(QString::fromStdString(message["content"].get<std::string>())) + 4;
        }
        QVERIFY(tokens <= inputs.totalBudget);
    }

    void testZeroBudgetDoesNotRead()
    {
        auto inputs            = baseInputs();
        inputs.totalBudget     = 0;
        inputs.candidatePaths  = {QStringLiteral("file.txt")};
        inputs.skillNames      = {QStringLiteral("skill")};
        int reads              = 0;
        inputs.readFileBounded = [&](const QString &) {
            ++reads;
            return QSocContextRestoreBuilder::FileRead{};
        };
        inputs.readSkill = [&](const QString &) -> std::optional<QString> {
            ++reads;
            return QString();
        };
        QVERIFY(QSocContextRestoreBuilder::build(inputs).isEmpty());
        QCOMPARE(reads, 0);
    }

    /* 1. QSocFileReadState recency ordering + cap, re-read bumps to front. */
    void testRecencyOrdering()
    {
        QSocFileReadState state;
        state.recordRead(QStringLiteral("/a"), QStringLiteral("x"));
        state.recordRead(QStringLiteral("/b"), QStringLiteral("y"));
        state.recordRead(QStringLiteral("/c"), QStringLiteral("z"));
        QCOMPARE(
            state.pathsByRecencyDesc(0),
            (QList<QString>{QStringLiteral("/c"), QStringLiteral("/b"), QStringLiteral("/a")}));

        state.recordRead(QStringLiteral("/a"), QStringLiteral("x2"));
        QCOMPARE(
            state.pathsByRecencyDesc(2),
            (QList<QString>{QStringLiteral("/a"), QStringLiteral("/c")}));
    }

    /* 2. Small file -> Read (content inlined, line count); big -> Referenced. */
    void testFileReadVsReferenced()
    {
        auto in             = baseInputs();
        in.maxTokensPerFile = 20; /* ~80 chars threshold */
        in.candidatePaths   = {QStringLiteral("/small"), QStringLiteral("/big")};
        const QString big   = QString(QStringLiteral("Z")).repeated(400);
        in.readFile         = [&](const QString &path) -> std::optional<QString> {
            if (path == QStringLiteral("/small")) {
                return QStringLiteral("alpha\nbeta\ngamma");
            }
            if (path == QStringLiteral("/big")) {
                return big;
            }
            return std::nullopt;
        };

        const QSocContextRestore out = QSocContextRestoreBuilder::build(in);
        QCOMPARE(out.files.size(), 2);

        const auto &small = out.files.at(0);
        QCOMPARE(small.mode, QSocContextRestore::Mode::Read);
        QCOMPARE(small.lines, 3);
        QVERIFY(small.attachmentText.contains(QStringLiteral("gamma")));

        const auto &bigItem = out.files.at(1);
        QCOMPARE(bigItem.mode, QSocContextRestore::Mode::Referenced);
        QVERIFY(!bigItem.attachmentText.contains(big)); /* no content inlined */

        QCOMPARE(out.readPaths(), QList<QString>{QStringLiteral("/small")});
        QCOMPARE(out.referencedPaths(), QList<QString>{QStringLiteral("/big")});
    }

    /* 3. At most maxFiles items even with more candidates. */
    void testMaxFilesCap()
    {
        auto in = baseInputs();
        for (int i = 0; i < 7; ++i) {
            in.candidatePaths.append(QStringLiteral("/f%1").arg(i));
        }
        in.readFile = [](const QString &) -> std::optional<QString> {
            return QStringLiteral("content");
        };
        const QSocContextRestore out = QSocContextRestoreBuilder::build(in);
        QCOMPARE(out.files.size(), QSocContextRestoreBuilder::kMaxFilesToRestore);
    }

    /* 4. Total file budget drops items that would overflow. */
    void testFileTotalBudget()
    {
        auto in           = baseInputs();
        in.candidatePaths = {QStringLiteral("/a"), QStringLiteral("/b"), QStringLiteral("/c")};
        in.readFile       = [](const QString &) -> std::optional<QString> {
            return QString(QStringLiteral("y")).repeated(100);
        };
        /* Each item (content + wrapper) is ~37 tokens; budget admits one. */
        in.fileBudget                = 50;
        const QSocContextRestore out = QSocContextRestoreBuilder::build(in);
        QCOMPARE(out.files.size(), 1);
    }

    /* 5. Excluded paths are skipped without consuming a slot. */
    void testExclusionSet()
    {
        auto in           = baseInputs();
        in.candidatePaths = {QStringLiteral("/keep"), QStringLiteral("/skip")};
        in.excludedPaths.insert(QStringLiteral("/skip"));
        in.readFile = [](const QString &) -> std::optional<QString> {
            return QStringLiteral("data");
        };
        const QSocContextRestore out = QSocContextRestoreBuilder::build(in);
        QCOMPARE(out.files.size(), 1);
        QCOMPARE(out.files.at(0).displayPath, QStringLiteral("/keep"));
    }

    /* 6. Skills: per-skill truncation + total budget + name join. */
    void testSkillTruncationAndBudget()
    {
        auto in                = baseInputs();
        in.maxTokensPerSkill   = 10; /* ~40 chars body cap */
        in.skillsBudget        = 25;
        in.skillNames          = {QStringLiteral("a"), QStringLiteral("b"), QStringLiteral("c")};
        const QString longBody = QString(QStringLiteral("k")).repeated(200);
        in.readSkill = [&](const QString &) -> std::optional<QString> { return longBody; };

        const QSocContextRestore out = QSocContextRestoreBuilder::build(in);
        QVERIFY(!out.skills.isEmpty());
        for (const auto &skill : out.skills) {
            QVERIFY(estimate(skill.attachmentText) <= in.maxTokensPerSkill + 6);
            QVERIFY(skill.attachmentText.contains(QStringLiteral("truncated")));
        }
        /* Budget should drop at least one of the three. */
        QVERIFY(out.skills.size() < 3);
    }

    /* 7. Running agents pass through to items with labels. */
    void testRunningAgents()
    {
        auto in = baseInputs();
        QSocContextRestoreBuilder::AgentRow
            row{.id      = QStringLiteral("ab12"),
                .label   = QStringLiteral("build rtl"),
                .summary = QStringLiteral("running")};
        in.agents.append(row);
        const QSocContextRestore out = QSocContextRestoreBuilder::build(in);
        QCOMPARE(out.agents.size(), 1);
        QCOMPARE(out.agentLabels(), QList<QString>{QStringLiteral("build rtl")});
        QVERIFY(out.agents.at(0).attachmentText.contains(QStringLiteral("ab12")));
    }

    /* 8. Disabled -> empty payload; toMessages stays empty. */
    void testDisabledNoop()
    {
        auto in           = baseInputs();
        in.enabled        = false;
        in.candidatePaths = {QStringLiteral("/a")};
        in.readFile       = [](const QString &) -> std::optional<QString> {
            return QStringLiteral("data");
        };
        const QSocContextRestore out = QSocContextRestoreBuilder::build(in);
        QVERIFY(out.isEmpty());
        QVERIFY(QSocContextRestoreBuilder::toMessages(out).empty());
    }

    /* toMessages: one message per file, one for skills, one for agents. */
    void testToMessages()
    {
        auto in           = baseInputs();
        in.candidatePaths = {QStringLiteral("/a"), QStringLiteral("/b")};
        in.readFile       = [](const QString &) -> std::optional<QString> {
            return QStringLiteral("body");
        };
        in.skillNames = {QStringLiteral("s1")};
        in.readSkill  = [](const QString &) -> std::optional<QString> {
            return QStringLiteral("skill body");
        };
        in.agents.append({QStringLiteral("id1"), QStringLiteral("lbl"), QStringLiteral("running")});

        const QSocContextRestore out  = QSocContextRestoreBuilder::build(in);
        const json               msgs = QSocContextRestoreBuilder::toMessages(out);
        /* 2 files + 1 skills block + 1 agents block = 4 messages. */
        QCOMPARE(static_cast<int>(msgs.size()), 4);
        for (const auto &msg : msgs) {
            QCOMPARE(QString::fromStdString(msg["role"].get<std::string>()), QStringLiteral("user"));
            QVERIFY(msg["content"].is_string());
        }
    }
    /* Integration: a compaction appends the provider's messages after the
     * kept window and reports them via signal + takeLastContextRestore. */
    void testCompactAppendsRestore()
    {
        QSocAgentConfig config;
        config.maxContextTokens      = 10000;
        config.compactThreshold      = 0.01; /* always run L2 */
        config.keepRecentMessages    = 2;
        config.contextRestoreEnabled = true;

        auto *registry = new QSocToolRegistry(this);
        auto *agent    = new QSocAgent(this, nullptr, registry, config);

        json msgs = json::array();
        msgs.push_back({{"role", "user"}, {"content", "Start"}});
        for (int i = 0; i < 6; ++i) {
            msgs.push_back(
                {{"role", "assistant"},
                 {"content", QString("step %1 ").arg(i).repeated(200).toStdString()}});
            msgs.push_back({{"role", "user"}, {"content", "ok"}});
        }
        agent->setMessages(msgs);

        QSocContextRestore restore;
        restore.files.append(
            {.displayPath    = QStringLiteral("/tmp/a.txt"),
             .lines          = 3,
             .attachmentText = QStringLiteral(
                 "[Restored file after "
                 "compaction: /tmp/a.txt (3 lines)]\nbody"),
             .mode = QSocContextRestore::Mode::Read});
        restore.files.append(
            {.displayPath    = QStringLiteral("/tmp/big.txt"),
             .lines          = 0,
             .attachmentText = QStringLiteral(
                 "[Referenced file after "
                 "compaction: /tmp/big.txt]"),
             .mode = QSocContextRestore::Mode::Referenced});
        restore.skills.append(
            {.name = QStringLiteral("commit"), .attachmentText = QStringLiteral("## commit\nbody")});

        agent->setContextRestoreProvider([restore]() { return restore; });

        QSignalSpy spy(agent, &QSocAgent::contextRestored);
        agent->compact();

        QCOMPARE(spy.count(), 1);

        const json after = agent->getMessages();
        const int  n     = static_cast<int>(after.size());
        QVERIFY(n >= 3);
        /* The last three messages are the restore: file, file, skills block. */
        const QString last = QString::fromStdString(
            after[static_cast<size_t>(n - 1)]["content"].get<std::string>());
        QVERIFY(last.contains(QStringLiteral("Skills restored after compaction")));
        bool sawFile = false;
        for (const auto &msg : after) {
            if (msg.contains("content") && msg["content"].is_string()
                && QString::fromStdString(msg["content"].get<std::string>())
                       .contains(QStringLiteral("/tmp/a.txt"))) {
                sawFile = true;
            }
        }
        QVERIFY(sawFile);

        const QSocContextRestore applied = agent->takeLastContextRestore();
        QCOMPARE(applied.files.size(), 2);
        QCOMPARE(applied.skills.size(), 1);
    }
};

} // namespace

QSOC_TEST_MAIN(Test)

#include "test_qsoccontextrestore.moc"
