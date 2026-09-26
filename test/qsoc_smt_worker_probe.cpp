// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocsmtengine.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>
#include <QJsonDocument>

int main()
{
    if (!QSocSmtEngine::applyLimits()) {
        return 13;
    }
    QByteArray input;
    char       buffer[4096];
    while (const auto count = std::fread(buffer, 1, sizeof(buffer), stdin)) {
        input.append(buffer, static_cast<qsizetype>(count));
    }
    const auto request = QJsonDocument::fromJson(input).object();
    const auto mode    = request.value("smtlib").toString();
    if (mode.contains("probe-crash")) {
        std::abort();
    }
    if (mode.contains("probe-memory")) {
        try {
            std::vector<std::unique_ptr<char[]>> blocks;
            while (true) {
                blocks.push_back(std::make_unique<char[]>(16 * 1024 * 1024));
            }
        } catch (const std::bad_alloc &) {
            std::_Exit(12);
        }
    }
    if (mode.contains("probe-output")) {
        const QByteArray output(1024 * 1024 + 1, 'x');
        std::fwrite(output.constData(), 1, static_cast<size_t>(output.size()), stdout);
        return 0;
    }
    if (mode.contains("probe-partial")) {
        std::fputs("{\"protocol\":1,\"solver_status\":\"sat\"", stdout);
        return 0;
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
    const auto result = QSocSmtEngine::execute(request, [selected](Phase phase) {
        if (phase != selected) {
            return;
        }
        std::signal(SIGTERM, SIG_IGN);
        while (true) {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });
    const auto output = QJsonDocument(result).toJson(QJsonDocument::Compact);
    std::fwrite(output.constData(), 1, static_cast<size_t>(output.size()), stdout);
    return 0;
}
