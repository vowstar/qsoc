// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

#include "common/qsoccellformal.h"
#include "common/qsoccellbinding.h"
#include "common/qsocverilogutils.h"

#include <QDir>
#include <QFileInfo>

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
    reg             past_clk;
    always @(`QSOC_FORMAL_STEP) begin
        past_clk       <= clk;
        past_tick      <= in_tick;
        past_valid     <= formal_reset_ni;
        past_rst_n     <= rst_n;
        past_test_en   <= test_en;
        past_en        <= en;
        past_div       <= div;
        past_div_valid <= div_valid;
    end

    /* The cell samples rst_n on two rising edges of clk; rq models that
     * chain, and boot holds rst_n low until the cell has taken it */
    reg [1:0] rq;
    reg [1:0] boot_rises;
    always @(`QSOC_FORMAL_STEP) begin
        if (rise) rq <= {rq[0], rst_n};
        if (!formal_reset_ni) boot_rises <= 0;
        else if (rise && boot_rises != 2'b11) boot_rises <= boot_rises + 1'b1;
    end
    wire synced   = boot_rises >= 2;
    wire drst_n   = synced && rq[1];  /* cell out of reset */
    wire in_reset = synced && !rq[1];
    reg  past_drst_n;
    always @(`QSOC_FORMAL_STEP) past_drst_n <= drst_n;
    always @(`QSOC_FORMAL_STEP) if (past_valid && boot_rises != 2'b11) assume (!rst_n);

    /* A request seen on a rising edge and not yet served */
    reg pending;
    always @(`QSOC_FORMAL_STEP) begin
        if (!drst_n) pending <= 1'b0;
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
            if (!AUTO_UPDATE && pending && drst_n) begin
                assume (div_valid);
                assume (div == past_div);
            end
        end
    end

    /* Reference: the ratio in use, updated by each load reported on div_ready.
     * With AUTO_UPDATE, div is synchronized and taken once it holds for two
     * cycles. */
    reg  [WIDTH-1:0] sync1, sync2, last, held, n_cur;
    wire [WIDTH-1:0] src = AUTO_UPDATE ? held : div;
    wire             loading = rise && div_ready && norm(src) != n_cur;

    always @(`QSOC_FORMAL_STEP) begin
        if (!drst_n) begin
            sync1 <= DEFAULT_VAL;
            sync2 <= DEFAULT_VAL;
            last  <= DEFAULT_VAL;
            held  <= DEFAULT_VAL;
            n_cur <= norm(DEFAULT_VAL);
        end else if (rise) begin
            sync1 <= div;
            sync2 <= sync1;
            last  <= sync2;
            if (sync2 == last) held <= sync2;
            if (div_ready) n_cur <= norm(src);
        end
    end

    /* The gate latch has seen a low phase of clk in scan mode, or with the
     * cell in reset */
    reg scan_open, reset_open;
    always @(`QSOC_FORMAL_STEP) begin
        if (!past_valid || !test_en) scan_open <= 1'b0;
        else if (!clk) scan_open <= 1'b1;
        if (!in_reset) reset_open <= 1'b0;
        else if (!clk) reset_open <= 1'b1;
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
        if (!drst_n) begin
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
        if (!drst_n || !past_valid || !settled) stable <= 0;
        else if (rise && stable != {CW{1'b1}}) stable <= stable + 1'b1;
    end
    wire quiet = settled && stable > BOUND;

    /* Rising edges with the output disabled */
    reg [CW-1:0] off;
    always @(`QSOC_FORMAL_STEP) begin
        if (!drst_n || en) off <= 0;
        else if (rise && off != {CW{1'b1}}) off <= off + 1'b1;
    end

    /* Rising edges a held request has waited */
    reg [CW-1:0] waiting;
    always @(`QSOC_FORMAL_STEP) begin
        if (!drst_n || !div_valid || !en || (rise && div_ready)) waiting <= 0;
        else if (rise && waiting != {CW{1'b1}}) waiting <= waiting + 1'b1;
    end

    always @(`QSOC_FORMAL_STEP) begin
        if (past_valid && past_drst_n && drst_n && !test_en) begin
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
        /* The output moves only with a clk edge, also when reset asserts */
        if (past_valid && synced && clk == past_clk) edge_only: assert (clk_out == past_out);
        if (past_valid && test_en && scan_open) scan: assert (clk_out == clk);
        if (past_valid && reset_open && !test_en && !CLOCK_DURING_RESET) reset_low: assert (!clk_out);
        if (past_valid && reset_open && CLOCK_DURING_RESET && DEFAULT_VAL < 2) reset_clock: assert (clk_out == clk);
    end

    always @(`QSOC_FORMAL_STEP) begin
        if (past_valid && drst_n && !test_en) begin
            reload: cover (loading && n_cur != norm(DEFAULT_VAL));
            odd_high: cover (edge_out && !clk_out && phase_ok && len == 3);
        end
    end
endmodule

/* AUTO_UPDATE across a skewed bus. The source is a register that holds each
 * value for at least three rising edges of clk. Each bit reaches div on time
 * or one rising edge late, chosen per bit and fixed while the bus settles.
 * The ratio in use shows on count: a wrap to zero that no load caused ends a
 * period of exactly the ratio. Every ratio shown must be a value the source
 * held, and once the source is quiet it must be the source value. */
module qsoc_clk_div_skew_formal #(
    parameter integer WIDTH       = 3,
    parameter integer DEFAULT_VAL = 2,
    parameter         LIVE        = 1'b1,
    parameter         FREE_CLOCK  = 1'b1
) (
    input wire             fclk,
`ifdef FORMAL_EXTERNAL_RESET
    input wire             formal_reset_ni,
`endif
    input wire             in_tick,
    input wire [WIDTH-1:0] in_src,
    input wire [WIDTH-1:0] in_late
);
    localparam integer MAXN  = (1 << WIDTH) - 1;
    localparam integer HOLD  = 3;
    localparam integer BOUND = 4 * MAXN + 16;
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

    reg             clk;
    reg             rst_n;
    reg [1:0]       boot_rises;
    reg [WIDTH-1:0] src;
    reg [WIDTH-1:0] src_q;  /* src at the last rising edge */
    reg [WIDTH-1:0] late;
    wire            rise = in_tick && !clk;
    always @(`QSOC_FORMAL_STEP) begin
        if (in_tick) clk <= ~clk;
        else src <= in_src;
        if (rise || !formal_reset_ni) src_q <= src;
        if (!in_tick && src == src_q) late <= in_late;
        rst_n <= formal_reset_ni && boot_rises == 2'b11;
    end

    /* The cell samples rst_n on two rising edges of clk */
    reg [1:0] rq;
    always @(`QSOC_FORMAL_STEP) begin
        if (rise) rq <= {rq[0], rst_n};
        if (!formal_reset_ni) boot_rises <= 0;
        else if (rise && boot_rises != 2'b11) boot_rises <= boot_rises + 1'b1;
    end
    wire drst_n = boot_rises >= 2 && rq[1];
    wire [WIDTH-1:0] div = (late & src_q) | (~late & src);

)sv",
    R"sv(    wire             clk_out;
    wire             div_ready;
    wire [WIDTH-1:0] count;
    qsoc_clk_div #(
        .WIDTH(WIDTH),
        .DEFAULT_VAL(DEFAULT_VAL),
        .CLOCK_DURING_RESET(1'b0),
        .AUTO_UPDATE(1'b1)
    ) dut (
        .clk(clk),
        .rst_n(rst_n),
        .en(1'b1),
        .test_en(1'b0),
        .div(div),
        .div_valid(1'b0),
        .div_ready(div_ready),
        .clk_out(clk_out),
        .count(count)
    );

    /* Source contract */
    reg past_valid = 1'b0;
    reg past_tick;
    reg [CW-1:0] age;  /* rising edges since src last moved */
    always @(`QSOC_FORMAL_STEP) begin
        past_valid <= formal_reset_ni;
        past_tick  <= in_tick;
        if (!formal_reset_ni || (!in_tick && in_src != src)) age <= 0;
        else if (rise && age != {CW{1'b1}}) age <= age + 1'b1;
        if (past_valid) begin
            if (!in_tick && in_src != src) assume (age >= HOLD);
            if (!FREE_CLOCK) assume (in_tick != past_tick);
        end
    end

    /* Ratios the source has held since reset */
    reg [MAXN:0] seen;
    always @(`QSOC_FORMAL_STEP) begin
        if (!drst_n) seen <= ({{MAXN{1'b0}}, 1'b1} << norm(DEFAULT_VAL)) | ({{MAXN{1'b0}}, 1'b1} << norm(src));
        else seen <= seen | ({{MAXN{1'b0}}, 1'b1} << norm(src));
    end

    /* Wraps of count, sampled at rising edges */
    reg             sampled;
    reg [WIDTH-1:0] count_q;
    reg             ready_q;
    reg [CW-1:0]    since_wrap;
    wire            wrap  = rise && sampled && drst_n && count == 0 && !ready_q;
    wire [WIDTH:0]  ratio = {1'b0, count_q} + 1'b1;
    always @(`QSOC_FORMAL_STEP) begin
        if (!drst_n) begin
            sampled    <= 1'b0;
            since_wrap <= 0;
        end else if (rise) begin
            sampled <= 1'b1;
            count_q <= count;
            ready_q <= div_ready;
            if (wrap) since_wrap <= 0;
            else if (since_wrap != {CW{1'b1}}) since_wrap <= since_wrap + 1'b1;
        end
    end

    wire quiet = age > BOUND;
    always @(`QSOC_FORMAL_STEP) begin
        if (past_valid && wrap) begin
            no_transient: assert (seen[ratio]);
            if (LIVE && quiet) taken: assert (ratio == norm(src));
        end
        if (past_valid && drst_n && LIVE && quiet) alive: assert (since_wrap <= MAXN + 1);
    end

    always @(`QSOC_FORMAL_STEP) begin
        if (past_valid && drst_n) begin
            mixed: cover (div != src && div != src_q);
            reload: cover (wrap && ratio != norm(DEFAULT_VAL) && ratio == norm(src));
        end
    end
endmodule

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

    /* Every input has had STAGES + 2 rising edges and a low phase since the
     * harness reset, so its synchronizers without reset and its gate latch
     * hold values they were given */
    reg [N-1:0] known;
    reg [3:0]   boot_rises [0:N-1];
    integer     b;
    always @(`QSOC_FORMAL_STEP) begin
        for (b = 0; b < N; b = b + 1) begin
            if (!formal_reset_ni) begin
                boot_rises[b] <= 0;
                known[b]      <= 1'b0;
            end else begin
                if (rose[b] && boot_rises[b] != 4'hf) boot_rises[b] <= boot_rises[b] + 1'b1;
                if (!clk[b] && boot_rises[b] >= STAGES + 2) known[b] <= 1'b1;
            end
        end
    end
    wire live = past_valid && &known && !test_en;

    /* Every input has had STAGES + 1 rising edges in reset, so its enable
     * synchronizer has emptied */
    reg [3:0] reset_rises [0:N-1];
    reg       drained;
    always @(`QSOC_FORMAL_STEP) begin
        for (b = 0; b < N; b = b + 1) begin
            if (rst_n) reset_rises[b] <= 0;
            else if (rose[b] && reset_rises[b] != 4'hf) reset_rises[b] <= reset_rises[b] + 1'b1;
        end
    end
    always @* begin
        drained = 1'b1;
        for (b = 0; b < N; b = b + 1) if (reset_rises[b] < STAGES + 1) drained = 1'b0;
    end

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

    /* Rising edges of each input since reset released. An input drives the
     * output no earlier than its edge 5 + STAGES: two for the reset
     * synchronizer, two for the glitch filter, STAGES for the enable
     * synchronizer and one for the gate latch. */
    localparam integer R = STAGES + 5;
    reg [4:0]   since_release [0:N-1];
    reg [N-1:0] released;
    integer     r;
    always @(`QSOC_FORMAL_STEP) begin
        for (r = 0; r < N; r = r + 1) begin
            if (!rst_n) since_release[r] <= 0;
            else if (rose[r] && since_release[r] != 5'h1f) since_release[r] <= since_release[r] + 1'b1;
        end
    end
    always @* begin
        for (r = 0; r < N; r = r + 1) released[r] = since_release[r] + (rose[r] ? 1 : 0) >= R;
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
        if (live && !CLOCK_DURING_RESET && !rst_n && drained && out_rise) reset_quiet: assert (0);
        if (live && !CLOCK_DURING_RESET && rst_n && out_rise) release_late: assert ((rose & released) != 0);
        if (FAIR && live && rst_n && sel == past_sel && held > T) follow: assert (clk_out == padded[sel]);
        if (live && phase_ok && rst_n) switched: cover (out_rise && sel != 0 && rose[sel]);
    end

    generate
        if (CLOCK_DURING_RESET) begin : g_reset_clock
            always @(`QSOC_FORMAL_STEP) if (live && !rst_n) reset_clock: cover (out_rise);
        end
    endgenerate
endmodule

/* Combinational clock roles against their Boolean functions */
module qsoc_ck_role_formal (
    input wire a,
    input wire b,
    input wire s
);
    wire buf_o, inv_o, or_o, mux_o, xor_o;
    qsoc_ck_buf  u_buf (.clk_in(a), .clk_out(buf_o));
    qsoc_ck_inv  u_inv (.clk_in(a), .clk_out(inv_o));
    qsoc_ck_or2  u_or  (.clk_in0(a), .clk_in1(b), .clk_out(or_o));
    qsoc_ck_mux2 u_mux (.clk_in0(a), .clk_in1(b), .clk_sel(s), .clk_out(mux_o));
)sv",
    R"sv(    qsoc_ck_xor2 u_xor (.clk_in0(a), .clk_in1(b), .clk_out(xor_o));
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

/* Clock gate: the output is clk gated by the enable latched while clk is at
 * its idle level, and never changes without a clk edge. The latched enable
 * is en, test_en, or with CLOCK_DURING_RESET the reset as sampled on two
 * rising edges of clk. */
module qsoc_clk_gate_formal #(
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

    reg  clk, en, test_en, rst_n;
    wire rise = in_tick && !clk;
    always @(`QSOC_FORMAL_STEP) begin
        if (in_tick) clk <= ~clk;
        else begin
            en      <= in_en;
            test_en <= in_test_en;
        end
        if (!formal_reset_ni) rst_n <= 1'b0;
        else if (!in_tick) rst_n <= in_rst_n;
    end

    wire clk_out;
    qsoc_clk_gate #(
        .CLOCK_DURING_RESET(CLOCK_DURING_RESET),
        .POLARITY(POLARITY)
    ) dut (
        .clk(clk),
        .en(en),
        .test_en(test_en),
        .rst_n(rst_n),
        .clk_out(clk_out)
    );

    /* Reference reset opening: reset sampled on two rising edges */
    reg [1:0] reset_q;
    reg [1:0] rises;
    always @(`QSOC_FORMAL_STEP) begin
        if (rise) reset_q <= {reset_q[0], !rst_n};
        if (!formal_reset_ni) rises <= 0;
        else if (rise && rises != 2'b11) rises <= rises + 1'b1;
    end
    wire reset_open = CLOCK_DURING_RESET && reset_q[1];
    wire open       = en | test_en | reset_open;

    /* Reference latch: transparent while clk is in its idle level */
    wire idle = POLARITY ? !clk : clk;
    reg  held;
    reg  known;
    always @(`QSOC_FORMAL_STEP) begin
        if (!formal_reset_ni) known <= 1'b0;
        else if (idle && rises >= 2) known <= 1'b1;
        if (idle) held <= open;
    end
    wire latched = idle ? open : held;
    wire gated   = POLARITY ? (clk & latched) : (clk | !latched);

    reg past_valid = 1'b0;
    reg past_clk, past_out;
    always @(`QSOC_FORMAL_STEP) begin
        past_valid <= formal_reset_ni;
        past_clk   <= clk;
        past_out   <= clk_out;
    end

    always @(`QSOC_FORMAL_STEP) begin
        if (past_valid && known) begin
            gate_ok: assert (clk_out == gated);
            /* Glitch free: without a clk edge the output holds */
            if (clk == past_clk) hold: assert (clk_out == past_out);
            blocked: cover (!open && clk && !clk_out);
            if (CLOCK_DURING_RESET) reset_pulse: cover (rst_n && reset_open && !en && !test_en && clk_out != past_out);
        end
    end
endmodule
)sv",
};

/* Release counter shared by the reset synchronizers, filled per cell */
const char *const resetHarness[] = {
    R"sv(
/* Reset release counter: the output releases on exactly the EDGE-th rising
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
    /* Release edge: COUNT, and at least MIN */
    localparam integer EDGE = (COUNT < @MIN@) ? @MIN@ : COUNT;
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
        if (past_valid && !test && known) release_exact: assert (out_n == ((rst_n || !ASYNC) && seen >= EDGE));
        if (past_valid && !test && ASYNC && !rst_n) assert_async: assert (!out_n);
        if (past_valid && test) scan: assert (out_n == (SCAN_RELEASE ? 1'b1 : rst_n));
        if (past_valid && !test && known) released: cover (out_n && seen == EDGE);
    end
endmodule
)sv",
};

const char *const powerHarness[] = {
    R"sv(
/* One formal step is half a clk period; inputs move just after rising edges.
 * With HEALTHY, dependencies are met and pgood follows the switch within a
 * delay that, with the two synchronizer cycles, fits either settle window, so
 * no fault may ever be raised. Outputs change only on rising edges or reset,
 * and valid outside S_ON follows pgood three rising edges late. */
module qsoc_power_fsm_formal #(
    parameter integer HAS_SWITCH = 1,
    parameter integer WAIT_DEP   = 2,
    parameter integer SETTLE_ON  = 5,
    parameter integer SETTLE_OFF = 5,
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
    reg p_clk_enable, p_rst_gate_n, p_pwr_switch, p_fault, p_test_en;
    reg [2:0] p_fault_clear;  /* newest in bit 0 */
    reg p_rst_n, p_ctrl;
    always @(`QSOC_FORMAL_STEP) begin
        if (!formal_reset_ni) past_valid <= 1'b0;
        else if (rise) past_valid <= 1'b1;
        if (rise) begin
            p_clk_enable  <= clk_enable;
            p_rst_gate_n  <= rst_gate_n;
            p_pwr_switch  <= pwr_switch;
            p_fault       <= fault;
            p_fault_clear <= {p_fault_clear[1:0], fault_clear};
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
                /* pgood lands delay + 2 edges after the switch moves and
                 * is seen two edges later */
                assume (delay + 4 <= SETTLE_MIN);
                assume (in_pgood == (HAS_SWITCH ? history[delay] : 1'b1));
            end
        end
    end

    /* pgood at the last three rising edges, newest in bit 0 */
    reg [2:0] pgood_q;
    always @(`QSOC_FORMAL_STEP) if (rise) pgood_q <= {pgood_q[1:0], pgood};

    /* Values at the previous step */
    reg       s_valid = 1'b0;
    reg       s_clk, s_rst_n, s_test_en;
    reg [5:0] s_out;
    wire [5:0] out = {clk_enable, rst_gate_n, pwr_switch, ready, valid, fault};
    always @(`QSOC_FORMAL_STEP) begin
        s_valid   <= formal_reset_ni;
        s_clk     <= clk;
        s_rst_n   <= rst_n;
        s_test_en <= test_en;
        s_out     <= out;
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
            /* The domain gate enable moves only while the domain reset is held */
            if (clk_enable != p_clk_enable) icg_in_reset: assert (!rst_gate_n && !p_rst_gate_n);
            /* fault_clear reaches the FSM through a two stage synchronizer */
            if (!fault && p_fault) fault_sticky: assert (p_fault_clear[2]);
        end
        /* Registered: no change without a rising edge, reset or scan change */
        if (s_valid && rst_n && s_rst_n && test_en == s_test_en && !(clk && !s_clk))
            registered: assert (out == s_out);
        if (past_valid && rst_n && !test_en) begin
            ready_is_release: assert (ready == rst_gate_n);
            if (valid && !ready) pgood_latency: assert (pgood_q[2]);
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
            valid_on: cover (valid && !ready && pwr_switch);
        end
    end
endmodule

/* Two FSMs see the same register writes, each bit of ctrl_enable and
 * fault_clear on time or one cycle late, chosen per copy at each write. A
 * write moves ctrl_enable and pulses fault_clear for one cycle, from a steady
 * state. pgood follows each copy's own switch a cycle late and reaches the
 * FSM three cycles after the switch, so a SETTLE_ON below 3 times out on
 * every turn on. Once writes stop, both copies must end in the same state,
 * so the result never depends on which bit arrived first. */
module qsoc_power_fsm_race_formal #(
    parameter integer WAIT_DEP   = 1,
    parameter integer SETTLE_ON  = 5,
    parameter integer SETTLE_OFF = 5
) (
    input wire       fclk,
`ifdef FORMAL_EXTERNAL_RESET
    input wire       formal_reset_ni,
`endif
    input wire       in_write,
    input wire       in_en,
    input wire       in_clr,
    input wire [1:0] in_late_a,
    input wire [1:0] in_late_b,
    input wire       in_dep_hard,
    input wire       in_dep_soft
);
    localparam integer Q  = 4 * (WAIT_DEP + SETTLE_ON + SETTLE_OFF) + 16;
    localparam integer CW = 8;

`ifndef FORMAL_EXTERNAL_RESET
    reg [1:0] formal_boot = 2'b00;
    always @(`QSOC_FORMAL_STEP) if (formal_boot != 2'b11) formal_boot <= formal_boot + 2'b01;
    wire formal_reset_ni = &formal_boot;
`endif

    reg          clk, rst_n, en, clr, en_q, clr_q, dep_hard, dep_soft;
    reg [1:0]    late_a, late_b;
    reg [CW-1:0] quiet;
    wire         write = in_write && quiet > Q;
    always @(`QSOC_FORMAL_STEP) begin
        clk <= ~clk;
        if (!formal_reset_ni) begin
            rst_n    <= 1'b0;
            en       <= 1'b0;
            clr      <= 1'b0;
            en_q     <= 1'b0;
            clr_q    <= 1'b0;
            quiet    <= {CW{1'b1}};  /* reset leaves a steady S_OFF */
            dep_hard <= in_dep_hard;
            dep_soft <= in_dep_soft;
        end else if (clk) begin
            /* Inputs move just after rising edges */
            rst_n <= 1'b1;
            en_q  <= en;
            clr_q <= clr;
            if (write) begin
                en     <= in_en;
                clr    <= in_clr;
                late_a <= in_late_a;
                late_b <= in_late_b;
                quiet  <= 0;
            end else begin
                clr <= 1'b0;
                if (quiet != {CW{1'b1}}) quiet <= quiet + 1'b1;
            end
        end
    end

    wire en_a  = late_a[0] ? en_q : en;
    wire clr_a = late_a[1] ? clr_q : clr;
    wire en_b  = late_b[0] ? en_q : en;
    wire clr_b = late_b[1] ? clr_q : clr;

    wire [5:0] out_a, out_b;
    reg        pgood_a, pgood_b;
    always @(`QSOC_FORMAL_STEP) begin
        if (!formal_reset_ni) begin
            pgood_a <= 1'b0;
            pgood_b <= 1'b0;
        end else if (clk) begin
            pgood_a <= out_a[3];
            pgood_b <= out_b[3];
        end
    end

    qsoc_power_fsm #(
        .HAS_SWITCH(1),
        .WAIT_DEP_CYCLES(WAIT_DEP),
        .SETTLE_ON_CYCLES(SETTLE_ON),
        .SETTLE_OFF_CYCLES(SETTLE_OFF)
    ) dut_a (
        .clk(clk), .rst_n(rst_n), .test_en(1'b0), .ctrl_enable(en_a), .fault_clear(clr_a),
        .dep_hard_all(dep_hard), .dep_soft_all(dep_soft), .pgood(pgood_a),
        .clk_enable(out_a[5]), .rst_gate_n(out_a[4]), .pwr_switch(out_a[3]),
        .ready(out_a[2]), .valid(out_a[1]), .fault(out_a[0])
    );
    qsoc_power_fsm #(
        .HAS_SWITCH(1),
        .WAIT_DEP_CYCLES(WAIT_DEP),
        .SETTLE_ON_CYCLES(SETTLE_ON),
        .SETTLE_OFF_CYCLES(SETTLE_OFF)
    ) dut_b (
        .clk(clk), .rst_n(rst_n), .test_en(1'b0), .ctrl_enable(en_b), .fault_clear(clr_b),
        .dep_hard_all(dep_hard), .dep_soft_all(dep_soft), .pgood(pgood_b),
        .clk_enable(out_b[5]), .rst_gate_n(out_b[4]), .pwr_switch(out_b[3]),
        .ready(out_b[2]), .valid(out_b[1]), .fault(out_b[0])
    );

    /* The racy write: leave S_FAULT and clear the fault at once */
    wire race = write && out_a[0] && en && !in_en && in_clr && in_late_a != in_late_b;

    always @(`QSOC_FORMAL_STEP) begin
        if (formal_reset_ni && quiet > Q) same_end: assert (out_a == out_b);
        if (formal_reset_ni) race_write: cover (race);
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
    int         minCount;
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
        .replace("@SCAN@", cell.scanRelease ? "1'b1" : "1'b0")
        .replace("@MIN@", QString::number(cell.minCount));
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
    {"clk_div_reset_clock_div2",
     "qsoc_cell_clock.v",
     false,
     "qsoc_clk_div_formal",
     "WIDTH 2 DEFAULT_VAL 2 CLOCK_DURING_RESET 1 LIVE 0"},
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
    {"clk_div_skew", "qsoc_cell_clock.v", false, "qsoc_clk_div_skew_formal", "WIDTH 3 LIVE 0"},
    {"clk_div_skew_live",
     "qsoc_cell_clock.v",
     false,
     "qsoc_clk_div_skew_formal",
     "WIDTH 2 FREE_CLOCK 0"},
    {"clk_div_skew_cover",
     "qsoc_cell_clock.v",
     true,
     "qsoc_clk_div_skew_formal",
     "WIDTH 2 FREE_CLOCK 0"},
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
    {"clk_role", "qsoc_cell_clock.v", false, "qsoc_ck_role_formal", ""},
    {"clk_gate_pos", "qsoc_cell_clock.v", false, "qsoc_clk_gate_formal", "POLARITY 1"},
    {"clk_gate_neg", "qsoc_cell_clock.v", false, "qsoc_clk_gate_formal", "POLARITY 0"},
    {"clk_gate_pos_reset_clock",
     "qsoc_cell_clock.v",
     false,
     "qsoc_clk_gate_formal",
     "POLARITY 1 CLOCK_DURING_RESET 1"},
    {"clk_gate_neg_reset_clock",
     "qsoc_cell_clock.v",
     false,
     "qsoc_clk_gate_formal",
     "POLARITY 0 CLOCK_DURING_RESET 1"},
    {"clk_gate_cover", "qsoc_cell_clock.v", true, "qsoc_clk_gate_formal", "POLARITY 1"},
    {"clk_gate_reset_clock_cover",
     "qsoc_cell_clock.v",
     true,
     "qsoc_clk_gate_formal",
     "POLARITY 1 CLOCK_DURING_RESET 1"},
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
    {"power_fsm_race", "qsoc_cell_power.v", false, "qsoc_power_fsm_race_formal", ""},
    {"power_fsm_race_short",
     "qsoc_cell_power.v",
     false,
     "qsoc_power_fsm_race_formal",
     "WAIT_DEP 0 SETTLE_ON 3 SETTLE_OFF 1"},
    {"power_fsm_race_cover",
     "qsoc_cell_power.v",
     true,
     "qsoc_power_fsm_race_formal",
     "WAIT_DEP 0 SETTLE_ON 3 SETTLE_OFF 1"},
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
                    false,
                    1})
               + "\n"
               + resetHarnessFor(
                   {"qsoc_rst_pipe",
                    "STAGE",
                    "clk",
                    "rst_in_n",
                    "test_enable",
                    "rst_out_n",
                    false,
                    false,
                    1})
               + "\n"
               + resetHarnessFor(
                   {"qsoc_rst_count",
                    "CYCLE",
                    "clk",
                    "rst_in_n",
                    "test_enable",
                    "rst_out_n",
                    true,
                    false,
                    2});
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
                true,
                1});
}

QStringList knownCells(const QStringList &sources)
{
    QStringList names;
    for (const QString &source : sources)
        names.append(QFileInfo(source).fileName());
    QStringList cells;
    for (const char *cell : cellOrder) {
        if (names.contains(cell))
            cells.append(cell);
    }
    return cells;
}

QString formalName(const QString &cell)
{
    return QString(cell).replace(".v", "_formal.sv");
}

} // namespace

QStringList QSocCellFormal::tasks(const QStringList &sources)
{
    const QStringList cells = knownCells(sources);
    QStringList       names;
    for (const Task &task : taskTable) {
        if (cells.contains(task.cell))
            names.append(task.name);
    }
    return names;
}

QMap<QString, QString> QSocCellFormal::generate(const QStringList &sources)
{
    const QStringList cells = knownCells(sources);
    if (cells.isEmpty())
        return {};

    QMap<QString, QString> files;
    QStringList            harnesses;
    QStringList            reads;
    for (const QString &source : sources)
        reads.append(QFileInfo(source).fileName());
    for (const QString &cell : cells) {
        files.insert(
            formalName(cell),
            QSocVerilogUtils::withTimescale(join(prelude) + "\n" + harnessFor(cell)));
        harnesses.append(formalName(cell));
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

    /* SymbiYosys flattens the design, so roles marked keep_hierarchy must not stop it */
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
            + reads.join(' ') + "\nread -formal " + harnesses.join(' ') + "\n" + script
            + "setattr -mod -unset keep_hierarchy\nsetattr -unset keep_hierarchy\n" + "\n[files]\n"
            + (sources + harnesses).join('\n') + "\n");
    return files;
}

namespace {

const int kSyncStages[] = {1, 2, 3, 5};

QString combinationalContract(const QString &role, const QStringList &inputs)
{
    QStringList ports;
    QStringList connect;
    for (const QString &input : inputs) {
        ports.append("    input wire " + input);
        connect.append(QString(".%1(%1)").arg(input));
    }
    return QString(
               "\n/* %1 with its declared cell equals the generic role for every input */\n"
               "module %1_contract (\n"
               "%2\n"
               ");\n"
               "    wire got;\n"
               "    wire want;\n"
               "    %1 u_dut (%3, .clk_out(got));\n"
               "    %1_ref u_ref (%3, .clk_out(want));\n"
               "    always @* contract: assert (got == want);\n"
               "endmodule\n")
        .arg(role, ports.join(",\n"), connect.join(", "));
}

/* Inputs move only while clk holds, as in the clock gate harness. */
QString gateContract(const QString &role, bool positive)
{
    return QString(
               "\n/* %1 with its declared cell equals the generic role once both latches\n"
               " * have loaded, that is after clk has rested at its idle level */\n"
               "module %1_contract (\n"
               "    input wire fclk,\n"
               "    input wire in_tick,\n"
               "    input wire in_en,\n"
               "    input wire in_test_en\n"
               ");\n"
               "    reg clk;\n"
               "    reg en;\n"
               "    reg test_en;\n"
               "    always @(`QSOC_FORMAL_STEP) begin\n"
               "        if (in_tick) clk <= ~clk;\n"
               "        else begin\n"
               "            en      <= in_en;\n"
               "            test_en <= in_test_en;\n"
               "        end\n"
               "    end\n"
               "\n"
               "    wire got;\n"
               "    wire want;\n"
               "    %1 u_dut (.clk(clk), .en(en), .test_en(test_en), .clk_out(got));\n"
               "    %1_ref u_ref (.clk(clk), .en(en), .test_en(test_en), .clk_out(want));\n"
               "\n"
               "    reg loaded = 1'b0;\n"
               "    always @(`QSOC_FORMAL_STEP) if (%2clk) loaded <= 1'b1;\n"
               "    always @* if (loaded) contract: assert (got == want);\n"
               "endmodule\n")
        .arg(role, positive ? "!" : "");
}

/* Both chains start in reset, then d and rst_n move only while clk holds. */
const char *const syncContract = R"sv(
/* qsoc_sync with its declared cell equals the generic role after reset:
 * same latency and same reset value */
module qsoc_sync_contract #(
    parameter integer STAGES      = 2,
    parameter [0:0]   RESET_VALUE = 1'b0
) (
    input wire fclk,
    input wire in_tick,
    input wire in_d,
    input wire in_rst_n
);
    reg [1:0] boot = 2'b00;
    reg       clk;
    reg       d;
    reg       rst_n;
    always @(`QSOC_FORMAL_STEP) begin
        if (boot != 2'b11) boot <= boot + 2'b01;
        if (in_tick) clk <= ~clk;
        else d <= in_d;
        if (boot != 2'b11) rst_n <= 1'b0;
        else if (!in_tick) rst_n <= in_rst_n;
    end

    wire got;
    wire want;
    qsoc_sync #(.STAGES(STAGES), .RESET_VALUE(RESET_VALUE)) u_dut (
        .clk(clk), .rst_n(rst_n), .d(d), .q(got));
    qsoc_sync_ref #(.STAGES(STAGES), .RESET_VALUE(RESET_VALUE)) u_ref (
        .clk(clk), .rst_n(rst_n), .d(d), .q(want));

    always @* if (boot == 2'b11) contract: assert (got == want);
endmodule
)sv";

QString contractHarness(const QString &role)
{
    if (role == "qsoc_sync")
        return syncContract;
    if (role.startsWith("qsoc_ck_icg_"))
        return gateContract(role, role.endsWith("_pos"));
    QStringList inputs = QSocCellBinding::rolePorts(role);
    inputs.removeAll("clk_out");
    return combinationalContract(role, inputs);
}

} // namespace

QStringList QSocCellFormal::contractTasks(const QStringList &roles)
{
    QStringList names;
    for (const QString &role : QSocCellBinding::roleNames()) {
        if (!roles.contains(role))
            continue;
        if (role != "qsoc_sync") {
            names.append(role);
            continue;
        }
        for (const int stages : kSyncStages) {
            for (const int level : {0, 1})
                names.append(QString("qsoc_sync_%1_%2").arg(stages).arg(level));
        }
    }
    return names;
}

QMap<QString, QString> QSocCellFormal::contracts(
    const QMap<QString, QString> &generic, const QStringList &sources)
{
    QMap<QString, QString> files;
    if (generic.isEmpty())
        return files;
    QStringList reads;
    for (const QString &source : sources)
        reads.append(QFileInfo(source).fileName());
    QStringList harnesses;
    for (auto role = generic.cbegin(); role != generic.cend(); ++role) {
        const QString reference
            = QString(role.value())
                  .replace("module " + role.key() + " ", "module " + role.key() + "_ref ");
        files.insert(
            role.key() + "_contract.sv", join(prelude) + reference + contractHarness(role.key()));
        harnesses.append(role.key() + "_contract.sv");
    }
    QString tasks;
    QString script;
    for (const QString &task : contractTasks(generic.keys())) {
        tasks += task + "\n";
        if (!task.startsWith("qsoc_sync_")) {
            script += QString("%1: prep -top %1_contract\n").arg(task);
            continue;
        }
        script += QString("%1: chparam -set STAGES %2 -set RESET_VALUE %3 qsoc_sync_contract\n")
                      .arg(task, task.section('_', 2, 2), task.section('_', 3, 3));
        script += QString("%1: prep -top qsoc_sync_contract\n").arg(task);
    }
    files.insert(
        "contract.sby",
        "# Role contracts of the declared cells: sby -f contract.sby [task]\n"
        "[tasks]\n"
            + tasks
            + "\n[options]\n"
              "mode prove\n"
              "aigsmt z3\n"
              "multiclock on\n"
              "\n[engines]\n"
              "abc pdr\n"
              "\n[script]\n"
              "read -formal -D SYNTHESIS "
            + (reads + harnesses).join(' ') + "\n" + script
            + "setattr -mod -unset keep_hierarchy\n"
              "\n[files]\n"
            + (sources + harnesses).join('\n') + "\n");
    return files;
}
