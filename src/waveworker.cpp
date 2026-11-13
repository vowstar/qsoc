// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "agent/waveform/qsocfstclient.h"
#include "agent/waveform/qsocfstworker.h"
#include "common/qsocprocesslimits.h"
#include "fstapi.h"

#include <QCoreApplication>
#include <QFile>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <cstdio>
#include <cstring>

namespace {
int probe(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QTemporaryDir    directory;
    if (!directory.isValid())
        return 1;
    const auto path   = QFile::encodeName(directory.filePath("probe.fst"));
    auto      *writer = fstWriterCreate(path.constData(), 1);
    if (!writer)
        return 1;
    fstWriterSetPackType(writer, FST_WR_PT_ZLIB);
    fstWriterSetScope(writer, FST_ST_VCD_MODULE, "top", nullptr);
    const auto clock = fstWriterCreateVar(writer, FST_VT_VCD_WIRE, FST_VD_IMPLICIT, 1, "clk", 0);
    fstWriterSetUpscope(writer);
    fstWriterEmitTimeChange(writer, 0);
    fstWriterEmitValueChange(writer, clock, "0");
    fstWriterEmitTimeChange(writer, 1);
    fstWriterEmitValueChange(writer, clock, "1");
    if (fstWriterClose2(writer) != 0)
        return 1;
    auto *reader = fstReaderOpen(path.constData());
    if (!reader)
        return 1;
    const auto close = qScopeGuard([&] { fstReaderClose(reader); });
    bool       found = false;
    while (const auto hierarchy = fstReaderIterateHier(reader)) {
        if (hierarchy->htyp == FST_HT_VAR)
            found = hierarchy->u.var.handle == clock
                    && std::strcmp(hierarchy->u.var.name, "clk") == 0;
    }
    char zero[2]{}, one[2]{};
    if (!found || fstReaderGetVarCount(reader) != 1
        || !fstReaderGetValueFromHandleAtTime(reader, 0, clock, zero) || std::strcmp(zero, "0") != 0
        || !fstReaderGetValueFromHandleAtTime(reader, 1, clock, one) || std::strcmp(one, "1") != 0
        || fstReaderGetFseekFailed(reader))
        return 1;
    std::puts("FST probe passed");
    return 0;
}
} // namespace

int main(int argc, char **argv)
{
    if (argc == 2 && std::strcmp(argv[1], "--probe") == 0)
        return probe(argc, argv);
    const auto limits = QSocProcessLimits::apply(QSocFstClient::memoryLimit);
    if (limits != QSocProcessLimits::ApplyResult::Success)
        return static_cast<int>(limits);
    return QSocFstWorker::run(argc, argv);
}
