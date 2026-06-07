// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/qsocmemorydream.h"

#include "agent/qsocagent.h"
#include "agent/qsocmemorymanager.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QEventLoop>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

namespace {
constexpr qint64 kMsPerHour = 3600LL * 1000LL;
} // namespace

qint64 QSocMemoryDream::readLastConsolidatedMs(const QString &lockPath)
{
    const QFileInfo info(lockPath);
    return info.exists() ? info.lastModified().toMSecsSinceEpoch() : 0;
}

bool QSocMemoryDream::sessionGatePasses(
    const QList<QSocSession::Info> &sessions,
    qint64                          lastMs,
    const QString                  &currentId,
    int                             minSessions)
{
    int touched = 0;
    for (const auto &info : sessions) {
        if (info.id == currentId) {
            continue;
        }
        if (info.lastModified.toMSecsSinceEpoch() > lastMs) {
            touched++;
        }
    }
    return touched >= minSessions;
}

QSocMemoryDream::LockResult QSocMemoryDream::tryAcquireLock(const QString &lockPath, int staleHours)
{
    LockResult      result;
    const QFileInfo info(lockPath);
    const qint64    nowMs = QDateTime::currentMSecsSinceEpoch();

    if (info.exists()) {
        const qint64 mtimeMs = info.lastModified().toMSecsSinceEpoch();
        if ((nowMs - mtimeMs) < static_cast<qint64>(staleHours) * kMsPerHour) {
            /* A fresh lock means another process is consolidating. */
            return result;
        }
        result.priorMtimeMs = mtimeMs;
    }

    QDir().mkpath(QFileInfo(lockPath).absolutePath());
    QFile file(lockPath);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        return result;
    }
    file.write(QByteArray::number(QCoreApplication::applicationPid()));
    file.close();
    result.acquired = true;
    return result;
}

void QSocMemoryDream::rollbackLock(const QString &lockPath, qint64 priorMtimeMs)
{
    if (priorMtimeMs <= 0) {
        QFile::remove(lockPath); /* No prior lock: restore absent state. */
        return;
    }
    QFile file(lockPath);
    if (file.open(QIODevice::ReadWrite)) {
        file.setFileTime(
            QDateTime::fromMSecsSinceEpoch(priorMtimeMs), QFileDevice::FileModificationTime);
        file.close();
    }
}

QString QSocMemoryDream::consolidationPrompt()
{
    return QStringLiteral(
        "You are the memory-consolidation sub-agent for QSoC, performing a "
        "reflective pass over the memory files so future sessions orient "
        "quickly. You may only use memory_read, memory_write, and "
        "memory_delete.\n\n"
        "Review both scopes: call memory_read with scope 'all' to list current "
        "topics, then read those that look stale or overlapping.\n\n"
        "Steps:\n"
        "1. Merge near-duplicate topics into one: memory_write the merged topic "
        "in its scope, then memory_delete the leftovers.\n"
        "2. Convert relative dates ('yesterday') to absolute dates.\n"
        "3. memory_delete facts that are contradicted or clearly superseded.\n"
        "4. Keep each topic focused; do not invent facts not already present.\n\n"
        "The MEMORY.md index rebuilds automatically on every write/delete. If "
        "the memory is already tight, do nothing. Use only a few turns.");
}

QSocMemoryDream::QSocMemoryDream(
    QSocAgent *parent, QSocMemoryManager *memoryManager, QLLMService *llmService)
    : parent_(parent)
    , memoryManager_(memoryManager)
    , llmService_(llmService)
{}

QSocMemoryDream::Outcome QSocMemoryDream::maybeRun(
    const QString                          &projectPath,
    const QString                          &currentSessionId,
    const std::function<void(QSocAgent *)> &onSpawn)
{
    if (!parent_ || !memoryManager_ || !llmService_) {
        return {};
    }

    const QSocAgentConfig cfg = parent_->getConfig();
    if (!cfg.memoryDreamEnabled) {
        return {};
    }

    /* Lock in the project scope so serialization matches the project-based
     * session gate; fall back to the user scope when no project is open. */
    QString lockDir = memoryManager_->projectMemoryDir();
    if (lockDir.isEmpty()) {
        lockDir = memoryManager_->userMemoryDir();
    }
    if (lockDir.isEmpty()) {
        return {};
    }

    /* Nothing to consolidate yet: skip without touching the lock. Use the
     * uncapped count so the reported before/after are accurate past 50. */
    const int before = memoryManager_->topicFileCount("all");
    if (before == 0) {
        return {};
    }

    const QString lockPath = QDir(lockDir).filePath(QStringLiteral(".consolidate.lock"));
    const qint64  lastMs   = readLastConsolidatedMs(lockPath);
    const qint64  nowMs    = QDateTime::currentMSecsSinceEpoch();

    /* Time gate. */
    if (lastMs > 0 && (nowMs - lastMs) < static_cast<qint64>(cfg.memoryDreamMinHours) * kMsPerHour) {
        return {};
    }

    /* Session-count gate. */
    if (!sessionGatePasses(
            QSocSession::listAll(projectPath),
            lastMs,
            currentSessionId,
            cfg.memoryDreamMinSessions)) {
        return {};
    }

    /* Serialize across processes; steal a lock older than one hour. */
    const LockResult lock = tryAcquireLock(lockPath, 1);
    if (!lock.acquired) {
        return {};
    }

    /* Constrained child: memory tools (incl. delete) only. */
    QSocAgentConfig childCfg = cfg;
    childCfg.isSubAgent      = true;
    childCfg.toolsAllow      = QStringList{"memory_read", "memory_write", "memory_delete"};
    childCfg.toolsDeny.clear();
    childCfg.maxTurnsOverride    = 8;
    childCfg.autoLoadMemory      = false;
    childCfg.memoryRecallEnabled = false;
    childCfg.injectProjectMd     = false;
    childCfg.skillListing.clear();
    childCfg.criticalReminder.clear();
    childCfg.planMode = false;
    /* Local-only work, so never remote. Model/effort inherit the user's
     * config (memory_dream_model may override); no cost bias, no silent
     * model swap. */
    childCfg.remoteMode           = false;
    childCfg.hooks                = QSocHookConfig();
    childCfg.systemPromptOverride = consolidationPrompt();
    if (!cfg.memoryDreamModel.isEmpty()) {
        childCfg.modelId = cfg.memoryDreamModel;
    }

    QLLMService *childLlm = llmService_->clone(nullptr);
    if (!cfg.memoryDreamModel.isEmpty()) {
        childLlm->setCurrentModel(cfg.memoryDreamModel);
    }
    auto *child = new QSocAgent(nullptr, childLlm, parent_->getToolRegistry(), childCfg);
    childLlm->setParent(child);
    child->setMemoryManager(memoryManager_);

    if (onSpawn) {
        onSpawn(child);
    }

    QEventLoop loop;
    bool       terminal = false;
    bool       failed   = false;
    const auto finish   = [&loop, &terminal](bool) {
        if (terminal) {
            return;
        }
        terminal = true;
        loop.quit();
    };
    QObject::connect(child, &QSocAgent::runComplete, &loop, [finish](const QString &) {
        finish(false);
    });
    QObject::connect(child, &QSocAgent::runError, &loop, [&failed, finish](const QString &) {
        failed = true;
        finish(true);
    });
    QObject::connect(child, &QSocAgent::runAborted, &loop, [&failed, finish](const QString &) {
        failed = true;
        finish(true);
    });

    child->runStream(QStringLiteral("Consolidate the memory now."));
    if (!terminal) {
        loop.exec();
    }

    delete child;

    const int after = memoryManager_->topicFileCount("all");

    /* A child can error on its final turn (reasoning models sometimes do)
     * after it already merged/pruned. Only treat it as a failure to roll
     * back when nothing actually changed; otherwise keep the lock so the
     * gate does not re-run a wasteful dream every session, and report it. */
    if (failed && after == before) {
        rollbackLock(lockPath, lock.priorMtimeMs);
        return {};
    }

    return {.ran = true, .before = before, .after = after};
}
