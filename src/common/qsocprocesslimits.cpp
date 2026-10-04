// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocprocesslimits.h"

#include <limits>
#include <vector>
#include <QProcess>

#ifdef Q_OS_WIN
#include <windows.h>
#else
#include <sys/resource.h>
#ifdef Q_OS_MACOS
#include <cerrno>
#include <mach/mach.h>
#endif
#endif

struct QSocProcessLimits::State
{
#ifdef Q_OS_WIN
    HANDLE                     job = nullptr;
    STARTUPINFOEXW             startup{};
    std::vector<unsigned char> attributes;
    bool                       initialized = false;

    ~State()
    {
        if (initialized)
            ::DeleteProcThreadAttributeList(startup.lpAttributeList);
        if (job != nullptr)
            ::CloseHandle(job);
    }
#endif
};

QSocProcessLimits::QSocProcessLimits()  = default;
QSocProcessLimits::~QSocProcessLimits() = default;

bool QSocProcessLimits::configure(QProcess &process, quint64 memoryBytes)
{
#ifdef Q_OS_WIN
    if (state_ || memoryBytes > std::numeric_limits<SIZE_T>::max())
        return false;
    auto state = std::make_unique<State>();
    state->job = ::CreateJobObjectW(nullptr, nullptr);
    if (state->job == nullptr)
        return false;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_PROCESS_MEMORY
                                              | JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    limits.ProcessMemoryLimit               = static_cast<SIZE_T>(memoryBytes);
    if (!::SetInformationJobObject(
            state->job, JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
        return false;
    SIZE_T size = 0;
    ::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    if (size == 0)
        return false;
    state->attributes.resize(size);
    state->startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(
        state->attributes.data());
    if (!::InitializeProcThreadAttributeList(state->startup.lpAttributeList, 1, 0, &size))
        return false;
    state->initialized = true;
    if (!::UpdateProcThreadAttribute(
            state->startup.lpAttributeList,
            0,
            PROC_THREAD_ATTRIBUTE_JOB_LIST,
            &state->job,
            sizeof(state->job),
            nullptr,
            nullptr))
        return false;
    auto *configuration = state.get();
    process.setCreateProcessArgumentsModifier(
        [configuration](QProcess::CreateProcessArguments *args) {
            configuration->startup.StartupInfo    = *args->startupInfo;
            configuration->startup.StartupInfo.cb = sizeof(STARTUPINFOEXW);
            args->startupInfo                     = &configuration->startup.StartupInfo;
            args->flags |= EXTENDED_STARTUPINFO_PRESENT;
        });
    state_ = std::move(state);
#else
    Q_UNUSED(process)
    Q_UNUSED(memoryBytes)
#endif
    return true;
}

QSocProcessLimits::ApplyResult QSocProcessLimits::apply(quint64 memoryBytes)
{
#ifdef Q_OS_WIN
    BOOL                                 member = FALSE;
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    if (!::IsProcessInJob(::GetCurrentProcess(), nullptr, &member) || !member
        || !::QueryInformationJobObject(
            nullptr, JobObjectExtendedLimitInformation, &limits, sizeof(limits), nullptr))
        return ApplyResult::QueryJob;
    if ((limits.BasicLimitInformation.LimitFlags & JOB_OBJECT_LIMIT_PROCESS_MEMORY) == 0
        || limits.ProcessMemoryLimit > memoryBytes)
        return ApplyResult::JobLimitMismatch;
    return ApplyResult::Success;
#elif defined(Q_OS_LINUX) || defined(Q_OS_MACOS)
    rlimit existing{};
    if (::getrlimit(RLIMIT_AS, &existing) != 0)
        return ApplyResult::ReadAddressLimit;
#ifdef Q_OS_MACOS
    const rlimit rejected{0, existing.rlim_max};
    const int    probeResult = ::setrlimit(RLIMIT_AS, &rejected);
    const int    probeError  = errno;
    if (probeResult == 0)
        return ApplyResult::CapabilityUnexpectedlyAllowed;
    if (probeResult != -1 || probeError != EINVAL)
        return ApplyResult::CapabilityUnexpectedError;
    rlimit unchanged{};
    if (::getrlimit(RLIMIT_AS, &unchanged) != 0)
        return ApplyResult::ReadCapabilityLimit;
    if (unchanged.rlim_cur != existing.rlim_cur || unchanged.rlim_max != existing.rlim_max)
        return ApplyResult::CapabilityLimitChanged;
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t      count = MACH_TASK_BASIC_INFO_COUNT;
    if (::task_info(
            mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count)
        != KERN_SUCCESS)
        return ApplyResult::TaskInfo;
    if (info.virtual_size > std::numeric_limits<quint64>::max() - memoryBytes)
        return ApplyResult::AddressSizeOverflow;
    memoryBytes += info.virtual_size;
#endif
    const auto   maximum = static_cast<rlim_t>(qMin<quint64>(memoryBytes, existing.rlim_max));
    const rlimit memory{maximum, maximum};
    const rlimit core{0, 0};
    if (::setrlimit(RLIMIT_AS, &memory) != 0)
        return ApplyResult::SetAddressLimit;
    if (::setrlimit(RLIMIT_CORE, &core) != 0)
        return ApplyResult::SetCoreLimit;
    rlimit installed{};
    if (::getrlimit(RLIMIT_AS, &installed) != 0)
        return ApplyResult::ReadInstalledLimit;
    if (installed.rlim_cur != maximum || installed.rlim_max != maximum)
        return ApplyResult::InstalledLimitMismatch;
    return ApplyResult::Success;
#else
    Q_UNUSED(memoryBytes)
    return ApplyResult::Unsupported;
#endif
}
