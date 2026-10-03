// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "smt/qsocsmtengine.h"
#include "smt/qsocsmtservice.h"
#include "smt/qsocsmtworker.h"

#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <thread>
#include <vector>
#include <QCoreApplication>
#include <QJsonDocument>
#include <QSaveFile>

QJsonObject execute(const QJsonObject &request)
{
    const auto mode = request.value("smtlib").toString();
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
    if (!QSocSmtEngine::applyLimits())
        return 13;
    return QSocSmtWorker::run(argc, argv, execute);
}
