// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocagent.h"
#include "agent/qsocsession.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/runtime/qsocagentruntime.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtCore>
#include <QtTest>

namespace {

using json = nlohmann::json;

/* Every location qsoc may write to points into one private fixture, set up
 * before QCoreApplication caches the environment. */
struct EnvBootstrap
{
    EnvBootstrap()
        : root(QDir(QDir::tempPath())
                   .filePath(
                       QStringLiteral("test_qsocsubagentruns-")
                       + QString::number(QCoreApplication::applicationPid())))
    {
        const QString qsocHome = root + QStringLiteral("/config/qsoc");
        const QString runtime  = root + QStringLiteral("/runtime");
        const QString temp     = root + QStringLiteral("/tmp");
        QDir().mkpath(qsocHome);
        QDir().mkpath(runtime);
        QDir().mkpath(temp);
        QFile::setPermissions(
            runtime, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
        qputenv("QSOC_HOME", qsocHome.toUtf8());
        qputenv("XDG_CONFIG_HOME", (root + QStringLiteral("/config")).toUtf8());
        qputenv("XDG_DATA_HOME", (root + QStringLiteral("/data")).toUtf8());
        qputenv("XDG_RUNTIME_DIR", runtime.toUtf8());
        qputenv("HOME", root.toUtf8());
        qputenv("TMPDIR", temp.toUtf8());
        qputenv("TEMP", temp.toUtf8());
        qputenv("TMP", temp.toUtf8());
        QFile touch(QDir(qsocHome).filePath(QStringLiteral("qsoc.yml")));
        if (touch.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            touch.close();
        }
    }
    QString root;
};

const EnvBootstrap g_env;

QString finishedRun(QSocSubAgentTaskSource &source, QSocAgent *agent = nullptr)
{
    const QString id = source.registerRun(QStringLiteral("label"), QStringLiteral("explore"), agent);
    source.start(id, []() {});
    source.appendTranscript(id, QStringLiteral("chunk"));
    source.markCompleted(id, QStringLiteral("done"));
    return id;
}

QJsonObject readJson(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        return {};
    }
    return QJsonDocument::fromJson(file.readAll()).object();
}

void writeMeta(const QString &dir, const QString &id, const QString &status, qint64 startedAtMs)
{
    QVERIFY(QDir().mkpath(dir));
    QFile file(QDir(dir).filePath(id + QStringLiteral(".meta.json")));
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    QJsonObject meta;
    meta["task_id"]       = id;
    meta["label"]         = QStringLiteral("old");
    meta["subagent_type"] = QStringLiteral("explore");
    meta["status"]        = status;
    meta["started_at_ms"] = startedAtMs;
    file.write(QJsonDocument(meta).toJson());
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void cleanupTestCase();
    void cleanup();
    void reopenedDirectoryContinuesIds();
    void runtimeSessionsDoNotShareRunFiles();
    void resumedHistoryReservesIds();
    void terminalRunStoresHistoryAndPlacement();
    void metaWriteDoesNotFollowLinks();
    void runsWithoutSessionStayInMemory();
    void legacyRunsAreReadOnlyAndRankBelowSession();
    void branchCopiesRunDirectory();

private:
    QString legacyDir() const { return QSocSubAgentTaskSource::legacyTranscriptDir(); }
};

void Test::cleanupTestCase()
{
    QDir(g_env.root).removeRecursively();
}

void Test::cleanup()
{
    QDir(legacyDir()).removeRecursively();
}

void Test::reopenedDirectoryContinuesIds()
{
    const QString dir = QDir(g_env.root).filePath(QStringLiteral("reopen/s.jsonl.agents"));
    {
        QSocSubAgentTaskSource first;
        first.setTranscriptDir(dir);
        QCOMPARE(finishedRun(first), QStringLiteral("a1"));
        QCOMPARE(finishedRun(first), QStringLiteral("a2"));
        QCOMPARE(finishedRun(first), QStringLiteral("a3"));
    }
    QSocSubAgentTaskSource reopened;
    reopened.setTranscriptDir(dir);
    QCOMPARE(finishedRun(reopened), QStringLiteral("a4"));
    QCOMPARE(
        readJson(reopened.metaPathFor(QStringLiteral("a1"))).value("label").toString(),
        QStringLiteral("label"));
}

void Test::runtimeSessionsDoNotShareRunFiles()
{
    QStringList paths;
    for (const QString &name : {QStringLiteral("projA"), QStringLiteral("projB")}) {
        const QString project = QDir(g_env.root).filePath(name);
        QVERIFY(QDir().mkpath(project));
        QSocAgentRuntimeOptions options;
        options.projectDirectory = project;
        QSocAgentRuntime runtime(options);
        QVERIFY(runtime.openSession());
        QSocSubAgentTaskSource *source = runtime.subAgentSource();
        QVERIFY(source != nullptr);
        const QString id   = finishedRun(*source);
        const QString path = source->transcriptPathFor(id);
        QVERIFY(QFileInfo::exists(path));
        QCOMPARE(QFileInfo(path).absolutePath(), runtime.sessionPath() + QStringLiteral(".agents"));
        paths.append(path);
    }
    QVERIFY(paths[0] != paths[1]);
    QVERIFY(QDir(legacyDir()).entryList(QDir::Files).isEmpty());
}

void Test::resumedHistoryReservesIds()
{
    QSocSubAgentTaskSource source;
    const json             history = json::array(
        {{{"role", "assistant"}, {"content", "a9 is only prose"}},
         {{"role", "tool"},
          {"tool_call_id", "c1"},
          {"content", R"({"status":"ok","task_id":"a7","result":"x"})"}}});
    source.reserveIdsFrom(history);
    QCOMPARE(finishedRun(source), QStringLiteral("a8"));
}

void Test::terminalRunStoresHistoryAndPlacement()
{
    const QString dir = QDir(g_env.root).filePath(QStringLiteral("history/s.jsonl.agents"));
    QSocSubAgentTaskSource source;
    source.setTranscriptDir(dir);
    auto      *child    = new QSocAgent();
    const json messages = json::array(
        {{{"role", "user"}, {"content", "inspect the bus"}},
         {{"role", "assistant"}, {"content", "the bus is fine"}}});
    child->setMessages(messages);
    const QString id = source.registerRun(QStringLiteral("label"), QStringLiteral("explore"), child);
    source.setDispatchMetadata(
        id,
        {QStringLiteral("sim"),
         QStringLiteral("operator@sim.invalid:22"),
         QStringLiteral("/work/tree")});
    source.start(id, []() {});
    source.markCompleted(id, QStringLiteral("done"));

    const QJsonObject meta = readJson(source.metaPathFor(id));
    QCOMPARE(meta.value("host").toString(), QStringLiteral("sim"));
    QCOMPARE(meta.value("endpoint").toString(), QStringLiteral("operator@sim.invalid:22"));
    QCOMPARE(meta.value("workspace").toString(), QStringLiteral("/work/tree"));
    const QString history = QDir(dir).filePath(meta.value("history_file").toString());
    QCOMPARE(QFileInfo(history).fileName(), id + QStringLiteral(".history.jsonl"));
    QCOMPARE(QSocSession::loadMessages(history), messages);

    QSocSubAgentTaskSource::HistoricalRun run;
    QVERIFY(source.findHistoricalRun(id, &run));
    QCOMPARE(run.historyFile, history);
    QCOMPARE(run.workspace, QStringLiteral("/work/tree"));
    QCOMPARE(run.host, QStringLiteral("sim"));
    QCOMPARE(run.endpoint, QStringLiteral("operator@sim.invalid:22"));
}

void Test::metaWriteDoesNotFollowLinks()
{
#ifndef Q_OS_UNIX
    QSKIP("symbolic links need Unix");
#else
    const QString dir    = QDir(g_env.root).filePath(QStringLiteral("links/s.jsonl.agents"));
    const QString victim = QDir(g_env.root).filePath(QStringLiteral("links/victim.txt"));
    QVERIFY(QDir().mkpath(dir));
    {
        QFile file(victim);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write("keep");
    }
    QVERIFY(QFile::link(victim, QDir(dir).filePath(QStringLiteral("a1.meta.json"))));
    QVERIFY(QFile::link(victim, QDir(dir).filePath(QStringLiteral("a1.jsonl"))));
    QSocSubAgentTaskSource source;
    source.setTranscriptDir(dir);
    const QString id = source.registerRun(QStringLiteral("l"), QStringLiteral("t"), nullptr);
    QCOMPARE(id, QStringLiteral("a2"));

    /* Links planted at the paths of a live run are refused, not followed. */
    for (const QString &suffix : {QStringLiteral(".meta.json"), QStringLiteral(".jsonl")}) {
        const QString path = QDir(dir).filePath(id + suffix);
        QFile::remove(path);
        QVERIFY(QFile::link(victim, path));
    }
    source.start(id, []() {});
    source.appendTranscript(id, QStringLiteral("chunk"));
    source.markCompleted(id, QStringLiteral("done"));
    QVERIFY(QFileInfo(QDir(dir).filePath(id + QStringLiteral(".meta.json"))).isSymLink());
    QFile file(victim);
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("keep"));
#endif
}

void Test::runsWithoutSessionStayInMemory()
{
    QSocSubAgentTaskSource source;
    const QString          id = finishedRun(source);
    QVERIFY(source.transcriptPathFor(id).isEmpty());
    QVERIFY(source.metaPathFor(id).isEmpty());
    QVERIFY(source.tailFor(id, 0).contains(QStringLiteral("done")));
    QVERIFY(!QFileInfo::exists(QDir::temp().filePath(QStringLiteral("qsoc-agents"))));
    QVERIFY(QDir(legacyDir()).entryList(QDir::Files).isEmpty());
}

void Test::legacyRunsAreReadOnlyAndRankBelowSession()
{
    const qint64  old     = QDateTime::currentMSecsSinceEpoch() - qint64{3} * 3600 * 1000;
    const QString session = QDir(g_env.root).filePath(QStringLiteral("legacy/s.jsonl.agents"));
    writeMeta(legacyDir(), QStringLiteral("a1"), QStringLiteral("running"), old);
    writeMeta(legacyDir(), QStringLiteral("a5"), QStringLiteral("completed"), old);
    writeMeta(session, QStringLiteral("a1"), QStringLiteral("completed"), old + 1);
    const QString     legacyMeta = QDir(legacyDir()).filePath(QStringLiteral("a1.meta.json"));
    const QJsonObject before     = readJson(legacyMeta);

    QSocSubAgentTaskSource source;
    source.setTranscriptDir(session);
    const auto runs = source.loadHistoricalRuns();
    QCOMPARE(runs.size(), qsizetype(3));
    int legacyCount = 0;
    for (const auto &run : runs) {
        legacyCount += run.legacy ? 1 : 0;
    }
    QCOMPARE(legacyCount, 2);

    QSocSubAgentTaskSource::HistoricalRun hit;
    QVERIFY(source.findHistoricalRun(QStringLiteral("a1"), &hit));
    QVERIFY(!hit.legacy);
    QCOMPARE(hit.status, QStringLiteral("completed"));
    QVERIFY(source.findHistoricalRun(QStringLiteral("a5"), &hit));
    QVERIFY(hit.legacy);
    QCOMPARE(readJson(legacyMeta), before);

    /* New runs are never written to the legacy directory. */
    QCOMPARE(finishedRun(source), QStringLiteral("a2"));
    QVERIFY(!QFileInfo::exists(QDir(legacyDir()).filePath(QStringLiteral("a2.meta.json"))));
}

void Test::branchCopiesRunDirectory()
{
    const QString project = QDir(g_env.root).filePath(QStringLiteral("branch"));
    QVERIFY(QDir().mkpath(project));
    QSocAgentRuntimeOptions options;
    options.projectDirectory = project;
    QSocAgentRuntime runtime(options);
    QVERIFY(runtime.openSession());
    const QString id       = finishedRun(*runtime.subAgentSource());
    const QString original = runtime.sessionPath();
    QVERIFY(runtime.executeCommand(QStringLiteral("/branch copy")));

    QString branched;
    for (const auto &info : runtime.listSessions()) {
        if (info.path != original) {
            branched = info.path;
        }
    }
    QVERIFY(!branched.isEmpty());
    const QString copied = branched + QStringLiteral(".agents/") + id
                           + QStringLiteral(".meta.json");
    QCOMPARE(readJson(copied).value("task_id").toString(), id);
#ifdef Q_OS_UNIX
    QCOMPARE(
        QFileInfo(branched + QStringLiteral(".agents")).permissions()
            & (QFileDevice::ReadGroup | QFileDevice::ReadOther),
        QFileDevice::Permissions());
#endif
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocsubagentsessionruns.moc"
