// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocsmtengine.h"
#include "common/qsocsmtservice.h"

#include <cstdio>
#include <cstdlib>
#include <new>
#include <QJsonDocument>

int main()
{
    if (!QSocSmtEngine::applyLimits()) {
        return 13;
    }
    try {
        QByteArray input;
        char       buffer[4096];
        while (const auto count = std::fread(buffer, 1, sizeof(buffer), stdin)) {
            input.append(buffer, static_cast<qsizetype>(count));
            if (input.size() > 2 * 1024 * 1024) {
                return 14;
            }
        }
        QJsonParseError error;
        const auto      request = QJsonDocument::fromJson(input, &error);
        const auto      result  = error.error == QJsonParseError::NoError && request.isObject()
                                      ? QSocSmtEngine::execute(request.object())
                                      : QSocSmtService::failure("error", "Invalid worker request");
        const auto      output  = QJsonDocument(result).toJson(QJsonDocument::Compact);
        if (output.size() > QSocSmtService::outputLimit) {
            return 15;
        }
        const auto written
            = std::fwrite(output.constData(), 1, static_cast<size_t>(output.size()), stdout);
        std::fflush(stdout);
        return written == static_cast<size_t>(output.size()) ? 0 : 16;
    } catch (const std::bad_alloc &) {
        std::_Exit(12);
    }
}
