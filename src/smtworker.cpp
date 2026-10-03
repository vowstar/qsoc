// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocsmtengine.h"
#include "common/qsocsmtworker.h"

int main(int argc, char **argv)
{
    if (!QSocSmtEngine::applyLimits())
        return 13;
    return QSocSmtWorker::run(argc, argv, [](const QJsonObject &request) {
        return QSocSmtEngine::execute(request);
    });
}
