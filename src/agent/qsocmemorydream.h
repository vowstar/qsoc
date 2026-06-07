// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCMEMORYDREAM_H
#define QSOCMEMORYDREAM_H

#include "agent/qsocsession.h"

#include <functional>
#include <QList>
#include <QString>

class QSocAgent;
class QSocMemoryManager;
class QLLMService;

/**
 * @brief Memory consolidation ("dream") via a constrained sub-agent.
 * @details Periodically a forked child restricted to memory_read /
 *          memory_write / memory_delete reviews the topic files: it merges
 *          near-duplicates, converts relative dates to absolute, drops
 *          contradicted facts, and prunes MEMORY.md. Gated by a time
 *          window and a session-count threshold, and serialized across
 *          processes by a lock file whose mtime doubles as the
 *          last-consolidated timestamp.
 *
 *          Gate decisions and the prompt are pure statics (unit-testable);
 *          maybeRun() performs the spawn at REPL idle.
 */
class QSocMemoryDream
{
public:
    struct LockResult
    {
        bool   acquired     = false;
        qint64 priorMtimeMs = 0; /* For rollback on failure. */
    };

    /* What a maybeRun() call did, for a user-facing summary line. */
    struct Outcome
    {
        bool ran    = false; /* A consolidation child actually ran */
        int  before = 0;     /* Topic count before consolidation */
        int  after  = 0;     /* Topic count after consolidation */
    };

    /* Last-consolidated timestamp = the lock file's mtime (0 if absent). */
    static qint64 readLastConsolidatedMs(const QString &lockPath);

    /* True when at least minSessions sessions (excluding currentId) were
     * modified after lastMs. Pure; unit-testable. */
    static bool sessionGatePasses(
        const QList<QSocSession::Info> &sessions,
        qint64                          lastMs,
        const QString                  &currentId,
        int                             minSessions);

    /* Acquire the lock: steal it when the holder is older than staleHours,
     * otherwise fail. On success the file mtime is refreshed to now. */
    static LockResult tryAcquireLock(const QString &lockPath, int staleHours);

    /* Restore the prior state after a failed run so the gate reopens:
     * delete when there was no prior lock, else rewind the mtime. */
    static void rollbackLock(const QString &lockPath, qint64 priorMtimeMs);

    /* Consolidation prompt for the dream child (scope-aware). */
    static QString consolidationPrompt();

    QSocMemoryDream(QSocAgent *parent, QSocMemoryManager *memoryManager, QLLMService *llmService);

    /* Run consolidation if the time and session gates pass and the lock is
     * free. Returns true when a dream child actually ran. Synchronous;
     * safe at REPL idle. onSpawn, when set, is invoked with the child just
     * before it starts (so the caller can show status / wire abort). */
    Outcome maybeRun(
        const QString                          &projectPath,
        const QString                          &currentSessionId,
        const std::function<void(QSocAgent *)> &onSpawn = {});

private:
    QSocAgent         *parent_;
    QSocMemoryManager *memoryManager_;
    QLLMService       *llmService_;
};

#endif // QSOCMEMORYDREAM_H
