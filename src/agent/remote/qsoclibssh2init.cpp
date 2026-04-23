// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/remote/qsoclibssh2init.h"

#include <libssh2.h>

#include <atomic>
#include <cstdlib>
#include <mutex>

namespace {

std::once_flag    g_initOnce;
std::atomic<int>  g_useCount{0};
std::atomic<bool> g_initialized{false};

void atexitShutdown()
{
    if (g_initialized.load()) {
        libssh2_exit();
        g_initialized.store(false);
    }
}

} // namespace

void QSocLibSsh2Init::ensure()
{
    std::call_once(g_initOnce, []() {
        libssh2_init(0);
        g_initialized.store(true);
        std::atexit(&atexitShutdown);
    });
    g_useCount.fetch_add(1, std::memory_order_relaxed);
}

int QSocLibSsh2Init::useCount()
{
    return g_useCount.load(std::memory_order_relaxed);
}
