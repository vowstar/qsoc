// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocmmioaxi.h"
#include "qsocmmioregisters.h"

namespace {
QString control(const QSocMmioPlan &plan)
{
    QString source = R"(
reg axi_write_busy_q;
reg [@A@:0] axi_write_address_q;
reg [@I@-1:0] axi_write_id_q;
reg [7:0] axi_write_len_q;
reg [8:0] axi_write_remaining_q;
reg [2:0] axi_write_size_q;
reg [1:0] axi_write_burst_q;
reg axi_write_invalid_q;
reg axi_write_error_q;
reg axi_w_pending_q;
reg [@D@-1:0] axi_w_data_q;
reg [@B@-1:0] axi_w_strobe_q;
reg axi_w_last_q;
reg axi_b_valid_q;
reg [1:0] axi_b_response_q;
reg axi_read_busy_q;
reg [@A@:0] axi_read_address_q;
reg [@I@-1:0] axi_read_id_q;
reg [7:0] axi_read_len_q;
reg [8:0] axi_read_remaining_q;
reg [2:0] axi_read_size_q;
reg [1:0] axi_read_burst_q;
reg axi_read_invalid_q;
reg axi_r_valid_q;
reg [@D@-1:0] axi_r_data_q;
reg [1:0] axi_r_response_q;
reg axi_r_last_q;

function burst_is_invalid;
    input [@A@-1:0] address;
    input [7:0] length;
    input [2:0] size;
    input [1:0] burst;
    reg [@A@+15:0] step, total, base, last;
    begin
        step = 1;
        step = step << size;
        total = step * ({1'b0, length} + 9'd1);
        base = address;
        base = base & ~(step - 1'b1);
        last = base + step - 1'b1;
        if (burst == 2'b01)
            last = base + total - 1'b1;
        if (burst == 2'b10)
            last = (base & ~(total - 1'b1)) + total - 1'b1;
        burst_is_invalid = size > @L@ || burst == 2'b11
            || (burst == 2'b00 && length > 8'd15)
            || (burst == 2'b10 && ((length != 1 && length != 3
                                  && length != 7 && length != 15)
                                  || (address & (step - 1'b1)) != 0))
            || (last >> @A@) != 0 || (last >> 12) != (address >> 12);
    end
endfunction

function [@A@:0] next_address;
    input [@A@:0] address;
    input [7:0] length;
    input [2:0] size;
    input [1:0] burst;
    reg [@A@:0] step, span, advanced;
    begin
        step = 1;
        step = step << size;
        span = step * ({1'b0, length} + 9'd1);
        advanced = (address & ~(step - 1'b1)) + step;
        next_address = address;
        if (burst == 2'b01)
            next_address = advanced;
        if (burst == 2'b10)
            next_address = (address & ~(span - 1'b1)) | (advanced & (span - 1'b1));
    end
endfunction

function [@B@-1:0] transfer_lanes;
    input [@A@:0] address;
    input [2:0] size;
    integer lane, low, high;
    begin
        low = address % @B@;
        high = (low / (1 << size) + 1) * (1 << size);
        transfer_lanes = 0;
        for (lane = 0; lane < @B@; lane = lane + 1)
            if (lane >= low && lane < high)
                transfer_lanes[lane] = 1'b1;
    end
endfunction

function [@D@-1:0] masked_read;
    input [@A@-1:0] address;
    input [@B@-1:0] lanes;
    reg [@D@-1:0] value;
    integer lane;
    begin
        value = read_register(address);
        masked_read = 0;
        for (lane = 0; lane < @B@; lane = lane + 1)
            if (lanes[lane])
                masked_read[lane*8 +: 8] = value[lane*8 +: 8];
    end
endfunction

wire [@A@-1:0] write_address = axi_write_address_q[@A@-1:0] & @A@'h@MASK@;
wire [@A@-1:0] read_address = axi_read_address_q[@A@-1:0] & @A@'h@MASK@;
wire [@B@-1:0] axi_write_lanes = transfer_lanes(axi_write_address_q, axi_write_size_q);
wire [@B@-1:0] axi_read_lanes = transfer_lanes(axi_read_address_q, axi_read_size_q);
wire axi_write_step = axi_write_busy_q && axi_w_pending_q;
wire axi_write_beat_error = axi_write_invalid_q || axi_write_address_q[@A@] @WRITE_OUTSIDE@
    || !address_is_mapped(write_address)
    || (axi_w_strobe_q & ~axi_write_lanes) != 0
    || (axi_w_last_q != (axi_write_remaining_q == 1));
wire axi_read_error = axi_read_invalid_q || axi_read_address_q[@A@] @READ_OUTSIDE@
    || !address_is_mapped(read_address);
wire write_fire = axi_write_step && !axi_write_beat_error;
wire [@D@-1:0] write_data = axi_w_data_q;
wire [@B@-1:0] write_strobe = axi_w_strobe_q & axi_write_lanes;
wire aw_take = s_axi_awvalid && s_axi_awready;
wire w_take = s_axi_wvalid && s_axi_wready;
wire ar_take = s_axi_arvalid && s_axi_arready;

assign s_axi_awready = rst_ni && !axi_write_busy_q && !axi_b_valid_q;
assign s_axi_wready = rst_ni && !axi_w_pending_q && !axi_b_valid_q;
assign s_axi_bid = axi_write_id_q;
assign s_axi_bresp = axi_b_response_q;
assign s_axi_bvalid = axi_b_valid_q;
assign s_axi_arready = rst_ni && !axi_read_busy_q && !axi_r_valid_q;
assign s_axi_rid = axi_read_id_q;
assign s_axi_rdata = axi_r_data_q;
assign s_axi_rresp = axi_r_response_q;
assign s_axi_rlast = axi_r_last_q;
assign s_axi_rvalid = axi_r_valid_q;

always @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
        axi_write_busy_q <= 0;
        axi_write_address_q <= 0;
        axi_write_id_q <= 0;
        axi_write_len_q <= 0;
        axi_write_remaining_q <= 0;
        axi_write_size_q <= 0;
        axi_write_burst_q <= 0;
        axi_write_invalid_q <= 0;
        axi_write_error_q <= 0;
        axi_w_pending_q <= 0;
        axi_w_data_q <= 0;
        axi_w_strobe_q <= 0;
        axi_w_last_q <= 0;
        axi_b_valid_q <= 0;
        axi_b_response_q <= 0;
    end else begin
        if (axi_b_valid_q && s_axi_bready)
            axi_b_valid_q <= 0;
        if (aw_take) begin
            axi_write_busy_q <= 1;
            axi_write_address_q <= {1'b0, s_axi_awaddr};
            axi_write_id_q <= s_axi_awid;
            axi_write_len_q <= s_axi_awlen;
            axi_write_remaining_q <= {1'b0, s_axi_awlen} + 9'd1;
            axi_write_size_q <= s_axi_awsize;
            axi_write_burst_q <= s_axi_awburst;
            axi_write_invalid_q <= burst_is_invalid(s_axi_awaddr, s_axi_awlen,
                                                   s_axi_awsize, s_axi_awburst);
            axi_write_error_q <= 0;
        end
        if (w_take) begin
            axi_w_pending_q <= 1;
            axi_w_data_q <= s_axi_wdata;
            axi_w_strobe_q <= s_axi_wstrb;
            axi_w_last_q <= s_axi_wlast;
        end
        if (axi_write_step) begin
            axi_w_pending_q <= 0;
            axi_write_error_q <= axi_write_error_q || axi_write_beat_error;
            axi_write_remaining_q <= axi_write_remaining_q - 1'b1;
            axi_write_address_q <= next_address(axi_write_address_q, axi_write_len_q,
                                               axi_write_size_q, axi_write_burst_q);
            if (axi_write_remaining_q == 1) begin
                axi_write_busy_q <= 0;
                axi_b_valid_q <= 1;
                axi_b_response_q <= (axi_write_error_q || axi_write_beat_error) ? 2'b10 : 2'b00;
            end
        end
    end
end

always @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
        axi_read_busy_q <= 0;
        axi_read_address_q <= 0;
        axi_read_id_q <= 0;
        axi_read_len_q <= 0;
        axi_read_remaining_q <= 0;
        axi_read_size_q <= 0;
        axi_read_burst_q <= 0;
        axi_read_invalid_q <= 0;
        axi_r_valid_q <= 0;
        axi_r_data_q <= 0;
        axi_r_response_q <= 0;
        axi_r_last_q <= 0;
    end else begin
        if (axi_r_valid_q && s_axi_rready)
            axi_r_valid_q <= 0;
        if (ar_take) begin
            axi_read_busy_q <= 1;
            axi_read_address_q <= {1'b0, s_axi_araddr};
            axi_read_id_q <= s_axi_arid;
            axi_read_len_q <= s_axi_arlen;
            axi_read_remaining_q <= {1'b0, s_axi_arlen} + 9'd1;
            axi_read_size_q <= s_axi_arsize;
            axi_read_burst_q <= s_axi_arburst;
            axi_read_invalid_q <= burst_is_invalid(s_axi_araddr, s_axi_arlen,
                                                  s_axi_arsize, s_axi_arburst);
        end
        if (axi_read_busy_q && !axi_r_valid_q) begin
            axi_r_valid_q <= 1;
            axi_r_data_q <= axi_read_error ? @D@'h0 : masked_read(read_address, axi_read_lanes);
            axi_r_response_q <= axi_read_error ? 2'b10 : 2'b00;
            axi_r_last_q <= axi_read_remaining_q == 1;
            axi_read_remaining_q <= axi_read_remaining_q - 1'b1;
            axi_read_address_q <= next_address(axi_read_address_q, axi_read_len_q,
                                              axi_read_size_q, axi_read_burst_q);
            if (axi_read_remaining_q == 1)
                axi_read_busy_q <= 0;
        end
    end
end
)";
    quint32 size   = 0;
    for (quint32 bytes = plan.dataWidth / 8; bytes > 1; bytes >>= 1) {
        ++size;
    }
    for (const QString &direction : {QString("write"), QString("read")}) {
        QString outside;
        if (plan.zeroFillBytes != 0) {
            outside = QString(
                          "|| ((axi_%1_address_q | ((%2'h1 << axi_%1_size_q) - 1'b1)) >= %2'h%3)")
                          .arg(direction)
                          .arg(plan.addressWidth + 1)
                          .arg(plan.zeroFillBytes, 0, 16);
        }
        source.replace("@" + direction.toUpper() + "_OUTSIDE@", outside);
    }
    source.replace("@A@", QString::number(plan.addressWidth));
    source.replace("@D@", QString::number(plan.dataWidth));
    source.replace("@B@", QString::number(plan.dataWidth / 8));
    source.replace("@I@", QString::number(plan.idWidth));
    source.replace("@L@", QString::number(size));
    const quint64 mask = (~quint64(0) >> (64 - plan.addressWidth))
                         & ~quint64(plan.dataWidth / 8 - 1);
    source.replace("@MASK@", QString::number(mask, 16));
    return source;
}
} // namespace

QList<QSocMmioPortDescription> QSocMmioAxi::ports(const QSocMmioPlan &plan)
{
    QList<QSocMmioPortDescription> ports = {{"clk_i", "input", 1}, {"rst_ni", "input", 1}};
    for (const QString &channel : {QString("aw"), QString("ar")}) {
        const QString name = "s_axi_" + channel;
        ports.append({name + "id", "input", plan.idWidth});
        ports.append({name + "addr", "input", plan.addressWidth});
        ports.append({name + "len", "input", 8});
        ports.append({name + "size", "input", 3});
        ports.append({name + "burst", "input", 2});
        ports.append({name + "lock", "input", 1});
        ports.append({name + "cache", "input", 4});
        ports.append({name + "prot", "input", 3});
        ports.append({name + "valid", "input", 1});
        ports.append({name + "ready", "output", 1});
    }
    ports.append({"s_axi_wdata", "input", plan.dataWidth});
    ports.append({"s_axi_wstrb", "input", plan.dataWidth / 8});
    ports.append({"s_axi_wlast", "input", 1});
    ports.append({"s_axi_wvalid", "input", 1});
    ports.append({"s_axi_wready", "output", 1});
    ports.append({"s_axi_bid", "output", plan.idWidth});
    ports.append({"s_axi_bresp", "output", 2});
    ports.append({"s_axi_bvalid", "output", 1});
    ports.append({"s_axi_bready", "input", 1});
    ports.append({"s_axi_rid", "output", plan.idWidth});
    ports.append({"s_axi_rdata", "output", plan.dataWidth});
    ports.append({"s_axi_rresp", "output", 2});
    ports.append({"s_axi_rlast", "output", 1});
    ports.append({"s_axi_rvalid", "output", 1});
    ports.append({"s_axi_rready", "input", 1});
    return ports;
}

QString QSocMmioAxi::generate(const QSocMmioPlan &plan)
{
    QStringList lines;
    lines.append(QString("module %1 (").arg(plan.moduleName));
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
