// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoccellformal.h"

#include <QDir>

namespace {

/* One formal step: the global clock under SymbiYosys, fclk elsewhere */
const char *const prelude[] = {
    R"sv(
`ifndef QSOC_FORMAL_STEP
`ifdef YOSYS
`define QSOC_FORMAL_STEP $global_clock
`else
`define QSOC_FORMAL_STEP posedge fclk
`endif
`endif
)sv",
};

const char *const clockHarness[] = {
    R"sv(
/* The divider input clock toggles on the steps chosen by in_tick. Inputs move
 * only on the other steps, so no input change ever races a clock edge. */
module qsoc_clk_div_formal #(
    parameter integer WIDTH              = 3,
    parameter integer DEFAULT_VAL        = 2,
    parameter         CLOCK_DURING_RESET = 1'b0,
    parameter         AUTO_UPDATE        = 1'b0,
    parameter         LIVE               = 1'b1,
    parameter         FREE_CLOCK         = 1'b1
) (
    input wire             fclk,
`ifdef FORMAL_EXTERNAL_RESET
    input wire             formal_reset_ni,
`endif
    input wire             in_tick,
    input wire             in_rst_n,
    input wire             in_en,
    input wire             in_test_en,
    input wire [WIDTH-1:0] in_div,
    input wire             in_div_valid
);
    localparam integer MAXN  = (1 << WIDTH) - 1;
    /* Edges for the output to settle once the inputs hold: with AUTO_UPDATE
     * an earlier value may still load ahead of the last one, and each load
     * waits out a phase of up to 2 * MAXN half periods */
    localparam integer BOUND = 4 * MAXN + 10;
    localparam integer CW    = 8;

    function [WIDTH-1:0] norm;
        input [WIDTH-1:0] value;
        norm = (value == {WIDTH{1'b0}}) ? {{(WIDTH - 1) {1'b0}}, 1'b1} : value;
    endfunction

`ifndef FORMAL_EXTERNAL_RESET
    reg [1:0] formal_boot = 2'b00;
    always @(`QSOC_FORMAL_STEP) if (formal_boot != 2'b11) formal_boot <= formal_boot + 2'b01;
    wire formal_reset_ni = &formal_boot;
`endif

    /* Stimulus */
    reg             clk;
    reg             rst_n;
    reg             en;
    reg             test_en;
    reg [WIDTH-1:0] div;
    reg             div_valid;
    wire            rise = in_tick && !clk;  /* this step raises clk */
    always @(`QSOC_FORMAL_STEP) begin
        if (in_tick) begin
            clk <= ~clk;
        end else begin
            en        <= in_en;
            test_en   <= in_test_en;
            div       <= in_div;
            div_valid <= in_div_valid;
        end
        /* Reset moves away from clock edges; the harness reset forces it */
        if (!formal_reset_ni) rst_n <= 1'b0;
        else if (!in_tick) rst_n <= in_rst_n;
    end

    wire             clk_out;
    wire             div_ready;
    wire [WIDTH-1:0] count;

    qsoc_clk_div #(
        .WIDTH(WIDTH),
        .DEFAULT_VAL(DEFAULT_VAL),
        .CLOCK_DURING_RESET(CLOCK_DURING_RESET),
        .AUTO_UPDATE(AUTO_UPDATE)
    ) dut (
        .clk(clk),
        .rst_n(rst_n),
        .en(en),
        .test_en(test_en),
        .div(div),
        .div_valid(div_valid),
        .div_ready(div_ready),
        .clk_out(clk_out),
        .count(count)
    );

    /* Environment */
    reg             past_valid = 1'b0;
    reg             past_rst_n;
    reg             past_test_en;
    reg             past_en;
    reg [WIDTH-1:0] past_div;
    reg             past_div_valid;
    reg             past_tick;
    always @(`QSOC_FORMAL_STEP) begin
        past_tick      <= in_tick;
        past_valid     <= formal_reset_ni;
        past_rst_n     <= rst_n;
        past_test_en   <= test_en;
        past_en        <= en;
        past_div       <= div;
        past_div_valid <= div_valid;
    end

    /* A request seen on a rising edge and not yet served */
    reg pending;
    always @(`QSOC_FORMAL_STEP) begin
        if (!rst_n) pending <= 1'b0;
        else if (rise) pending <= div_valid && !div_ready;
    end

    always @(`QSOC_FORMAL_STEP) begin
        if (past_valid) begin
            /* Scan mode is static */
            assume (test_en == past_test_en);
            /* Unless the clock is free, it toggles on every other step, and
             * inputs move on the steps between */
            if (!FREE_CLOCK) assume (in_tick != past_tick);
            /* Synchronous inputs are launched while clk is high */
            if (!clk) begin
                assume (en == past_en);
                if (!AUTO_UPDATE) begin
                    assume (div == past_div);
                    assume (div_valid == past_div_valid);
                end
            end
            /* A request holds its value until the edge that serves it */
            if (!AUTO_UPDATE && pending && rst_n) begin
                assume (div_valid);
                assume (div == past_div);
            end
        end
    end

    /* Reference: the ratio in use, updated by each load reported on div_ready */
    reg  [WIDTH-1:0] sync1, sync2, n_cur;
    wire [WIDTH-1:0] src = AUTO_UPDATE ? sync2 : div;
    wire             loading = rise && div_ready && norm(src) != n_cur;

    always @(`QSOC_FORMAL_STEP) begin
        if (!rst_n) begin
            sync1 <= DEFAULT_VAL;
            sync2 <= DEFAULT_VAL;
            n_cur <= norm(DEFAULT_VAL);
        end else if (rise) begin
            sync1 <= div;
            sync2 <= sync1;
            if (div_ready) n_cur <= norm(src);
        end
    end

    /* Output phases, measured in half periods of clk */
    reg             past_out;
    reg             phase_ok;
    reg             steady;
    reg [CW-1:0]    len;
    reg [WIDTH-1:0] n_start;
    wire            edge_out = clk_out != past_out;
    always @(`QSOC_FORMAL_STEP) past_out <= clk_out;

    always @(`QSOC_FORMAL_STEP) begin
        if (!rst_n) begin
            phase_ok <= 1'b0;
            len      <= 0;
        end else if (edge_out) begin
            phase_ok <= 1'b1;
            steady   <= !loading;
            len      <= in_tick ? 1 : 0;
            n_start  <= n_cur;
        end else begin
            if (in_tick && len != {CW{1'b1}}) len <= len + 1'b1;
            if (loading) steady <= 1'b0;
        end
    end

    /* Rising edges since the inputs last moved, for bounded liveness */
    reg [CW-1:0] stable;
    wire         settled = en && (AUTO_UPDATE ? div == past_div : !div_valid);
    always @(`QSOC_FORMAL_STEP) begin
        if (!rst_n || !past_valid || !settled) stable <= 0;
        else if (rise && stable != {CW{1'b1}}) stable <= stable + 1'b1;
    end
    wire quiet = settled && stable > BOUND;

    /* Rising edges with the output disabled */
    reg [CW-1:0] off;
    always @(`QSOC_FORMAL_STEP) begin
        if (!rst_n || en) off <= 0;
        else if (rise && off != {CW{1'b1}}) off <= off + 1'b1;
    end

    /* Rising edges a held request has waited */
    reg [CW-1:0] waiting;
    always @(`QSOC_FORMAL_STEP) begin
        if (!rst_n || !div_valid || !en || (rise && div_ready)) waiting <= 0;
        else if (rise && waiting != {CW{1'b1}}) waiting <= waiting + 1'b1;
    end

    always @(`QSOC_FORMAL_STEP) begin
        if (past_valid && past_rst_n && rst_n && !test_en) begin
            if (edge_out && phase_ok) begin
                /* No phase is shorter than the old or new ratio: no runt pulse */
                min_width: assert (len >= ((n_start < n_cur) ? n_start : n_cur));
                /* A high phase with no load inside lasts exactly its ratio */
                if (!clk_out && steady) hi_exact: assert (len == n_start);
                /* A low phase in steady state lasts exactly the ratio */
                if (LIVE && clk_out && quiet && stable > BOUND + 2 * MAXN + 2)
                    lo_exact: assert (len == n_cur);
            end
            count_range: assert (count < n_cur || count == 0);
            if (!AUTO_UPDATE && loading) load_req: assert (div_valid);
            if (LIVE && quiet) begin
                alive: assert (len <= 2 * MAXN + 2);
                if (AUTO_UPDATE) taken: assert (n_cur == norm(div));
            end
            if (LIVE && !en && off > 4 * MAXN + 4) stopped: assert (!clk_out);
            if (LIVE && !AUTO_UPDATE) served: assert (waiting <= BOUND);
        end
        if (past_valid && test_en) scan: assert (clk_out == clk);
        if (past_valid && !rst_n && !test_en && !CLOCK_DURING_RESET) reset_low: assert (!clk_out);
        if (past_valid && !rst_n && CLOCK_DURING_RESET && DEFAULT_VAL < 2) reset_clock: assert (clk_out == clk);
    end

    always @(`QSOC_FORMAL_STEP) begin
        if (past_valid && rst_n && !test_en) begin
            reload: cover (loading && n_cur != norm(DEFAULT_VAL));
            odd_high: cover (edge_out && !clk_out && phase_ok && len == 3);
        end
    end
endmodule
)sv",
    R"sv(
/* Each input clock toggles on its own in_tick bit. Select, reset and scan
 * inputs move only on steps where no clock toggles. Every high phase of the
 * output must be one whole high phase of one input, and every low phase must
 * contain a whole low phase of the input that ends it. With FAIR, clocks never
 * stall for more than P steps and a stable select is eventually followed. */
module qsoc_clk_mux_gf_formal #(
    parameter integer N                  = 2,
    parameter integer STAGES             = 2,
    parameter         CLOCK_DURING_RESET = 1'b0,
    parameter         FAIR               = 1'b0
) (
    input wire         fclk,
`ifdef FORMAL_EXTERNAL_RESET
    input wire         formal_reset_ni,
`endif
    input wire [N-1:0] in_tick,
    input wire         in_test_tick,
    input wire [5:0]   in_sel,
    input wire         in_rst_n,
    input wire         in_test_en
);
    localparam integer W  = (N <= 2) ? 1 : (N <= 4) ? 2 : (N <= 8) ? 3 : 4;
    localparam integer P  = 3;
    localparam integer T  = 16 * (STAGES + 4) * (P + 1);
    localparam integer CW = 10;
    /* Rising edges of every input a switch may take before the next one */
    localparam integer M  = 2 * STAGES + 8;

`ifndef FORMAL_EXTERNAL_RESET
    reg [1:0] formal_boot = 2'b00;
    always @(`QSOC_FORMAL_STEP) if (formal_boot != 2'b11) formal_boot <= formal_boot + 2'b01;
    wire formal_reset_ni = &formal_boot;
`endif

    reg [N-1:0] clk;
    reg         test_clk;
    reg [W-1:0] sel;
    reg         rst_n;
    reg         test_en;
    wire        still = !(|in_tick) && !in_test_tick;
    always @(`QSOC_FORMAL_STEP) begin
        clk      <= clk ^ in_tick;
        test_clk <= test_clk ^ in_test_tick;
        if (still) begin
            sel     <= in_sel[W-1:0];
            test_en <= in_test_en;
        end
        if (!formal_reset_ni) rst_n <= 1'b0;
        else if (still) rst_n <= in_rst_n;
    end

    wire clk_out;
    qsoc_clk_mux_gf #(
        .NUM_INPUTS(N),
        .NUM_SYNC_STAGES(STAGES),
        .CLOCK_DURING_RESET(CLOCK_DURING_RESET)
    ) dut (
        .clk_in(clk),
        .test_clk(test_clk),
        .test_en(test_en),
        .async_rst_n(rst_n),
        .async_sel(sel),
        .clk_out(clk_out)
    );

    reg         past_valid = 1'b0;
    reg [N-1:0] past_clk;
    reg         past_out;
    reg         past_test_en;
    always @(`QSOC_FORMAL_STEP) begin
        past_valid   <= formal_reset_ni;
        past_clk     <= clk;
        past_out     <= clk_out;
        past_test_en <= test_en;
    end
    wire [15:0]  padded   = {{(16 - N) {1'b0}}, clk};
    wire [N-1:0] rose     = clk & ~past_clk;
    wire [N-1:0] fell     = ~clk & past_clk;
    wire         out_rise = clk_out && !past_out;
    wire         out_fall = !clk_out && past_out;

    /* Rising edges of each input since select or reset last moved */
    reg [4:0]   since [0:N-1];
    reg [W-1:0] past_sel;
    reg         past_rst_n;
    reg         done;
    integer     j;
    always @(`QSOC_FORMAL_STEP) begin
        past_sel   <= sel;
        past_rst_n <= rst_n;
        for (j = 0; j < N; j = j + 1) begin
            if (!formal_reset_ni || sel != past_sel || rst_n != past_rst_n) since[j] <= 0;
            else if (rose[j] && since[j] != 5'h1f) since[j] <= since[j] + 1'b1;
        end
    end
    always @* begin
        done = 1'b1;
        for (j = 0; j < N; j = j + 1) if (since[j] < M) done = 1'b0;
    end

    /* The output has produced a pulse of the selected clock since select or
     * reset last moved */
    reg switched_in;
    always @(`QSOC_FORMAL_STEP) begin
        if (!formal_reset_ni || sel != past_sel || rst_n != past_rst_n) switched_in <= 1'b0;
        else if (out_rise && rose == ({{(N - 1) {1'b0}}, 1'b1} << sel)) switched_in <= 1'b1;
    end

    /* Scan mode is static; with FAIR no clock stalls longer than P steps */
    reg [3:0] idle [0:N-1];
    integer   k;
    always @(`QSOC_FORMAL_STEP) begin
        for (k = 0; k < N; k = k + 1)
            idle[k] <= (!formal_reset_ni || in_tick[k]) ? 4'd0 : idle[k] + 4'd1;
        if (past_valid) begin
            assume (test_en == past_test_en);
            /* Select names an input, and moves only once the output has
             * shown the selected clock, i.e. the last switch is complete */
            assume (sel < N);
            if (!switched_in) assume (sel == past_sel);
            /* With the reset bypass, select is fixed in reset and just after it */
            if (CLOCK_DURING_RESET && (!rst_n || !past_rst_n || !done)) assume (sel == past_sel);
            /* and reset asserts only once the last switch has completed */
            if (CLOCK_DURING_RESET && past_rst_n && !switched_in) assume (rst_n);
            /* Reset is held for M rising edges of every input */
            if (!done && !past_rst_n) assume (!rst_n);
            if (FAIR) for (k = 0; k < N; k = k + 1) assume (idle[k] < P);
        end
    end

    /* Every input has had a low phase since the harness reset, so each gate
     * latch holds a value it was given */
    reg [N-1:0] known;
    always @(`QSOC_FORMAL_STEP) begin
        if (!formal_reset_ni) known <= 0;
        else known <= known | ~clk;
    end
    wire live = past_valid && &known && !test_en;

    /* Candidate sources of the current high phase, and inputs that fell during
     * the current low phase */
    reg [N-1:0] src;
    reg [N-1:0] fell_low;
    reg         phase_ok;
    reg         hi_ok;
    always @(`QSOC_FORMAL_STEP) begin
        if (!live) begin
            phase_ok <= 1'b0;
            hi_ok    <= 1'b0;
        end else if (out_rise) begin
            hi_ok <= 1'b1;
            src   <= rose;
        end else if (out_fall) begin
            phase_ok <= 1'b1;
            fell_low <= fell;
        end else begin
            src      <= src & clk;
            fell_low <= fell_low | fell;
        end
    end

    /* Steps with a fixed, valid select and reset released */
    reg [CW-1:0] held;
    always @(`QSOC_FORMAL_STEP) begin
        if (!live || !rst_n || sel != past_sel || sel >= N) held <= 0;
        else if (held != {CW{1'b1}}) held <= held + 1'b1;
    end

    always @(`QSOC_FORMAL_STEP) begin
        if (past_valid && test_en) scan: assert (clk_out == test_clk);
        if (live && past_out && hi_ok) begin
            if (!out_fall) hi_hold: assert ((src & clk) != 0);
            if (out_fall) hi_end: assert ((src & fell) != 0);
        end
        if (live && out_rise) begin
            hi_start: assert (rose != 0);
            if (phase_ok) lo_whole: assert ((rose & fell_low) != 0);
        end
        if (live && !CLOCK_DURING_RESET && !rst_n && out_rise) reset_quiet: assert (0);
        if (FAIR && live && rst_n && sel == past_sel && held > T) follow: assert (clk_out == padded[sel]);
        if (live && phase_ok && rst_n) switched: cover (out_rise && sel != 0 && rose[sel]);
    end

    generate
        if (CLOCK_DURING_RESET) begin : g_reset_clock
            always @(`QSOC_FORMAL_STEP) if (live && !rst_n) reset_clock: cover (out_rise);
        end
    endgenerate
endmodule

/* Two-input cells and the buffer against their Boolean functions */
module qsoc_tc_clk_formal (
    input wire a,
    input wire b,
    input wire s
);
    wire buf_o, inv_o, or_o, mux_o, xor_o;
    qsoc_tc_clk_buf  u_buf (.clk(a), .clk_out(buf_o));
    qsoc_tc_clk_inv  u_inv (.CLK_IN(a), .CLK_OUT(inv_o));
    qsoc_tc_clk_or2  u_or  (.CLK_IN0(a), .CLK_IN1(b), .CLK_OUT(or_o));
    qsoc_tc_clk_mux2 u_mux (.CLK_IN0(a), .CLK_IN1(b), .CLK_SEL(s), .CLK_OUT(mux_o));
    qsoc_tc_clk_xor2 u_xor (.CLK_IN0(a), .CLK_IN1(b), .CLK_OUT(xor_o));
    always @* begin
        buf_ok: assert (buf_o == a);
        inv_ok: assert (inv_o == !a);
        or_ok:  assert (or_o == (a | b));
        mux_ok: assert (mux_o == (s ? b : a));
        xor_ok: assert (xor_o == (a ^ b));
    end
endmodule

/* OR tree against the reduction OR */
module qsoc_clk_or_tree_formal #(
    parameter integer N = 3
) (
    input wire [N-1:0] clk_in
);
    wire clk_out;
    qsoc_clk_or_tree #(.INPUT_COUNT(N)) dut (.clk_in(clk_in), .clk_out(clk_out));
    always @* or_tree: assert (clk_out == |clk_in);
endmodule

/* Raw mux: every in-range select picks its own input */
module qsoc_clk_mux_raw_formal #(
    parameter integer N = 3
) (
    input wire [N-1:0] clk_in,
    input wire [5:0]   sel
);
    localparam integer W = (N <= 2) ? 1 : (N <= 4) ? 2 : (N <= 8) ? 3 : (N <= 16) ? 4 : 5;
    wire clk_out;
    qsoc_clk_mux_raw #(.NUM_INPUTS(N)) dut (.clk_in(clk_in), .clk_sel(sel[W-1:0]), .clk_out(clk_out));
    wire [63:0] padded = {{(64 - N) {1'b0}}, clk_in};
    always @* if (sel < N) mux_raw: assert (clk_out == padded[sel]);
endmodule

/* Clock gate: output follows clk while the enable latched at the edge that
 * opens the phase is set, never changes without a clk edge, and is bypassed
 * in scan mode and, with CLOCK_DURING_RESET, in reset. */
module qsoc_tc_clk_gate_formal #(
    parameter CLOCK_DURING_RESET = 1'b0,
    parameter POLARITY           = 1'b1
) (
    input wire fclk,
`ifdef FORMAL_EXTERNAL_RESET
    input wire formal_reset_ni,
`endif
    input wire in_tick,
    input wire in_en,
    input wire in_test_en,
    input wire in_rst_n
);
`ifndef FORMAL_EXTERNAL_RESET
    reg [1:0] formal_boot = 2'b00;
    always @(`QSOC_FORMAL_STEP) if (formal_boot != 2'b11) formal_boot <= formal_boot + 2'b01;
    wire formal_reset_ni = &formal_boot;
`endif

    reg clk, en, test_en, rst_n;
    always @(`QSOC_FORMAL_STEP) begin
        if (in_tick) clk <= ~clk;
        else begin
            en      <= in_en;
            test_en <= in_test_en;
            rst_n   <= in_rst_n;
        end
    end

    wire clk_out;
    qsoc_tc_clk_gate #(
        .CLOCK_DURING_RESET(CLOCK_DURING_RESET),
        .POLARITY(POLARITY)
    ) dut (
        .clk(clk),
        .en(en),
        .test_en(test_en),
        .rst_n(rst_n),
        .clk_out(clk_out)
    );

    wire bypass = test_en | (!rst_n & CLOCK_DURING_RESET);
    wire open   = en | test_en;
    /* Reference latch: transparent while clk is in its idle level */
    wire idle = POLARITY ? !clk : clk;
    reg  held;
    reg  known;
    always @(`QSOC_FORMAL_STEP) begin
        if (!formal_reset_ni) known <= 1'b0;
        else if (idle) known <= 1'b1;
        if (idle) held <= open;
    end
    wire latched = idle ? open : held;
    wire gated   = POLARITY ? (clk & latched) : (clk | !latched);

    reg past_valid = 1'b0;
    reg past_clk, past_bypass, past_out;
    always @(`QSOC_FORMAL_STEP) begin
        past_valid  <= formal_reset_ni;
        past_clk    <= clk;
        past_bypass <= bypass;
        past_out    <= clk_out;
    end

    always @(`QSOC_FORMAL_STEP) begin
        if (past_valid && known) begin
            gate_ok: assert (clk_out == (bypass ? clk : gated));
            /* Glitch free: without a clk edge or a bypass change the output holds */
            if (clk == past_clk && bypass == past_bypass) hold: assert (clk_out == past_out);
            blocked: cover (!bypass && clk && !clk_out);
        end
    end
endmodule
)sv",
};

/* Release counter shared by the reset synchronizers, filled per cell */
const char *const resetHarness[] = {
    R"sv(
/* Reset release counter: the output releases on exactly the COUNT-th rising
 * edge of clk after the input releases. ASYNC cells assert at once, others on
 * the next rising edge. In scan mode the output follows the input, or is held
 * released when SCAN_RELEASE is set. */
module @MODULE@_formal #(
    parameter integer COUNT = 3
) (
    input wire fclk,
`ifdef FORMAL_EXTERNAL_RESET
    input wire formal_reset_ni,
`endif
    input wire in_tick,
    input wire in_rst_n,
    input wire in_test
);
    localparam ASYNC        = @ASYNC@;
    localparam SCAN_RELEASE = @SCAN@;
    localparam integer CW   = 8;

`ifndef FORMAL_EXTERNAL_RESET
    reg [1:0] formal_boot = 2'b00;
    always @(`QSOC_FORMAL_STEP) if (formal_boot != 2'b11) formal_boot <= formal_boot + 2'b01;
    wire formal_reset_ni = &formal_boot;
`endif

    reg  clk;
    reg  rst_n;
    reg  test;
    wire rise = in_tick && !clk;
    always @(`QSOC_FORMAL_STEP) begin
        if (in_tick) clk <= ~clk;
        else test <= in_test;
        /* Reset moves away from clock edges; the harness reset forces it */
        if (!formal_reset_ni) rst_n <= 1'b0;
        else if (!in_tick) rst_n <= in_rst_n;
    end

    wire out_n;
    @MODULE@ #(.@PARAM@(COUNT)) dut (
        .@CLK@(clk),
        .@IN@(rst_n),
        .@TEST@(test),
        .@OUT@(out_n)
    );

    reg past_valid = 1'b0;
    reg past_test;
    always @(`QSOC_FORMAL_STEP) begin
        past_valid <= formal_reset_ni;
        past_test  <= test;
    end
    always @(`QSOC_FORMAL_STEP) if (past_valid) assume (test == past_test);

    /* Rising edges seen with the input released */
    reg [CW-1:0] seen;
    reg          known;
    always @(`QSOC_FORMAL_STEP) begin
        if (!formal_reset_ni) begin
            seen  <= 0;
            known <= ASYNC;
        end else if (!rst_n && ASYNC) begin
            seen  <= 0;
            known <= 1'b1;
        end else if (rise) begin
            if (!rst_n) seen <= 0;
            else if (seen != {CW{1'b1}}) seen <= seen + 1'b1;
            if (!rst_n) known <= 1'b1;
        end
    end

    always @(`QSOC_FORMAL_STEP) begin
        if (past_valid && !test && known) release_exact: assert (out_n == ((rst_n || !ASYNC) && seen >= COUNT));
        if (past_valid && !test && ASYNC && !rst_n) assert_async: assert (!out_n);
        if (past_valid && test) scan: assert (out_n == (SCAN_RELEASE ? 1'b1 : rst_n));
        if (past_valid && !test && known) released: cover (out_n && seen == COUNT);
    end
endmodule
)sv",
};

const char *const powerHarness[] = {
    R"sv(
/* One formal step is half a clk period; inputs move just after rising edges.
 * With HEALTHY, dependencies are met and pgood follows the switch within a
 * delay shorter than either settle window, so no fault may ever be raised. */
module qsoc_power_fsm_formal #(
    parameter integer HAS_SWITCH = 1,
    parameter integer WAIT_DEP   = 2,
    parameter integer SETTLE_ON  = 3,
    parameter integer SETTLE_OFF = 3,
    parameter         HEALTHY    = 1'b1
) (
    input wire fclk,
`ifdef FORMAL_EXTERNAL_RESET
    input wire formal_reset_ni,
`endif
    input wire in_rst_n,
    input wire in_test_en,
    input wire in_ctrl_enable,
    input wire in_fault_clear,
    input wire in_dep_hard_all,
    input wire in_dep_soft_all,
    input wire in_pgood,
    input wire [2:0] in_delay
);
    localparam integer K  = 4 * (WAIT_DEP + SETTLE_ON + SETTLE_OFF) + 16;
    localparam integer CW = 8;

`ifndef FORMAL_EXTERNAL_RESET
    reg [1:0] formal_boot = 2'b00;
    always @(`QSOC_FORMAL_STEP) if (formal_boot != 2'b11) formal_boot <= formal_boot + 2'b01;
    wire formal_reset_ni = &formal_boot;
`endif

    reg  clk;
    reg  rst_n, test_en, ctrl_enable, fault_clear, dep_hard_all, dep_soft_all, pgood;
    wire rise = !clk;
    always @(`QSOC_FORMAL_STEP) begin
        clk <= ~clk;
        if (clk) begin
            test_en      <= in_test_en;
            ctrl_enable  <= in_ctrl_enable;
            fault_clear  <= in_fault_clear;
            dep_hard_all <= in_dep_hard_all;
            dep_soft_all <= in_dep_soft_all;
            pgood        <= in_pgood;
        end
        if (!formal_reset_ni) rst_n <= 1'b0;
        else if (clk) rst_n <= in_rst_n;
    end

    wire clk_enable, rst_gate_n, pwr_switch, ready, valid, fault;
    qsoc_power_fsm #(
        .HAS_SWITCH(HAS_SWITCH),
        .WAIT_DEP_CYCLES(WAIT_DEP),
        .SETTLE_ON_CYCLES(SETTLE_ON),
        .SETTLE_OFF_CYCLES(SETTLE_OFF)
    ) dut (
        .clk(clk),
        .rst_n(rst_n),
        .test_en(test_en),
        .ctrl_enable(ctrl_enable),
        .fault_clear(fault_clear),
        .dep_hard_all(dep_hard_all),
        .dep_soft_all(dep_soft_all),
        .pgood(pgood),
        .clk_enable(clk_enable),
        .rst_gate_n(rst_gate_n),
        .pwr_switch(pwr_switch),
        .ready(ready),
        .valid(valid),
        .fault(fault)
    );

    /* Values at the previous rising edge */
    reg past_valid = 1'b0;
    reg p_clk_enable, p_rst_gate_n, p_pwr_switch, p_fault, p_fault_clear, p_test_en;
    reg p_rst_n, p_ctrl;
    always @(`QSOC_FORMAL_STEP) begin
        if (!formal_reset_ni) past_valid <= 1'b0;
        else if (rise) past_valid <= 1'b1;
        if (rise) begin
            p_clk_enable  <= clk_enable;
            p_rst_gate_n  <= rst_gate_n;
            p_pwr_switch  <= pwr_switch;
            p_fault       <= fault;
            p_fault_clear <= fault_clear;
            p_test_en     <= test_en;
            p_rst_n       <= rst_n;
            p_ctrl        <= ctrl_enable;
        end
    end

    /* Healthy supply: pgood is the switch delayed by a fixed number of cycles */
    localparam integer SETTLE_MIN = (SETTLE_ON < SETTLE_OFF) ? SETTLE_ON : SETTLE_OFF;
    reg [7:0] history;
    reg [2:0] delay;
    always @(`QSOC_FORMAL_STEP) begin
        if (!formal_reset_ni) delay <= in_delay;
        if (!rst_n) begin
            history <= 0;
        end else if (rise) begin
            history <= {history[6:0], pwr_switch};
        end
    end

    always @(`QSOC_FORMAL_STEP) begin
        if (formal_reset_ni) begin
            if (HEALTHY) begin
                assume (!in_test_en && in_dep_hard_all && in_dep_soft_all && in_rst_n);
                /* A domain without a switch is always on */
                if (!HAS_SWITCH) assume (in_ctrl_enable);
                /* pgood lands delay + 2 edges after the switch moves */
                assume (delay + 2 <= SETTLE_MIN);
                assume (in_pgood == (HAS_SWITCH ? history[delay] : 1'b1));
            end
        end
    end

    /* Cycles ctrl_enable has held its value */
    reg [CW-1:0] held;
    always @(`QSOC_FORMAL_STEP) begin
        if (!rst_n || !past_valid) held <= 0;
        else if (rise) held <= (ctrl_enable == p_ctrl && held != {CW{1'b1}}) ? held + 1'b1 : 0;
    end

    always @(`QSOC_FORMAL_STEP) begin
        if (past_valid && rst_n && !test_en) begin
            rst_needs_clk: assert (!rst_gate_n || clk_enable);
            if (HAS_SWITCH) clk_needs_power: assert (!clk_enable || pwr_switch);
            ready_means_on: assert (!ready || (rst_gate_n && clk_enable && valid));
        end
        if (past_valid && rise && rst_n && p_rst_n && !test_en && !p_test_en) begin
            if (rst_gate_n && !p_rst_gate_n) clk_before_release: assert (p_clk_enable);
            if (!clk_enable && p_clk_enable) reset_before_gate: assert (!p_rst_gate_n);
            if (!fault && p_fault) fault_sticky: assert (p_fault_clear);
        end
        if (past_valid && rst_n && test_en)
            scan_on: assert (clk_enable && rst_gate_n && ready && valid && (pwr_switch || !HAS_SWITCH));
        if (HEALTHY && past_valid) begin
            no_fault: assert (!fault);
            if (held > K && ctrl_enable && p_ctrl) powered_up: assert (ready);
            if (held > K && !ctrl_enable && !p_ctrl) powered_down: assert (!ready && !clk_enable && !pwr_switch);
        end
        if (past_valid && rst_n && !test_en) begin
            up: cover (ready);
            down_again: cover (!pwr_switch && p_pwr_switch && !fault);
        end
    end
endmodule
)sv",
};

template<size_t N>
QString join(const char *const (&parts)[N])
{
    QString text;
    for (const char *part : parts)
        text += QString::fromUtf8(part);
    return text;
}

struct ResetCell
{
    const char *module;
    const char *parameter;
    const char *clock;
    const char *input;
    const char *test;
    const char *output;
    bool        async;
    bool        scanRelease;
};

QString resetHarnessFor(const ResetCell &cell)
{
    return join(resetHarness)
        .replace("@MODULE@", cell.module)
        .replace("@PARAM@", cell.parameter)
        .replace("@CLK@", cell.clock)
        .replace("@IN@", cell.input)
        .replace("@TEST@", cell.test)
        .replace("@OUT@", cell.output)
        .replace("@ASYNC@", cell.async ? "1'b1" : "1'b0")
        .replace("@SCAN@", cell.scanRelease ? "1'b1" : "1'b0");
}

struct Task
{
    const char *name;
    const char *cell;
    bool        cover;
    const char *top;
    const char *parameters;
};

/* Divider safety runs at WIDTH 3, the narrowest width where a truncated
 * phase can be shorter than both ratios, with a clock that may stall. Its
 * counter based liveness counts edges, so it runs at WIDTH 2 with a clock
 * on every other step to stay within a CI budget. Each setup has a cover
 * task, so an environment that cannot move shows up as unreached, and each
 * cover task has a prove task of the same parameters, since cover mode checks
 * its asserts only up to the depth where the covers are reached. */
const Task taskTable[] = {
    {"clk_div_explicit",
     "qsoc_cell_clock.v",
     false,
     "qsoc_clk_div_formal",
     "WIDTH 3 AUTO_UPDATE 0 LIVE 0"},
    {"clk_div_auto",
     "qsoc_cell_clock.v",
     false,
     "qsoc_clk_div_formal",
     "WIDTH 3 AUTO_UPDATE 1 LIVE 0"},
    {"clk_div_explicit_live",
     "qsoc_cell_clock.v",
     false,
     "qsoc_clk_div_formal",
     "WIDTH 2 AUTO_UPDATE 0 FREE_CLOCK 0"},
    {"clk_div_auto_live",
     "qsoc_cell_clock.v",
     false,
     "qsoc_clk_div_formal",
     "WIDTH 2 AUTO_UPDATE 1 FREE_CLOCK 0"},
    {"clk_div_reset_clock",
     "qsoc_cell_clock.v",
     false,
     "qsoc_clk_div_formal",
     "WIDTH 2 DEFAULT_VAL 1 CLOCK_DURING_RESET 1 LIVE 0"},
    {"clk_div_cover",
     "qsoc_cell_clock.v",
     true,
     "qsoc_clk_div_formal",
     "WIDTH 3 AUTO_UPDATE 1 LIVE 0"},
    {"clk_div_live_cover",
     "qsoc_cell_clock.v",
     true,
     "qsoc_clk_div_formal",
     "WIDTH 2 AUTO_UPDATE 1 FREE_CLOCK 0"},
    {"clk_mux_gf", "qsoc_cell_clock.v", false, "qsoc_clk_mux_gf_formal", "N 2"},
    {"clk_mux_gf_one_stage", "qsoc_cell_clock.v", false, "qsoc_clk_mux_gf_formal", "N 2 STAGES 1"},
    {"clk_mux_gf_three", "qsoc_cell_clock.v", false, "qsoc_clk_mux_gf_formal", "N 3"},
    {"clk_mux_gf_reset_clock",
     "qsoc_cell_clock.v",
     false,
     "qsoc_clk_mux_gf_formal",
     "N 2 CLOCK_DURING_RESET 1"},
    {"clk_mux_gf_live", "qsoc_cell_clock.v", false, "qsoc_clk_mux_gf_formal", "N 2 FAIR 1"},
    {"clk_mux_gf_cover", "qsoc_cell_clock.v", true, "qsoc_clk_mux_gf_formal", "N 2"},
    {"clk_mux_gf_live_cover", "qsoc_cell_clock.v", true, "qsoc_clk_mux_gf_formal", "N 2 FAIR 1"},
    {"clk_mux_gf_reset_clock_cover",
     "qsoc_cell_clock.v",
     true,
     "qsoc_clk_mux_gf_formal",
     "N 2 CLOCK_DURING_RESET 1"},
    {"clk_mux_raw_3", "qsoc_cell_clock.v", false, "qsoc_clk_mux_raw_formal", "N 3"},
    {"clk_mux_raw_5", "qsoc_cell_clock.v", false, "qsoc_clk_mux_raw_formal", "N 5"},
    {"clk_mux_raw_8", "qsoc_cell_clock.v", false, "qsoc_clk_mux_raw_formal", "N 8"},
    {"clk_or_tree_5", "qsoc_cell_clock.v", false, "qsoc_clk_or_tree_formal", "N 5"},
    {"clk_tc", "qsoc_cell_clock.v", false, "qsoc_tc_clk_formal", ""},
    {"clk_gate_pos", "qsoc_cell_clock.v", false, "qsoc_tc_clk_gate_formal", "POLARITY 1"},
    {"clk_gate_neg", "qsoc_cell_clock.v", false, "qsoc_tc_clk_gate_formal", "POLARITY 0"},
    {"clk_gate_pos_reset_clock",
     "qsoc_cell_clock.v",
     false,
     "qsoc_tc_clk_gate_formal",
     "POLARITY 1 CLOCK_DURING_RESET 1"},
    {"clk_gate_neg_reset_clock",
     "qsoc_cell_clock.v",
     false,
     "qsoc_tc_clk_gate_formal",
     "POLARITY 0 CLOCK_DURING_RESET 1"},
    {"clk_gate_cover", "qsoc_cell_clock.v", true, "qsoc_tc_clk_gate_formal", "POLARITY 1"},
    {"rst_sync_1", "qsoc_cell_reset.v", false, "qsoc_rst_sync_formal", "COUNT 1"},
    {"rst_sync_3", "qsoc_cell_reset.v", false, "qsoc_rst_sync_formal", "COUNT 3"},
    {"rst_pipe_1", "qsoc_cell_reset.v", false, "qsoc_rst_pipe_formal", "COUNT 1"},
    {"rst_pipe_3", "qsoc_cell_reset.v", false, "qsoc_rst_pipe_formal", "COUNT 3"},
    {"rst_count_1", "qsoc_cell_reset.v", false, "qsoc_rst_count_formal", "COUNT 1"},
    {"rst_count_3", "qsoc_cell_reset.v", false, "qsoc_rst_count_formal", "COUNT 3"},
    {"rst_count_5", "qsoc_cell_reset.v", false, "qsoc_rst_count_formal", "COUNT 5"},
    {"rst_cover", "qsoc_cell_reset.v", true, "qsoc_rst_count_formal", "COUNT 3"},
    {"power_fsm", "qsoc_cell_power.v", false, "qsoc_power_fsm_formal", ""},
    {"power_fsm_always_on", "qsoc_cell_power.v", false, "qsoc_power_fsm_formal", "HAS_SWITCH 0"},
    {"power_fsm_any", "qsoc_cell_power.v", false, "qsoc_power_fsm_formal", "HEALTHY 0"},
    {"power_fsm_cover", "qsoc_cell_power.v", true, "qsoc_power_fsm_formal", ""},
    {"power_rst_sync_1", "qsoc_cell_power.v", false, "qsoc_power_rst_sync_formal", "COUNT 1"},
    {"power_rst_sync_3", "qsoc_cell_power.v", false, "qsoc_power_rst_sync_formal", "COUNT 3"},
};

const char *const cellOrder[] = {"qsoc_cell_clock.v", "qsoc_cell_reset.v", "qsoc_cell_power.v"};

QString harnessFor(const QString &cell)
{
    if (cell == "qsoc_cell_clock.v")
        return join(clockHarness);
    if (cell == "qsoc_cell_reset.v") {
        return resetHarnessFor(
                   {"qsoc_rst_sync",
                    "STAGE",
                    "clk",
                    "rst_in_n",
                    "test_enable",
                    "rst_out_n",
                    true,
                    false})
               + "\n"
               + resetHarnessFor(
                   {"qsoc_rst_pipe",
                    "STAGE",
                    "clk",
                    "rst_in_n",
                    "test_enable",
                    "rst_out_n",
                    false,
                    false})
               + "\n"
               + resetHarnessFor(
                   {"qsoc_rst_count",
                    "CYCLE",
                    "clk",
                    "rst_in_n",
                    "test_enable",
                    "rst_out_n",
                    true,
                    false});
    }
    return join(powerHarness) + "\n"
           + resetHarnessFor(
               {"qsoc_power_rst_sync",
                "STAGE",
                "clk_dom",
                "rst_gate_n",
                "test_en",
                "rst_dom_n",
                true,
                true});
}

QStringList knownCells(const QStringList &cellFiles)
{
    QStringList cells;
    for (const char *cell : cellOrder) {
        if (cellFiles.contains(cell))
            cells.append(cell);
    }
    return cells;
}

QString formalName(const QString &cell)
{
    return QString(cell).replace(".v", "_formal.sv");
}

} // namespace

QStringList QSocCellFormal::tasks(const QStringList &cellFiles)
{
    const QStringList cells = knownCells(cellFiles);
    QStringList       names;
    for (const Task &task : taskTable) {
        if (cells.contains(task.cell))
            names.append(task.name);
    }
    return names;
}

QMap<QString, QString> QSocCellFormal::generate(const QStringList &cellFiles, const QString &cellDir)
{
    const QStringList cells = knownCells(cellFiles);
    if (cells.isEmpty())
        return {};

    QMap<QString, QString> files;
    QStringList            harnesses;
    QStringList            sources;
    for (const QString &cell : cells) {
        files.insert(formalName(cell), join(prelude) + "\n" + harnessFor(cell));
        harnesses.append(formalName(cell));
        sources.append(QDir::cleanPath(cellDir + "/" + cell));
    }

    QString tasksSection;
    QString script;
    for (const Task &task : taskTable) {
        if (!cells.contains(task.cell))
            continue;
        tasksSection += QString("%1 %2\n").arg(task.name, task.cover ? "cover" : "prove");
        const QStringList words = QString(task.parameters).split(' ', Qt::SkipEmptyParts);
        if (!words.isEmpty()) {
            QString chparam = QString("%1: chparam").arg(task.name);
            for (int i = 0; i + 1 < words.size(); i += 2)
                chparam += QString(" -set %1 %2").arg(words.at(i), words.at(i + 1));
            script += chparam + " " + task.top + "\n";
        }
        script += QString("%1: prep -top %2\n").arg(task.name, task.top);
    }

    files.insert(
        "check.sby",
        "# Formal checks of the primitive cells: sby -f check.sby [task]\n"
        "[tasks]\n"
            + tasksSection
            + "\n[options]\n"
              "prove: mode prove\n"
              "prove: aigsmt z3\n"
              "cover: mode cover\n"
              "cover: depth 80\n"
              "multiclock on\n"
              "\n[engines]\n"
              "prove: abc pdr\n"
              "cover: smtbmc z3\n"
              "\n[script]\n"
              "read -formal -D SYNTHESIS "
            + QStringList(cells).join(' ') + "\nread -formal " + harnesses.join(' ') + "\n" + script
            + "\n[files]\n" + (sources + harnesses).join('\n') + "\n");
    files.insert("cell_formal.fl", (sources + harnesses).join('\n') + "\n");
    return files;
}
