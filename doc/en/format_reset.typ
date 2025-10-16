= RESET CONTROLLER FORMAT
<reset-format>
The reset section defines reset controller primitives that generate proper reset signaling throughout the SoC. Reset primitives provide comprehensive reset management with support for multiple reset sources, component-based processing, signal polarity handling, and standardized module generation.

== RESET OVERVIEW
<soc-net-reset-overview>
Reset controllers are essential for proper SoC operation, ensuring that all logic blocks start in a known state and can be reset reliably. QSoC supports sophisticated reset topologies with multiple reset sources mapping to multiple reset targets through a clear source → target → link relationship structure.

Key features include:
- Component-based reset processing architecture
- Signal polarity normalization (active high/low)
- Multi-source to multi-target reset matrices
- Structured YAML configuration without string parsing
- Test mode bypass support
- Standalone reset controller module generation

== RESET STRUCTURE
<soc-net-reset-structure>
Reset controllers use a modern structured YAML format that eliminates complex string parsing and provides component-based processing:

```yaml
# Modern component-based reset controller format
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

== RESET COMPONENTS
<soc-net-reset-components>
Reset controllers use component-based architecture with three standard reset processing modules. Each link can specify different processing attributes, automatically selecting the appropriate component:

=== qsoc_rst_sync - Asynchronous Reset Synchronizer
<soc-net-reset-sync>
Provides asynchronous assert, synchronous deassert functionality (active-low):
- Async assert when reset input becomes active
- Sync deassert after STAGE clocks when reset input becomes inactive
- Test bypass when test_enable=1
- Parameters: STAGE (>=2 recommended for metastability resolution)

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

Configuration:
```yaml
sync:
  clock: clk_sys              # Required: clock for pipeline
  stage: 3                    # Number of pipeline stages
```

=== qsoc_rst_count - Counter-based Reset Release
<soc-net-reset-count>
Provides counter-based reset timing (active-low):
- After rst_in_n deasserts, count CYCLE cycles then release
- Test bypass when test_enable=1
- Parameters: CYCLE (number of cycles before release)

Configuration:
```yaml
count:
  clock: clk_sys              # Required: clock for counter
  cycle: 255                  # Number of cycles to count
```

== RESET PROPERTIES
<soc-net-reset-properties>
Reset controller properties provide structured configuration:

#figure(
  align(center)[#table(
    columns: (0.2fr, 0.3fr, 0.5fr),
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
Reset targets define output reset signals with structured link definitions:

#figure(
  align(center)[#table(
    columns: (0.3fr, 0.7fr),
    align: (auto, left),
    table.header([Property], [Description]),
    table.hline(),
    [active],
    [Target signal polarity: `low` (active low) or `high` (active high) - *REQUIRED*],
    [link], [Map of source connections with component attributes],
  )],
  caption: [RESET TARGET PROPERTIES],
  kind: table,
)

== RESET REASON RECORDING
<soc-net-reset-reason>
Reset controllers can optionally record the source of the last reset using sync-clear async-capture sticky flags with bit vector output. This implementation provides reliable narrow pulse capture and flexible software decoding.

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

=== Implementation Details
<soc-net-reset-reason-implementation>
The reset reason recorder uses *sync-clear async-capture* sticky flags to avoid S+R register timing issues:
- Each non-POR reset source gets a dedicated sticky flag (async-set on event, sync-clear during clear window)
- Clean async-set + sync-clear architecture avoids problematic S+R registers that cause STA difficulties
- Event normalization converts all sources to LOW-active format for consistent handling
- 2-cycle clear window after POR release or software clear pulse ensures proper initialization
- Output gating with valid signal prevents invalid data during initialization
- Always-on clock ensures operation even when main clocks are stopped
- Root reset signal explicitly specified in `reason.root_reset` field
- *Generate statement optimization*: Uses Verilog `generate` blocks to reduce code duplication for multiple sticky flags

=== Generated Logic Example
<soc-net-reset-reason-logic>
```verilog
// Event normalization: convert all sources to LOW-active format
wire ext_rst_n_event_n = ext_rst_n;   // Already LOW-active
wire wdt_rst_n_event_n = wdt_rst_n;   // Already LOW-active
wire i3c_soc_rst_event_n = ~i3c_soc_rst;  // Convert HIGH-active to LOW-active

// 2-cycle clear controller and valid signal generation
reg        init_done;  // Set after first post-POR action
reg [1:0]  clr_sr;     // 2-cycle clear shift register
reg        valid_q;    // reason_valid register
wire       clr_en = |clr_sr;  // Clear enable (any bit in shift register)

// Sticky flags: async-set on event, sync-clear during clear window
reg [2:0] flags;

// Event vector for generate block
wire [2:0] src_event_n = {
    i3c_soc_rst_event_n,
    wdt_rst_n_event_n,
    ext_rst_n_event_n
};

// Reset reason flags generation using generate for loop
genvar reason_idx;
generate
    for (reason_idx = 0; reason_idx < 3; reason_idx = reason_idx + 1) begin : gen_reason
        always @(posedge clk_32k or negedge src_event_n[reason_idx]) begin
            if (!src_event_n[reason_idx]) begin
                flags[reason_idx] <= 1'b1;      // Async set on event assert
            end else if (clr_en) begin
                flags[reason_idx] <= 1'b0;      // Sync clear during clear window
            end
        end
    end
endgenerate

// Output gating: zeros until valid
assign reason_valid = valid_q;
assign reason = reason_valid ? flags : 3'b0;
```

== CODE GENERATION
<soc-net-reset-generation>
Reset controllers generate standalone modules that are instantiated in the main design, providing clean separation and reusability. Additionally, QSoC automatically generates a `reset_cell.v` template file containing the required reset component modules (`qsoc_rst_sync`, `qsoc_rst_pipe`, `qsoc_rst_count`).

=== Generated Code Structure
<soc-net-reset-code-structure>
The reset controller generates a dedicated module with:
1. Clock inputs (system clock and optional always-on clock for reason recording)
2. Reset source signal inputs with polarity documentation
3. Reset target signal outputs with polarity documentation
4. Optional reset reason output bus (if recording enabled)
5. Control signal inputs (test enable and optional reason clear signal)
6. Internal wire declarations for signal normalization
7. Reset logic using simplified DFF-based implementations
8. Optional reset reason recording logic (Per-source sticky flags)
9. Output assignment logic with proper signal combination

=== Variable Naming Conventions
<soc-net-reset-naming>
Reset logic uses simplified variable naming for improved readability:
- *Wire names*: `{source}_{target}_sync` (e.g., `por_rst_n_cpu_rst_n_sync`)
- *Generate blocks*: Use descriptive names for clarity:
  - Genvar: `reason_idx` (not generic `i`)
  - Block name: `gen_reason` (describes functionality)
- *Register names*: `{type}_{source}_{target}_{suffix}` format:
  - Flip-flops: `sync_por_rst_n_cpu_rst_n_ff`
  - Counters: `count_wdt_rst_n_cpu_rst_n_counter`
  - Count flags: `count_wdt_rst_n_cpu_rst_n_counting`
  - Stage wires: `sync_count_trig_rst_dma_rst_n_sync_stage1`
- *Component prefixes*: `sync` (qsoc_rst_sync), `count` (qsoc_rst_count), `pipe` (qsoc_rst_pipe)
- *No controller prefixes*: Variables use only essential identifiers for conciseness

=== Generated Modules
<soc-net-reset-modules>
The reset controller generates dedicated modules with component-based implementations:
- Component instantiation using qsoc_rst_sync, qsoc_rst_pipe, and qsoc_rst_count modules
- Async reset synchronizer (qsoc_rst_sync) when async attribute is specified
- Sync reset pipeline (qsoc_rst_pipe) when sync attribute is specified
- Counter-based reset release (qsoc_rst_count) when count attribute is specified
- Custom combinational logic for signal routing and polarity handling

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

=== Reset Component Modules
<soc-net-reset-component-modules>
The reset controller uses three standard component modules:

*qsoc_rst_sync*: Asynchronous reset synchronizer (active-low)
- Async assert, sync deassert after STAGE clocks
- Test bypass when test_enable=1
- Parameters: STAGE (>=2 recommended)

*qsoc_rst_pipe*: Synchronous reset pipeline (active-low)
- Adds STAGE cycle release delay to a sync reset
- Test bypass when test_enable=1
- Parameters: STAGE (>=1)

*qsoc_rst_count*: Counter-based reset release (active-low)
- After rst_in_n deasserts, count CYCLE then release
- Test bypass when test_enable=1
- Parameters: CYCLE (number of cycles before release)

=== Auto-generated Template File: reset_cell.v
<soc-net-reset-template-file>
When any `reset` primitive is present, QSoC ensures an output file `reset_cell.v` exists containing all required template cells:

- `qsoc_rst_sync` - Asynchronous reset synchronizer with test enable
- `qsoc_rst_pipe` - Synchronous reset pipeline with test enable
- `qsoc_rst_count` - Counter-based reset release with test enable

The generated file includes proper header comments, timescale directives, and include guards to prevent multiple inclusions.

File generation behavior:
- Always overwrites existing files with complete template set
- Use `--force` option for explicit overwrite confirmation

Users should replace these template implementations with their technology-specific standard cell implementations before using in production.

Example template structure:
```verilog
/**
 * @file reset_cell.v
 * @brief Template reset cells for QSoC reset primitives
 *
 * CAUTION: Please replace the templates in this file
 *          with your technology's standard-cell implementations
 *          before using in production.
 */

`timescale 1ns/10ps

`ifndef DEF_QSOC_RST_SYNC
`define DEF_QSOC_RST_SYNC
module qsoc_rst_sync #(
  parameter [31:0] STAGE = 32'h3
)(
  input  wire clk,
  input  wire rst_in_n,
  input  wire test_enable,
  output wire rst_out_n
);
  // Template implementation
endmodule
`endif

// Additional modules: qsoc_rst_pipe, qsoc_rst_count...
```

=== Diagram Output
<soc-net-reset-diagram>
Generates `.typ` circuit diagram alongside Verilog.

*Elements*: Sources → OR → ASYNC/SYNC/COUNT → Targets (with active levels/parameters)

*Files*: `<module>.v`, `<module>.typ` (compile: `typst compile <module>.typ`)

== BEST PRACTICES
<soc-net-reset-practices>

=== Design Guidelines
<soc-net-reset-design-guidelines>
- Use `async` component for most digital logic requiring synchronized reset release
- Use direct assignment only for simple pass-through or clock-independent paths
- Implement power-on-reset with `count` component for reliable startup timing
- Group related resets in the same controller for better organization
- Use descriptive reset source and target names

=== YAML Structure Guidelines
<soc-net-reset-yaml-guidelines>
- Always use singular forms (`source`, `target`) instead of plurals
- Specify clear type names instead of cryptic abbreviations
- Use structured parameters instead of string parsing
- Maintain consistent polarity naming (`low`/`high`)
- Include test_enable bypass for DFT compliance
