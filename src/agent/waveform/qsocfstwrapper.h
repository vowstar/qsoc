// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#ifndef QSOCFSTWRAPPER_H
#define QSOCFSTWRAPPER_H

#include "fstinput.h"

#include <cstdint>
#include <memory>

class QSocFstWrapper
{
public:
    struct Limits
    {
        unsigned checkpointCount  = 16;
        uint64_t checkpointStride = 16ULL * 1024 * 1024;
        uint64_t memoryBytes      = 8ULL * 1024 * 1024;
    };

    struct Statistics
    {
        uint64_t sourceReads          = 0;
        uint64_t sourceBytes          = 0;
        uint64_t inflatedBytes        = 0;
        uint64_t checkpointHits       = 0;
        uint64_t checkpointCopies     = 0;
        uint64_t copiedPendingBytes   = 0;
        uint64_t restoredPendingBytes = 0;
        uint64_t copiesInsideBlock    = 0;
        uint64_t restoresInsideBlock  = 0;
        uint64_t coldRewinds          = 0;
        uint64_t memoryBytes          = 0;
        uint64_t peakMemoryBytes      = 0;
        unsigned checkpoints          = 0;
        bool     validated            = false;
    };

    explicit QSocFstWrapper(const QSocFstInput &source);
    QSocFstWrapper(const QSocFstInput &source, Limits limits);
    ~QSocFstWrapper();
    QSocFstWrapper(const QSocFstWrapper &)            = delete;
    QSocFstWrapper &operator=(const QSocFstWrapper &) = delete;

    QSocFstInput input();
    int          finish();
    int          pin(uint64_t offset);
    int          status() const;
    Statistics   statistics() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

#endif
