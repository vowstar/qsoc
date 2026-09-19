// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsocprcmformal.h"

namespace {

QString range(quint32 width)
{
    return width == 1 ? QString() : QString("[%1:0] ").arg(width - 1);
}

QString apbContract()
{
    return R"(
wire access = @s_apb_pselx@ && @s_apb_penable@;
wire commit = access && @s_apb_pready@;
wire write = commit && @s_apb_pwrite@ && @s_apb_paddr@ == 0;
wire [D-1:0] data = @s_apb_pwdata@;
wire [D/8-1:0] strobe = @s_apb_pstrb@;
always @(posedge clk) begin
    if (ready) begin
        apb_ready: assert(@s_apb_pready@ == !clear);
        if (history && $past(ready)) begin
            if ($past(@s_apb_pselx@ && (!@s_apb_penable@ || !@s_apb_pready@))) begin
                assume(access);
                assume({@s_apb_paddr@, @s_apb_pwrite@, @s_apb_pwdata@,
                        @s_apb_pstrb@, @s_apb_pprot@} ==
                       $past({@s_apb_paddr@, @s_apb_pwrite@, @s_apb_pwdata@,
                              @s_apb_pstrb@, @s_apb_pprot@}));
            end else assume(!@s_apb_penable@);
        end
        if (commit) begin
            apb_error: assert(@s_apb_pslverr@ == !mapped(@s_apb_paddr@));
            if (!@s_apb_pwrite@ && (@s_apb_paddr@ == 0 || @s_apb_paddr@ == D/8))
                apb_request: assert(@s_apb_prdata@[M-1:0] == request);
        end
        cover(commit && @s_apb_pwrite@ && @s_apb_paddr@ == 0);
        cover(commit && !@s_apb_pwrite@ && @s_apb_paddr@ == 0);
        cover(commit && !mapped(@s_apb_paddr@));
        if (@HAS_MANAGEMENT@)
            cover(access && clear && (!@HAS_RUN@ || seen_run) && seen_management);
    end else assume(!@s_apb_pselx@ && !@s_apb_penable@);
end
)";
}

QString axiContract()
{
    return R"(
reg aw = 0, w = 0, b = 0, r = 0;
reg [A-1:0] address = 0;
reg [D-1:0] saved_data = 0;
reg [D/8-1:0] saved_strobe = 0;
reg [1:0] bresp = 0, rresp = 0;
reg [M-1:0] read_request = 0;
reg compare_request = 0;
wire aw_take = @s_axi_awvalid@ && @s_axi_awready@;
wire w_take = @s_axi_wvalid@ && @s_axi_wready@;
wire ar_take = @s_axi_arvalid@ && @s_axi_arready@;
wire [A-1:0] write_address = aw ? address : @s_axi_awaddr@;
wire [D-1:0] data = w ? saved_data : @s_axi_wdata@;
wire [D/8-1:0] strobe = w ? saved_strobe : @s_axi_wstrb@;
wire commit = ready && !b && (aw || aw_take) && (w || w_take) && !clear;
wire write = commit && write_address == 0;
always @(posedge clk or negedge ready) begin
    if (!ready) begin
        aw <= 0; w <= 0; b <= 0; r <= 0;
        address <= 0; saved_data <= 0; saved_strobe <= 0;
        bresp <= 0; rresp <= 0; read_request <= 0; compare_request <= 0;
    end else begin
        if (b && @s_axi_bready@) b <= 0;
        if (r && @s_axi_rready@) r <= 0;
        if (aw_take) begin aw <= 1; address <= @s_axi_awaddr@; end
        if (w_take) begin w <= 1; saved_data <= @s_axi_wdata@; saved_strobe <= @s_axi_wstrb@; end
        if (commit) begin
            aw <= 0; w <= 0; b <= 1;
            bresp <= mapped(write_address) ? 0 : 2;
        end
        if (ar_take) begin
            r <= 1;
            rresp <= mapped(@s_axi_araddr@) ? 0 : 2;
            read_request <= request;
            compare_request <= @s_axi_araddr@ == 0 || @s_axi_araddr@ == D/8;
        end
    end
end
always @(posedge clk) begin
    if (ready) begin
        aw_ready: assert(@s_axi_awready@ == (!aw && !b));
        w_ready: assert(@s_axi_wready@ == (!w && !b));
        ar_ready: assert(@s_axi_arready@ == (!r && !clear));
        b_valid: assert(@s_axi_bvalid@ == b);
        r_valid: assert(@s_axi_rvalid@ == r);
        if (b) b_error: assert(@s_axi_bresp@ == bresp);
        if (r) begin
            r_error: assert(@s_axi_rresp@ == rresp);
            if (compare_request) r_request: assert(@s_axi_rdata@[M-1:0] == read_request);
        end
        if (history && $past(ready)) begin
            if ($past(@s_axi_awvalid@ && !@s_axi_awready@))
                assume(@s_axi_awvalid@ && {@s_axi_awaddr@, @s_axi_awprot@} == $past({@s_axi_awaddr@, @s_axi_awprot@}));
            if ($past(@s_axi_wvalid@ && !@s_axi_wready@))
                assume(@s_axi_wvalid@ && {@s_axi_wdata@, @s_axi_wstrb@} == $past({@s_axi_wdata@, @s_axi_wstrb@}));
            if ($past(@s_axi_arvalid@ && !@s_axi_arready@))
                assume(@s_axi_arvalid@ && {@s_axi_araddr@, @s_axi_arprot@} == $past({@s_axi_araddr@, @s_axi_arprot@}));
            if ($past(@s_axi_bvalid@ && !@s_axi_bready@))
                b_hold: assert(@s_axi_bvalid@ && @s_axi_bresp@ == $past(@s_axi_bresp@));
            if ($past(@s_axi_rvalid@ && !@s_axi_rready@))
                r_hold: assert(@s_axi_rvalid@ && {@s_axi_rdata@, @s_axi_rresp@} == $past({@s_axi_rdata@, @s_axi_rresp@}));
        end
        cover(b && @s_axi_bready@);
        cover(r && @s_axi_rready@);
        cover(b && bresp == 2);
        cover(r && rresp == 2);
        if (@HAS_MANAGEMENT@ && (!@HAS_RUN@ || seen_run) && seen_management) begin
            cover(aw && !w && clear);
            cover(w && !aw && clear);
            cover(b && !@s_axi_bready@ && clear);
            cover(r && !@s_axi_rready@ && clear);
        end
    end
end
)";
}

} // namespace

QSocMmioFormalCollateral QSocPrcmFormal::generate(
    const QSocPrcmBindingPlan &binding,
    const QSocPrcmCircuit     &circuit,
    const QString             &moduleName,
    int                        sampleStage)
{
    QSocMmioFormalCollateral result;
    const auto              &input    = binding.input;
    const auto              &domain   = *input.domain.cbegin();
    const auto              &resource = binding.domain[input.domain.firstKey()];
    const auto              &supply   = input.supplyTable[domain.supply];
    QMap<QString, QString>   portName;
    QStringList              declaration, connection;
    int                      index = 0;
    for (const auto &port : circuit.port) {
        const auto name     = "port_" + QString::number(index++);
        portName[port.name] = name;
        QString type        = "wire ";
        QString initial;
        if (port.direction == "input") {
            type = "(* anyseq *) reg ";
            if (port.name == input.clockInput) {
                type    = "reg ";
                initial = " = 0";
            } else if (port.name == supply.valid.signal) {
                type    = "reg ";
                initial = " = 0";
            } else if (
                port.name == domain.isolation.completion.signal
                || port.name == domain.quiesce.completion.signal) {
                type    = "reg ";
                initial = " = 1";
            }
        }
        declaration.append(type + range(port.width) + name + initial + ';');
        connection.append("    ." + port.name + "(" + name + ")");
    }
    QMap<QString, QString> token      = portName;
    QString                management = "1'b0";
    for (const auto &target : binding.reset.targets) {
        if (target.name == input.resetTarget)
            management = (target.active == "low" ? "!" : "") + portName[target.name];
    }
    token["CLOCK"]    = portName[input.clockInput];
    token["COLD"]     = portName[input.resetSource];
    token["POWER"]    = portName[supply.request];
    token["PGOOD"]    = portName[supply.valid.signal];
    token["ISOLATE"]  = portName[domain.isolation.request];
    token["ISOLATED"] = portName[domain.isolation.completion.signal];
    token["STOP"]     = portName[domain.quiesce.request];
    token["IDLE"]     = portName[domain.quiesce.completion.signal];
    token["RESET"]    = (resource.resetTargetActiveLow ? "!" : "") + portName[domain.reset.target];
    token["GATED"]    = portName[domain.clock.target];
    token["MANAGEMENT"]     = management;
    token["HAS_MANAGEMENT"] = input.resetTarget.isEmpty() ? "0" : "1";
    token["STAGE"]          = QString::number(sampleStage);
    token["D"]              = QString::number(input.dataWidth);
    token["A"]              = QString::number(input.addressWidth);
    token["M"]              = QString::number(circuit.mmio.registers[0].fields[0].width);
    token["INITIAL"]        = QString("%1'd%2")
                                  .arg(circuit.mmio.registers[0].fields[0].width)
                                  .arg(*circuit.mmio.registers[0].fields[0].resetValue);
    bool    hasRun          = false;
    quint64 offCode         = 0;
    quint64 activeCode      = domain.mode.cbegin()->code;
    const auto writeData = portName[input.bus == QSocMmioBus::Apb4 ? "s_apb_pwdata" : "s_axi_wdata"];
    for (const auto &mode : domain.mode) {
        if (!mode.power)
            offCode = mode.code;
        else if (!hasRun || !mode.reset)
            activeCode = mode.code;
        hasRun |= !mode.reset;
    }
    token["HAS_RUN"]    = hasRun ? "1" : "0";
    token["COVER_MODE"] = QString("%1 == (seen_run ? %2'd%3 : %2'd%4)")
                              .arg(writeData)
                              .arg(input.dataWidth)
                              .arg(offCode)
                              .arg(activeCode);
    token["STROBE"]     = portName[input.bus == QSocMmioBus::Apb4 ? "s_apb_pstrb" : "s_axi_wstrb"];
    QString body        = R"(
localparam D = @D@, A = @A@, M = @M@, STAGE = @STAGE@;
wire clk = @CLOCK@;
always @($global_clock) @CLOCK@ <= !@CLOCK@;
initial assume(!@COLD@);
reg [STAGE-1:0] awake = 0;
always @(posedge clk or negedge @COLD@) begin
    if (!@COLD@) awake <= 0;
    else awake <= {awake[STAGE-2:0], 1'b1};
end
wire ready = awake[STAGE-1];
reg [STAGE-1:0] clear_sample = '1;
always @(posedge clk or negedge ready) begin
    if (!ready) clear_sample <= '1;
    else clear_sample <= {clear_sample[STAGE-2:0], @MANAGEMENT@};
end
wire clear = clear_sample[STAGE-1];
(* anyseq *) reg [3:0] advance;
if (COVER) begin
    reg boot = 0;
    always @(posedge clk) boot <= 1;
    always @* begin
        assume(@COLD@ == boot);
        assume(advance[2:0] == 3'b111);
        assume(@COVER_MODE@);
        assume(@STROBE@ == '1);
    end
end
always @(posedge clk) begin
    if (advance[0]) @PGOOD@ <= @POWER@ && !advance[3];
    if (advance[1]) @ISOLATED@ <= @ISOLATE@;
    if (advance[2]) @IDLE@ <= @STOP@;
end
function mapped(input [A-1:0] address);
    mapped = address == 0 || address == D/8 || address == D/4;
endfunction
reg [M-1:0] request = @INITIAL@;
reg history = 0, seen_run = 0, seen_management = 0, seen_work = 0;
wire reset_active = @RESET@;
always @(posedge clk) begin
    history <= 1;
    if (!ready) begin seen_run <= 0; seen_management <= 0; seen_work <= 0; end
    else begin
        if (!@STOP@) seen_work <= 1;
        if (@POWER@ && @PGOOD@ && !reset_active && !@ISOLATED@ && !@IDLE@)
            seen_run <= 1;
        if (history && $past(ready) && (!@HAS_RUN@ || seen_run) &&
            !$past(@MANAGEMENT@) && @MANAGEMENT@) seen_management <= 1;
        if (history && $past(ready) && $past(@POWER@) && !@POWER@) begin
            power_off: assert(@ISOLATED@ && reset_active && !@GATED@);
            if (seen_work) drain: assert(@IDLE@);
        end
        if (@HAS_RUN@) begin
            cover(seen_run);
            cover(seen_run && @POWER@ && !@PGOOD@);
            cover(seen_run && !@POWER@ && !@PGOOD@ && reset_active && @ISOLATED@ && @IDLE@);
        end
        if (@HAS_MANAGEMENT@) cover((!@HAS_RUN@ || seen_run) && seen_management);
    end
end
)";
    body += input.bus == QSocMmioBus::Apb4 ? apbContract() : axiContract();
    body += R"(
wire [D-1:0] byte_mask;
for (genvar lane = 0; lane < D/8; lane = lane + 1)
    assign byte_mask[8*lane +: 8] = {8{strobe[lane]}};
always @(posedge clk or negedge ready) begin
    if (!ready) request <= @INITIAL@;
    else if (clear) request <= @INITIAL@;
    else if (write) request <= (request & ~byte_mask[M-1:0]) | (data[M-1:0] & byte_mask[M-1:0]);
end
endmodule
)";
    for (auto value = token.cbegin(); value != token.cend(); ++value)
        body.replace('@' + value.key() + '@', value.value());
    const auto top       = moduleName + "_formal";
    result.systemVerilog = "module " + top + " #(parameter COVER = 0);\n" + declaration.join('\n')
                           + '\n' + moduleName + " dut(\n" + connection.join(",\n") + "\n);\n"
                           + body;
    auto files           = circuit.rtl.keys();
    files.append(top + ".sv");
    result.sby = "[tasks]\nprove\ncover\n[options]\nprove: mode prove\ncover: mode cover\n"
                 "depth 160\ntimeout 120\nmulticlock on\nprove: aigsmt z3\n[engines]\n"
                 "prove: abc pdr\ncover: smtbmc z3\n[script]\nread -formal -noautowire "
                 + files.join(' ') + "\ncover: chparam -set COVER 1 " + top + "\nprep -top " + top
                 + " -flatten\ncheck -assert\n[files]\n" + files.join('\n') + '\n';
    return result;
}
