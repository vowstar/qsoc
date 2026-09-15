// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "qsocmmioaxiverification.h"

#include <algorithm>
#include <QSet>

namespace {
QString literal(quint32 width, quint64 value)
{
    return QString("%1'h%2").arg(width).arg(value, 0, 16);
}

QString range(quint32 width)
{
    return width == 1 ? QString() : QString("[%1:0] ").arg(width - 1);
}

QString bitOf(const QString &name, quint32 width, quint32 bit)
{
    return width == 1 ? name : QString("%1[%2]").arg(name).arg(bit);
}

QString prefixFor(const QSocMmioPlan &plan)
{
    QString    prefix = "axi_model_";
    const auto ports  = QSocMmioGenerator::describePorts(plan);
    for (;;) {
        bool collision = false;
        for (const auto &port : ports)
            collision |= port.name.startsWith(prefix);
        if (!collision)
            return prefix;
        prefix += '_';
    }
}

void appendSignals(QStringList *lines, const QSocMmioPlan &plan, bool formal)
{
    for (const auto &port : QSocMmioGenerator::describePorts(plan)) {
        if (port.name == "clk_i" || (formal && port.name == "rst_ni"))
            continue;
        const QString qualifier = port.direction == "input"
                                      ? (formal ? "(* anyseq *) reg " : "logic ")
                                      : "wire ";
        lines->append(
            qualifier + range(port.width) + port.name
            + (formal || port.direction == "output" ? ";" : " = '0;"));
    }
}

void appendDut(QStringList *lines, const QSocMmioPlan &plan, const QString &bus)
{
    lines->append(plan.moduleName + " dut (");
    QStringList connections;
    for (const auto &port : QSocMmioGenerator::describePorts(plan)) {
        connections.append(QString("    .%1(%2%1)").arg(port.name, bus));
    }
    lines->append(connections.join(",\n"));
    lines->append(");");
}

QString model(const QSocMmioPlan &plan, bool formal)
{
    const QString prefix = prefixFor(plan);
    const quint32 bytes  = plan.dataWidth / 8;
    QStringList   fields, readBits, writeBits, resetBits, fieldChecks;
    QSet<quint64> words;
    qsizetype     index = 0;
    for (const auto &reg : plan.registers) {
        words.insert(reg.byteOffset);
        for (const auto &field : reg.fields) {
            const QString name = "@P@field_" + QString::number(index++);
            if (qsocMmioHasStorage(field.access)) {
                fields.append("reg " + range(field.width) + name + ";");
                resetBits.append(name + " <= " + literal(field.width, *field.resetValue) + ";");
            } else {
                const QString value = field.constantValue.has_value()
                                          ? literal(field.width, *field.constantValue)
                                          : field.inputPort;
                fields.append("wire " + range(field.width) + name + " = " + value + ";");
            }
            for (quint32 bit = 0; bit < field.width; ++bit) {
                const quint64 address = reg.byteOffset + (field.lsb + bit) / 8;
                const quint32 lane    = quint32(address % bytes);
                const quint32 busBit  = lane * 8 + (field.lsb + bit) % 8;
                words.insert(address - lane);
                const QString bound = literal(plan.addressWidth + 1, address);
                readBits.append(
                    QString("if (address <= %1 && last_byte >= %1) @P@read_value[%2] = %3;")
                        .arg(bound)
                        .arg(busBit)
                        .arg(bitOf(name, field.width, bit)));
                if (qsocMmioHasStorage(field.access)) {
                    const QString target = bitOf(name, field.width, bit);
                    QString       value  = QString("@P@data[%1]").arg(busBit);
                    if (field.access == QSocMmioAccess::WriteOneClear)
                        value = target + " & ~" + value;
                    writeBits.append(QString(
                                         "if (@P@commit && !@P@beat_error && @P@wa <= %1 && "
                                         "@P@wend >= %1 && @P@mask[%2]) %3 <= %4;")
                                         .arg(bound)
                                         .arg(lane)
                                         .arg(target, value));
                }
            }
            if (field.access == QSocMmioAccess::WriteOneClear) {
                writeBits.append("if (" + field.inputPort + ") " + name + " <= 1'b1;");
            }
            if (!field.outputPort.isEmpty()) {
                const QString condition = field.outputPort + " == " + name;
                fieldChecks.append(
                    formal
                        ? QString("@P@FIELD_%1: assert(%2);").arg(index).arg(condition)
                        : QString("if (!(%1)) `uvm_error(\"FIELD_%2\", \"register value mismatch\")")
                              .arg(condition)
                              .arg(index));
            }
        }
    }
    QStringList map;
    auto        sortedWords = words.values();
    std::sort(sortedWords.begin(), sortedWords.end());
    for (quint64 word : sortedWords) {
        map.append(QString("if (address / @B@ == %1) @P@mapped = 1;")
                       .arg(literal(plan.addressWidth + 1, word / bytes)));
    }
    QString                              source = R"(
@FIELDS@
reg @P@aw, @P@w, @P@ar, @P@bv, @P@rv;
reg [@A@:0] @P@waddress, @P@raddress;
reg [7:0] @P@wlength, @P@rlength;
reg [8:0] @P@wi, @P@ri;
reg [2:0] @P@wsize, @P@rsize;
reg [1:0] @P@wburst, @P@rburst;
reg [@I@-1:0] @P@wid, @P@rid;
reg [@D@-1:0] @P@data, @P@rdata;
reg [@B@-1:0] @P@mask;
reg @P@last, @P@rlast, @P@werror, @P@berror, @P@rerror;
reg @P@wbad, @P@rbad;

function [@A@:0] @P@address;
    input [@A@:0] start;
    input [7:0] length;
    input [8:0] beat;
    input [2:0] size;
    input [1:0] burst;
    reg [@A@+15:0] step, span, value;
    begin
        step = 1;
        step = step << size;
        span = step * (length + 9'd1);
        value = start;
        if (beat != 0 && burst == 1) value = (start + (beat << size)) & ~(step - 1);
        if (burst == 2) value = (start & ~(span - 1)) | ((start + (beat << size)) & (span - 1));
        @P@address = value[@A@:0];
    end
endfunction

function @P@invalid;
    input [@A@-1:0] start;
    input [7:0] length;
    input [2:0] size;
    input [1:0] burst;
    reg [@A@+15:0] step, span, finish;
    begin
        step = 1;
        step = step << size;
        span = step * (length + 9'd1);
        finish = start | (step - 1);
        if (burst == 1) finish = (start & ~(step - 1)) + span - 1;
        if (burst == 2) finish = start | (span - 1);
        @P@invalid = size > $clog2(@B@) || burst == 3
            || (burst == 0 && length > 15)
            || (burst == 2 && ((length != 1 && length != 3 && length != 7 && length != 15)
                              || (start & (step - 1)) != 0))
            || (finish >> @A@) != 0 || finish / 4096 != start / 4096;
    end
endfunction

function @P@mapped;
    input [@A@:0] address;
    input [2:0] size;
    reg [@A@+15:0] step, last_byte;
    begin
        step = 1;
        step = step << size;
        last_byte = address | (step - 1);
        @P@mapped = 0;
        @MAP@
        @APERTURE@
        if (address[@A@]) @P@mapped = 0;
    end
endfunction

function [@D@-1:0] @P@read_value;
    input [@A@:0] address;
    input [2:0] size;
    reg [@A@+15:0] step, last_byte;
    begin
        step = 1;
        step = step << size;
        last_byte = address | (step - 1);
        @P@read_value = 0;
        @READ_BITS@
    end
endfunction

wire [@A@:0] @P@wa = @P@waddress;
wire [@A@:0] @P@ra = @P@raddress;
wire [@A@+15:0] @P@wstep = @EXT@'h1 << @P@wsize;
wire [@A@+15:0] @P@wend = @P@wa | (@P@wstep - 1);
reg @P@strobe_error;
integer @P@lane;
always @* begin
    @P@strobe_error = 0;
    for (@P@lane = 0; @P@lane < @B@; @P@lane = @P@lane + 1)
        if (@P@mask[@P@lane] && (@P@wa / @B@ * @B@ + @P@lane < @P@wa
            || @P@wa / @B@ * @B@ + @P@lane > @P@wend)) @P@strobe_error = 1;
end
wire @P@commit = @P@aw && @P@w;
wire @P@beat_error = @P@wbad || !@P@mapped(@P@wa, @P@wsize)
    || @P@strobe_error || (@P@last != (@P@wi == 1));

always @(posedge clk_i or negedge rst_ni) begin
    if (!rst_ni) begin
        @P@aw <= 0; @P@w <= 0; @P@ar <= 0; @P@bv <= 0; @P@rv <= 0;
        @P@waddress <= 0; @P@raddress <= 0; @P@wlength <= 0; @P@rlength <= 0;
        @P@wi <= 0; @P@ri <= 0; @P@wsize <= 0; @P@rsize <= 0;
        @P@wburst <= 0; @P@rburst <= 0; @P@wid <= 0; @P@rid <= 0;
        @P@data <= 0; @P@mask <= 0; @P@last <= 0;
        @P@rdata <= 0; @P@rlast <= 0; @P@werror <= 0;
        @P@berror <= 0; @P@rerror <= 0; @P@wbad <= 0; @P@rbad <= 0;
        @RESET_BITS@
    end else begin
        if (s_axi_awvalid && s_axi_awready) begin
            @P@aw <= 1; @P@wi <= {1'b0, s_axi_awlen} + 9'd1; @P@werror <= 0;
            @P@waddress <= s_axi_awaddr; @P@wlength <= s_axi_awlen;
            @P@wsize <= s_axi_awsize; @P@wburst <= s_axi_awburst; @P@wid <= s_axi_awid;
            @P@wbad <= @P@invalid(s_axi_awaddr, s_axi_awlen, s_axi_awsize, s_axi_awburst);
        end
        if (s_axi_wvalid && s_axi_wready) begin
            @P@w <= 1; @P@data <= s_axi_wdata; @P@mask <= s_axi_wstrb; @P@last <= s_axi_wlast;
        end
        if (s_axi_bvalid && s_axi_bready) @P@bv <= 0;
        if (@P@commit) begin
            @P@waddress <= @P@address(@P@waddress, @P@wlength, 9'd1, @P@wsize, @P@wburst);
            @P@w <= 0; @P@wi <= @P@wi - 1'b1; @P@werror <= @P@werror || @P@beat_error;
            if (@P@wi == 1) begin
                @P@aw <= 0; @P@bv <= 1; @P@berror <= @P@werror || @P@beat_error;
            end
        end
        if (s_axi_arvalid && s_axi_arready) begin
            @P@ar <= 1; @P@ri <= {1'b0, s_axi_arlen} + 9'd1;
            @P@raddress <= s_axi_araddr; @P@rlength <= s_axi_arlen;
            @P@rsize <= s_axi_arsize; @P@rburst <= s_axi_arburst; @P@rid <= s_axi_arid;
            @P@rbad <= @P@invalid(s_axi_araddr, s_axi_arlen, s_axi_arsize, s_axi_arburst);
        end
        if (s_axi_rvalid && s_axi_rready) @P@rv <= 0;
        if (@P@ar && !@P@rv) begin
            @P@raddress <= @P@address(@P@raddress, @P@rlength, 9'd1, @P@rsize, @P@rburst);
            @P@rv <= 1; @P@ri <= @P@ri - 1'b1; @P@rlast <= @P@ri == 1;
            @P@rerror <= @P@rbad || !@P@mapped(@P@ra, @P@rsize);
            @P@rdata <= (@P@rbad || !@P@mapped(@P@ra, @P@rsize)) ? '0 : @P@read_value(@P@ra, @P@rsize);
            if (@P@ri == 1) @P@ar <= 0;
        end
        @WRITE_BITS@
    end
end
always @(posedge clk_i) begin
    if (rst_ni) begin
        @CHECKS@
        @FIELD_CHECKS@
    end
end
)";
    QStringList                          checks;
    const QList<QPair<QString, QString>> predicates
        = {{"AW_READY", "s_axi_awready == (!@P@aw && !@P@bv)"},
           {"W_READY", "s_axi_wready == (!@P@w && !@P@bv)"},
           {"AR_READY", "s_axi_arready == (!@P@ar && !@P@rv)"},
           {"B_VALID", "s_axi_bvalid == @P@bv"},
           {"R_VALID", "s_axi_rvalid == @P@rv"},
           {"B_ID", "!s_axi_bvalid || s_axi_bid == @P@wid"},
           {"B_RESPONSE", "!s_axi_bvalid || s_axi_bresp == (@P@berror ? 2'b10 : 2'b00)"},
           {"R_ID", "!s_axi_rvalid || s_axi_rid == @P@rid"},
           {"R_RESPONSE", "!s_axi_rvalid || s_axi_rresp == (@P@rerror ? 2'b10 : 2'b00)"},
           {"R_LAST", "!s_axi_rvalid || s_axi_rlast == @P@rlast"},
           {"READBACK", "!s_axi_rvalid || s_axi_rdata == @P@rdata"}};
    for (const auto &item : predicates) {
        checks.append(
            formal ? "@P@" + item.first + ": assert(" + item.second + ");"
                   : "if (!(" + item.second + ")) `uvm_error(\"" + item.first
                         + "\", \"AXI contract mismatch\")");
    }
    source.replace("@FIELDS@", fields.join('\n'));
    source.replace("@RESET_BITS@", resetBits.join('\n'));
    source.replace("@READ_BITS@", readBits.join('\n'));
    source.replace("@WRITE_BITS@", writeBits.join('\n'));
    source.replace("@FIELD_CHECKS@", fieldChecks.join('\n'));
    source.replace("@CHECKS@", checks.join('\n'));
    source.replace("@MAP@", map.join('\n'));
    source.replace(
        "@APERTURE@",
        plan.zeroFillBytes == 0 ? QString()
                                : QString("@P@mapped = last_byte < %1;")
                                      .arg(literal(plan.addressWidth + 1, plan.zeroFillBytes)));
    source.replace("@P@", prefix);
    source.replace("@D@", QString::number(plan.dataWidth));
    source.replace("@A@", QString::number(plan.addressWidth));
    source.replace("@EXT@", QString::number(plan.addressWidth + 16));
    source.replace("@B@", QString::number(bytes));
    source.replace("@I@", QString::number(plan.idWidth));
    return source;
}
} // namespace

QString QSocMmioAxiVerification::formal(const QSocMmioPlan &plan)
{
    QStringList lines;
    lines.append(QString("module %1_formal(input wire clk_i").arg(plan.moduleName));
    lines.append("`ifdef FORMAL_EXTERNAL_RESET\n, input wire formal_reset_ni\n`endif\n);");
    lines.append("`ifdef FORMAL_EXTERNAL_RESET\nwire rst_ni = formal_reset_ni;\n`else");
    lines.append(
        "reg [1:0] " + prefixFor(plan) + "reset_count = 0;\n(* anyseq *) reg " + prefixFor(plan)
        + "reset_request;");
    lines.append(
        QString("always @(posedge clk_i) if (%1reset_count != 3) %1reset_count <= %1reset_count + 1'b1;")
            .arg(prefixFor(plan)));
    lines.append(QString("wire rst_ni = %1reset_count == 3 && !%1reset_request;\n`endif")
                     .arg(prefixFor(plan)));
    appendSignals(&lines, plan, true);
    appendDut(&lines, plan, QString());
    lines.append(model(plan, true));
    lines.append("always @(posedge clk_i) begin\n    if (rst_ni) begin");
    const QString prefix = prefixFor(plan);
    for (const QString &channel : {QString("aw"), QString("ar")}) {
        lines.append(QString(
                         "        if (s_axi_%1valid) assume(!%2invalid(s_axi_%1addr, s_axi_%1len, "
                         "s_axi_%1size, s_axi_%1burst));")
                         .arg(channel, prefix));
    }
    lines.append(QString("        if (%1commit) assume(!%1strobe_error && (%1last == (%1wi == 1)));")
                     .arg(prefix));
    for (const QString &channel : {QString("aw"), QString("w"), QString("ar")}) {
        QStringList stable;
        for (const auto &port : QSocMmioGenerator::describePorts(plan)) {
            if (port.direction == "input" && port.name.startsWith("s_axi_" + channel))
                stable.append(port.name);
        }
        const QString name = "s_axi_" + channel;
        lines.append(
            QString("        if ($past(rst_ni && %1valid && !%1ready)) assume($stable({%2}));")
                .arg(name, stable.join(", ")));
    }
    lines.append(
        "        cover(s_axi_bvalid && s_axi_bready);\n        cover(s_axi_rvalid && s_axi_rready "
        "&& s_axi_rlast);");
    lines.append("        cover(s_axi_bvalid && !s_axi_bready && s_axi_rvalid && !s_axi_rready);");
    lines.append("    end\nend\nendmodule");
    return lines.join('\n') + '\n';
}

QSocMmioUvmCollateral QSocMmioAxiVerification::uvm(const QSocMmioPlan &plan)
{
    QSocMmioUvmCollateral result;
    const QString         interfaceName = plan.moduleName + "_uvm_if";
    QStringList           lines;
    lines.append("`include \"uvm_macros.svh\"");
    lines.append("interface " + interfaceName + "(input logic clk_i);");
    lines.append("timeunit 1ns; timeprecision 1ps;\nimport uvm_pkg::*;");
    appendSignals(&lines, plan, false);
    lines.append(model(plan, false));
    lines.append("endinterface");
    result.interfaceSource = lines.join('\n') + '\n';
    QString       source   = R"(`include "uvm_macros.svh"
package @M@_uvm_pkg;
timeunit 1ns; timeprecision 1ps;
import uvm_pkg::*;
class @M@_uvm_test extends uvm_test;
    `uvm_component_utils(@M@_uvm_test)
    virtual @M@_uvm_if vif;
    function new(string name = "@M@_uvm_test", uvm_component parent = null);
        super.new(name, parent);
    endfunction
    function void build_phase(uvm_phase phase);
        super.build_phase(phase);
        if (!uvm_config_db #(virtual @M@_uvm_if)::get(this, "", "vif", vif))
            `uvm_fatal("CONFIG", "missing interface")
    endfunction
    task write_access(input bit [@A@-1:0] address, input int length, size, burst,
                      input bit [@B@-1:0] mask);
        @(negedge vif.clk_i);
        vif.s_axi_awaddr = address;
        vif.s_axi_awlen = 8'(length - 1);
        vif.s_axi_awsize = 3'(size);
        vif.s_axi_awburst = 2'(burst);
        vif.s_axi_awid = '1;
        vif.s_axi_awvalid = 1;
        do @(posedge vif.clk_i); while (!vif.s_axi_awready);
        @(negedge vif.clk_i);
        vif.s_axi_awvalid = 0;
        vif.s_axi_awaddr = ~address;
        for (int beat = 0; beat < length; beat++) begin
            @(negedge vif.clk_i);
            vif.s_axi_wvalid = 1;
            for (int lane = 0; lane < @B@; lane++)
                vif.s_axi_wdata[lane*8 +: 8] = 8'(lane * 13 + beat * 7 + 1);
            vif.s_axi_wstrb = mask;
            vif.s_axi_wlast = beat == length - 1;
            do @(posedge vif.clk_i); while (!vif.s_axi_wready);
            @(negedge vif.clk_i);
            vif.s_axi_wvalid = 0;
            vif.s_axi_wdata = '0;
            vif.s_axi_wstrb = ~mask;
            vif.s_axi_wlast = 0;
        end
        while (!vif.s_axi_bvalid) @(negedge vif.clk_i);
        repeat (3) @(negedge vif.clk_i);
        vif.s_axi_bready = 1;
        @(negedge vif.clk_i);
        vif.s_axi_bready = 0;
    endtask
    task read_access(input bit [@A@-1:0] address, input int length, size, burst);
        @(negedge vif.clk_i);
        vif.s_axi_araddr = address;
        vif.s_axi_arlen = 8'(length - 1);
        vif.s_axi_arsize = 3'(size);
        vif.s_axi_arburst = 2'(burst);
        vif.s_axi_arid = '1;
        vif.s_axi_arvalid = 1;
        do @(posedge vif.clk_i); while (!vif.s_axi_arready);
        @(negedge vif.clk_i);
        vif.s_axi_arvalid = 0;
        vif.s_axi_araddr = ~address;
        for (int beat = 0; beat < length; beat++) begin
            while (!vif.s_axi_rvalid) @(negedge vif.clk_i);
            repeat (3) @(negedge vif.clk_i);
            vif.s_axi_rready = 1;
            @(negedge vif.clk_i);
            vif.s_axi_rready = 0;
        end
    endtask
    task run_phase(uvm_phase phase);
        bit [@B@-1:0] mask;
        phase.raise_objection(this);
        vif.rst_ni = 0;
        repeat (3) @(negedge vif.clk_i);
        vif.rst_ni = 1;
        @RUN@
        @(negedge vif.clk_i);
        vif.rst_ni = 0;
        repeat (2) @(negedge vif.clk_i);
        vif.rst_ni = 1;
        @RESET_READS@
        phase.drop_objection(this);
    endtask
endclass
endpackage
)";
    QSet<quint64> words;
    const quint32 bytes = plan.dataWidth / 8;
    for (const auto &reg : plan.registers) {
        words.insert(reg.byteOffset);
        for (const auto &field : reg.fields) {
            for (quint32 bit = 0; bit < field.width; bit += 8) {
                const quint64 address = reg.byteOffset + (field.lsb + bit) / 8;
                words.insert(address - address % bytes);
            }
            const quint64 last = reg.byteOffset + (field.lsb + field.width - 1) / 8;
            words.insert(last - last % bytes);
        }
    }
    auto addresses = words.values();
    std::sort(addresses.begin(), addresses.end());
    QStringList runs, reads;
    for (quint64 address : addresses) {
        const QString addr = literal(plan.addressWidth, address);
        reads.append("read_access(" + addr + ", 1, $clog2(@B@), 1);");
        runs.append(reads.last());
        runs.append("write_access(" + addr + ", 1, $clog2(@B@), 1, '1);");
        runs.append(reads.last());
        runs.append(QString(
                        "for (int lane = 0; lane < @B@; lane++) begin\n"
                        "mask = '0; mask[lane] = 1;\n"
                        "write_access(%1, 1, $clog2(@B@), 1, mask);\n"
                        "read_access(%1, 1, $clog2(@B@), 1);\nend")
                        .arg(addr));
        runs.append("write_access(" + addr + ", 1, $clog2(@B@), 1, '0);");
    }
    if (!addresses.isEmpty()) {
        const QString addr = literal(plan.addressWidth, addresses.first());
        runs.append("write_access(" + addr + ", 4, $clog2(@B@), 1, '1);");
        runs.append("read_access(" + addr + ", 4, $clog2(@B@), 1);");
        runs.append("read_access(" + addr + ", 4, $clog2(@B@), 0);");
        runs.append("read_access(" + addr + ", 4, $clog2(@B@), 2);");
    }
    source.replace("@RUN@", runs.join('\n'));
    source.replace("@RESET_READS@", reads.join('\n'));
    source.replace("@M@", plan.moduleName);
    source.replace("@A@", QString::number(plan.addressWidth));
    source.replace("@B@", QString::number(bytes));
    result.packageSource = source;
    lines.clear();
    lines.append(QString(
                     "module %1_uvm_tb;\ntimeunit 1ns; timeprecision 1ps;\nimport "
                     "uvm_pkg::*;\nimport %1_uvm_pkg::*;")
                     .arg(plan.moduleName));
    lines.append("reg clk_i = 0;\nalways #5ns clk_i = !clk_i;");
    lines.append(interfaceName + " bus(clk_i);");
    appendDut(&lines, plan, "bus.");
    lines.append(QString(
                     "initial begin\nuvm_config_db #(virtual %1)::set(null, \"*\", \"vif\", "
                     "bus);\nrun_test(\"%2_uvm_test\");\nend")
                     .arg(interfaceName, plan.moduleName));
    lines.append("initial begin #10ms; $fatal(1, \"QSOC_UVM_TIMEOUT\"); end");
    lines.append(
        "final begin\nif (uvm_report_server::get_server().get_severity_count(UVM_ERROR) != 0\n"
        " || uvm_report_server::get_server().get_severity_count(UVM_FATAL) != 0)\n"
        "$fatal(1, \"QSOC_UVM_FAILED\");\nend\nendmodule");
    result.testbenchSource = lines.join('\n') + '\n';
    result.fileList
        = QString("%1.v\n%1_uvm_if.sv\n%1_uvm_pkg.sv\n%1_uvm_tb.sv\n").arg(plan.moduleName);
    return result;
}
