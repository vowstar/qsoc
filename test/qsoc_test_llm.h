// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOC_TEST_LLM_H
#define QSOC_TEST_LLM_H

#include "qsoc_test_pty.h"

#include <nlohmann/json.hpp>

#include <QTemporaryDir>

#include <memory>

struct QSocTestLlmMock
{
    QTemporaryDir dir{QDir::tempPath() + QStringLiteral("/test_qsoc_llm_XXXXXX")};
    std::unique_ptr<QSocTestPty::BoundedProcess> process
        = std::make_unique<QSocTestPty::BoundedProcess>();
    int port = 0;

    QString log() const { return QDir(dir.path()).filePath(QStringLiteral("requests.jsonl")); }

    bool start(const QMap<QString, QString> &variables, const QString &failMode = {})
    {
        port = QSocTestPty::pickFreePort();
        if (!dir.isValid() || port <= 0) {
            return false;
        }
        auto environment = QSocTestPty::isolatedEnvironment(dir.path());
        environment.insert(QStringLiteral("MOCK_TTL"), QStringLiteral("120"));
        environment.insert(QStringLiteral("MOCK_REQUEST_LOG"), log());
        for (auto it = variables.constBegin(); it != variables.constEnd(); ++it) {
            environment.insert(it.key(), it.value());
        }
        process->setProcessEnvironment(environment);
        process->setStandardOutputFile(QDir(dir.path()).filePath(QStringLiteral("mock.out")));
        process->setStandardErrorFile(QDir(dir.path()).filePath(QStringLiteral("mock.err")));
        QStringList arguments{QString::number(port)};
        if (!failMode.isEmpty()) {
            arguments.append(failMode);
        }
        process->start(QString::fromUtf8(QSOC_MOCK_LLM_PATH), arguments);
        return process->waitForStarted(5000)
               && QSocTestPty::waitForMockReady(*process, port, 45000);
    }

    QList<nlohmann::json> requests() const
    {
        QFile file(log());
        if (!file.open(QIODevice::ReadOnly)) {
            return {};
        }
        QList<nlohmann::json> out;
        for (const QByteArray &line : file.readAll().split('\n')) {
            if (!line.trimmed().isEmpty()) {
                out.append(nlohmann::json::parse(line.toStdString()));
            }
        }
        return out;
    }
};

#endif // QSOC_TEST_LLM_H
