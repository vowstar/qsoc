// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocfstwrapper.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <new>

namespace {
constexpr uint32_t bufferBytes        = 128 * 1024;
constexpr unsigned maximumCheckpoints = 16;

uint64_t bigEndian(const unsigned char *bytes)
{
    uint64_t value = 0;
    for (unsigned index = 0; index < 8; ++index)
        value = (value << 8) | bytes[index];
    return value;
}

struct Budget
{
    bool reserve(uint64_t bytes)
    {
        if (bytes > limit - used)
            return false;
        used += bytes;
        peak = std::max(peak, used);
        return true;
    }

    void release(uint64_t bytes) { used -= bytes; }

    struct alignas(std::max_align_t) Allocation
    {
        uint64_t bytes;
    };

    static voidpf allocate(voidpf opaque, uInt count, uInt size)
    {
        auto &budget = *static_cast<Budget *>(opaque);
        if (size && count > (std::numeric_limits<size_t>::max() - sizeof(Allocation)) / size)
            return nullptr;
        const size_t bytes = size_t(count) * size + sizeof(Allocation);
        if (!budget.reserve(bytes))
            return nullptr;
        auto allocation = static_cast<Allocation *>(std::malloc(bytes));
        if (!allocation) {
            budget.release(bytes);
            return nullptr;
        }
        allocation->bytes = bytes;
        return allocation + 1;
    }

    static void free(voidpf opaque, voidpf address)
    {
        if (!address)
            return;
        auto allocation = static_cast<Allocation *>(address) - 1;
        static_cast<Budget *>(opaque)->release(allocation->bytes);
        std::free(allocation);
    }

    uint64_t limit = 0;
    uint64_t used  = 0;
    uint64_t peak  = 0;
};

struct Inflater
{
    explicit Inflater(Budget &owner)
        : budget(owner)
    {}
    Inflater(const Inflater &)            = delete;
    Inflater &operator=(const Inflater &) = delete;
    ~Inflater()
    {
        if (initialized)
            inflateEnd(&stream);
        budget.release(sizeof(Inflater));
    }

    bool contains(uint64_t position) const
    {
        return position >= outputOffset && position - outputOffset < outputCount;
    }

    Budget                                &budget;
    z_stream                               stream{};
    std::array<unsigned char, bufferBytes> input{};
    std::array<unsigned char, bufferBytes> output{};
    uint64_t                               sourceOffset = 17;
    uint64_t                               offset       = 0;
    uint64_t                               outputOffset = 0;
    uint64_t                               touched      = 0;
    uint32_t                               outputCount  = 0;
    bool                                   initialized  = false;
    bool                                   finished     = false;
};
} // namespace

struct QSocFstWrapper::State
{
    State(const QSocFstInput &input, const Limits &policy)
        : source(input)
        , limits(policy)
    {
        budget.limit = limits.memoryBytes;
        if (limits.checkpointCount > maximumCheckpoints || limits.checkpointStride < 4096
            || !budget.reserve(sizeof(State)) || !source.read_at || !source.size) {
            error = INPUT_RANGE;
            return;
        }
        error = source.size(source.user, &sourceSize);
        unsigned char header[17];
        uint32_t      done = 0;
        while (!error && done < sizeof(header)) {
            uint32_t count = 0;
            error = source.read_at(source.user, done, header + done, sizeof(header) - done, &count);
            ++stats.sourceReads;
            stats.sourceBytes += count;
            if (!error && (!count || count > sizeof(header) - done))
                error = INPUT_IO_ERROR;
            done += count;
        }
        if (error)
            return;
        expandedSize       = bigEndian(header + 9);
        const auto section = bigEndian(header + 1);
        if (header[0] != FST_BL_ZWRAPPER || section < 16 || sourceSize < 17
            || section != sourceSize - 1 || expandedSize < 9) {
            error = INPUT_MALFORMED;
            return;
        }
        frontier = fresh();
        if (!frontier)
            error = INPUT_RANGE;
    }

    std::unique_ptr<Inflater> allocate()
    {
        if (!budget.reserve(sizeof(Inflater)))
            return {};
        auto value = std::unique_ptr<Inflater>(new (std::nothrow) Inflater(budget));
        if (!value)
            budget.release(sizeof(Inflater));
        return value;
    }

    std::unique_ptr<Inflater> fresh()
    {
        auto value = allocate();
        if (!value)
            return {};
        value->stream.zalloc = Budget::allocate;
        value->stream.zfree  = Budget::free;
        value->stream.opaque = &budget;
        value->initialized   = inflateInit2(&value->stream, 31) == Z_OK;
        return value->initialized ? std::move(value) : nullptr;
    }

    std::unique_ptr<Inflater> copy(Inflater &original)
    {
        auto value = allocate();
        if (!value || inflateCopy(&value->stream, &original.stream) != Z_OK)
            return {};
        value->initialized = true;
        // inflateCopy keeps pointers to the caller's input and output buffers.
        std::memcpy(value->output.data(), original.output.data(), original.outputCount);
        if (original.stream.avail_in) {
            std::memcpy(value->input.data(), original.stream.next_in, original.stream.avail_in);
            value->stream.next_in = value->input.data();
        } else {
            value->stream.next_in = nullptr;
        }
        value->stream.next_out  = nullptr;
        value->stream.avail_out = 0;
        value->sourceOffset     = original.sourceOffset;
        value->offset           = original.offset;
        value->outputOffset     = original.outputOffset;
        value->outputCount      = original.outputCount;
        value->finished         = original.finished;
        stats.copiedPendingBytes += original.stream.avail_in;
        return value;
    }

    bool stopped()
    {
        if (!error && source.cancelled && source.cancelled(source.user))
            error = INPUT_CANCELLED;
        return error != INPUT_OK;
    }

    bool fill(Inflater &value)
    {
        if (value.stream.avail_in)
            return true;
        if (value.sourceOffset >= sourceSize)
            return false;
        const auto requested = static_cast<uint32_t>(
            std::min<uint64_t>(bufferBytes, sourceSize - value.sourceOffset));
        uint32_t received = 0;
        error             = source.read_at(
            source.user, value.sourceOffset, value.input.data(), requested, &received);
        ++stats.sourceReads;
        stats.sourceBytes += received;
        if (error)
            return false;
        if (!received || received > requested) {
            error = INPUT_IO_ERROR;
            return false;
        }
        value.sourceOffset += received;
        value.stream.next_in  = value.input.data();
        value.stream.avail_in = received;
        return true;
    }

    void checkpoint(Inflater &value)
    {
        if (!limits.checkpointCount || value.finished || !value.offset
            || value.offset % limits.checkpointStride != 0)
            return;
        const unsigned count = limits.checkpointCount - (anchor ? 1U : 0U);
        if (!count)
            return;
        unsigned slot = 0;
        for (unsigned index = 0; index < count; ++index) {
            if (checkpoints[index] && checkpoints[index]->offset == value.offset) {
                checkpoints[index]->touched = ++clock;
                return;
            }
            if (!checkpoints[index]) {
                slot = index;
                break;
            }
            if (checkpoints[index]->touched < checkpoints[slot]->touched)
                slot = index;
        }
        checkpoints[slot].reset();
        checkpoints[slot] = copy(value);
        if (checkpoints[slot]) {
            checkpoints[slot]->touched = ++clock;
            ++stats.checkpointCopies;
            if (!(value.stream.data_type & 128))
                ++stats.copiesInsideBlock;
        }
    }

    bool pump(Inflater &value, uint32_t maximum)
    {
        if (stopped() || value.finished)
            return false;
        const auto toCheckpoint = limits.checkpointStride - value.offset % limits.checkpointStride;
        const auto capacity     = static_cast<uint32_t>(std::min<uint64_t>(maximum, toCheckpoint));
        value.outputOffset      = value.offset;
        value.outputCount       = 0;
        while (!stopped() && !value.finished && value.outputCount == 0) {
            if (!fill(value) && error)
                return false;
            value.stream.next_out  = value.output.data();
            value.stream.avail_out = capacity;
            const auto before      = value.stream.avail_in;
            const int  result      = inflate(&value.stream, Z_NO_FLUSH);
            value.outputCount      = capacity - value.stream.avail_out;
            if (value.outputCount > expandedSize - value.offset) {
                error = INPUT_MALFORMED;
                return false;
            }
            value.offset += value.outputCount;
            stats.inflatedBytes += value.outputCount;
            if (source.progress)
                source.progress(source.user, value.sourceOffset - value.stream.avail_in);
            if (result == Z_STREAM_END) {
                if (value.sourceOffset == sourceSize && !value.stream.avail_in) {
                    value.finished = true;
                    if (value.offset != expandedSize)
                        error = INPUT_MALFORMED;
                } else {
                    auto       next      = value.stream.next_in;
                    const auto available = value.stream.avail_in;
                    if (inflateReset2(&value.stream, 31) != Z_OK)
                        error = INPUT_IO_ERROR;
                    value.stream.next_in  = next;
                    value.stream.avail_in = available;
                }
            } else if (result != Z_OK && result != Z_BUF_ERROR) {
                error = result == Z_MEM_ERROR ? INPUT_RANGE : INPUT_MALFORMED;
            } else if (!value.outputCount && before == value.stream.avail_in) {
                error = INPUT_MALFORMED;
            }
        }
        checkpoint(value);
        return !error;
    }

    Inflater *choose(uint64_t offset)
    {
        if (offset >= frontier->offset || frontier->contains(offset))
            return frontier.get();
        if (backward && backward->contains(offset))
            return backward.get();
        Inflater *best = anchor && (anchor->offset <= offset || anchor->contains(offset))
                             ? anchor.get()
                             : nullptr;
        for (auto &entry : checkpoints) {
            if (entry && entry->offset <= offset && (!best || entry->offset > best->offset))
                best = entry.get();
        }
        if (backward && backward->offset <= offset && (!best || backward->offset >= best->offset))
            return backward.get();
        backward.reset();
        if (best) {
            backward = copy(*best);
            if (backward) {
                best->touched = ++clock;
                ++stats.checkpointHits;
                stats.restoredPendingBytes += best->stream.avail_in;
                if (!(best->stream.data_type & 128))
                    ++stats.restoresInsideBlock;
            }
        }
        if (!backward) {
            for (auto &entry : checkpoints)
                entry.reset();
            backward = fresh();
            ++stats.coldRewinds;
        }
        if (!backward)
            error = INPUT_RANGE;
        return backward.get();
    }

    int read(uint64_t offset, void *destination, uint32_t requested, uint32_t *received)
    {
        *received = 0;
        if (stopped())
            return error;
        if (!requested || requested > bufferBytes || offset > expandedSize)
            return INPUT_RANGE;
        if (offset == expandedSize)
            return INPUT_EOF;
        auto value = choose(offset);
        if (!value)
            return error;
        while (!error && value->offset < offset)
            pump(
                *value,
                static_cast<uint32_t>(std::min<uint64_t>(bufferBytes, offset - value->offset)));
        const auto wanted = static_cast<uint32_t>(
            std::min<uint64_t>(requested, expandedSize - offset));
        auto output = static_cast<unsigned char *>(destination);
        while (!error && *received < wanted) {
            if (!value->contains(offset) && !pump(*value, wanted - *received))
                break;
            const auto position = static_cast<uint32_t>(offset - value->outputOffset);
            const auto count    = std::min(wanted - *received, value->outputCount - position);
            std::memcpy(output + *received, value->output.data() + position, count);
            *received += count;
            offset += count;
        }
        return error;
    }

    Budget                                                    budget;
    QSocFstInput                                              source;
    Limits                                                    limits;
    Statistics                                                stats;
    uint64_t                                                  sourceSize   = 0;
    uint64_t                                                  expandedSize = 0;
    uint64_t                                                  clock        = 0;
    int                                                       error        = INPUT_OK;
    std::unique_ptr<Inflater>                                 frontier;
    std::unique_ptr<Inflater>                                 backward;
    std::unique_ptr<Inflater>                                 anchor;
    std::array<std::unique_ptr<Inflater>, maximumCheckpoints> checkpoints;
};

QSocFstWrapper::QSocFstWrapper(const QSocFstInput &source)
    : QSocFstWrapper(source, Limits{})
{
    auto &s = *state_;
    if (s.limits.checkpointCount)
        s.limits.checkpointStride = std::max(
            s.limits.checkpointStride, s.expandedSize / (s.limits.checkpointCount + 1) + 1);
}
QSocFstWrapper::QSocFstWrapper(const QSocFstInput &source, Limits limits)
    : state_(std::make_unique<State>(source, limits))
{}
QSocFstWrapper::~QSocFstWrapper() = default;

QSocFstInput QSocFstWrapper::input()
{
    auto &s = *state_;
    return QSocFstInput{
        1,
        sizeof(QSocFstInput),
        &s,
        [](void *user, uint64_t offset, void *destination, uint32_t count, uint32_t *received) {
            return static_cast<State *>(user)->read(offset, destination, count, received);
        },
        [](void *user, uint64_t *size) {
            const auto &value = *static_cast<State *>(user);
            *size             = value.expandedSize;
            return value.error;
        },
        [](void *user) {
            const auto &value = *static_cast<State *>(user);
            return int(
                value.error == INPUT_CANCELLED
                || (value.source.cancelled && value.source.cancelled(value.source.user)));
        },
        [](void *user, uint64_t) {
            const auto &value = *static_cast<State *>(user);
            if (value.source.progress)
                value.source.progress(value.source.user, value.frontier ? value.frontier->offset : 0);
        },
        [](void *user, uint64_t size) {
            const auto &value = *static_cast<State *>(user);
            return value.source.temporary_file
                       ? value.source.temporary_file(value.source.user, size)
                       : nullptr;
        },
        s.source.hierarchy_limit};
}

int QSocFstWrapper::finish()
{
    auto &s = *state_;
    while (!s.stopped() && !s.frontier->finished)
        s.pump(*s.frontier, bufferBytes);
    s.stats.validated = !s.error && s.frontier->finished;
    return s.error;
}

int QSocFstWrapper::pin(uint64_t offset)
{
    auto &s = *state_;
    if (s.stopped() || !s.limits.checkpointCount)
        return s.error;
    if (offset >= s.expandedSize)
        return INPUT_RANGE;
    auto *value = s.choose(offset);
    if (!value)
        return s.error;
    while (!s.error && value->offset < offset)
        s.pump(*value, static_cast<uint32_t>(std::min<uint64_t>(bufferBytes, offset - value->offset)));
    if (s.error)
        return s.error;
    s.anchor.reset();
    s.checkpoints[s.limits.checkpointCount - 1].reset();
    s.anchor = s.copy(*value);
    return s.error;
}

int QSocFstWrapper::status() const
{
    return state_->error;
}

QSocFstWrapper::Statistics QSocFstWrapper::statistics() const
{
    auto result            = state_->stats;
    result.memoryBytes     = state_->budget.used;
    result.peakMemoryBytes = state_->budget.peak;
    result.checkpoints     = state_->anchor ? 1U : 0U;
    for (const auto &entry : state_->checkpoints)
        result.checkpoints += entry ? 1 : 0;
    return result;
}
