// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocmmioahb.h"
#include "qsocmmioregisters.h"

namespace {
QString control(const QSocMmioPlan &plan)
{
    QString source = R"(
reg ahb_active_q;
reg ahb_write_q;
reg [@A@-1:0] ahb_address_q;
reg [2:0] ahb_size_q;
reg ahb_error_q;
reg ahb_error_last_q;

function access_is_invalid;
    input [@A@-1:0] address;
    input [2:0] size;
    reg [@A@:0] bytes, last;
    begin
        bytes = 1;
        bytes = bytes << size;
        last = {1'b0, address} + bytes - 1'b1;
        access_is_invalid = size > @L@ || (address & (bytes - 1'b1)) != 0
            || last[@A@] || !address_is_mapped(address & @A@'h@MASK@) @OUTSIDE@;
    end
endfunction

function [@B@-1:0] byte_lanes;
    input [@A@-1:0] address;
    input [2:0] size;
    integer lane, first;
    begin
        first = address % @B@;
        byte_lanes = 0;
        for (lane = 0; lane < @B@; lane = lane + 1)
            if (lane >= first && lane < first + (1 << size))
                byte_lanes[lane] = 1'b1;
    end
endfunction

wire ahb_selected = s_ahb_hsel && s_ahb_htrans[1];
wire [@A@-1:0] write_address = ahb_address_q & @A@'h@MASK@;
wire [@D@-1:0] write_data = s_ahb_hwdata;
wire [@B@-1:0] write_strobe = byte_lanes(ahb_address_q, ahb_size_q);
wire write_fire = rst_ni && ahb_active_q && ahb_write_q && !ahb_error_q
               && s_ahb_hready;

assign s_ahb_hreadyout = !rst_ni || !ahb_active_q || !ahb_error_q || ahb_error_last_q;
assign s_ahb_hresp = rst_ni && ahb_active_q && ahb_error_q;
assign s_ahb_hrdata = rst_ni && ahb_active_q && !ahb_write_q && !ahb_error_q
                   ? read_register(write_address) : @D@'h0;

always @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
        ahb_active_q <= 0;
        ahb_write_q <= 0;
        ahb_address_q <= 0;
        ahb_size_q <= 0;
        ahb_error_q <= 0;
    end else if (s_ahb_hready) begin
        ahb_active_q <= ahb_selected;
        if (ahb_selected) begin
            ahb_write_q <= s_ahb_hwrite;
            ahb_address_q <= s_ahb_haddr;
            ahb_size_q <= s_ahb_hsize;
            ahb_error_q <= access_is_invalid(s_ahb_haddr, s_ahb_hsize);
        end
    end
end

always @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni)
        ahb_error_last_q <= 0;
    else if (ahb_active_q && ahb_error_q && !ahb_error_last_q)
        ahb_error_last_q <= 1;
    else if (s_ahb_hready)
        ahb_error_last_q <= 0;
end
)";
    quint32 size   = 0;
    for (quint32 bytes = plan.dataWidth / 8; bytes > 1; bytes >>= 1) {
        ++size;
    }
    const quint64 mask = (~quint64(0) >> (64 - plan.addressWidth))
                         & ~quint64(plan.dataWidth / 8 - 1);
    source.replace(
        "@OUTSIDE@",
        plan.zeroFillBytes == 0
            ? QString()
            : QString("|| last >= %1'h%2").arg(plan.addressWidth + 1).arg(plan.zeroFillBytes, 0, 16));
    source.replace("@A@", QString::number(plan.addressWidth));
    source.replace("@D@", QString::number(plan.dataWidth));
    source.replace("@B@", QString::number(plan.dataWidth / 8));
    source.replace("@L@", QString::number(size));
    source.replace("@MASK@", QString::number(mask, 16));
    return source;
}
} // namespace

QList<QSocMmioPortDescription> QSocMmioAhb::ports(const QSocMmioPlan &plan)
{
    return {
        {"clk_i", "input", 1},
        {"rst_ni", "input", 1},
        {"s_ahb_hsel", "input", 1},
        {"s_ahb_haddr", "input", plan.addressWidth},
        {"s_ahb_htrans", "input", 2},
        {"s_ahb_hwrite", "input", 1},
        {"s_ahb_hsize", "input", 3},
        {"s_ahb_hburst", "input", 3},
        {"s_ahb_hprot", "input", 4},
        {"s_ahb_hmastlock", "input", 1},
        {"s_ahb_hwdata", "input", plan.dataWidth},
        {"s_ahb_hready", "input", 1},
        {"s_ahb_hrdata", "output", plan.dataWidth},
        {"s_ahb_hreadyout", "output", 1},
        {"s_ahb_hresp", "output", plan.bus == QSocMmioBus::Ahb ? 2u : 1u}};
}

QString QSocMmioAhb::generate(const QSocMmioPlan &plan)
{
    QStringList lines = {QString("module %1 (").arg(plan.moduleName)};
    QStringList declarations;
    for (const auto &port : QSocMmioGenerator::describePorts(plan)) {
        const QString range = port.width == 1 ? QString() : QString("[%1:0] ").arg(port.width - 1);
        declarations.append(QString("    %1 wire %2%3").arg(port.direction, range, port.name));
    }
    lines.append(declarations.join(",\n"));
    lines.append(");\n");
    const auto registers
        = QSocMmioRegisters::generate(plan, "write_address", "write_data", "write_strobe");
    lines.append(registers.storage);
    lines.append(registers.decode);
    lines.append(control(plan));
    lines.append(registers.write);
    lines.append("endmodule\n");
    return lines.join('\n');
}
