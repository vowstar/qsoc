// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocfstworker.h"
#include "common/qsocipc.h"
#include "common/qsoclocalpeer.h"
#include "common/qsocprocessowner.h"
#include "fstinput.h"
#include "qsocfstclient.h"
#include "qsocfstwrapper.h"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QHash>
#include <QJsonArray>
#include <QLocalSocket>
#include <QSet>
#include <QTemporaryFile>

#include <clocale>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <vector>

#ifdef Q_OS_WIN
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {
constexpr int wireLimit = 256 * 1024;
constexpr int idleTtlMs = 5 * 60 * 1000;

bool finiteReal(const QByteArray &value)
{
    bool       ok     = false;
    const auto number = value.toDouble(&ok);
    return ok && std::isfinite(number);
}

class Reader
{
public:
    explicit Reader(QLocalSocket &socket)
        : socket_(socket)
    {}

    QJsonObject receive()
    {
        QElapsedTimer idle;
        idle.start();
        while (true) {
            buffer_ += socket_.read(wireLimit + QSocIpc::headerBytes - buffer_.size());
            QJsonObject message;
            const auto  decoded = QSocIpc::decode(buffer_, message, wireLimit);
            if (decoded == QSocIpc::DecodeResult::Complete) {
                if (message.value("type") == "keepalive") {
                    idle.restart();
                    continue;
                }
                return message;
            }
            if (decoded == QSocIpc::DecodeResult::Invalid || idle.elapsed() >= idleTtlMs
                || socket_.state() != QLocalSocket::ConnectedState)
                return {};
            socket_.waitForReadyRead(20);
        }
    }

    bool send(const QJsonObject &message)
    {
        const auto output = QSocIpc::frame(message, wireLimit);
        if (output.isEmpty() || socket_.write(output) != output.size())
            return false;
        QElapsedTimer elapsed;
        elapsed.start();
        while (socket_.bytesToWrite() > 0) {
            if (elapsed.elapsed() >= 2000 || socket_.state() != QLocalSocket::ConnectedState)
                return false;
            socket_.waitForBytesWritten(20);
        }
        return true;
    }

    QJsonObject exchange(const QJsonObject &message)
    {
        return send(message) ? receive() : QJsonObject{};
    }

    void acknowledge(const QJsonObject &message)
    {
        if (exchange(message).value("type") != "ack")
            std::_Exit(24);
    }

    static int size(void *user, uint64_t *size)
    {
        *size = static_cast<Reader *>(user)->size_;
        return INPUT_OK;
    }

    static int readAt(
        void *user, uint64_t offset, void *destination, uint32_t requested, uint32_t *received)
    {
        return readSource(user, "main", offset, destination, requested, received);
    }

    static int readHierarchyAt(
        void *user, uint64_t offset, void *destination, uint32_t requested, uint32_t *received)
    {
        return readSource(user, "hierarchy", offset, destination, requested, received);
    }

    static int hierarchySize(void *user, uint64_t *size)
    {
        *size = static_cast<Reader *>(user)->hierarchySize_;
        return INPUT_OK;
    }

    static int readSource(
        void       *user,
        const char *role,
        uint64_t    offset,
        void       *destination,
        uint32_t    requested,
        uint32_t   *received)
    {
        auto &reader = *static_cast<Reader *>(user);
        *received    = 0;
        if (!requested || requested > 128 * 1024)
            return INPUT_RANGE;
        const auto reply = reader.exchange(
            {{"type", "read"},
             {"source", QString::fromLatin1(role)},
             {"offset", QString::number(offset)},
             {"count", static_cast<int>(requested)}});
        if (reply.value("type") != "data" || !reply.value("bytes").isString())
            return INPUT_IO_ERROR;
        const auto encoded = reply.value("bytes").toString().toLatin1();
        const auto decoded
            = QByteArray::fromBase64Encoding(encoded, QByteArray::AbortOnBase64DecodingErrors);
        if (!decoded || decoded.decoded.isEmpty() || decoded.decoded.size() > requested)
            return INPUT_IO_ERROR;
        std::memcpy(destination, decoded.decoded.constData(), decoded.decoded.size());
        *received = static_cast<uint32_t>(decoded.decoded.size());
        return INPUT_OK;
    }

    static int cancelled(void *) { return 0; }

    static void progress(void *user, uint64_t offset)
    {
        static_cast<Reader *>(user)->acknowledge(
            {{"type", "progress"}, {"offset", QString::number(offset)}});
    }

    static FILE *temporary(void *user, uint64_t expected)
    {
        auto &reader = *static_cast<Reader *>(user);
        if (!reader.temporary_.empty() || expected > QSocFstClient::temporaryLimit)
            return nullptr;
        auto file = std::make_unique<QTemporaryFile>(QDir::current().filePath("hierarchy-XXXXXX"));
        if (!file->open())
            return nullptr;
#ifdef Q_OS_WIN
        const int duplicate = _dup(file->handle());
        FILE     *stream    = duplicate >= 0 ? _fdopen(duplicate, "w+b") : nullptr;
        if (!stream && duplicate >= 0)
            _close(duplicate);
#else
        const int duplicate = ::dup(file->handle());
        FILE     *stream    = duplicate >= 0 ? ::fdopen(duplicate, "w+b") : nullptr;
        if (!stream && duplicate >= 0)
            ::close(duplicate);
#endif
        if (stream)
            reader.temporary_.push_back(std::move(file));
        return stream;
    }

    static void event(void *user, uint64_t time, fstHandle handle, const unsigned char *value)
    {
        auto     &reader = *static_cast<Reader *>(user);
        const int width  = reader.widths_.value(handle);
        if (width < 1 || width > 65536 || !value)
            std::_Exit(14);
        const bool real  = reader.reals_.contains(handle);
        const auto bytes = QByteArray(
            reinterpret_cast<const char *>(value),
            real ? qsizetype(std::strlen(reinterpret_cast<const char *>(value))) : width);
        if (bytes.size() > 65536)
            std::_Exit(14);
        if (real && !finiteReal(bytes)) {
            reader.fail("nonfinite_real", INPUT_UNSUPPORTED);
            std::_Exit(13);
        }
        reader.acknowledge(
            {{"type", "event"},
             {"time", QString::number(time)},
             {"handle", static_cast<double>(handle)},
             {"value", QString::fromLatin1(bytes)},
             {"real", real}});
    }

    int fail(const QString &phase, int status)
    {
        send(
            {{"type", "error"},
             {"phase",
              status == INPUT_MISSING_HIERARCHY ? QStringLiteral("hierarchy_missing") : phase},
             {"status", status}});
        return 13;
    }

    int parse()
    {
        const auto open = receive();
        bool       ok   = false;
        size_           = open.value("size").toString().toULongLong(&ok);
        if (!ok || open.value("type") != "open" || !size_)
            return fail("open", INPUT_MALFORMED);
        if (open.contains("hierarchy_size")) {
            hierarchySize_ = open.value("hierarchy_size").toString().toULongLong(&ok);
            if (!ok)
                return fail("open", INPUT_MALFORMED);
            hasHierarchy_ = true;
        }
        QSocFstInput input{
            1,
            sizeof(QSocFstInput),
            this,
            readAt,
            size,
            cancelled,
            progress,
            temporary,
            QSocFstClient::temporaryLimit};
        unsigned char first       = 0;
        uint32_t      received    = 0;
        const int     firstStatus = readAt(this, 0, &first, 1, &received);
        if (firstStatus || received != 1)
            return fail("open", firstStatus ? firstStatus : INPUT_IO_ERROR);
        std::unique_ptr<QSocFstWrapper> wrapper;
        if (first == FST_BL_ZWRAPPER) {
            wrapper = std::make_unique<QSocFstWrapper>(input);
            if (wrapper->status())
                return fail("wrapper", wrapper->status());
            input = wrapper->input();
        }
        QSocFstInput hierarchyInput{
            1,
            sizeof(QSocFstInput),
            this,
            readHierarchyAt,
            hierarchySize,
            cancelled,
            progress,
            temporary,
            QSocFstClient::temporaryLimit};
        int                                                          status = INPUT_OK;
        std::unique_ptr<fstReaderContext, decltype(&fstReaderClose)> reader(
            fstReaderOpenWithInputAndHierarchy(
                &input, hasHierarchy_ ? &hierarchyInput : nullptr, &status),
            fstReaderClose);
        if (!reader)
            return fail("open", wrapper && wrapper->status() ? wrapper->status() : status);
        const auto inputStatus = [&] {
            return wrapper && wrapper->status() ? wrapper->status()
                                                : fstReaderInputStatus(reader.get());
        };
        const auto declarations = fstReaderGetVarCount(reader.get());
        if (declarations > 50000)
            return fail("hierarchy", INPUT_RANGE);
        acknowledge(
            {{"type", "header"},
             {"timescale", fstReaderGetTimescale(reader.get())},
             {"declarations", QString::number(declarations)},
             {"start", QString::number(fstReaderGetStartTime(reader.get()))},
             {"end", QString::number(fstReaderGetEndTime(reader.get()))}});
        QStringList scopes;
        quint64     variables = 0;
        qsizetype   nameBytes = 0;
        while (const auto hierarchy = fstReaderIterateHier(reader.get())) {
            if (hierarchy->htyp == FST_HT_SCOPE) {
                if (scopes.size() >= 64 || hierarchy->u.scope.name_length > 256)
                    return fail("hierarchy", INPUT_RANGE);
                scopes.append(QString::fromUtf8(hierarchy->u.scope.name));
            } else if (hierarchy->htyp == FST_HT_UPSCOPE) {
                if (scopes.isEmpty())
                    return fail("hierarchy", INPUT_MALFORMED);
                scopes.removeLast();
            } else if (hierarchy->htyp == FST_HT_VAR) {
                const auto &var  = hierarchy->u.var;
                const auto  name = (scopes.isEmpty() ? QString() : scopes.join('.') + '.')
                                   + QString::fromUtf8(var.name);
                nameBytes += name.size() * 2;
                if (++variables > declarations || var.length < 1 || var.length > 65536
                    || var.handle < 1 || var.handle > fstReaderGetMaxHandle(reader.get())
                    || name.size() > 4096 || nameBytes > 8 * 1024 * 1024)
                    return fail("hierarchy", INPUT_RANGE);
                const bool real = var.typ == FST_VT_VCD_REAL || var.typ == FST_VT_VCD_REALTIME
                                  || var.typ == FST_VT_VCD_REAL_PARAMETER
                                  || var.typ == FST_VT_SV_SHORTREAL;
                if (var.typ == FST_VT_GEN_STRING)
                    return fail("variable_type", INPUT_UNSUPPORTED);
                uint32_t geometryLength = 0;
                int      geometryReal   = 0;
                if (!fstReaderInputSignalInfo(reader.get(), var.handle, &geometryLength, &geometryReal)
                    || (real ? geometryLength != 8 : geometryLength != var.length)
                    || real != bool(geometryReal)
                    || (widths_.contains(var.handle)
                        && (widths_.value(var.handle) != int(var.length)
                            || reals_.contains(var.handle) != real)))
                    return fail("geometry", INPUT_MALFORMED);
                widths_.insert(var.handle, static_cast<int>(var.length));
                if (real)
                    reals_.insert(var.handle);
                acknowledge(
                    {{"type", "signal"},
                     {"name", name},
                     {"width", static_cast<int>(var.length)},
                     {"handle", static_cast<double>(var.handle)},
                     {"kind",
                      real                        ? "real"
                      : var.typ == FST_VT_VCD_REG ? "reg"
                                                  : "wire"}});
            }
        }
        status = inputStatus();
        if (status || variables != declarations || !scopes.isEmpty())
            return fail("hierarchy", status ? status : INPUT_MALFORMED);
        if (wrapper && wrapper->finish())
            return fail("wrapper", wrapper->status());
        const auto selection     = exchange({{"type", "hierarchy_end"}});
        auto       nextSelection = selection;
        bool       reused        = false;
        while (!nextSelection.isEmpty()) {
            const auto before = wrapper ? wrapper->statistics() : QSocFstWrapper::Statistics{};
            const auto result = readWindow(reader.get(), wrapper.get(), nextSelection);
            if (result)
                return result;
            QJsonObject cache{{"reader_reused", reused}};
            if (wrapper) {
                const auto after              = wrapper->statistics();
                cache["wrapper_cold_rewinds"] = QString::number(
                    after.coldRewinds - before.coldRewinds);
                cache["wrapper_checkpoint_hits"] = QString::number(
                    after.checkpointHits - before.checkpointHits);
                cache["wrapper_inflated_bytes"] = QString::number(
                    after.inflatedBytes - before.inflatedBytes);
            }
            nextSelection = exchange({{"type", "complete"}, {"cache", cache}});
            reused        = true;
        }
        return 0;
    }

private:
    int readWindow(fstReaderContext *reader, QSocFstWrapper *wrapper, const QJsonObject &selection)
    {
        const auto inputStatus = [&] {
            return wrapper && wrapper->status() ? wrapper->status() : fstReaderInputStatus(reader);
        };
        int           status  = INPUT_OK;
        bool          startOk = false, endOk = false;
        const quint64 start   = selection.value("start").toString().toULongLong(&startOk);
        const quint64 end     = selection.value("end").toString().toULongLong(&endOk);
        const auto    handles = selection.value("handles").toArray();
        if (selection.value("type") != "select" || !startOk || !endOk || start > end
            || handles.isEmpty() || handles.size() > 32)
            return fail("selection", INPUT_MALFORMED);
        if (wrapper) {
            const auto initial = start ? qMin<quint64>(start - 1, fstReaderGetEndTime(reader)) : 0;
            const auto offset  = fstReaderInputSectionOffset(reader, initial);
            const auto pinned  = wrapper->pin(offset);
            if (pinned)
                return fail("wrapper", pinned);
        }
        QSet<fstHandle> selected;
        fstReaderClrFacProcessMaskAll(reader);
        for (const auto handleValue : handles) {
            const auto number = handleValue.toDouble();
            if (number < 1 || number > std::numeric_limits<fstHandle>::max()
                || number != static_cast<fstHandle>(number)
                || !widths_.contains(static_cast<fstHandle>(number)))
                return fail("selection", INPUT_MALFORMED);
            const auto handle = static_cast<fstHandle>(number);
            if (selected.contains(handle))
                continue;
            selected.insert(handle);
            fstReaderSetFacProcessMask(reader, handle);
            if (start > 0 && start - 1 >= fstReaderGetStartTime(reader)) {
                QByteArray    value(qMax(widths_.value(handle), 128) + 1, '\0');
                const quint64 initialTime = qMin<quint64>(start - 1, fstReaderGetEndTime(reader));
                const auto    found
                    = fstReaderGetValueFromHandleAtTime(reader, initialTime, handle, value.data());
                status = inputStatus();
                if (status)
                    return fail("initial", status);
                QByteArray initialValue = found ? QByteArray(found) : QByteArray();
                if (reals_.contains(handle)
                    && (initialValue.startsWith('r') || initialValue.startsWith('R')))
                    initialValue.remove(0, 1);
                if (found && reals_.contains(handle) && !finiteReal(initialValue))
                    return fail("nonfinite_real", INPUT_UNSUPPORTED);
                if (found)
                    acknowledge(
                        {{"type", "initial"},
                         {"handle", static_cast<double>(handle)},
                         {"value", QString::fromLatin1(initialValue)}});
            }
        }
        fstReaderSetLimitTimeRange(reader, start, end);
        const int result = fstReaderGetValueChangeSectionCount(reader)
                                   && start <= fstReaderGetEndTime(reader)
                               ? fstReaderIterBlocks(reader, event, this, nullptr)
                               : 1;
        status           = inputStatus();
        if (!result || status || fstReaderGetFseekFailed(reader))
            return fail("events", status ? status : INPUT_MALFORMED);
        if (wrapper && wrapper->finish())
            return fail("wrapper", wrapper->status());
        return 0;
    }

private:
    QLocalSocket                                &socket_;
    QByteArray                                   buffer_;
    quint64                                      size_          = 0;
    quint64                                      hierarchySize_ = 0;
    bool                                         hasHierarchy_  = false;
    QHash<fstHandle, int>                        widths_;
    QSet<fstHandle>                              reals_;
    std::vector<std::unique_ptr<QTemporaryFile>> temporary_;
};
} // namespace

int QSocFstWorker::run(int argc, char **argv)
{
    try {
        QCoreApplication application(argc, argv);
        if (!std::setlocale(LC_NUMERIC, "C"))
            return 14;
        const auto arguments = application.arguments();
        if (arguments.size() != 5 || arguments[1] != "--socket" || arguments[3] != "--owner-pid")
            return 14;
        bool             ok     = false;
        const auto       parent = arguments[4].toLongLong(&ok);
        QSocProcessOwner owner;
        if (!ok || !owner.watch(parent))
            return 18;
        QLocalSocket socket;
        socket.setReadBufferSize(wireLimit + QSocIpc::headerBytes);
        socket.connectToServer(arguments[2]);
        if (!socket.waitForConnected(2000) || !QSocLocalPeer::sameUser(socket)
            || QSocLocalPeer::processId(socket) != parent)
            return 17;
        Reader reader(socket);
        if (!reader.send({{"type", "hello"}, {"protocol", 2}}))
            return 16;
        return reader.parse();
    } catch (const std::bad_alloc &) {
        std::_Exit(12);
    }
}
