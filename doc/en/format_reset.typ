= Reset Controller Format
<reset-format>
The `reset` section defines reset sources, targets, polarity, and release processing.

#block(
  fill: rgb("#fffce8"),
  inset: 8pt,
  radius: 4pt,
  stroke: rgb("#a08410") + 0.5pt,
)[
  *Standalone module:* each `reset:` entry generates a module named by
  `name`, and you instantiate it yourself (@soc-net-verilog-structure).

  *Auto-input:* a `link` source that is neither a declared `source` nor a
  target becomes an input port, which is how software resets such as
  `rst_sw_cpu_n` enter. A misspelled source therefore shows up as an extra
  unconnected port, not as an error.
]

== Reset Overview
<soc-net-reset-overview>
Each target combines its linked reset sources. Polarity normalization and
processing stages control assertion and release.

== Reset Structure
<soc-net-reset-structure>
Reset sources and targets use the following structure:

```yaml
# Reset controller
reset:
  - name: main_reset_ctrl          # Reset controller instance name (required)
    test_enable: test_en           # Test enable bypass signal (optional)
    source:                        # Reset source definitions (singular)
      por_rst_n:
        active: low                # Active low reset source
      i3c_soc_rst:
        active: high               # Active high reset source
      trig_rst:
        active: low                # Trigger-based reset (active low)
    target:                        # Reset target definitions (singular)
      cpu_rst_n:
        active: low                # Active low target output
        link:                      # Link definitions for each source
          por_rst_n:
            async:                 # Component: qsoc_rst_sync
              clock: clk_sys       # Clock is required for each component
              stage: 4             # 4-stage synchronizer
          i3c_soc_rst:             # Direct assignment (no components)
      peri_rst_n:
        active: low
        link:
          por_rst_n:             # Direct assignment (no components)
```

== Processing Levels
<soc-net-reset-levels>
Reset controllers operate at two distinct processing levels with defined component support:

#figure(
  align(center)[#table(
    columns: (0.2fr, 0.2fr, 0.2fr, 0.4fr),
    align: (auto, center, center, left),
    table.header([Component], [Target Level], [Link Level], [Description]),
    table.hline(),
    [async], [Yes], [Yes], [Asynchronous reset synchronizer (qsoc_rst_sync)],
    [sync], [Yes], [Yes], [Synchronous reset pipeline (qsoc_rst_pipe)],
    [count], [Yes], [Yes], [Counter-based reset release (qsoc_rst_count)],
  )],
  caption: [PROCESSING LEVEL SUPPORT],
  kind: table,
)

=== Processing Order
<soc-net-reset-processing-order>
Signal processing follows a defined order at each level:

*Link Level*: `source` → active-low normalization → `[async|sync|count]` → output wire

*Target Level*: `[AND of all link outputs]` → `[async|sync|count]` → final output

=== Architecture Comparison
<soc-net-reset-architecture-comparison>
Two architectures are supported for multi-source reset targets:

*Per-link Processing* (component on each link):
```text
src_a ──→ [ARSR] ─┐
src_b ──→ [ARSR] ─┼──→ [AND] ──→ target_rst_n
src_c ──→ [ARSR] ─┘
```
- Each source independently synchronized
- Higher area cost (N synchronizers)
- Use when sources have different clock domain requirements

*Post-AND Processing* (component after AND):
```text
src_a ──────────┐
src_b ──────────┼──→ [AND] ──→ [ARSR] ──→ target_rst_n
src_c ──────────┘
```
- Single synchronizer after combining
- Lower area cost (1 synchronizer)
- Functionally equivalent for reset behavior
- Recommended for most use cases

=== Configuration Examples
<soc-net-reset-config-examples>
```yaml
# Per-link processing: each source has its own synchronizer
rst_cpu_n:
  active: low
  link:
    rst_por_n:
      async:                    # Link-level async
        clock: clk_cpu
        stage: 4
    rst_wdt_n:
      async:                    # Link-level async
        clock: clk_cpu
        stage: 4

# Post-AND processing: single synchronizer after AND (recommended)
rst_cpu_n:
  active: low
  async:                        # Target-level async (Post-AND ARSR)
    clock: clk_cpu
    stage: 4
  link:
    rst_por_n:                  # Direct connection
    rst_wdt_n:                  # Direct connection
```

== Reset Components
<soc-net-reset-components>
Reset controllers use component-based architecture with three standard reset processing modules. Each link can specify different processing attributes, automatically selecting the appropriate component:

=== qsoc_rst_sync - Asynchronous Reset Synchronizer
<soc-net-reset-sync>
Provides asynchronous assert, synchronous deassert functionality (active-low):
- Async assert when reset input becomes active
- Sync deassert after STAGE clocks when reset input becomes inactive
- Test bypass when test_enable=1
- Parameters: STAGE (>=2 required for any asynchronous source)

Configuration:
```yaml
async:
  clock: clk_sys              # Required: clock for synchronization
  stage: 4                    # Number of synchronizer stages
```

=== qsoc_rst_pipe - Synchronous Reset Pipeline
<soc-net-reset-pipe>
Adds synchronous delay to reset release (active-low):
- Adds STAGE cycle release delay to a synchronous reset
- Test bypass when test_enable=1
- Parameters: STAGE (>=1)

*Contract:* the `sync` input must already be synchronous to its `clock`, for
example the output of an `async` stage on the same clock. `qsoc_rst_pipe` has
no synchronizer.

Configuration:
```yaml
sync:
  clock: clk_sys              # Required: clock for pipeline
  stage: 3                    # Number of pipeline stages
```

=== qsoc_rst_count - Counter-based Reset Release
<soc-net-reset-count>
Provides counter-based reset timing (active-low):
- Asserts at once; releases on the CYCLE-th rising edge of `clock` after
  rst_in_n deasserts. The first two edges pass a `qsoc_sync`, so the output
  never goes metastable, and `cycle: 1` releases on the second edge
- Test bypass when test_enable=1
- Parameters: CYCLE (release edge, at least 1; 1 and 2 behave the same)

Configuration:
```yaml
count:
  clock: clk_sys              # Required: clock for counter
  cycle: 255                  # Number of cycles to count
```

=== Test Bypass Behavior
<soc-net-reset-test-bypass>
In each cell the bypass is a `qsoc_ck_mux2` role instance `u_test_mux`
selected by `test_enable`, and `qsoc_rst_sync` synchronizes with a
`qsoc_sync` instance `u_sync` (@cell-roles). In test mode the raw reset input
reaches every consumer combinationally, and `qsoc_rst_count` skips its whole
delay. DFT and STA have to cover three consequences:

- The synchronizer flops themselves get no async-deassert protection while
  `test_enable` is high, so releasing `test_enable` and the reset in the wrong
  order is a metastability event
- The bypass path is not covered by the synchronizer's recovery and removal
  constraints and needs its own STA exception
- Toggle coverage of the synchronizer chain requires those flops to be
  scan-observable; the bypass mux alone does not provide it

`stage: 1` is accepted and elaborates to a single flop with no synchronization.
Use it only when the source is already synchronous to `clock`. A `stage` or
`cycle` below 1 is an error, both in the netlist and as a parameter override
on the cell, which then fails to elaborate.

Note that the power controller uses the opposite convention: there `test_en`
forces the domain reset permanently released (@soc-net-power-fsm).

== Reset Properties
<soc-net-reset-properties>
Reset controller properties provide structured configuration:

#figure(
  align(center)[#table(
    columns: (0.45fr, 0.3fr, 1fr),
    align: (auto, left, left),
    table.header([Property], [Type], [Description]),
    table.hline(),
    [name], [String], [Reset controller instance name (required)],
    [test_enable], [String], [Test enable bypass signal (optional)],
    [reason], [Map], [Reset reason recording configuration block (optional)],
    [reason.clock],
    [String],
    [Always-on clock for recording logic (default: clk_32k). Generated as module input port.],
    [reason.output],
    [String],
    [Output bit vector bus name (default: reason). Generated as module output port.],
    [reason.valid],
    [String],
    [Valid signal name (default: reason_valid). Generated as module output port.],
    [reason.clear],
    [String],
    [Software clear signal name (optional). Generated as module input port if specified.],
    [reason.root_reset],
    [String],
    [Root reset signal name for async clear (required when reason recording enabled). Must exist in source list.],
    [source], [Map], [Reset source definitions with polarity (required)],
    [target], [Map], [Reset target definitions with links (required)],
  )],
  caption: [RESET CONTROLLER PROPERTIES],
  kind: table,
)

=== Source Properties
<soc-net-reset-source-properties>
Reset sources define input reset signals with structured polarity specification:

#figure(
  align(center)[#table(
    columns: (0.3fr, 0.7fr),
    align: (auto, left),
    table.header([Property], [Description]),
    table.hline(),
    [active],
    [Signal polarity: `low` (active low) or `high` (active high) - *REQUIRED*],
  )],
  caption: [RESET SOURCE PROPERTIES],
  kind: table,
)

=== Target Properties
<soc-net-reset-target-properties>
Reset targets define output reset signals with optional target-level processing and link definitions:

#figure(
  align(center)[#table(
    columns: (0.3fr, 0.7fr),
    align: (auto, left),
    table.header([Property], [Description]),
    table.hline(),
    [active],
    [Target signal polarity: `low` (active low) or `high` (active high) - *REQUIRED*],
    [async],
    [Target-level async reset synchronizer (Post-AND ARSR). Applied after all links are combined.],
    [async.clock],
    [Clock for synchronization - *REQUIRED* when async specified],
    [async.stage],
    [Number of synchronizer stages, at least 1 (default: 3, recommended: ≥2)],
    [sync],
    [Target-level sync reset pipeline. Applied after all links are combined.],
    [sync.clock], [Clock for pipeline - *REQUIRED* when sync specified],
    [sync.stage], [Number of pipeline stages, at least 1 (default: 4)],
    [count],
    [Target-level counter-based reset release. Applied after all links are combined.],
    [count.clock], [Clock for counter - *REQUIRED* when count specified],
    [count.cycle], [Number of cycles before release, at least 1 (default: 16)],
    [link],
    [Map of source connections with optional link-level component attributes],
  )],
  caption: [RESET TARGET PROPERTIES],
  kind: table,
)

=== Link Properties
<soc-net-reset-link-properties>
Link-level processing uses key existence for component selection:

#figure(
  align(center)[#table(
    columns: (0.3fr, 0.7fr),
    align: (auto, left),
    table.header([Property], [Description]),
    table.hline(),
    [async],
    [Link-level async reset synchronizer configuration (map format)],
    [async.clock],
    [Clock for synchronization - *REQUIRED* when async specified],
    [async.stage], [Number of synchronizer stages, at least 1 (default: 3)],
    [sync],
    [Link-level sync reset pipeline configuration (map format)],
    [sync.clock], [Clock for pipeline - *REQUIRED* when sync specified],
    [sync.stage], [Number of pipeline stages, at least 1 (default: 4)],
    [count],
    [Link-level counter-based reset release configuration (map format)],
    [count.clock], [Clock for counter - *REQUIRED* when count specified],
    [count.cycle], [Number of cycles before release, at least 1 (default: 16)],
    [(empty)],
    [Direct connection, no processing; source passes through to target AND],
  )],
  caption: [RESET LINK PROPERTIES],
  kind: table,
)

== Reset Reason Recording
<soc-net-reset-reason>
Reset controllers can record reset sources as sticky flags in a bit vector. Sources set the flags asynchronously; software clears them synchronously.

=== Configuration
<soc-net-reset-reason-config>
Enable reset reason recording with the simplified configuration format:
```yaml
reset:
  - name: my_reset_ctrl
    source:
      por_rst_n:
        active: low               # Root reset (excluded from bit vector)
      ext_rst_n:
        active: low               # bit[0]
      wdt_rst_n:
        active: low               # bit[1]
      i3c_soc_rst:
        active: high              # bit[2]

    # Simplified reason configuration
    reason:
      clock: clk_32k               # Always-on clock for recording logic
      output: reason               # Output bit vector name
      valid: reason_valid          # Valid signal name
      clear: reason_clear          # Software clear signal
      root_reset: por_rst_n        # Root reset signal for async clear (explicitly specified)
```

=== Reset Reason Behavior
<soc-net-reset-reason-implementation>
Every source other than `root_reset` owns one flag, in declaration order, and
every declared source becomes a controller input, even one that feeds no
target. A source sets its flag at once when it asserts. Release of
`root_reset` or a `reason.clear` pulse starts a two-cycle clear window, and
`reason.output` reads zero until `reason.valid` rises after that window. Use
an always-on `reason.clock`.

`root_reset` releases the recorder on the second rising edge of
`reason.clock` (`qsoc_sync` instance `u_root_sync`). A flag set by an event
that releases inside the clear window is cleared or kept on a clock edge,
never on a race.

Integration constraints:

- Each reset source becomes the asynchronous reset of its own `qsoc_sync`,
  whose output sets its flag flop, so a design with N recorded sources gains N
  asynchronous assertion paths that need STA exceptions
- These flops carry no `test_enable` bypass, unlike every other cell in
  `qsoc_cell_reset.v`. They are not controllable from scan and the reason register
  cannot be initialized by a scan pattern
- `reason.clear` passes a two-stage `qsoc_sync` instance `u_swc_sync` on
  `reason.clock` before edge detection. Hold it for at least one cycle of
  `reason.clock`

== Code Generation
<soc-net-reset-generation>
Run the generator with `qsoc generate verilog` (@verilog-generation).
Connectivity and width problems are reported as described in
@validation-format.


=== Instance Names
<soc-net-reset-naming>
Link `k` of a target drives the wire `<target>_link<k>_n` through the
instance `i_<target>_link<k>_<async|sync|count>`. A target-level component is
`i_<target>_target_<async|sync|count>`. `<target>` drops a trailing `_n`, so
target `cpu_rst_n` gives `i_cpu_rst_link0_async`.

=== Generated Code Example
<soc-net-reset-example>
```verilog
module rstctrl (
    /* Clock inputs */
    input  wire clk_sys,
    /* Reset sources */
    input  wire por_rst_n,
    /* Test enable signals */
    input  wire test_en,
    /* Reset targets */
    output wire cpu_rst_n
);

    /* Wire declarations */
    wire cpu_rst_link0_n;

    /* Reset logic instances */
    /* Target: cpu_rst_n */
    qsoc_rst_sync #(
        .STAGE(4)
    ) i_cpu_rst_link0_async (
        .clk        (clk_sys),
        .rst_in_n   (por_rst_n),
        .test_enable(test_en),
        .rst_out_n  (cpu_rst_link0_n)
    );

    /* Target output assignments */
    assign cpu_rst_n = cpu_rst_link0_n;

endmodule
```

=== Auto-generated Template File: qsoc_cell_reset.v
<soc-net-reset-template-file>
Every run rewrites `output/qsoc_cell/rtl/qsoc_cell_reset.v`
(@verilog-output-layout) with the three cells of @soc-net-reset-components.
They instantiate the `qsoc_sync` and `qsoc_ck_mux2` roles.

=== Diagram Output
<soc-net-reset-diagram>
Generates a `.typ` circuit diagram in the `doc/` directory of the top unit.

*Elements*: Sources → AND → ASYNC/SYNC/COUNT → Targets (with active levels/parameters)

The AND combines active-low resets, so any asserted source asserts the target.

*Files*: `output/<top>/doc/<module>.typ` (compile: `typst compile <module>.typ`)
