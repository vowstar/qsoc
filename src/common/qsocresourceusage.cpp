// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocresourceusage.h"
#include "common/qsocresourceusage_p.h"

#include <algorithm>
#include <chrono>
#include <limits>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSet>
#include <QStorageInfo>
#include <QThread>

#ifdef Q_OS_WIN
#include <windows.h>

#include <psapi.h>
#include <tlhelp32.h>
#elif defined(Q_OS_MACOS)
#include <libproc.h>
#include <mach/mach.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <unistd.h>
#elif defined(Q_OS_LINUX)
#include <unistd.h>
#endif

namespace {

QJsonValue number(quint64 value)
{
    if (value > static_cast<quint64>((std::numeric_limits<qint64>::max)()))
        return QJsonValue::Null;
    return static_cast<qint64>(value);
}

QJsonValue nanoseconds(quint64 ticks, quint64 frequency)
{
    if (frequency == 0)
        return QJsonValue::Null;
    const long double value = static_cast<long double>(ticks) * 1000000000.0L / frequency;
    if (value >= (std::numeric_limits<qint64>::max)())
        return QJsonValue::Null;
    return static_cast<qint64>(value);
}

QJsonObject emptyProcess(qint64 pid)
{
    QJsonObject result{{"pid", pid}};
    for (const auto *key :
         {"start_id",
          "cpu_time_ns",
          "resident_bytes",
          "peak_resident_bytes",
          "private_commit_bytes",
          "private_resident_bytes",
          "proportional_bytes",
          "footprint_bytes"})
        result.insert(QLatin1String(key), QJsonValue::Null);
    return result;
}

struct Identity
{
    qint64  parent = 0;
    QString start;
};

#ifdef Q_OS_LINUX
QByteArray readFile(const QString &path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return {};
    return file.read(1024 * 1024);
}

QList<QByteArray> processFields(qint64 pid)
{
    const auto data = readFile(QStringLiteral("/proc/%1/stat").arg(pid));
    const auto end  = data.lastIndexOf(')');
    if (end < 0)
        return {};
    return data.mid(end + 2).simplified().split(' ');
}

Identity identity(qint64 pid)
{
    const auto fields = processFields(pid);
    if (fields.size() <= 19)
        return {};
    bool       ok     = false;
    const auto parent = fields[1].toLongLong(&ok);
    if (!ok)
        return {};
    return {parent, QString::fromLatin1(fields[19])};
}

QHash<QByteArray, quint64> kilobytes(const QByteArray &data)
{
    QHash<QByteArray, quint64> result;
    for (const auto &line : data.split('\n')) {
        const auto fields = line.simplified().split(' ');
        if (fields.size() != 3 || fields[2] != "kB")
            continue;
        bool       ok    = false;
        const auto value = fields[1].toULongLong(&ok);
        if (ok && value <= (std::numeric_limits<quint64>::max)() / 1024)
            result.insert(fields[0], value * 1024);
    }
    return result;
}

void copyMemory(
    QJsonObject                      &result,
    const QHash<QByteArray, quint64> &values,
    const char                       *source,
    const char                       *target)
{
    const auto found = values.constFind(source);
    if (found != values.cend())
        result.insert(QLatin1String(target), number(found.value()));
}
QString mountPath(QByteArray value)
{
    for (const auto &escape :
         {QByteArray("040"), QByteArray("011"), QByteArray("012"), QByteArray("134")}) {
        bool       ok   = false;
        const auto byte = escape.toInt(&ok, 8);
        if (ok)
            value.replace('\\' + escape, QByteArray(1, static_cast<char>(byte)));
    }
    return QString::fromLocal8Bit(value);
}

} // namespace

void QSocResourceUsage::detail::sampleCgroupMemory(QJsonObject &result, const QString &procDirectory)
{
    QString    group;
    const auto memberships = readFile(QDir(procDirectory).filePath("cgroup"));
    for (const auto &line : memberships.split('\n')) {
        const auto fields = line.split(':');
        if (fields.size() == 3 && fields[1].split(',').contains("memory")) {
            result.insert("memory_cgroup_version", "v1");
            return;
        }
        if (line.startsWith("0::"))
            group = QString::fromLocal8Bit(line.mid(3));
    }
    if (group.isEmpty()) {
        if (!memberships.isEmpty())
            result.insert("memory_cgroup_version", "v1");
        return;
    }
    result.insert("memory_cgroup_version", "v2");
    QString mount, root;
    for (const auto &line : readFile(QDir(procDirectory).filePath("mountinfo")).split('\n')) {
        if (!line.contains(" - cgroup2 "))
            continue;
        const auto fields = line.split(' ');
        if (fields.size() < 6)
            continue;
        const auto candidate = mountPath(fields[3]);
        if (candidate == "/" || group == candidate || group.startsWith(candidate + '/')) {
            root  = candidate;
            mount = mountPath(fields[4]);
            break;
        }
    }
    if (mount.isEmpty() || result.value("memory_available_bytes").isNull())
        return;
    const auto relative  = root == "/" ? group.mid(1) : group.mid(root.size() + 1);
    QString    directory = QDir::cleanPath(QDir(mount).filePath(relative));
    if (directory != mount && !directory.startsWith(mount + '/'))
        return;
    quint64 available = static_cast<quint64>(result.value("memory_available_bytes").toInteger());
    bool    limited   = false;
    while (true) {
        const auto maximum = readFile(directory + "/memory.max").trimmed();
        if (maximum.isEmpty()) {
            // The hierarchy root has no memory.max; other missing files are unknown.
            if (directory != mount || root != "/" || QFileInfo::exists(directory + "/memory.max")
                || !readFile(directory + "/cgroup.controllers")
                        .simplified()
                        .split(' ')
                        .contains("memory"))
                return;
        } else if (maximum != "max") {
            bool       maxOk = false, currentOk = false;
            const auto limit = maximum.toULongLong(&maxOk);
            const auto current
                = readFile(directory + "/memory.current").trimmed().toULongLong(&currentOk);
            if (!maxOk || !currentOk)
                return;
            available = (std::min) (available, current >= limit ? quint64(0) : limit - current);
            limited   = true;
        }
        if (directory == mount)
            break;
        const auto parent = QFileInfo(directory).absolutePath();
        if (parent == directory)
            return;
        directory = parent;
    }
    result.insert("memory_cgroup_limited", limited);
    result.insert("memory_effective_available_bytes", number(available));
    result.insert(
        "memory_effective_available_kind",
        limited ? "estimate_cgroup_v2" : "host_cgroup_v2_unlimited");
    result.insert("memory_effective_scope", "host_and_visible_cgroup_v2");
}

namespace {
#elif defined(Q_OS_WIN)
quint64 fileTime(const FILETIME &time)
{
    return (static_cast<quint64>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
}

QString processStart(HANDLE handle)
{
    FILETIME start{}, end{}, kernel{}, user{};
    if (!::GetProcessTimes(handle, &start, &end, &kernel, &user)
        || ::WaitForSingleObject(handle, 0) != WAIT_TIMEOUT)
        return {};
    return QString::number(fileTime(start));
}
#elif defined(Q_OS_MACOS)
Identity identity(qint64 pid)
{
    proc_bsdinfo info{};
    if (::proc_pidinfo(static_cast<int>(pid), PROC_PIDTBSDINFO, 0, &info, sizeof(info))
        != sizeof(info))
        return {};
    return {
        info.pbi_ppid,
        QString::number(info.pbi_start_tvsec) + ':' + QString::number(info.pbi_start_tvusec)};
}

quint64 sysctlNumber(const char *name)
{
    quint64 value = 0;
    size_t  size  = sizeof(value);
    return ::sysctlbyname(name, &value, &size, nullptr, 0) == 0 ? value : 0;
}
#endif

QHash<qint64, Identity> identities()
{
    QHash<qint64, Identity> result;
#ifdef Q_OS_WIN
    const HANDLE snapshot = ::CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE)
        return result;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    if (::Process32FirstW(snapshot, &entry)) {
        do {
            const HANDLE handle = ::OpenProcess(
                PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, entry.th32ProcessID);
            if (handle == nullptr)
                continue;
            const auto start = processStart(handle);
            ::CloseHandle(handle);
            if (!start.isEmpty())
                result.insert(entry.th32ProcessID, {entry.th32ParentProcessID, start});
        } while (::Process32NextW(snapshot, &entry));
    }
    ::CloseHandle(snapshot);
#elif defined(Q_OS_MACOS)
    const int bytes = ::proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
    if (bytes <= 0 || bytes > 16 * 1024 * 1024)
        return result;
    QList<pid_t> pids(bytes / static_cast<int>(sizeof(pid_t)) + 128);
    const int    count = ::proc_listpids(
        PROC_ALL_PIDS, 0, pids.data(), static_cast<int>(pids.size() * sizeof(pid_t)));
    for (int i = 0; i < count / static_cast<int>(sizeof(pid_t)); ++i) {
        if (pids[i] > 0)
            result.insert(pids[i], identity(pids[i]));
    }
#elif defined(Q_OS_LINUX)
    for (const auto &name :
         QDir(QStringLiteral("/proc")).entryList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        bool       ok  = false;
        const auto pid = name.toLongLong(&ok);
        if (ok && pid > 0)
            result.insert(pid, identity(pid));
    }
#endif
    return result;
}

} // namespace

QJsonObject QSocResourceUsage::system()
{
    const auto  sampled = std::chrono::duration_cast<std::chrono::nanoseconds>(
                              std::chrono::steady_clock::now().time_since_epoch())
                              .count();
    QJsonObject result{
        {"memory_available_kind", "unknown"},
        {"sampled_at_ns", static_cast<qint64>(sampled)},
        {"memory_effective_available_kind", "unknown"},
        {"memory_effective_scope", "unknown"},
        {"memory_cgroup_version", "unknown"},
        {"memory_cgroup_limited", QJsonValue::Null},
        {"memory_effective_available_bytes", QJsonValue::Null}};
    for (const auto *key :
         {"cpu_logical_count",
          "cpu_total_ns",
          "cpu_busy_ns",
          "memory_total_bytes",
          "memory_available_bytes"})
        result.insert(QLatin1String(key), QJsonValue::Null);
#ifdef Q_OS_WIN
    const auto logical = ::GetActiveProcessorCount(ALL_PROCESSOR_GROUPS);
#elif defined(Q_OS_UNIX)
    const auto logical = ::sysconf(_SC_NPROCESSORS_ONLN);
#else
    const auto logical = QThread::idealThreadCount();
#endif
    if (logical > 0)
        result.insert("cpu_logical_count", static_cast<qint64>(logical));
#ifdef Q_OS_LINUX
    const auto fields
        = readFile(QStringLiteral("/proc/stat")).split('\n').value(0).simplified().split(' ');
    if (fields.size() >= 9 && fields[0] == "cpu") {
        quint64 total = 0;
        bool    valid = true;
        for (int i = 1; i <= 8; ++i) {
            bool       ok    = false;
            const auto ticks = fields[i].toULongLong(&ok);
            valid = valid && ok && ticks <= (std::numeric_limits<quint64>::max)() - total;
            if (valid)
                total += ticks;
        }
        const long frequency = ::sysconf(_SC_CLK_TCK);
        if (valid && frequency > 0) {
            const auto idle = fields[4].toULongLong() + fields[5].toULongLong();
            result.insert("cpu_total_ns", nanoseconds(total, frequency));
            result.insert("cpu_busy_ns", nanoseconds(total - idle, frequency));
        }
    }
    const auto memory = kilobytes(readFile(QStringLiteral("/proc/meminfo")));
    copyMemory(result, memory, "MemTotal:", "memory_total_bytes");
    copyMemory(result, memory, "MemAvailable:", "memory_available_bytes");
    if (!result.value("memory_available_bytes").isNull())
        result.insert("memory_available_kind", "estimate");
    detail::sampleCgroupMemory(result, QStringLiteral("/proc/self"));
#elif defined(Q_OS_WIN)
    FILETIME idle{}, kernel{}, user{};
    // GetSystemTimes reports only the calling processor group above 64 CPUs.
    if (logical <= 64 && ::GetSystemTimes(&idle, &kernel, &user)) {
        const auto total = fileTime(kernel) + fileTime(user);
        result.insert("cpu_total_ns", nanoseconds(total, 10000000));
        result.insert("cpu_busy_ns", nanoseconds(total - fileTime(idle), 10000000));
    }
    MEMORYSTATUSEX memory{};
    memory.dwLength = sizeof(memory);
    if (::GlobalMemoryStatusEx(&memory)) {
        result.insert("memory_total_bytes", number(memory.ullTotalPhys));
        result.insert("memory_available_bytes", number(memory.ullAvailPhys));
        result.insert("memory_available_kind", "native");
    }
#elif defined(Q_OS_MACOS)
    const auto                host = ::mach_host_self();
    host_cpu_load_info_data_t cpu{};
    mach_msg_type_number_t    count = HOST_CPU_LOAD_INFO_COUNT;
    if (::host_statistics(host, HOST_CPU_LOAD_INFO, reinterpret_cast<host_info_t>(&cpu), &count)
        == KERN_SUCCESS) {
        quint64 total = 0;
        for (const auto ticks : cpu.cpu_ticks)
            total += ticks;
        const auto frequency = ::sysconf(_SC_CLK_TCK);
        if (frequency > 0) {
            result.insert("cpu_total_ns", nanoseconds(total, frequency));
            result
                .insert("cpu_busy_ns", nanoseconds(total - cpu.cpu_ticks[CPU_STATE_IDLE], frequency));
        }
    }
    const auto total = sysctlNumber("hw.memsize");
    if (total > 0)
        result.insert("memory_total_bytes", number(total));
    vm_statistics64_data_t memory{};
    count              = HOST_VM_INFO64_COUNT;
    vm_size_t pageSize = 0;
    if (::host_page_size(host, &pageSize) == KERN_SUCCESS
        && ::host_statistics64(host, HOST_VM_INFO64, reinterpret_cast<host_info64_t>(&memory), &count)
               == KERN_SUCCESS) {
        // Speculative pages are included in free_count; do not count them twice.
        const quint64 available = (static_cast<quint64>(memory.free_count) + memory.inactive_count)
                                  * pageSize;
        result.insert("memory_available_bytes", number(available));
        result.insert("memory_available_kind", "estimate");
    }
    ::mach_port_deallocate(::mach_task_self(), host);
#endif
    return result;
}

QJsonObject QSocResourceUsage::process(qint64 pid)
{
    auto result = emptyProcess(pid);
#ifdef Q_OS_WIN
    const qint64 maximumPid = (std::numeric_limits<DWORD>::max)();
#else
    const qint64 maximumPid = (std::numeric_limits<int>::max)();
#endif
    if (pid <= 0 || pid > maximumPid)
        return result;
#ifdef Q_OS_LINUX
    const auto before = identity(pid);
    if (before.start.isEmpty())
        return result;
    result.insert("start_id", before.start);
    const auto fields    = processFields(pid);
    const auto frequency = ::sysconf(_SC_CLK_TCK);
    if (fields.size() > 12 && frequency > 0) {
        bool       userOk = false, kernelOk = false;
        const auto user   = fields[11].toULongLong(&userOk);
        const auto kernel = fields[12].toULongLong(&kernelOk);
        if (userOk && kernelOk)
            result.insert("cpu_time_ns", nanoseconds(user + kernel, frequency));
    }
    const auto directory = QStringLiteral("/proc/%1/").arg(pid);
    const auto status    = kilobytes(readFile(directory + "status"));
    copyMemory(result, status, "VmRSS:", "resident_bytes");
    copyMemory(result, status, "VmHWM:", "peak_resident_bytes");
    const auto mappings = kilobytes(readFile(directory + "smaps_rollup"));
    copyMemory(result, mappings, "Pss:", "proportional_bytes");
    if (mappings.contains("Private_Clean:") && mappings.contains("Private_Dirty:"))
        result.insert(
            "private_resident_bytes",
            number(mappings.value("Private_Clean:") + mappings.value("Private_Dirty:")));
    if (identity(pid).start != before.start)
        return emptyProcess(pid);
#elif defined(Q_OS_WIN)
    const HANDLE handle = ::OpenProcess(
        PROCESS_QUERY_LIMITED_INFORMATION | SYNCHRONIZE, FALSE, static_cast<DWORD>(pid));
    if (handle == nullptr)
        return result;
    FILETIME start{}, end{}, kernel{}, user{};
    if (::GetProcessTimes(handle, &start, &end, &kernel, &user)) {
        result.insert("start_id", QString::number(fileTime(start)));
        result.insert("cpu_time_ns", nanoseconds(fileTime(kernel) + fileTime(user), 10000000));
    }
    PROCESS_MEMORY_COUNTERS_EX memory{};
    if (::K32GetProcessMemoryInfo(
            handle, reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory), sizeof(memory))) {
        result.insert("resident_bytes", number(memory.WorkingSetSize));
        result.insert("peak_resident_bytes", number(memory.PeakWorkingSetSize));
        result.insert("private_commit_bytes", number(memory.PrivateUsage));
    }
    if (::WaitForSingleObject(handle, 0) != WAIT_TIMEOUT)
        result = emptyProcess(pid);
    ::CloseHandle(handle);
#elif defined(Q_OS_MACOS)
    const auto before = identity(pid);
    if (before.start.isEmpty())
        return result;
    result.insert("start_id", before.start);
    proc_taskinfo info{};
    if (::proc_pidinfo(static_cast<int>(pid), PROC_PIDTASKINFO, 0, &info, sizeof(info))
        == sizeof(info)) {
        // libproc counters use hardware ticks, including under translation.
        result.insert(
            "cpu_time_ns",
            nanoseconds(info.pti_total_user + info.pti_total_system, sysctlNumber("hw.tbfrequency")));
        result.insert("resident_bytes", number(info.pti_resident_size));
    }
    rusage_info_v0 usage{};
    if (::proc_pid_rusage(
            static_cast<int>(pid), RUSAGE_INFO_V0, reinterpret_cast<rusage_info_t *>(&usage))
        == 0)
        result.insert("footprint_bytes", number(usage.ri_phys_footprint));
    if (identity(pid).start != before.start)
        return emptyProcess(pid);
#endif
    return result;
}

QJsonArray QSocResourceUsage::processTree(qint64 rootPid)
{
    QJsonArray result;
    const auto records = identities();
    if (!records.contains(rootPid) || records.value(rootPid).start.isEmpty())
        return result;
    QMultiHash<qint64, qint64> children;
    for (auto it = records.cbegin(); it != records.cend(); ++it) {
        if (it.value().start.isEmpty())
            continue;
#ifdef Q_OS_WIN
        const auto parent = records.constFind(it.value().parent);
        if (parent == records.cend()
            || !detail::followsParentStart(parent.value().start, it.value().start))
            continue;
#endif
        children.insert(it.value().parent, it.key());
    }
    QList<qint64> pending{rootPid};
    QSet<qint64>  seen;
    while (!pending.isEmpty()) {
        const auto pid = pending.takeLast();
        if (seen.contains(pid))
            continue;
        seen.insert(pid);
        auto item = process(pid);
        if (item.value("start_id").toString() != records.value(pid).start)
            continue;
        item.insert("parent_pid", records.value(pid).parent);
        result.append(item);
        pending.append(children.values(pid));
    }
    if (process(rootPid).value("start_id").toString() != records.value(rootPid).start)
        return {};
    return result;
}

QJsonObject QSocResourceUsage::storage(const QString &path)
{
    QJsonObject result{
        {"path", path},
        {"valid", false},
        {"root_path", QJsonValue::Null},
        {"total_bytes", QJsonValue::Null},
        {"available_bytes", QJsonValue::Null},
        {"read_only", QJsonValue::Null}};
    if (path.isEmpty())
        return result;
    QStorageInfo info(path);
    info.refresh();
    if (!info.isValid() || !info.isReady())
        return result;
    result.insert("valid", true);
    result.insert("root_path", info.rootPath());
    if (info.bytesTotal() >= 0)
        result.insert("total_bytes", info.bytesTotal());
    if (info.bytesAvailable() >= 0)
        result.insert("available_bytes", info.bytesAvailable());
    result.insert("read_only", info.isReadOnly());
    return result;
}
