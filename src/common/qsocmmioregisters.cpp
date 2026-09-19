// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocmmioregisters.h"

#include <QMap>

namespace {

QString literal(quint32 width, quint64 value)
{
    return QString("%1'h%2").arg(width).arg(value, 0, 16);
}

QString fieldName(qsizetype index)
{
    return QString("mmio_field_%1_q").arg(index);
}

QString slice(const QString &name, quint32 width, quint32 low, quint32 count)
{
    if (width == 1) {
        return name;
    }
    return count == 1 ? QString("%1[%2]").arg(name).arg(low)
                      : QString("%1[%2:%3]").arg(name).arg(low + count - 1).arg(low);
}

struct ByteSlice
{
    qsizetype      index;
    QSocMmioAccess access;
    quint32        width;
    quint32        fieldLow;
    quint32        wordLow;
    quint32        count;
};

QMap<quint64, QList<ByteSlice>> wordSlices(const QSocMmioPlan &plan)
{
    QMap<quint64, QList<ByteSlice>> words;
    qsizetype                       index = 0;
    const quint32                   bytes = plan.dataWidth / 8;
    for (const auto &reg : plan.registers) {
        words[reg.byteOffset];
        for (const auto &field : reg.fields) {
            for (quint32 bit = 0; bit < field.width;) {
                const quint32 position = field.lsb + bit;
                const quint64 address  = reg.byteOffset + position / 8;
                const quint32 count    = qMin(field.width - bit, 8 - position % 8);
                words[address - address % bytes].append(
                    {index,
                     field.access,
                     field.width,
                     bit,
                     quint32(address % bytes) * 8 + position % 8,
                     count});
                bit += count;
            }
            ++index;
        }
    }
    return words;
}

void appendStorage(QStringList *lines, const QSocMmioPlan &plan)
{
    qsizetype index = 0;
    for (const auto &reg : plan.registers) {
        for (const auto &field : reg.fields) {
            const QString name  = fieldName(index++);
            const QString range = field.width == 1 ? QString()
                                                   : QString("[%1:0] ").arg(field.width - 1);
            if (qsocMmioHasStorage(field.access)) {
                lines->append(QString("reg %1%2;").arg(range, name));
                if (!field.outputPort.isEmpty()) {
                    lines->append(QString("assign %1 = %2;").arg(field.outputPort, name));
                }
            } else {
                const QString value = field.constantValue.has_value()
                                          ? literal(field.width, *field.constantValue)
                                          : field.inputPort;
                lines->append(QString("wire %1%2 = %3;").arg(range, name, value));
            }
        }
    }
    lines->append(QString());
}

void appendDecode(
    QStringList *lines, const QSocMmioPlan &plan, const QMap<quint64, QList<ByteSlice>> &words)
{
    lines->append("function address_is_mapped;");
    lines->append(QString("    input [%1:0] address;").arg(plan.addressWidth - 1));
    lines->append("    begin");
    QString fallback = "1'b0";
    if (plan.zeroFillBytes != 0) {
        fallback = plan.addressWidth < 64 && plan.zeroFillBytes == (quint64(1) << plan.addressWidth)
                       ? "1'b1"
                       : "address < " + literal(plan.addressWidth, plan.zeroFillBytes);
    }
    lines->append("        address_is_mapped = " + fallback + ";");
    lines->append("        case (address)");
    for (auto it = words.cbegin(); it != words.cend(); ++it) {
        lines->append(
            "            " + literal(plan.addressWidth, it.key()) + ": address_is_mapped = 1'b1;");
    }
    lines->append("            default: begin end");
    lines->append("        endcase");
    lines->append("    end");
    lines->append("endfunction");
    lines->append(QString());
    lines->append(QString("function [%1:0] read_register;").arg(plan.dataWidth - 1));
    lines->append(QString("    input [%1:0] address;").arg(plan.addressWidth - 1));
    lines->append("    begin");
    lines->append("        read_register = " + literal(plan.dataWidth, 0) + ";");
    lines->append("        case (address)");
    for (auto it = words.cbegin(); it != words.cend(); ++it) {
        lines->append("            " + literal(plan.addressWidth, it.key()) + ": begin");
        for (const auto &part : it.value()) {
            lines->append(
                QString("                %1 = %2;")
                    .arg(
                        slice("read_register", plan.dataWidth, part.wordLow, part.count),
                        slice(fieldName(part.index), part.width, part.fieldLow, part.count)));
        }
        lines->append("            end");
    }
    lines->append("            default: begin end");
    lines->append("        endcase");
    lines->append("    end");
    lines->append("endfunction");
    lines->append(QString());
}

void appendClear(QStringList *lines, const QSocMmioPlan &plan)
{
    if (plan.clearPort.isEmpty())
        return;
    lines->append("        if (" + plan.clearPort + ") begin");
    qsizetype index = 0;
    for (const auto &reg : plan.registers) {
        for (const auto &field : reg.fields) {
            if (field.access == QSocMmioAccess::ReadWrite)
                lines->append(QString("            %1 <= %2;")
                                  .arg(fieldName(index), literal(field.width, *field.resetValue)));
            ++index;
        }
    }
    lines->append("        end");
}

void appendWrite(
    QStringList                           *lines,
    const QSocMmioPlan                    &plan,
    const QMap<quint64, QList<ByteSlice>> &words,
    const QString                         &address,
    const QString                         &data,
    const QString                         &strobeName)
{
    lines->append("always @(posedge clk_i or negedge rst_ni) begin");
    lines->append("    if (!rst_ni) begin");
    qsizetype index = 0;
    for (const auto &reg : plan.registers) {
        for (const auto &field : reg.fields) {
            if (qsocMmioHasStorage(field.access)) {
                lines->append(QString("        %1 <= %2;")
                                  .arg(fieldName(index), literal(field.width, *field.resetValue)));
            }
            ++index;
        }
    }
    lines->append("    end else begin");
    lines->append("        if (write_fire) begin");
    lines->append("            case (" + address + ")");
    for (auto it = words.cbegin(); it != words.cend(); ++it) {
        lines->append("                " + literal(plan.addressWidth, it.key()) + ": begin");
        for (const auto &part : it.value()) {
            if (!qsocMmioHasStorage(part.access)) {
                continue;
            }
            const QString lhs = slice(fieldName(part.index), part.width, part.fieldLow, part.count);
            QString       rhs = slice(data, plan.dataWidth, part.wordLow, part.count);
            if (part.access == QSocMmioAccess::WriteOneClear) {
                rhs = lhs + " & ~" + rhs;
            }
            const QString strobe = plan.dataWidth == 8
                                       ? strobeName
                                       : QString("%1[%2]").arg(strobeName).arg(part.wordLow / 8);
            lines->append(QString("                    if (%1)").arg(strobe));
            lines->append(QString("                        %1 <= %2;").arg(lhs, rhs));
        }
        lines->append("                end");
    }
    lines->append("                default: begin end");
    lines->append("            endcase");
    lines->append("        end");
    appendClear(lines, plan);
    index = 0;
    for (const auto &reg : plan.registers) {
        for (const auto &field : reg.fields) {
            if (field.access == QSocMmioAccess::WriteOneClear) {
                lines->append(QString("        if (%1)").arg(field.inputPort));
                lines->append(QString("            %1 <= 1'b1;").arg(fieldName(index)));
            }
            ++index;
        }
    }
    lines->append("    end");
    lines->append("end");
}

} // namespace

QSocMmioRegisterRtl QSocMmioRegisters::generate(
    const QSocMmioPlan &plan, const QString &address, const QString &data, const QString &strobe)
{
    const auto  words = wordSlices(plan);
    QStringList storage, decode, write;
    appendStorage(&storage, plan);
    appendDecode(&decode, plan, words);
    appendWrite(&write, plan, words, address, data, strobe);
    return {storage.join('\n'), decode.join('\n'), write.join('\n')};
}
