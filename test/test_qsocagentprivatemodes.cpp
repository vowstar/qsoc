// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocsession.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/qsoctoolresultstore.h"
#include "qsoc_test.h"

#include <nlohmann/json.hpp>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QtCore>
#include <QtTest>

#ifdef Q_OS_UNIX
#include <sys/stat.h>
#endif

namespace {

/* Mode bits as stat reports them, without Qt's per-user view. */
int modeOf(const QString &path)
{
#ifdef Q_OS_UNIX
    struct stat info{};
    if (::lstat(QFile::encodeName(path).constData(), &info) != 0) {
        return -1;
    }
    return static_cast<int>(info.st_mode & 0777);
#else
    Q_UNUSED(path);
    return -1;
#endif
}

void chmodTo(const QString &path, int mode)
{
#ifdef Q_OS_UNIX
    QVERIFY(::chmod(QFile::encodeName(path).constData(), static_cast<mode_t>(mode)) == 0);
#else
    Q_UNUSED(path);
    Q_UNUSED(mode);
#endif
}

class Test : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase();
    void cleanupTestCase();
    void sessionFilesAreOwnerOnly();
    void existingSessionIsTightenedOnWrite();
    void sessionIgnoreFileIsCreatedOnce();
    void artifactStoreIsOwnerOnly();
    void subAgentRunFilesAreOwnerOnly();

private:
    QTemporaryDir root;
#ifdef Q_OS_UNIX
    mode_t savedUmask = 0;
#endif
};

void Test::initTestCase()
{
#ifndef Q_OS_UNIX
    QSKIP("Unix file modes only");
#else
    QVERIFY(root.isValid());
    savedUmask = ::umask(0002);
#endif
}

void Test::cleanupTestCase()
{
#ifdef Q_OS_UNIX
    ::umask(savedUmask);
#endif
}

void Test::sessionFilesAreOwnerOnly()
{
    const QString project  = root.filePath(QStringLiteral("fresh"));
    const QString sessions = QDir(project).filePath(QStringLiteral(".qsoc/sessions"));
    const QString path     = QDir(sessions).filePath(QStringLiteral("s1.jsonl"));
    QSocSession   session(QStringLiteral("s1"), path, QSocSession::StorageMode::Fresh);
    QVERIFY(session.appendMessage({{"role", "user"}, {"content", "hello"}}));
    QCOMPARE(modeOf(path), 0600);
    QCOMPARE(modeOf(sessions), 0700);

    QVERIFY(session.appendSnapshot(nlohmann::json::array({{{"role", "user"}, {"content", "x"}}})));
    QCOMPARE(modeOf(path), 0600);
    QVERIFY(session.rewriteMessages(nlohmann::json::array({{{"role", "user"}, {"content", "y"}}})));
    QCOMPARE(modeOf(path), 0600);
}

void Test::existingSessionIsTightenedOnWrite()
{
    const QString sessions = root.filePath(QStringLiteral("old/.qsoc/sessions"));
    QVERIFY(QDir().mkpath(sessions));
    const QString path = QDir(sessions).filePath(QStringLiteral("s2.jsonl"));
    {
        QFile file(path);
        QVERIFY(file.open(QIODevice::WriteOnly));
        file.write(
            R"({"type":"message","role":"user","content":"old"})"
            "\n");
    }
    chmodTo(sessions, 0775);
    chmodTo(path, 0664);

    QSocSession session(QStringLiteral("s2"), path, QSocSession::StorageMode::Existing);
    QVERIFY(session.appendMessage({{"role", "assistant"}, {"content", "new"}}));
    QCOMPARE(modeOf(path), 0600);
    QCOMPARE(modeOf(sessions), 0700);
    QCOMPARE(QSocSession::loadMessages(path).size(), size_t(2));
}

void Test::sessionIgnoreFileIsCreatedOnce()
{
    const QString project = root.filePath(QStringLiteral("ignore"));
    const QString ignore  = QDir(project).filePath(QStringLiteral(".qsoc/.gitignore"));
    const QString path    = QDir(project).filePath(QStringLiteral(".qsoc/sessions/s3.jsonl"));
    QSocSession   session(QStringLiteral("s3"), path, QSocSession::StorageMode::Fresh);
    QVERIFY(session.appendMessage({{"role", "user"}, {"content", "hello"}}));
    QFile file(ignore);
    QVERIFY(file.open(QIODevice::ReadOnly));
    const QByteArray generated = file.readAll();
    file.close();
    QVERIFY(generated.split('\n').contains("sessions/"));
    QVERIFY(generated.split('\n').contains("file-history/"));

    /* A user-owned ignore file is never rewritten. */
    QVERIFY(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write("custom\n");
    file.close();
    QSocSession other(
        QStringLiteral("s4"),
        QDir(project).filePath(QStringLiteral(".qsoc/sessions/s4.jsonl")),
        QSocSession::StorageMode::Fresh);
    QVERIFY(other.appendMessage({{"role", "user"}, {"content", "hello"}}));
    QVERIFY(file.open(QIODevice::ReadOnly));
    QCOMPARE(file.readAll(), QByteArray("custom\n"));
}

void Test::artifactStoreIsOwnerOnly()
{
    const QString       directory = root.filePath(QStringLiteral("s.jsonl.artifacts"));
    QSocToolResultStore store(directory, QStringLiteral("owner"));
    QVERIFY(store.isBound());
    QString    error;
    const auto reference = store.publish(
        QStringLiteral("payload"), QStringLiteral("ok"), QStringLiteral("unknown"), &error);
    QVERIFY2(reference.has_value(), qPrintable(error));
    QCOMPARE(modeOf(directory), 0700);
    QCOMPARE(modeOf(QDir(directory).filePath(QStringLiteral(".scope"))), 0600);
    QCOMPARE(modeOf(QDir(directory).filePath(reference->id + QStringLiteral(".qtr"))), 0600);
}

void Test::subAgentRunFilesAreOwnerOnly()
{
    const QString          directory = root.filePath(QStringLiteral("runs"));
    QSocSubAgentTaskSource source;
    source.setTranscriptDir(directory);
    const QString id
        = source.registerRun(QStringLiteral("label"), QStringLiteral("explore"), nullptr);
    source.start(id, []() {});
    source.appendTranscript(id, QStringLiteral("chunk"));
    source.markCompleted(id, QStringLiteral("done"));
    QCOMPARE(modeOf(directory), 0700);
    QCOMPARE(modeOf(source.transcriptPathFor(id)), 0600);
    QCOMPARE(modeOf(source.metaPathFor(id)), 0600);
}

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocagentprivatemodes.moc"
