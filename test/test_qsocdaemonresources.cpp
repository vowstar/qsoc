// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/daemon/qsocdaemonresources.h"
#include "qsoc_test.h"

#include <QDateTime>
#include <QElapsedTimer>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#endif

namespace {

bool processExists(qint64 pid)
{
#ifdef Q_OS_WIN
    HANDLE process = ::OpenProcess(SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (!process)
        return false;
    const bool alive = ::WaitForSingleObject(process, 0) == WAIT_TIMEOUT;
    ::CloseHandle(process);
    return alive;
#else
    return ::kill(static_cast<pid_t>(pid), 0) == 0 || errno != ESRCH;
#endif
}

qint64 readPid(const QString &path)
{
    QFile file(path);
    return file.open(QIODevice::ReadOnly) ? file.readAll().toLongLong() : 0;
}

class TestQSocDaemonResources : public QObject
{
    Q_OBJECT

private slots:
    void validatePaths()
    {
        QJsonArray paths;
        QVERIFY(QSocDaemonResources::validatePaths({}, paths));
        QVERIFY(paths.isEmpty());
        QVERIFY(!QSocDaemonResources::validatePaths({{"paths", "invalid"}}, paths));
        QVERIFY(!QSocDaemonResources::validatePaths({{"paths", QJsonArray{"relative"}}}, paths));
        QVERIFY(!QSocDaemonResources::validatePaths({{"paths", QJsonArray{17}}}, paths));
        QVERIFY(!QSocDaemonResources::validatePaths({{"limits", 1}}, paths));
        QJsonArray excessive;
        for (int index = 0; index < 9; ++index)
            excessive.append(QDir::tempPath());
        QVERIFY(!QSocDaemonResources::validatePaths({{"paths", excessive}}, paths));
        QVERIFY(
            QSocDaemonResources::validatePaths({{"paths", QJsonArray{QDir::tempPath()}}}, paths));
    }

    void timeoutKillsTheProbe()
    {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto          marker = fixture.filePath("ready");
        QSocDaemonResources resources(
            nullptr,
            QStringLiteral(QSOC_OWNER_PROBE_PATH),
            {"child", marker, QString::number(QCoreApplication::applicationPid())});
        QJsonObject   result;
        QJsonObject   reentrant;
        QElapsedTimer elapsed;
        elapsed.start();
        resources
            .request(this, {}, [this, &resources, &result, &reentrant](const QJsonObject &snapshot) {
                result = snapshot;
                resources.request(this, {}, [&reentrant](const QJsonObject &reply) {
                    reentrant = reply;
                });
            });
        QTRY_VERIFY_WITH_TIMEOUT(readPid(marker) > 0, 2000);
        const auto pid = readPid(marker);
        QVERIFY(processExists(pid));
        QJsonObject busy;
        resources.request(this, {}, [&busy](const QJsonObject &snapshot) { busy = snapshot; });
        QCOMPARE(busy.value("status").toString(), QStringLiteral("busy"));
        QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 4500);
        QCOMPARE(result.value("status").toString(), QStringLiteral("timeout"));
        QCOMPARE(reentrant.value("status").toString(), QStringLiteral("busy"));
        QVERIFY(elapsed.elapsed() < 5000);
        QTRY_VERIFY_WITH_TIMEOUT(!processExists(pid), 3000);
        QVERIFY(QFile::remove(marker));
        QJsonObject recovered;
        resources.request(this, {}, [&recovered](const QJsonObject &reply) { recovered = reply; });
        QTRY_VERIFY_WITH_TIMEOUT(readPid(marker) > 0, 2000);
        const auto nextPid = readPid(marker);
        QVERIFY(nextPid != pid);
        QVERIFY(recovered.isEmpty());
        resources.cancel(this);
        QTRY_VERIFY_WITH_TIMEOUT(!processExists(nextPid), 3000);
    }

    void shutdownDoesNotWaitForTheProbe()
    {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto marker    = fixture.filePath("ready");
        auto       resources = std::make_unique<QSocDaemonResources>(
            nullptr,
            QStringLiteral(QSOC_OWNER_PROBE_PATH),
            QStringList{"child", marker, QString::number(QCoreApplication::applicationPid())});
        bool delivered = false;
        resources->request(this, {}, [&delivered](const QJsonObject &) { delivered = true; });
        QTRY_VERIFY_WITH_TIMEOUT(readPid(marker) > 0, 2000);
        const auto    pid = readPid(marker);
        QElapsedTimer elapsed;
        elapsed.start();
        resources.reset();
        QVERIFY(elapsed.elapsed() < 250);
        QTRY_VERIFY_WITH_TIMEOUT(!processExists(pid), 3000);
        QVERIFY(!delivered);
    }

    void snapshotExcludesTheProbe()
    {
        QSocDaemonResources resources(nullptr, QStringLiteral(QSOC_AGENTD_PATH));
        QJsonObject         result;
        resources.request(this, {}, [&result](const QJsonObject &snapshot) { result = snapshot; });
        QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 5000);
        QCOMPARE(result.value("scope").toString(), QStringLiteral("local_daemon"));
        QVERIFY(
            QDateTime::fromString(result.value("sampled_at_utc").toString(), Qt::ISODateWithMs)
                .isValid());
        QVERIFY(result.value("collection_duration_ms").toInteger(-1) >= 0);
        const auto processes = result.value("processes").toArray();
        QVERIFY(!processes.isEmpty());
        QCOMPARE(
            processes.first().toObject().value("pid").toInteger(),
            QCoreApplication::applicationPid());
        QCOMPARE(processes.size(), 1);
        QVERIFY(result.value("storage").toArray().isEmpty());
    }

    void rejectProbeOutput_data()
    {
        QTest::addColumn<QString>("mode");
        QTest::newRow("oversized") << QStringLiteral("resource-output");
        QTest::newRow("invalid") << QStringLiteral("resource-invalid");
        QTest::newRow("empty") << QStringLiteral("disarm");
    }

    void rejectProbeOutput()
    {
        QFETCH(QString, mode);
        QSocDaemonResources resources(
            nullptr,
            QStringLiteral(QSOC_OWNER_PROBE_PATH),
            {mode, QStringLiteral("unused"), QString::number(QCoreApplication::applicationPid())});
        QJsonObject result;
        resources.request(this, {}, [&result](const QJsonObject &snapshot) { result = snapshot; });
        QTRY_VERIFY_WITH_TIMEOUT(!result.isEmpty(), 5000);
        QCOMPARE(result.value("status").toString(), QStringLiteral("unknown"));
        QVERIFY(!result.value("reason").toString().isEmpty());
    }

    void cancellationReclaimsTheProbe()
    {
        QTemporaryDir fixture;
        QVERIFY(fixture.isValid());
        const auto          marker = fixture.filePath("ready");
        QSocDaemonResources resources(
            nullptr,
            QStringLiteral(QSOC_OWNER_PROBE_PATH),
            {"child", marker, QString::number(QCoreApplication::applicationPid())});
        bool delivered = false;
        resources.request(this, {}, [&delivered](const QJsonObject &) { delivered = true; });
        QTRY_VERIFY_WITH_TIMEOUT(readPid(marker) > 0, 2000);
        const auto pid = readPid(marker);
        resources.cancel(this);
        QTRY_VERIFY_WITH_TIMEOUT(!processExists(pid), 3000);
        QVERIFY(!delivered);
    }
};

} // namespace

QSOC_TEST_MAIN(TestQSocDaemonResources)
#include "test_qsocdaemonresources.moc"
