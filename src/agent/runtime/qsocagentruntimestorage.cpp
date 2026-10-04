// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
#include "agent/qsocagent.h"
#include "agent/qsoccontextrestore.h"
#include "agent/qsocfilehistory.h"
#include "agent/qsocsubagenttasksource.h"
#include "agent/remote/qsocsftpclient.h"
#include "agent/remote/qsoctoolremote.h"
#include "agent/runtime/qsocagentruntime_p.h"
#include "agent/tool/qsoctoolagent.h"
#include "agent/tool/qsoctoolfile.h"
#include "agent/tool/qsoctoolpath.h"
#include "agent/tool/qsoctoolskill.h"
#include "common/qsocprojectmanager.h"
#include <algorithm>
#include <limits>
#include <QDir>
#include <QFile>
#include <QStandardPaths>
#include <QTextStream>

void QSocAgentRuntime::wireSessionTools()
{
    if (d->subAgentTaskSource) {
        d->subAgentTaskSource->setTranscriptDir(
            d->currentSession ? d->currentSession->filePath() + QStringLiteral(".agents")
                              : QString());
    }
    auto *history = d->currentFileHistory.get();
    if (history) {
        history->setLiveAccessor(
            isRemote() ? remoteLiveFileAccessor(d->remoteConn)
                       : QSocFileHistory::localAccessor(
                             d->projectManager->getProjectPath(),
                             [this](const QString &path, QString *entry) {
                                 return d->pathContext->resolveWritableEntry(path, entry);
                             }));
    }
    for (auto *registry : {d->localRegistry, d->remoteRegistry}) {
        if (!registry)
            continue;
        auto *write = registry->getTool("write_file");
        auto *edit  = registry->getTool("edit_file");
        if (auto *tool = dynamic_cast<QSocToolFileWrite *>(write))
            tool->setFileHistory(history);
        if (auto *tool = dynamic_cast<QSocToolFileEdit *>(edit))
            tool->setFileHistory(history);
        if (auto *tool = dynamic_cast<QSocToolRemoteFileWrite *>(write))
            tool->setFileHistory(history);
        if (auto *tool = dynamic_cast<QSocToolRemoteFileEdit *>(edit))
            tool->setFileHistory(history);
    }
}

void QSocAgentRuntime::wireContextRestore()
{
    d->agent->setCandidateRestoreProvider([this](const json &recentTail, qint64 remainingTokens) {
        auto                  *agent          = d->agent;
        auto                  *projectManager = d->projectManager;
        auto                  *remoteConn     = d->remoteConn;
        auto                  *pathContext    = d->pathContext;
        const auto            &invokedSkills  = d->invokedSkills;
        QMap<QString, QString> skillPaths;
        for (const auto &skill : QSocToolSkillFind(nullptr, projectManager).scanAllSkills())
            skillPaths.insert("/" + skill.name, skill.path);
        const QSocAgentConfig             cfg = agent->getConfig();
        QSocContextRestoreBuilder::Inputs inputs;
        inputs.enabled     = cfg.contextRestoreEnabled;
        inputs.totalBudget = static_cast<int>(
            qMin<qint64>(remainingTokens, std::numeric_limits<int>::max()));
        inputs.estimateTokens = [agent](const QString &text) { return agent->estimateTokens(text); };
        inputs.truncateTokens = [agent](const QString &text, int maxTokens) {
            return agent->truncateTokens(text, maxTokens);
        };
        inputs.maxFiles          = cfg.contextRestoreMaxFiles;
        inputs.fileBudget        = cfg.contextRestoreFileBudget;
        inputs.maxTokensPerFile  = cfg.contextRestoreMaxTokensFile;
        inputs.maxTokensPerSkill = cfg.contextRestoreMaxTokensSkill;
        inputs.skillsBudget      = cfg.contextRestoreSkillBudget;

        const qint64 maxReadBytes = qBound<qint64>(
            qint64(1),
            static_cast<qint64>(cfg.contextRestoreMaxTokensFile) * 8,
            qint64(1024 * 1024));
        if (remoteConn->session() != nullptr && remoteConn->sftp() != nullptr) {
            inputs.candidatePaths  = remoteConn->path()->readState().pathsByRecencyDesc(0);
            inputs.readFileBounded = [remoteConn, maxReadBytes](const QString &path) {
                QSocContextRestoreBuilder::FileRead result;
                auto                               *sftp = remoteConn->sftp();
                if (sftp == nullptr) {
                    return result;
                }
                QString          error;
                const QByteArray data = sftp->readFile(path, maxReadBytes + 1, &error);
                result.available      = error.isEmpty();
                result.oversized      = data.size() > maxReadBytes;
                if (result.available && !result.oversized) {
                    result.content = QString::fromUtf8(data);
                }
                return result;
            };
        } else if (pathContext != nullptr) {
            inputs.candidatePaths  = pathContext->readState().pathsByRecencyDesc(0);
            inputs.readFileBounded = [maxReadBytes](const QString &path) {
                QSocContextRestoreBuilder::FileRead result;
                QFile                               file(path);
                if (!file.open(QIODevice::ReadOnly)) {
                    return result;
                }
                result.available = true;
                result.oversized = file.size() > maxReadBytes;
                if (!result.oversized) {
                    const QByteArray data = file.read(maxReadBytes + 1);
                    result.oversized      = data.size() > maxReadBytes;
                    result.content        = result.oversized ? QString() : QString::fromUtf8(data);
                }
                return result;
            };
        }

        /* Exclusions: qsoc memory files (already re-injected per turn by
         * recall) and files still present in the kept window. */
        QSet<QString> excluded;
        const QString userMem
            = QDir(QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation))
                  .filePath(QStringLiteral("memory"));
        QString projMem;
        if (projectManager != nullptr && !projectManager->getProjectPath().isEmpty()) {
            projMem
                = QDir(projectManager->getProjectPath()).filePath(QStringLiteral(".qsoc/memory"));
        }
        for (const QString &path : inputs.candidatePaths) {
            if (path.startsWith(userMem) || (!projMem.isEmpty() && path.startsWith(projMem))) {
                excluded.insert(path);
            }
        }
        excluded.unite(QSocContextRestoreBuilder::recentFilePaths(recentTail));
        inputs.excludedPaths = excluded;

        /* Skills, most-recent first; body re-read by name. */
        QList<QPair<quint64, QString>> ordered;
        for (auto it = invokedSkills.constBegin(); it != invokedSkills.constEnd(); ++it) {
            ordered.append({it.value(), it.key()});
        }
        std::sort(ordered.begin(), ordered.end(), [](const auto &lhs, const auto &rhs) {
            return lhs.first > rhs.first;
        });
        for (const auto &pair : ordered) {
            inputs.skillNames.append(pair.second);
        }
        const qint64 maxSkillBytes = qBound<qint64>(
            qint64(1),
            static_cast<qint64>(cfg.contextRestoreMaxTokensSkill) * 8,
            qint64(1024 * 1024));
        inputs.readSkill = [&skillPaths,
                            maxSkillBytes](const QString &name) -> std::optional<QString> {
            const QString path = skillPaths.value(QStringLiteral("/") + name);
            QFile         file(path);
            if (path.isEmpty() || !file.open(QIODevice::ReadOnly)) {
                return std::nullopt;
            }
            if (file.size() > maxSkillBytes) {
                return QStringLiteral("Skill source: %1 (read the file before use)").arg(path);
            }
            const QByteArray data = file.read(maxSkillBytes + 1);
            if (data.size() > maxSkillBytes) {
                return QStringLiteral("Skill source: %1 (read the file before use)").arg(path);
            }
            QTextStream stream(data, QIODevice::ReadOnly | QIODevice::Text);
            return stream.readAll();
        };

        /* Running background sub-agents. */
        if (auto *reg = agent->getToolRegistry()) {
            if (auto *spawnTool = dynamic_cast<QSocToolAgent *>(
                    reg->getTool(QStringLiteral("agent")))) {
                if (auto *src = spawnTool->taskSource()) {
                    for (const auto &row : src->listTasks()) {
                        if (row.status == QSocTask::Status::Running) {
                            inputs.agents.append(
                                {.id = row.id, .label = row.label, .summary = row.summary});
                        }
                    }
                }
            }
        }

        return QSocContextRestoreBuilder::build(inputs);
    });
}
