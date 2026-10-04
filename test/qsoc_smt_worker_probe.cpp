// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "smt/qsocsmtengine.h"
#include "smt/qsocsmtservice.h"
#include "smt/qsocsmtworker.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <thread>
#include <utility>
#include <vector>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QSaveFile>

#ifdef Q_OS_WIN
#include <windows.h>
#elif defined(Q_OS_MACOS)
#include <mach/mach.h>
#include <sys/resource.h>
#endif

#ifdef Q_OS_MACOS
namespace {

struct InheritedLimit
{
    quint64 initialVirtual = 0;
    quint64 originalHard   = 0;
    quint64 hard           = 0;
};

InheritedLimit inheritedLimit;

bool configureInheritedLimit()
{
    mach_task_basic_info_data_t info{};
    mach_msg_type_number_t      count = MACH_TASK_BASIC_INFO_COUNT;
    rlimit                      original{};
    if (::task_info(
            mach_task_self(), MACH_TASK_BASIC_INFO, reinterpret_cast<task_info_t>(&info), &count)
            != KERN_SUCCESS
        || ::getrlimit(RLIMIT_AS, &original) != 0)
        return false;
    constexpr quint64 budget = QSocSmtService::memoryLimitMiB * 1024ULL * 1024ULL;
    if (info.virtual_size < 2
        || info.virtual_size > (std::numeric_limits<quint64>::max() - budget) / 2)
        return false;
    const quint64 target = info.virtual_size + budget;
    const quint64 hard   = qMin<quint64>(target + info.virtual_size / 2, original.rlim_max);
    if (hard <= target)
        return false;
    const rlimit inherited{static_cast<rlim_t>(hard), static_cast<rlim_t>(hard)};
    if (::setrlimit(RLIMIT_AS, &inherited) != 0)
        return false;
    inheritedLimit = {info.virtual_size, original.rlim_max, hard};
    return true;
}

} // namespace
#endif

QJsonObject execute(const QJsonObject &request)
{
    const auto mode = request.value("smtlib").toString();
#ifdef Q_OS_MACOS
    if (mode.contains("probe-inherited-limit")) {
        rlimit installed{};
        if (::getrlimit(RLIMIT_AS, &installed) != 0)
            std::_Exit(28);
        auto result = QSocSmtEngine::execute(request);
        result.insert("probe_initial_virtual", QString::number(inheritedLimit.initialVirtual));
        result.insert("probe_original_hard", QString::number(inheritedLimit.originalHard));
        result.insert("probe_inherited_hard", QString::number(inheritedLimit.hard));
        result.insert("probe_installed_soft", QString::number(installed.rlim_cur));
        result.insert("probe_installed_hard", QString::number(installed.rlim_max));
        return result;
    }
#endif
    if (mode.contains("probe-crash")) {
        std::abort();
    }
    if (mode.contains("probe-memory")) {
        const auto exceeded
            = QSocSmtService::failure("error", "Memory probe exceeded its allocation cap");
        constexpr size_t blockBytes = 16 * 1024 * 1024;
        constexpr int    maxBlocks  = (QSocSmtService::memoryLimitMiB + 256) / 16;
        try {
            std::vector<std::unique_ptr<char[]>> blocks;
            for (int index = 0; index < maxBlocks; ++index) {
                auto           block = std::make_unique<char[]>(blockBytes);
                volatile char *data  = block.get();
                for (size_t offset = 0; offset < blockBytes; offset += 4096)
                    data[offset] = 1;
                blocks.push_back(std::move(block));
            }
            return exceeded;
        } catch (const std::bad_alloc &) {
            std::_Exit(12);
        }
    }
    if (mode.contains("probe-output")) {
        auto result = QSocSmtService::failure("error", "Probe output");
        result.insert("output", QString(QSocSmtService::outputLimit + 1, 'x'));
        return result;
    }
    if (mode.contains("probe-partial")) {
        std::_Exit(0);
    }
    using Phase    = QSocSmtEngine::Phase;
    Phase selected = Phase::Solve;
    if (mode.contains("probe-parse")) {
        selected = Phase::Parse;
    } else if (mode.contains("probe-verify")) {
        selected = Phase::Verify;
    } else if (mode.contains("probe-serialize")) {
        selected = Phase::Serialize;
    }
    QString marker;
    for (const auto &line : mode.split('\n')) {
        if (line.startsWith("; probe-ready: "))
            marker = line.mid(15);
    }
    const auto result = QSocSmtEngine::execute(request, [selected, marker](Phase phase) {
        if (phase != selected) {
            return;
        }
        std::signal(SIGTERM, SIG_IGN);
        if (!marker.isEmpty()) {
            QSaveFile  ready(marker);
            const auto pid = QByteArray::number(QCoreApplication::applicationPid());
            if (!ready.open(QIODevice::WriteOnly) || ready.write(pid) != pid.size()
                || !ready.commit())
                std::_Exit(19);
        }
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    return result;
}

int main(int argc, char **argv)
{
    const auto startup = qEnvironmentVariable("QSOC_TEST_SMT_STARTUP");
#ifdef Q_OS_MACOS
    if (startup == "finite-cap" && !configureInheritedLimit())
        return 28;
#endif
    const auto limits = QSocSmtEngine::applyLimits();
    if (limits != QSocProcessLimits::ApplyResult::Success)
        return static_cast<int>(limits);
    if (startup == "exit")
        return 23;
    if (startup == "crash") {
#ifdef Q_OS_WIN
        ::SetErrorMode(SEM_NOGPFAULTERRORBOX);
        ::RaiseException(EXCEPTION_ACCESS_VIOLATION, EXCEPTION_NONCONTINUABLE, 0, nullptr);
#else
        std::abort();
#endif
    }
    return QSocSmtWorker::run(argc, argv, execute);
}
