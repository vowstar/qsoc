// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocmmioformal.h"

#include "common/qsocmmiogenerator.h"

#include <algorithm>
#include <QSet>
#include <QStringList>

namespace {

class IdentifierAllocator
{
public:
    void reserve(const QString &name) { usedNames.insert(name); }

    QString take(const QString &base)
    {
        QString name = base;
        while (usedNames.contains(name)) {
            name += QLatin1Char('_');
        }
        usedNames.insert(name);
        return name;
    }

private:
    QSet<QString> usedNames;
};

struct FormalNames
{
    QString     resetShift;
    QString     runtimeReset;
    QString     dutInstance;
    QString     responseOkay;
    QString     responseSlverr;
    QString     mappedFunction;
    QString     mappedAddress;
    QString     readFunction;
    QString     readAddress;
    QString     awPending;
    QString     awAddress;
    QString     wPending;
    QString     wData;
    QString     wStrobe;
    QString     bValid;
    QString     bResponse;
    QString     rValid;
    QString     rData;
    QString     rResponse;
    QString     awReady;
    QString     wReady;
    QString     arReady;
    QString     awTake;
    QString     wTake;
    QString     arTake;
    QString     writeAddress;
    QString     writeData;
    QString     writeStrobe;
    QString     writeFire;
    QString     pastValid;
    QStringList fields;
};

QString packedRange(quint32 width)
{
    return width == 1 ? QString() : QString(" [%1:0]").arg(width - 1);
}

QString bitRange(quint32 high, quint32 low)
{
    return high == low ? QString("[%1]").arg(low) : QString("[%1:%2]").arg(high).arg(low);
}

/**
 * @brief The slice of a field's storage a lane touches; a one-bit field is
 * a scalar reg and a bit select on it is not portable.
 */
QString storageSlice(const QSocMmioFieldPlan &field, quint32 high, quint32 low)
{
    return field.width == 1 ? QString() : bitRange(high - field.lsb, low - field.lsb);
}

QString verilogLiteral(quint32 width, quint64 value)
{
    return QString("%1'h%2").arg(width).arg(QString::number(value, 16));
}

QString zeroLiteral(quint32 width)
{
    return QString("%1'b0").arg(width);
}

QString addressLiteral(const QSocMmioPlan &plan, quint64 byteOffset)
{
    const int digits = static_cast<int>((plan.addressWidth + 3) / 4);
    return QString("%1'h%2").arg(plan.addressWidth).arg(byteOffset, digits, 16, QLatin1Char('0'));
}

QStringList axiSignals()
{
    return {
        "clk_i",         "rst_ni",        "s_axi_awaddr", "s_axi_awprot", "s_axi_awvalid",
        "s_axi_awready", "s_axi_wdata",   "s_axi_wstrb",  "s_axi_wvalid", "s_axi_wready",
        "s_axi_bresp",   "s_axi_bvalid",  "s_axi_bready", "s_axi_araddr", "s_axi_arprot",
        "s_axi_arvalid", "s_axi_arready", "s_axi_rdata",  "s_axi_rresp",  "s_axi_rvalid",
        "s_axi_rready",
    };
}

QStringList dutPorts(const QSocMmioPlan &plan)
{
    QStringList ports = axiSignals();
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (!field.inputPort.isEmpty()) {
                ports.append(field.inputPort);
            }
            if (!field.outputPort.isEmpty()) {
                ports.append(field.outputPort);
            }
        }
    }
    return ports;
}

FormalNames allocateNames(const QSocMmioPlan &plan)
{
    IdentifierAllocator identifiers;
    identifiers.reserve(plan.moduleName);
    identifiers.reserve(plan.moduleName + QStringLiteral("_formal"));
    for (const QString &port : dutPorts(plan)) {
        identifiers.reserve(port);
    }

    FormalNames names;
    names.resetShift     = identifiers.take("formal_reset_q");
    names.runtimeReset   = identifiers.take("formal_runtime_reset_ni");
    names.dutInstance    = identifiers.take("formal_dut");
    names.responseOkay   = identifiers.take("FORMAL_AXI_RESP_OKAY");
    names.responseSlverr = identifiers.take("FORMAL_AXI_RESP_SLVERR");
    names.mappedFunction = identifiers.take("formal_address_is_mapped");
    names.mappedAddress  = identifiers.take("formal_mapped_address");
    names.readFunction   = identifiers.take("formal_read_register");
    names.readAddress    = identifiers.take("formal_read_address");
    names.awPending      = identifiers.take("formal_aw_pending_q");
    names.awAddress      = identifiers.take("formal_awaddr_q");
    names.wPending       = identifiers.take("formal_w_pending_q");
    names.wData          = identifiers.take("formal_wdata_q");
    names.wStrobe        = identifiers.take("formal_wstrb_q");
    names.bValid         = identifiers.take("formal_bvalid_q");
    names.bResponse      = identifiers.take("formal_bresp_q");
    names.rValid         = identifiers.take("formal_rvalid_q");
    names.rData          = identifiers.take("formal_rdata_q");
    names.rResponse      = identifiers.take("formal_rresp_q");
    names.awReady        = identifiers.take("formal_awready");
    names.wReady         = identifiers.take("formal_wready");
    names.arReady        = identifiers.take("formal_arready");
    names.awTake         = identifiers.take("formal_aw_take");
    names.wTake          = identifiers.take("formal_w_take");
    names.arTake         = identifiers.take("formal_ar_take");
    names.writeAddress   = identifiers.take("formal_write_address");
    names.writeData      = identifiers.take("formal_write_data");
    names.writeStrobe    = identifiers.take("formal_write_strobe");
    names.writeFire      = identifiers.take("formal_write_fire");
    names.pastValid      = identifiers.take("formal_past_valid");

    int fieldIndex = 0;
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (field.access == QSocMmioAccess::ReadWrite) {
                names.fields.append(
                    identifiers.take(QString("formal_field_%1_q").arg(fieldIndex++)));
            }
        }
    }
    return names;
}

/**
 * @brief The reset the harness runs under.
 *
 * The default builds a two-cycle reset from an `initial` value. A tool that
 * ignores `initial` blocks defines `FORMAL_EXTERNAL_RESET` and drives
 * `formal_reset_ni` as a port instead. Either way a free input may pull
 * reset again at any time, so re-entering reset stays under proof. The
 * request is registered first: an asynchronous reset that moves on the
 * clock edge itself races the sampling of the properties, and the tools do
 * not agree on who wins.
 */
void appendClockAndReset(QStringList *lines, const FormalNames &names)
{
    lines->append(QString("reg %1;").arg(names.pastValid));
    lines->append(QString("(* anyseq *) reg %1;").arg(names.runtimeReset));
    lines->append(QString("reg %1_q;").arg(names.runtimeReset));
    lines->append("always @(posedge clk_i)");
    lines->append(QString("    %1_q <= %1;").arg(names.runtimeReset));
    lines->append("`ifdef FORMAL_EXTERNAL_RESET");
    lines->append(QString("wire rst_ni = formal_reset_ni && (!%1 || %2_q);")
                      .arg(names.pastValid, names.runtimeReset));
    lines->append("`else");
    lines->append(QString("reg [2:0] %1;").arg(names.resetShift));
    lines->append(QString("wire rst_ni = %1[1] && (!%1[2] || %2_q);")
                      .arg(names.resetShift, names.runtimeReset));
    lines->append(QString());
    lines->append(QString("initial %1 = 3'b000;").arg(names.resetShift));
    lines->append(QString());
    lines->append("always @(posedge clk_i)");
    lines->append(QString("    %1 <= {%1[1:0], 1'b1};").arg(names.resetShift));
    lines->append("`endif");
    lines->append(QString());
}

void appendAxiSignals(QStringList *lines, const QSocMmioPlan &plan)
{
    lines->append(QString("(* anyseq *) reg [%1:0] s_axi_awaddr;").arg(plan.addressWidth - 1));
    lines->append("(* anyseq *) reg [2:0]  s_axi_awprot;");
    lines->append("(* anyseq *) reg        s_axi_awvalid;");
    lines->append("wire                    s_axi_awready;");
    lines->append(QString("(* anyseq *) reg [%1:0] s_axi_wdata;").arg(plan.dataWidth - 1));
    lines->append(QString("(* anyseq *) reg [%1:0]  s_axi_wstrb;").arg(plan.dataWidth / 8 - 1));
    lines->append("(* anyseq *) reg        s_axi_wvalid;");
    lines->append("wire                    s_axi_wready;");
    lines->append("wire [1:0]              s_axi_bresp;");
    lines->append("wire                    s_axi_bvalid;");
    lines->append("(* anyseq *) reg        s_axi_bready;");
    lines->append(QString("(* anyseq *) reg [%1:0] s_axi_araddr;").arg(plan.addressWidth - 1));
    lines->append("(* anyseq *) reg [2:0]  s_axi_arprot;");
    lines->append("(* anyseq *) reg        s_axi_arvalid;");
    lines->append("wire                    s_axi_arready;");
    lines->append(QString("wire [%1:0]             s_axi_rdata;").arg(plan.dataWidth - 1));
    lines->append("wire [1:0]              s_axi_rresp;");
    lines->append("wire                    s_axi_rvalid;");
    lines->append("(* anyseq *) reg        s_axi_rready;");
}

void appendSidebandSignals(QStringList *lines, const QSocMmioPlan &plan)
{
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (!field.inputPort.isEmpty()) {
                lines->append(QString("(* anyseq *) reg%1 %2;")
                                  .arg(packedRange(field.width), field.inputPort));
            }
            if (!field.outputPort.isEmpty()) {
                lines->append(QString("wire%1 %2;").arg(packedRange(field.width), field.outputPort));
            }
        }
    }
    lines->append(QString());
}

void appendDut(QStringList *lines, const QSocMmioPlan &plan, const FormalNames &names)
{
    lines->append(QString("%1 %2 (").arg(plan.moduleName, names.dutInstance));
    const QStringList ports = dutPorts(plan);
    for (qsizetype index = 0; index < ports.size(); ++index) {
        const QString suffix = index + 1 == ports.size() ? QString() : QString(",");
        lines->append(QString("    .%1(%1)%2").arg(ports.at(index), suffix));
    }
    lines->append(");");
    lines->append(QString());
}

void appendModelStorage(QStringList *lines, const QSocMmioPlan &plan, const FormalNames &names)
{
    lines->append(QString("localparam [1:0] %1 = 2'b00;").arg(names.responseOkay));
    lines->append(QString("localparam [1:0] %1 = 2'b10;").arg(names.responseSlverr));
    lines->append(QString());
    int storageIndex = 0;
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (field.access == QSocMmioAccess::ReadWrite) {
                lines->append(QString("reg%1 %2;")
                                  .arg(packedRange(field.width), names.fields.at(storageIndex++)));
            }
        }
    }
    lines->append(QString("reg %1;").arg(names.awPending));
    lines->append(QString("reg [%1:0] %2;").arg(plan.addressWidth - 1).arg(names.awAddress));
    lines->append(QString("reg %1;").arg(names.wPending));
    lines->append(QString("reg [%1:0] %2;").arg(plan.dataWidth - 1).arg(names.wData));
    lines->append(QString("reg [%1:0] %2;").arg(plan.dataWidth / 8 - 1).arg(names.wStrobe));
    lines->append(QString("reg %1;").arg(names.bValid));
    lines->append(QString("reg [1:0] %1;").arg(names.bResponse));
    lines->append(QString("reg %1;").arg(names.rValid));
    lines->append(QString("reg [%1:0] %2;").arg(plan.dataWidth - 1).arg(names.rData));
    lines->append(QString("reg [1:0] %1;").arg(names.rResponse));
    lines->append(QString());
}

void appendAddressFunction(QStringList *lines, const QSocMmioPlan &plan, const FormalNames &names)
{
    lines->append(QString("function %1;").arg(names.mappedFunction));
    lines->append(
        QString("    input [%1:0] %2;").arg(plan.addressWidth - 1).arg(names.mappedAddress));
    lines->append("    begin");
    lines->append(QString("        case (%1)").arg(names.mappedAddress));
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        lines->append(QString("            %1: %2 = 1'b1;")
                          .arg(addressLiteral(plan, reg.byteOffset), names.mappedFunction));
    }
    lines->append(QString("            default: %1 = 1'b0;").arg(names.mappedFunction));
    lines->append("        endcase");
    lines->append("    end");
    lines->append("endfunction");
    lines->append(QString());
}

QString readSource(const QSocMmioFieldPlan &field, const FormalNames &names, int storageIndex)
{
    if (field.access == QSocMmioAccess::ReadWrite) {
        return names.fields.at(storageIndex);
    }
    if (!field.inputPort.isEmpty()) {
        return field.inputPort;
    }
    return verilogLiteral(field.width, *field.constantValue);
}

void appendReadFunction(QStringList *lines, const QSocMmioPlan &plan, const FormalNames &names)
{
    lines->append(QString("function [%1:0] %2;").arg(plan.dataWidth - 1).arg(names.readFunction));
    lines->append(QString("    input [%1:0] %2;").arg(plan.addressWidth - 1).arg(names.readAddress));
    lines->append("    begin");
    lines->append(QString("        %1 = %2;").arg(names.readFunction, zeroLiteral(plan.dataWidth)));
    lines->append(QString("        case (%1)").arg(names.readAddress));
    int storageIndex = 0;
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        lines->append(QString("            %1: begin").arg(addressLiteral(plan, reg.byteOffset)));
        for (const QSocMmioFieldPlan &field : reg.fields) {
            const QString source = readSource(field, names, storageIndex);
            if (field.access == QSocMmioAccess::ReadWrite) {
                ++storageIndex;
            }
            lines->append(QString("                %1%2 = %3;")
                              .arg(
                                  names.readFunction,
                                  bitRange(field.lsb + field.width - 1, field.lsb),
                                  source));
        }
        lines->append("            end");
    }
    lines->append("            default: begin end");
    lines->append("        endcase");
    lines->append("    end");
    lines->append("endfunction");
    lines->append(QString());
}

void appendModelWires(QStringList *lines, const QSocMmioPlan &plan, const FormalNames &names)
{
    lines->append(QString("wire %1 = rst_ni && !%2 && !%3;")
                      .arg(names.awReady, names.awPending, names.bValid));
    lines->append(
        QString("wire %1 = rst_ni && !%2 && !%3;").arg(names.wReady, names.wPending, names.bValid));
    lines->append(QString("wire %1 = rst_ni && !%2;").arg(names.arReady, names.rValid));
    lines->append(QString("wire %1 = s_axi_awvalid && %2;").arg(names.awTake, names.awReady));
    lines->append(QString("wire %1 = s_axi_wvalid && %2;").arg(names.wTake, names.wReady));
    lines->append(QString("wire %1 = s_axi_arvalid && %2;").arg(names.arTake, names.arReady));
    lines->append(QString("wire [%1:0] %2 = %3 ? %4 : s_axi_awaddr;")
                      .arg(plan.addressWidth - 1)
                      .arg(names.writeAddress, names.awPending, names.awAddress));
    lines->append(QString("wire [%1:0] %2 = %3 ? %4 : s_axi_wdata;")
                      .arg(plan.dataWidth - 1)
                      .arg(names.writeData, names.wPending, names.wData));
    lines->append(QString("wire [%1:0] %2 = %3 ? %4 : s_axi_wstrb;")
                      .arg(plan.dataWidth / 8 - 1)
                      .arg(names.writeStrobe, names.wPending, names.wStrobe));
    lines->append(QString("wire %1 = !%2 && (%3 || %4) && (%5 || %6);")
                      .arg(
                          names.writeFire,
                          names.bValid,
                          names.awPending,
                          names.awTake,
                          names.wPending,
                          names.wTake));
    lines->append(QString());
}

void appendWriteCase(QStringList *lines, const QSocMmioPlan &plan, const FormalNames &names)
{
    lines->append(QString("            case (%1)").arg(names.writeAddress));
    int storageIndex = 0;
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        const bool hasWriteField
            = std::any_of(reg.fields.cbegin(), reg.fields.cend(), [](const QSocMmioFieldPlan &field) {
                  return field.access == QSocMmioAccess::ReadWrite;
              });
        if (!hasWriteField) {
            continue;
        }
        lines->append(
            QString("                %1: begin").arg(addressLiteral(plan, reg.byteOffset)));
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (field.access != QSocMmioAccess::ReadWrite) {
                continue;
            }
            const QString &storage = names.fields.at(storageIndex++);
            for (quint32 lane = 0; lane < plan.dataWidth / 8; ++lane) {
                const quint32 laneLow   = lane * 8;
                const quint32 laneHigh  = laneLow + 7;
                const quint32 fieldLow  = field.lsb;
                const quint32 fieldHigh = field.lsb + field.width - 1;
                const quint32 low       = std::max(laneLow, fieldLow);
                const quint32 high      = std::min(laneHigh, fieldHigh);
                if (low > high) {
                    continue;
                }
                lines->append(
                    QString("                    if (%1[%2])").arg(names.writeStrobe).arg(lane));
                lines->append(QString("                        %1%2 <= %3%4;")
                                  .arg(
                                      storage,
                                      storageSlice(field, high, low),
                                      names.writeData,
                                      bitRange(high, low)));
            }
        }
        lines->append("                end");
    }
    lines->append("                default: begin end");
    lines->append("            endcase");
}

void appendWriteModel(QStringList *lines, const QSocMmioPlan &plan, const FormalNames &names)
{
    lines->append("always @(posedge clk_i or negedge rst_ni) begin");
    lines->append("    if (!rst_ni) begin");
    lines->append(QString("        %1 <= 1'b0;").arg(names.awPending));
    lines->append(QString("        %1 <= %2;").arg(names.awAddress, zeroLiteral(plan.addressWidth)));
    lines->append(QString("        %1 <= 1'b0;").arg(names.wPending));
    lines->append(QString("        %1 <= %2;").arg(names.wData, zeroLiteral(plan.dataWidth)));
    lines->append(QString("        %1 <= %2;").arg(names.wStrobe, zeroLiteral(plan.dataWidth / 8)));
    lines->append(QString("        %1 <= 1'b0;").arg(names.bValid));
    lines->append(QString("        %1 <= %2;").arg(names.bResponse, names.responseOkay));
    int storageIndex = 0;
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (field.access != QSocMmioAccess::ReadWrite) {
                continue;
            }
            lines->append(QString("        %1 <= %2;")
                              .arg(
                                  names.fields.at(storageIndex++),
                                  verilogLiteral(field.width, *field.resetValue)));
        }
    }
    lines->append("    end else begin");
    lines->append(QString("        if (%1 && s_axi_bready)").arg(names.bValid));
    lines->append(QString("            %1 <= 1'b0;").arg(names.bValid));
    lines->append(QString("        if (%1) begin").arg(names.awTake));
    lines->append(QString("            %1 <= 1'b1;").arg(names.awPending));
    lines->append(QString("            %1 <= s_axi_awaddr;").arg(names.awAddress));
    lines->append("        end");
    lines->append(QString("        if (%1) begin").arg(names.wTake));
    lines->append(QString("            %1 <= 1'b1;").arg(names.wPending));
    lines->append(QString("            %1 <= s_axi_wdata;").arg(names.wData));
    lines->append(QString("            %1 <= s_axi_wstrb;").arg(names.wStrobe));
    lines->append("        end");
    lines->append(QString("        if (%1) begin").arg(names.writeFire));
    lines->append(QString("            %1 <= 1'b0;").arg(names.awPending));
    lines->append(QString("            %1 <= 1'b0;").arg(names.wPending));
    lines->append(QString("            %1 <= 1'b1;").arg(names.bValid));
    lines->append(QString("            %1 <= %2(%3) ? %4 : %5;")
                      .arg(
                          names.bResponse,
                          names.mappedFunction,
                          names.writeAddress,
                          names.responseOkay,
                          names.responseSlverr));
    appendWriteCase(lines, plan, names);
    lines->append("        end");
    lines->append("    end");
    lines->append("end");
    lines->append(QString());
}

void appendReadModel(QStringList *lines, const QSocMmioPlan &plan, const FormalNames &names)
{
    lines->append("always @(posedge clk_i or negedge rst_ni) begin");
    lines->append("    if (!rst_ni) begin");
    lines->append(QString("        %1 <= 1'b0;").arg(names.rValid));
    lines->append(QString("        %1 <= %2;").arg(names.rData, zeroLiteral(plan.dataWidth)));
    lines->append(QString("        %1 <= %2;").arg(names.rResponse, names.responseOkay));
    lines->append("    end else begin");
    lines->append(QString("        if (%1 && s_axi_rready)").arg(names.rValid));
    lines->append(QString("            %1 <= 1'b0;").arg(names.rValid));
    lines->append(QString("        if (%1) begin").arg(names.arTake));
    lines->append(QString("            %1 <= 1'b1;").arg(names.rValid));
    lines->append(
        QString("            %1 <= %2(s_axi_araddr);").arg(names.rData, names.readFunction));
    lines->append(
        QString("            %1 <= %2(s_axi_araddr) ? %3 : %4;")
            .arg(names.rResponse, names.mappedFunction, names.responseOkay, names.responseSlverr));
    lines->append("        end");
    lines->append("    end");
    lines->append("end");
    lines->append(QString());
}

void appendMasterAssumptions(QStringList *lines, const FormalNames &names)
{
    lines->append(QString("        if ($past(s_axi_awvalid && !%1)) begin").arg(names.awReady));
    lines->append("            assume(s_axi_awvalid);");
    lines->append("            assume(s_axi_awaddr == $past(s_axi_awaddr));");
    lines->append("            assume(s_axi_awprot == $past(s_axi_awprot));");
    lines->append("        end");
    lines->append(QString("        if ($past(s_axi_wvalid && !%1)) begin").arg(names.wReady));
    lines->append("            assume(s_axi_wvalid);");
    lines->append("            assume(s_axi_wdata == $past(s_axi_wdata));");
    lines->append("            assume(s_axi_wstrb == $past(s_axi_wstrb));");
    lines->append("        end");
    lines->append(QString("        if ($past(s_axi_arvalid && !%1)) begin").arg(names.arReady));
    lines->append("            assume(s_axi_arvalid);");
    lines->append("            assume(s_axi_araddr == $past(s_axi_araddr));");
    lines->append("            assume(s_axi_arprot == $past(s_axi_arprot));");
    lines->append("        end");
}

void appendResponseStability(QStringList *lines)
{
    lines->append("        if ($past(s_axi_bvalid && !s_axi_bready)) begin");
    lines->append("            assert(s_axi_bvalid);");
    lines->append("            assert(s_axi_bresp == $past(s_axi_bresp));");
    lines->append("        end");
    lines->append("        if ($past(s_axi_rvalid && !s_axi_rready)) begin");
    lines->append("            assert(s_axi_rvalid);");
    lines->append("            assert(s_axi_rdata == $past(s_axi_rdata));");
    lines->append("            assert(s_axi_rresp == $past(s_axi_rresp));");
    lines->append("        end");
}

void appendOutputAssertions(QStringList *lines, const QSocMmioPlan &plan, const FormalNames &names)
{
    lines->append(QString("        assert(s_axi_awready == %1);").arg(names.awReady));
    lines->append(QString("        assert(s_axi_wready == %1);").arg(names.wReady));
    lines->append(QString("        assert(s_axi_arready == %1);").arg(names.arReady));
    lines->append(QString("        assert(s_axi_bvalid == %1);").arg(names.bValid));
    lines->append(QString("        assert(s_axi_bresp == %1);").arg(names.bResponse));
    lines->append(QString("        assert(s_axi_rvalid == %1);").arg(names.rValid));
    lines->append(QString("        assert(s_axi_rdata == %1);").arg(names.rData));
    lines->append(QString("        assert(s_axi_rresp == %1);").arg(names.rResponse));
    int storageIndex = 0;
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (field.access != QSocMmioAccess::ReadWrite) {
                continue;
            }
            if (!field.outputPort.isEmpty()) {
                lines->append(QString("        assert(%1 == %2);")
                                  .arg(field.outputPort, names.fields.at(storageIndex)));
            }
            ++storageIndex;
        }
    }
    lines->append("        assert(!s_axi_bvalid || (!s_axi_awready && !s_axi_wready));");
    lines->append("        assert(!s_axi_rvalid || !s_axi_arready);");
}

void appendCovers(QStringList *lines, const QSocMmioPlan &plan, const FormalNames &names)
{
    lines->append(QString("        cover(%1 && %2);").arg(names.awTake, names.wTake));
    lines->append(QString("        cover(%1 && %2);").arg(names.awPending, names.wTake));
    lines->append(QString("        cover(%1 && %2);").arg(names.wPending, names.awTake));
    lines->append(QString("        cover(%1 && %2(%3));")
                      .arg(names.writeFire, names.mappedFunction, names.writeAddress));
    lines->append(
        QString("        cover(%1 && %2(s_axi_araddr));").arg(names.arTake, names.mappedFunction));
    lines->append(
        QString("        cover(s_axi_bvalid && s_axi_bresp == %1);").arg(names.responseSlverr));
    lines->append(
        QString("        cover(s_axi_rvalid && s_axi_rresp == %1);").arg(names.responseSlverr));
    lines->append("        cover(s_axi_bvalid && !s_axi_bready);");
    lines->append("        cover(s_axi_rvalid && !s_axi_rready);");
    lines->append(QString("        cover(%1 && %2 && s_axi_araddr == %3 && %4(%3));")
                      .arg(names.arTake, names.writeFire, names.writeAddress, names.mappedFunction));
    for (quint32 lane = 0; lane < plan.dataWidth / 8; ++lane) {
        lines->append(
            QString("        cover(%1 && %2(%3) && %4[%5]);")
                .arg(names.writeFire, names.mappedFunction, names.writeAddress, names.writeStrobe)
                .arg(lane));
    }
}

void appendResetCovers(QStringList *lines, const FormalNames &names)
{
    lines->append(QString("    if (%1 && !rst_ni && $past(rst_ni)) begin").arg(names.pastValid));
    lines->append(QString("        cover($past(%1 && !%2));").arg(names.awPending, names.wPending));
    lines->append(QString("        cover($past(%1 && !%2));").arg(names.wPending, names.awPending));
    lines->append(QString("        cover($past(%1));").arg(names.bValid));
    lines->append(QString("        cover($past(%1));").arg(names.rValid));
    lines->append("    end");
}

void appendResetAssertions(QStringList *lines, const QSocMmioPlan &plan, const FormalNames &names)
{
    lines->append("    if (!rst_ni) begin");
    lines->append("        assert(!s_axi_awready);");
    lines->append("        assert(!s_axi_wready);");
    lines->append("        assert(!s_axi_arready);");
    lines->append("        assert(!s_axi_bvalid);");
    lines->append(QString("        assert(s_axi_bresp == %1);").arg(names.responseOkay));
    lines->append("        assert(!s_axi_rvalid);");
    lines->append(QString("        assert(s_axi_rdata == %1);").arg(zeroLiteral(plan.dataWidth)));
    lines->append(QString("        assert(s_axi_rresp == %1);").arg(names.responseOkay));
    lines->append(QString("        assert(!%1);").arg(names.awPending));
    lines->append(
        QString("        assert(%1 == %2);").arg(names.awAddress, zeroLiteral(plan.addressWidth)));
    lines->append(QString("        assert(!%1);").arg(names.wPending));
    lines->append(
        QString("        assert(%1 == %2);").arg(names.wData, zeroLiteral(plan.dataWidth)));
    lines->append(
        QString("        assert(%1 == %2);").arg(names.wStrobe, zeroLiteral(plan.dataWidth / 8)));
    lines->append(QString("        assert(!%1);").arg(names.bValid));
    lines->append(QString("        assert(%1 == %2);").arg(names.bResponse, names.responseOkay));
    lines->append(QString("        assert(!%1);").arg(names.rValid));
    lines->append(
        QString("        assert(%1 == %2);").arg(names.rData, zeroLiteral(plan.dataWidth)));
    lines->append(QString("        assert(%1 == %2);").arg(names.rResponse, names.responseOkay));
    int storageIndex = 0;
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (field.access != QSocMmioAccess::ReadWrite) {
                continue;
            }
            const QString resetValue = verilogLiteral(field.width, *field.resetValue);
            lines->append(
                QString("        assert(%1 == %2);").arg(names.fields.at(storageIndex), resetValue));
            if (!field.outputPort.isEmpty()) {
                lines->append(
                    QString("        assert(%1 == %2);").arg(field.outputPort, resetValue));
            }
            ++storageIndex;
        }
    }
    lines->append("    end");
}

void appendProperties(QStringList *lines, const QSocMmioPlan &plan, const FormalNames &names)
{
    lines->append("`ifdef FORMAL_EXTERNAL_RESET");
    lines->append("always @(posedge clk_i or negedge formal_reset_ni)");
    lines->append(QString("    %1 <= formal_reset_ni;").arg(names.pastValid));
    lines->append("`else");
    lines->append(QString("initial %1 = 1'b0;").arg(names.pastValid));
    lines->append(QString());
    lines->append("always @(posedge clk_i)");
    lines->append(QString("    %1 <= 1'b1;").arg(names.pastValid));
    lines->append("`endif");
    lines->append(QString());
    lines->append("always @(posedge clk_i) begin");
    lines->append(QString("    if (%1 && rst_ni && $past(rst_ni)) begin").arg(names.pastValid));
    appendMasterAssumptions(lines, names);
    appendResponseStability(lines);
    lines->append("    end");
    lines->append(QString());
    lines->append("    if (rst_ni) begin");
    appendOutputAssertions(lines, plan, names);
    appendCovers(lines, plan, names);
    lines->append("    end");
    lines->append(QString());
    appendResetCovers(lines, names);
    lines->append(QString());
    appendResetAssertions(lines, plan, names);
    lines->append("end");
    lines->append(QString());
}

QString buildSystemVerilog(const QSocMmioPlan &plan)
{
    const FormalNames names = allocateNames(plan);
    QStringList       lines;
    lines.append("// Generated by QSoC. Do not edit.");
    lines.append(QString("module %1_formal (").arg(plan.moduleName));
    lines.append("    input wire clk_i");
    lines.append("`ifdef FORMAL_EXTERNAL_RESET");
    lines.append("    , input wire formal_reset_ni");
    lines.append("`endif");
    lines.append(");");
    lines.append(QString());
    appendClockAndReset(&lines, names);
    appendAxiSignals(&lines, plan);
    appendSidebandSignals(&lines, plan);
    appendDut(&lines, plan, names);
    appendModelStorage(&lines, plan, names);
    appendAddressFunction(&lines, plan, names);
    appendReadFunction(&lines, plan, names);
    appendModelWires(&lines, plan, names);
    appendWriteModel(&lines, plan, names);
    appendReadModel(&lines, plan, names);
    appendProperties(&lines, plan, names);
    lines.append("endmodule");
    return lines.join('\n') + '\n';
}

QString buildSby(const QSocMmioPlan &plan)
{
    return QStringLiteral(
               "[tasks]\n"
               "prove\n"
               "bmc\n"
               "cover\n"
               "\n"
               "[options]\n"
               "prove: mode prove\n"
               "prove: depth 24\n"
               "prove: aigsmt none\n"
               "bmc: mode bmc\n"
               "bmc: depth 24\n"
               "cover: mode cover\n"
               "cover: depth 24\n"
               "\n"
               "[engines]\n"
               "prove: abc pdr\n"
               "bmc: smtbmc z3\n"
               "cover: smtbmc z3\n"
               "\n"
               "[script]\n"
               "read -formal -sv %1.v %1_formal.sv\n"
               "prep -top %1_formal\n"
               "\n"
               "[files]\n"
               "%1.v\n"
               "%1_formal.sv\n")
        .arg(plan.moduleName);
}

} // namespace

QSocMmioFormalCollateral QSocMmioFormal::generate(const QSocMmioPlan &plan)
{
    /* The reference model here mirrors read-write storage only. A write-one-clear
     * field would keep its storage slot but not its behaviour, so the proof would
     * compare the design against a model of a different register. Emit nothing
     * rather than a proof that means something else. */
    for (const QSocMmioRegisterPlan &reg : plan.registers) {
        for (const QSocMmioFieldPlan &field : reg.fields) {
            if (field.access == QSocMmioAccess::WriteOneClear) {
                return {};
            }
        }
    }
    return {buildSystemVerilog(plan), buildSby(plan)};
}
