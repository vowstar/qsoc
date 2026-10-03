// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocresourceusage.h"
#include "common/qsocresourceusage_p.h"
#include "qsoc_test.h"

#include <limits>
#include <QElapsedTimer>
#include <QProcess>
#include <QScopeGuard>
#include <QStorageInfo>
#include <QTemporaryDir>
#include <QtTest>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

namespace {

bool includesPid(const QJsonArray &tree, qint64 pid)
{
    for (const auto &entry : tree) {
        if (entry.toObject().value("pid").toInteger() == pid)
            return true;
    }
    return false;
}

class Test : public QObject
{
    Q_OBJECT
private slots:
    void parentIdentityRejectsReusedProcessIds()
    {
        using QSocResourceUsage::detail::followsParentStart;
        QVERIFY(followsParentStart("999", "1000"));
        QVERIFY(followsParentStart("1000", "1000"));
        QVERIFY(!followsParentStart("1000", "999"));
        QVERIFY(!followsParentStart({}, "1000"));
        QVERIFY(!followsParentStart("1000", {}));
        QVERIFY(!followsParentStart("invalid", "1000"));
        QVERIFY(!followsParentStart("0", "1000"));
    }

    void systemCounters()
    {
        const auto before = QSocResourceUsage::system();
        QVERIFY(before.value("cpu_logical_count").toInteger() > 0);
        QVERIFY(before.value("memory_total_bytes").toInteger() > 0);
        QVERIFY(before.value("memory_available_bytes").toInteger(-1) >= 0);
        QVERIFY(
            before.value("memory_available_bytes").toInteger()
            <= before.value("memory_total_bytes").toInteger());
        QVERIFY(before.contains("memory_effective_available_bytes"));
        if (!before.value("memory_effective_available_bytes").isNull()) {
            QVERIFY(
                before.value("memory_effective_available_bytes").toInteger()
                <= before.value("memory_available_bytes").toInteger());
            QVERIFY(before.value("memory_effective_available_kind").toString() != "unknown");
        }
        QTest::qWait(30);
        const auto after = QSocResourceUsage::system();
        QVERIFY(
            after.value("sampled_at_ns").toInteger() > before.value("sampled_at_ns").toInteger());
        if (!before.value("cpu_total_ns").isNull() && !after.value("cpu_total_ns").isNull()) {
            QVERIFY(
                after.value("cpu_total_ns").toInteger() >= before.value("cpu_total_ns").toInteger());
            QVERIFY(
                after.value("cpu_busy_ns").toInteger() <= after.value("cpu_total_ns").toInteger());
        }
    }

    void processCounters()
    {
        const auto pid    = QCoreApplication::applicationPid();
        const auto before = QSocResourceUsage::process(pid);
        QCOMPARE(before.value("pid").toInteger(), pid);
        QVERIFY(!before.value("start_id").toString().isEmpty());
        QVERIFY(before.value("resident_bytes").toInteger() > 0);
        QVERIFY(before.value("cpu_time_ns").toInteger(-1) >= 0);
        QElapsedTimer timer;
        timer.start();
        while (timer.elapsed() < 80)
            Q_UNUSED(timer.nsecsElapsed())
        const auto after = QSocResourceUsage::process(pid);
        QCOMPARE(after.value("start_id"), before.value("start_id"));
        QVERIFY(after.value("cpu_time_ns").toInteger() > before.value("cpu_time_ns").toInteger());
#ifdef Q_OS_LINUX
        QVERIFY(after.value("private_commit_bytes").isNull());
        QVERIFY(after.value("footprint_bytes").isNull());
#elif defined(Q_OS_WIN)
        QVERIFY(after.value("private_commit_bytes").toInteger() > 0);
        QVERIFY(after.value("proportional_bytes").isNull());
#elif defined(Q_OS_MACOS)
        QVERIFY(after.value("footprint_bytes").toInteger() > 0);
        QVERIFY(after.value("private_commit_bytes").isNull());
#endif
    }

    void missingProcessIsUnknown()
    {
        for (const auto pid : {qint64(-1), qint64(0), (std::numeric_limits<qint64>::max)()}) {
            const auto result = QSocResourceUsage::process(pid);
            for (auto it = result.begin(); it != result.end(); ++it) {
                if (it.key() != "pid")
                    QVERIFY2(it.value().isNull(), qPrintable(it.key()));
            }
        }
    }

    void processTreeTracksOnlyDescendants()
    {
        QTemporaryDir working;
        QVERIFY(working.isValid());
        QProcess   child, sibling;
        const auto cleanup     = qScopeGuard([&] {
            for (auto *process : {&child, &sibling}) {
                if (process->state() != QProcess::NotRunning) {
                    process->kill();
                    process->waitForFinished(5000);
                }
            }
        });
        auto       environment = QProcessEnvironment::systemEnvironment();
        environment.insert("QSOC_RESOURCE_TEST_CHILD", "1");
        for (auto *process : {&child, &sibling}) {
            process->setWorkingDirectory(working.path());
            process->setProcessEnvironment(environment);
            process->start(QCoreApplication::applicationFilePath(), {"holdForParent"});
            QVERIFY(process->waitForStarted(5000));
        }
        const auto pid  = child.processId();
        const auto tree = QSocResourceUsage::processTree(QCoreApplication::applicationPid());
        QVERIFY(includesPid(tree, pid));
        QVERIFY(includesPid(tree, sibling.processId()));
        const auto subtree = QSocResourceUsage::processTree(pid);
        QVERIFY(includesPid(subtree, pid));
        QVERIFY(!includesPid(subtree, sibling.processId()));
        QVERIFY(!includesPid(subtree, QCoreApplication::applicationPid()));
#ifdef Q_OS_WIN
        const HANDLE retained = ::OpenProcess(
            PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
        QVERIFY(retained != nullptr);
        const auto releaseHandle = qScopeGuard([retained] { ::CloseHandle(retained); });
#endif
        child.kill();
        QVERIFY(child.waitForFinished(5000));
#ifdef Q_OS_WIN
        QCOMPARE(::WaitForSingleObject(retained, 0), DWORD(WAIT_OBJECT_0));
#endif
        QVERIFY(QSocResourceUsage::process(pid).value("start_id").isNull());
        QVERIFY(QSocResourceUsage::processTree(pid).isEmpty());
    }

    void storageUsesAvailableCapacity()
    {
        QTemporaryDir working;
        QVERIFY(working.isValid());
        const auto   result = QSocResourceUsage::storage(working.path());
        QStorageInfo expected(working.path());
        expected.refresh();
        QVERIFY(result.value("valid").toBool());
        QCOMPARE(result.value("root_path").toString(), expected.rootPath());
        QCOMPARE(result.value("total_bytes").toInteger(), expected.bytesTotal());
        QVERIFY(result.value("available_bytes").toInteger(-1) >= 0);
        QVERIFY(
            result.value("available_bytes").toInteger() <= result.value("total_bytes").toInteger());
        QCOMPARE(result.value("read_only").toBool(), expected.isReadOnly());
        const auto unknown = QSocResourceUsage::storage({});
        QVERIFY(!unknown.value("valid").toBool());
        QVERIFY(unknown.value("available_bytes").isNull());
        QVERIFY(unknown.value("read_only").isNull());
    }

#ifdef Q_OS_LINUX
    void cgroupLimitsAndUnavailableInputs()
    {
        QTemporaryDir working;
        QVERIFY(working.isValid());
        const auto mount = working.filePath("hierarchy with space");
        QVERIFY(QDir().mkpath(mount + "/parent/child"));
        const auto write = [](const QString &path, const QByteArray &value) {
            QFile file(path);
            return file.open(QIODevice::WriteOnly) && file.write(value) == value.size();
        };
        QVERIFY(write(working.filePath("cgroup"), "0::/parent/child\n"));
        const auto mountInfo = "1 0 0:1 / " + QFile::encodeName(mount).replace(" ", "\\040")
                               + " rw - cgroup2 none rw\n";
        QVERIFY(write(working.filePath("mountinfo"), mountInfo));
        QVERIFY(write(mount + "/cgroup.controllers", "cpu memory\n"));
        QVERIFY(write(mount + "/parent/memory.max", "800\n"));
        QVERIFY(write(mount + "/parent/memory.current", "600\n"));
        QVERIFY(write(mount + "/parent/child/memory.max", "500\n"));
        QVERIFY(write(mount + "/parent/child/memory.current", "100\n"));
        const auto sample = [&] {
            QJsonObject value{
                {"memory_available_bytes", 1000},
                {"memory_effective_available_bytes", QJsonValue::Null},
                {"memory_effective_available_kind", "unknown"}};
            QSocResourceUsage::detail::sampleCgroupMemory(value, working.path());
            return value;
        };
        QCOMPARE(sample().value("memory_effective_available_bytes").toInteger(), 200);
        QVERIFY(write(mount + "/parent/memory.current", "900\n"));
        QCOMPARE(sample().value("memory_effective_available_bytes").toInteger(), 0);
        QVERIFY(write(mount + "/parent/memory.max", "max\n"));
        QCOMPARE(sample().value("memory_effective_available_bytes").toInteger(), 400);
        QVERIFY(write(mount + "/parent/child/memory.max", "max\n"));
        QCOMPARE(sample().value("memory_effective_available_bytes").toInteger(), 1000);
        QCOMPARE(sample().value("memory_cgroup_limited").toBool(), false);
        QVERIFY(write(mount + "/memory.max", {}));
        QVERIFY(sample().value("memory_effective_available_bytes").isNull());
        QVERIFY(QFile::remove(mount + "/memory.max"));
        QVERIFY(QFile::remove(mount + "/parent/child/memory.max"));
        QVERIFY(sample().value("memory_effective_available_bytes").isNull());
        QVERIFY(write(mount + "/parent/child/memory.max", "500\n"));
        QVERIFY(QFile::remove(mount + "/parent/child/memory.current"));
        QVERIFY(sample().value("memory_effective_available_bytes").isNull());
        QVERIFY(write(working.filePath("cgroup"), "2:memory:/parent/child\n"));
        QCOMPARE(sample().value("memory_cgroup_version").toString(), "v1");
        QVERIFY(sample().value("memory_effective_available_bytes").isNull());
    }
#endif

    void holdForParent()
    {
        if (qEnvironmentVariableIsSet("QSOC_RESOURCE_TEST_CHILD"))
            QTest::qWait(10000);
    }
};

} // namespace

QSOC_TEST_MAIN(Test)
#include "test_qsocresourceusage.moc"
