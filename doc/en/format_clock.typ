= CLOCK CONTROLLER FORMAT
<clock-format>
The clock section defines clock controller primitives using a simplified format that eliminates complex type configurations. Clock operations are specified through direct attributes, with automatic selection of mux types based on signal presence.

== CLOCK OVERVIEW
<soc-net-clock-overview>
Clock controllers manage clock distribution with two processing levels: link-level and target-level. Each level supports specific operations in a defined order, providing clear signal flow without explicit type enumeration.

Key features include:
- Two-level processing: link-level (div→inv) and target-level (mux→icg→div→inv)
- Automatic mux type selection based on reset signal presence
- Direct attribute specification without complex type parsing
- ETH Zurich glitch-free mux implementation with DFT support
- Template RTL cells replaceable with foundry-specific IP
- Complete processing chain from multiple sources to single target

== CLOCK STRUCTURE
<soc-net-clock-structure>
Clock controllers use direct attribute specification without explicit type enumeration. Processing operations are determined by attribute presence rather than complex type parsing:

```yaml
# Clock controller with two-level processing
clock:
  - name: soc_clk_ctrl                # Controller instance name (required)
    test_enable: test_en              # Test enable bypass signal (optional)
    input:
      osc_24m:
        freq: 24MHz
      pll_800m:
        freq: 800MHz
      test_clk:
        freq: 100MHz
    target:
      # Simple pass-through
      adc_clk:
        freq: 24MHz
        link:
          osc_24m:                    # Direct connection (no attributes)

      # Target-level ICG
      dbg_clk:
        freq: 800MHz
        icg:                          # Target-level ICG (uses controller test_enable)
          enable: dbg_clk_en
        link:
          pll_800m:                   # Direct connection

      # Target-level divider
      uart_clk:
        freq: 200MHz
        div:                          # Target-level divider (static mode)
          default: 4                  # Default division value (auto width = 3 bits)
          reset: rst_n
        link:
          pll_800m:                   # Direct connection

      # Link-level processing
      slow_clk_n:
        freq: 12MHz
        link:
          osc_24m:
            div:                      # Link-level divider (static mode)
              default: 2              # Default division value (auto width = 2 bits)
              reset: rst_n
            inv: true                 # Link-level inverter (boolean)

      # Alternative link-level inverter syntax
      alt_clk_n:
        freq: 12MHz
        link:
          osc_24m: inv              # Link-level inverter (scalar)

      # Multi-source without reset (auto STD_MUX)
      func_clk:
        freq: 100MHz
        div:                          # Target-level divider
          default: 8                  # Default division value
          width: 4                    # Required: divider width in bits
          reset: rst_n
        link:
          pll_800m:                   # Direct connection
          test_clk:                   # Direct connection
        select: func_sel              # No reset → auto STD_MUX

      # Multi-source with reset (auto GF_MUX)
      safe_clk:
        freq: 24MHz
        link:
          osc_24m:                    # Direct connection
          test_clk:                   # Direct connection
        select: safe_sel
        reset: sys_rst_n              # Has reset → auto GF_MUX
        test_clock: test_clock        # DFT test clock (uses controller test_enable)
```

== PROCESSING LEVELS
<soc-net-clock-levels>
Clock controllers operate at two distinct processing levels with defined operation support:

#figure(
  align(center)[#table(
    columns: (0.2fr, 0.2fr, 0.2fr, 0.4fr),
    align: (auto, center, center, left),
    table.header([Operation], [Target Level], [Link Level], [Description]),
    table.hline(),
    [ICG], [✓], [✗], [Clock gating with enable signal],
    [DIV], [✓], [✓], [Clock division with configurable ratio],
    [INV], [✓], [✓], [Clock signal inversion],
    [STA_GUIDE], [✓], [✓], [STA guide buffer for timing constraints],
    [MUX], [✓], [N/A], [Multi-source selection (>1 link only)],
  )],
  caption: [PROCESSING LEVEL SUPPORT],
  kind: table,
)

=== Processing Order
<soc-net-clock-processing-order>
Signal processing follows a defined order at each level:

*Link Level*: `source` → `icg` → `div` → `inv` → `sta_guide` → output to target

*Target Level*: `mux` → `icg` → `div` → `inv` → `sta_guide` → final output

=== Operation Examples
<soc-net-clock-operations>
Operations are specified through direct attributes without type enumeration:

```yaml
# Target-level ICG (clock gating)
target:
  gated_clk:
    freq: 800MHz
    icg:
      enable: clk_enable            # Gate enable signal
      polarity: high                # Enable polarity (default: high)
    link:
      pll_clk:                      # Direct connection

# Target-level divider
target:
  div_clk:
    freq: 200MHz
    div:
      default: 4                    # Default division value (required, ≥1)
      width: 3                      # Divider width in bits (required)
      reset: rst_n                  # Reset signal
      value: uart_div_value         # Dynamic division control input (optional)
      valid: uart_div_valid         # Division value valid signal (optional)
      ready: uart_div_ready         # Division ready output signal (optional)
    link:
      pll_clk:                      # Direct connection

# Static divider (no value signal)
target:
  static_clk:
    freq: 100MHz
    div:
      default: 8                    # Static division, tied to constant 8
      width: 4                      # Divider width in bits (required)
      reset: rst_n                  # Reset signal
      # No 'value' signal = static mode
    link:
      pll_800m:                     # Direct connection

# Target-level inverter
target:
  inv_clk:
    freq: 100MHz
    inv: true                       # Clock inversion
    link:
      pll_clk:                      # Direct connection

# Target-level STA guide buffer
target:
  cpu_clk:
    freq: 800MHz
    sta_guide:                      # STA guide for timing constraints
      cell: TSMC_CKBUF_X2           # Foundry-specific cell name
      in: I                         # Input port name
      out: Z                        # Output port name
      instance: u_cpu_clk_sta       # Instance name
    link:
      pll_clk:                      # Direct connection

# Link-level processing (multiple syntax options)
target:
  processed_clk:
    freq: 50MHz
    link:
      pll_clk:
        div:
          default: 16               # Default division value
          width: 5                  # Divider width in bits (required)
          reset: rst_n
        inv: true                   # Link-level inverter (boolean)
        sta_guide:                  # Link-level STA guide
          cell: FOUNDRY_GUIDE_BUF   # Foundry-specific cell
          in: A                     # Input port
          out: Y                    # Output port
          instance: u_pll_sta       # Instance name

# Alternative compact syntax for link-level inverter
target:
  compact_clk:
    freq: 50MHz
    link:
      pll_clk: inv                  # Link-level inverter only (scalar)

# Combined target and link processing
target:
  complex_clk:
    freq: 25MHz
    icg:
      enable: clk_en              # Target-level ICG
    div:
      default: 2                  # Target-level divider
      width: 2                    # Required: divider width in bits
      reset: rst_n
    inv: true                     # Target-level inverter
    link:
      pll_clk:
        div:
          default: 16             # Link-level divider first
          width: 5                # Required: divider width in bits
          reset: rst_n
```

== DIVIDER CONFIGURATION
<soc-net-clock-divider-config>
Clock dividers support two operational modes: static (constant division) and dynamic (runtime controllable division). The mode is determined by the presence of the `value` signal.

=== Divider Parameters
<soc-net-clock-divider-params>
#figure(
  align(center)[#table(
    columns: (0.2fr, 0.15fr, 0.65fr),
    align: (auto, center, left),
    table.header([Parameter], [Required], [Description]),
    table.hline(),
    [default],
    [✓],
    [Default division value (≥1), used as reset default and static constant],
    [width],
    [◐],
    [Divider width in bits (required for dynamic mode, auto-calculated for static mode)],
    [reset],
    [],
    [Reset signal name (active-low), divider uses default value during reset],
    [enable], [], [Enable signal name, disables divider when inactive],
    [test_enable], [], [Test enable bypass signal],
    [value], [], [Dynamic division input signal (empty = static mode)],
    [valid], [], [Division value valid strobe signal],
    [ready], [], [Division ready output status signal],
    [count], [], [Division counter output for debugging],
  )],
  caption: [DIVIDER CONFIGURATION PARAMETERS],
  kind: table,
)

=== Static Mode (Constant Division)
<soc-net-clock-divider-static>
When no `value` signal is specified, the divider operates in static mode with constant division:

```yaml
# Static divider example
target:
  uart_clk:
    freq: 100MHz
    div:
      default: 8                    # Constant division by 8
      width: 4                      # 4-bit divider (max value 15)
      reset: rst_n                  # Reset to division by 8
    link:
      pll_800m:                     # 800MHz / 8 = 100MHz
```

Generated Verilog ties the division input to the default constant:
```verilog
qsoc_clk_div #(
    .WIDTH(4),
    .DEFAULT_VAL(8)
) u_uart_clk_target_div (
    .clk(source_clock),
    .rst_n(rst_n),
    .div(4'd8),                   // Tied to constant
    .div_valid(1'b1),
    // ...
);
```

=== Dynamic Mode (Runtime Control)
<soc-net-clock-divider-dynamic>
When a `value` signal is specified, the divider accepts runtime division control:

```yaml
# Dynamic divider example
target:
  cpu_clk:
    freq: 400MHz
    div:
      default: 2                    # Reset default: 800MHz / 2 = 400MHz
      width: 8                      # 8-bit divider (max value 255)
      value: cpu_div_ratio          # Runtime division control
      valid: cpu_div_valid          # Optional: division update strobe
      ready: cpu_div_ready          # Optional: division ready status
      reset: rst_n
    link:
      pll_800m:                     # Variable: 800MHz / cpu_div_ratio
```

Generated Verilog connects the runtime control signals:
```verilog
qsoc_clk_div #(
    .WIDTH(8),
    .DEFAULT_VAL(2)
) u_cpu_clk_target_div (
    .clk(source_clock),
    .rst_n(rst_n),
    .div(cpu_div_ratio),          // Connected to runtime input
    .div_valid(cpu_div_valid),    // Connected to valid signal
    .div_ready(cpu_div_ready),    // Connected to ready output
    // ...
);
```

=== Mode Selection Rules
<soc-net-clock-divider-mode-rules>
- *Static Mode*: `value` parameter absent or empty → division tied to `default` constant
- *Dynamic Mode*: `value` parameter present → division connected to runtime signal
- *Reset Behavior*: Both modes use `default` value during reset condition
- *Bypass Operation*: Division by 1 automatically enables bypass mode in the primitive

=== Width Calculation and Validation
<soc-net-clock-divider-width-validation>
The divider configuration enforces different width requirements based on the operational mode:

*Static Mode Width Calculation:*
- Width is automatically calculated from `default` value: `width = ceil(log2(default + 1))`
- Manual width specification overrides the calculated value
- Examples:
  - `default: 8` → auto width = 4 bits (8 < 2^4 = 16)
  - `default: 15` → auto width = 4 bits (15 < 2^4 = 16)
  - `default: 16` → auto width = 5 bits (16 < 2^5 = 32)

*Dynamic Mode Width Requirements:*
- Width must be explicitly specified (no auto-calculation)
- System validates that `default` value fits within specified width
- ERROR generated if `default > (2^width - 1)`
- Examples:
  - `default: 100, width: 8` → OK (100 < 255)
  - `default: 256, width: 8` → ERROR (256 > 255)

```yaml
# Static mode examples
target:
  auto_width_clk:
    div:
      default: 8                    # Auto width = 4 bits
      # No width needed

  manual_width_clk:
    div:
      default: 8                    # Manual override
      width: 6                      # Uses 6 bits instead of auto 4

# Dynamic mode examples
target:
  valid_dynamic_clk:
    div:
      default: 100                  # Valid: 100 < 255
      width: 8                      # Required for dynamic mode
      value: div_control

  invalid_dynamic_clk:
    div:
      default: 300                  # ERROR: 300 > 255 for 8-bit width
      width: 8                      # Width too small
      value: div_control
```

== CLOCK MULTIPLEXING
<soc-net-clock-mux>
Targets with multiple links (≥2) automatically generate multiplexers. Mux type is determined by reset signal presence:

#figure(
  align(center)[#table(
    columns: (0.25fr, 0.25fr, 0.5fr),
    align: (auto, left, left),
    table.header([Condition], [Mux Type], [Characteristics]),
    table.hline(),
    [No reset signal], [STD_MUX], [Combinational mux, immediate switching],
    [Has reset signal], [GF_MUX], [Glitch-free mux with synchronization],
  )],
  caption: [AUTOMATIC MUX TYPE SELECTION],
  kind: table,
)

=== Multiplexer Configuration
<soc-net-clock-mux-config>
```yaml
# Standard mux (no reset signal)
target:
  func_clk:
    freq: 100MHz
    div:
      ratio: 8
      reset: rst_n
    link:
      pll_800m:                   # Direct connection
      test_clk:                   # Direct connection
    select: func_sel              # Required for multi-link
    # No reset → automatic STD_MUX selection

# Glitch-free mux (has reset signal)
target:
  safe_clk:
    freq: 24MHz
    link:
      osc_24m:                    # Direct connection
      test_clk:                   # Direct connection
    select: safe_sel              # Required for multi-link
    reset: sys_rst_n              # Reset signal → automatic GF_MUX selection
    test_enable: test_enable      # DFT test enable (optional)
    test_clock: test_clock        # DFT test clock (optional)
```

== CLOCK PROPERTIES
<soc-net-clock-properties>
Clock controller properties define inputs, processing, and outputs:

#figure(
  align(center)[#table(
    columns: (0.25fr, 0.25fr, 0.5fr),
    align: (auto, left, left),
    table.header([Property], [Type], [Description]),
    table.hline(),
    [name], [String], [Clock controller instance name - *REQUIRED*],
    [clock], [String], [Default synchronous clock for operations - *REQUIRED*],
    [input], [Map], [Clock input definitions with frequency specs (required)],
    [target], [Map], [Clock target definitions with processing (required)],
  )],
  caption: [CLOCK CONTROLLER PROPERTIES],
  kind: table,
)

=== Target Properties
<soc-net-clock-target-properties>
Clock targets define output signals with two-level processing:

#figure(
  align(center)[#table(
    columns: (0.3fr, 0.7fr),
    align: (auto, left),
    table.header([Property], [Description]),
    table.hline(),
    [freq], [Target frequency for SDC generation (required)],
    [link], [Map of source connections (required)],
    [icg], [Target-level clock gating configuration (optional)],
    [div], [Target-level division configuration (optional)],
    [inv], [Target-level inversion flag (optional)],
    [sta_guide], [Target-level STA guide buffer configuration (optional)],
    [select], [Mux select signal (required for ≥2 links)],
    [reset], [Reset signal for GF_MUX auto-selection (optional)],
    [test_enable], [DFT test enable signal (GF_MUX only, optional)],
    [test_clock], [DFT test clock signal (GF_MUX only, optional)],
  )],
  caption: [CLOCK TARGET PROPERTIES],
  kind: table,
)

=== Link Properties
<soc-net-clock-link-properties>
Link-level processing uses key existence for operations:

#figure(
  align(center)[#table(
    columns: (0.3fr, 0.7fr),
    align: (auto, left),
    table.header([Property], [Description]),
    table.hline(),
    [div],
    [Division configuration with default/width/reset and optional dynamic control (map format)],
    [inv], [Clock inversion (key existence enables inversion)],
    [sta_guide], [STA guide buffer configuration with foundry cell details],
  )],
  caption: [CLOCK LINK PROPERTIES],
  kind: table,
)

== STA GUIDE BUFFERS
<soc-net-clock-sta-guide>
STA (Static Timing Analysis) guide buffers are foundry-specific cells inserted at the end of clock processing chains to assist with timing constraints during physical design.

=== Purpose and Usage
<soc-net-clock-sta-purpose>
STA guide buffers serve several purposes in physical design flows:
- Provide insertion points for timing constraints during place and route
- Help STA tools with accurate timing analysis at specific clock distribution points
- Allow foundry-specific timing models to be applied at critical clock nodes
- Enable better correlation between pre-layout and post-layout timing

Guide buffers are inserted as the final element in both target-level and link-level processing chains, ensuring they appear at the ultimate output of clock generation logic.

=== Configuration Parameters
<soc-net-clock-sta-config>
STA guide buffer configuration requires four parameters:

#figure(
  align(center)[#table(
    columns: (0.25fr, 0.75fr),
    align: (auto, left),
    table.header([Parameter], [Description]),
    table.hline(),
    [cell],
    [Foundry-specific cell name (e.g., "TSMC_CKBUF_X2", "FOUNDRY_GUIDE_BUF")],
    [in], [Input port name of the foundry cell (e.g., "I", "A", "CK")],
    [out], [Output port name of the foundry cell (e.g., "Z", "Y", "Q")],
    [instance],
    [Instance name for the generated buffer (e.g., "u_cpu_clk_sta")],
  )],
  caption: [STA GUIDE BUFFER PARAMETERS],
  kind: table,
)

=== Configuration Examples
<soc-net-clock-sta-examples>
```yaml
# Target-level STA guide with TSMC buffer
target:
  cpu_clk:
    freq: 800MHz
    sta_guide:
      cell: TSMC_CKBUF_X2           # TSMC foundry cell
      in: I                         # Input port name
      out: Z                        # Output port name
      instance: u_cpu_clk_sta_guide # Instance name
    link:
      pll_800m:                     # Source connection

# Link-level STA guide in processing chain
target:
  dsp_clk:
    freq: 400MHz
    link:
      pll_800m:
        icg:
          enable: dsp_enable        # Link-level gating
        div:
          default: 2                # Link-level division
          width: 2                  # Required: divider width in bits
          reset: rst_n
        sta_guide:                  # STA guide at end of chain
          cell: FOUNDRY_GUIDE_BUF   # Generic foundry cell
          in: A                     # Input port
          out: Y                    # Output port
          instance: u_pll_dsp_sta   # Instance name

# Combined target and link STA guides
target:
  complex_clk:
    freq: 100MHz
    sta_guide:                      # Target-level STA guide
      cell: TSMC_CKBUF_X1           # Target buffer
      in: I
      out: Z
      instance: u_complex_target_sta
    link:
      source_clk:
        div:
          default: 4                # Default division value
          width: 3                  # Required: divider width in bits
          reset: rst_n
        sta_guide:                  # Link-level STA guide
          cell: TSMC_CKBUF_X1       # Link buffer
          in: I
          out: Z
          instance: u_complex_link_sta
```

=== Generated Verilog
<soc-net-clock-sta-verilog>
STA guide buffers generate direct foundry cell instantiations:

```verilog
// Target-level STA guide example
wire cpu_clk_sta_out;
TSMC_CKBUF_X2 u_cpu_clk_target_sta (
    .I(intermediate_signal),
    .Z(cpu_clk_sta_out)
);
assign cpu_clk = cpu_clk_sta_out;

// Link-level STA guide example
wire u_dsp_clk_pll_800m_sta_wire;
FOUNDRY_GUIDE_BUF u_dsp_clk_pll_800m_sta (
    .A(previous_stage_output),
    .Y(u_dsp_clk_pll_800m_sta_wire)
);
assign clk_dsp_clk_from_pll_800m = u_dsp_clk_pll_800m_sta_wire;
```

== TEMPLATE RTL CELLS
<soc-net-clock-templates>
Clock controllers generate template RTL cells that serve as behavioral placeholders:

```verilog
// Standard clock mux (combinational, N-input)
module QSOC_CLKMUX_STD_CELL #(
    parameter NUM_INPUTS = 2
) (
    input  [NUM_INPUTS-1:0] clk_in,
    input  [1:0] clk_sel,            // Supports up to 4 inputs
    output clk_out
);
    assign clk_out = (clk_sel == 0) ? clk_in[0] :
                     (clk_sel == 1) ? clk_in[1] :
                     (clk_sel == 2) ? clk_in[2] : clk_in[3];
endmodule

// Glitch-free clock mux (N-input with synchronization)
module QSOC_CLKMUX_GF_CELL #(
    parameter NUM_INPUTS = 2,
    parameter NUM_SYNC_STAGES = 2,
    parameter CLOCK_DURING_RESET = 1
) (
    input  [NUM_INPUTS-1:0] clk_in,
    input  test_clk,
    input  test_en,
    input  async_rst_n,
    input  [1:0] async_sel,          // Supports up to 4 inputs
    output clk_out
);
    // Simplified behavioral model
    assign clk_out = test_en ? test_clk :
                     (async_sel == 0) ? clk_in[0] :
                     (async_sel == 1) ? clk_in[1] :
                     (async_sel == 2) ? clk_in[2] : clk_in[3];
endmodule

// Clock gate cell
module QSOC_CKGATE_CELL #(
    parameter enable_reset = 1'b0
) (
    input  clk,
    input  en,
    input  test_en,
    input  rst_n,
    output clk_out
);
    // Gate enable with test and reset support
    wire final_en = test_en | (!rst_n & enable_reset) | (rst_n & en);
    assign clk_out = clk & final_en;
endmodule

// Clock divider cell
module QSOC_CLKDIV_CELL #(
    parameter integer width = 4,
    parameter integer default_val = 1,
    parameter enable_reset = 1'b0
) (
    input  clk,
    input  rst_n,
    input  en,
    input  test_en,
    input  [width-1:0] div,
    input  div_valid,
    output div_ready,
    output clk_out,
    output [width-1:0] count
);
    // Simplified behavioral model
    assign div_ready = 1'b1;
    assign clk_out = test_en ? clk : clk;  // Template
endmodule

// Clock inverter cell
module QSOC_CKINV_CELL (
    input  clk_in,
    output clk_out
);
    assign clk_out = ~clk_in;
endmodule
```

=== Auto-generated Template File: clock_cell.v
<soc-net-clock-template-file>
When any `clock` primitive is present, QSoC ensures an output file `clock_cell.v` exists containing all required template cells:

- `QSOC_CKGATE_CELL` - Clock gate with test enable
- `QSOC_CKINV_CELL` - Clock inverter
- `QSOC_CLKOR2_CELL` - 2-input clock OR gate
- `QSOC_CLKMUX2_CELL` - 2-input clock mux
- `QSOC_CLKXOR2_CELL` - 2-input clock XOR gate
- `QSOC_CLKDIV_CELL` - Configurable clock divider
- `QSOC_CLKMUX_GF_CELL` - N-input glitch-free mux
- `QSOC_CLKMUX_STD_CELL` - N-input standard mux
- `QSOC_CLKOR_TREE` - N-input clock OR tree

File generation behavior:
- Creates new file with header and all templates if missing
- Appends only missing templates to existing file
- Leaves complete files unchanged
- Use `--force` option to overwrite existing files completely

All templates use pure Verilog 2005 syntax with behavioral models:
- No SystemVerilog features (`always @(*)` instead of `always_comb`)
- Standard data types (`wire`/`reg` instead of `logic`)
- Integer parameters instead of typed parameters
- Active-low reset signals use `rst_n` naming convention
- Simplified parameter names for clarity (`width`, `default_val`, `enable_reset`)

Template cells must be replaced with foundry-specific implementations before production use.

== SIGNAL DEDUPLICATION
<soc-net-clock-signal-dedup>
Clock generators implement automatic signal deduplication to prevent duplicate port declarations in generated Verilog modules.

=== Deduplication Features
<soc-net-clock-dedup-features>
- Port deduplication: Same-name signals appear only once in module ports
- Output-priority: Output signals take precedence over input signals when conflicts occur
- QSet-based tracking: Efficient duplicate detection across all signal types
- Parameter unification: All qsoc_tc_clk_gate instances use CLOCK_DURING_RESET parameter
- Error detection: Duplicate output target names generate ERROR messages

=== Implementation Details
<soc-net-clock-dedup-details>
The deduplication system uses QSet containers to track signal names across:
- ICG control signals (enable, test_enable, reset)
- MUX control signals (select, reset, test_enable, test_clock)
- Divider control signals (division ratio, enable, reset)
- Clock input signals (skips duplicates with default clock)

When the same signal name is used across multiple targets, only one port declaration is generated in the final Verilog module.

== CODE GENERATION
<soc-net-clock-generation>
Clock controllers generate standalone modules that provide clean clock management infrastructure.

=== Generated Code Structure
<soc-net-clock-code-structure>
The clock controller generates a dedicated `clkctrl` module with:
1. Template RTL cell definitions (if not already defined)
2. Clock and test enable inputs with frequency documentation
3. Clock input signal declarations with frequency specifications
4. Clock target signal outputs with frequency documentation
5. Internal wire declarations for intermediate clock signals
6. Clock logic instantiations using template cells or foundry IP
7. Output assignment logic with proper multiplexing and inversion

=== Generated Code Example
<soc-net-clock-code-example>
```verilog
// Template cells generated first
module CKGATE_CELL (...);
// ... template implementations

module clkctrl (
    /* Default clock */
    input  clk_sys,       /**< Default synchronous clock */
    /* Clock inputs */
    input  osc_24m,       /**< Clock input: osc_24m (24MHz) */
    input  pll_800m,      /**< Clock input: pll_800m (800MHz) */
    /* Clock targets */
    output adc_clk,       /**< Clock target: adc_clk (24MHz) */
    output uart_clk,      /**< Clock target: uart_clk (200MHz) */
    /* Test enable */
    input  test_en        /**< Test enable signal */
);

    /* Wire declarations for clock connections */
    wire clk_adc_clk_from_osc_24m;
    wire clk_uart_clk_from_pll_800m;

    /* Clock logic instances */
    // osc_24m -> adc_clk: Direct connection
    assign clk_adc_clk_from_osc_24m = osc_24m;

    // pll_800m -> uart_clk: Target-level divider
    qsoc_clk_div #(
        .WIDTH(8),
        .DEFAULT_VAL(4),
        .CLOCK_DURING_RESET(1'b0)
    ) u_uart_clk_target_div (
        .clk(pll_800m),
        .rst_n(rst_n),
        .en(1'b1),
        .test_en(test_en),
        .div(8'd4),                     // Static mode: tied to constant
        .div_valid(1'b1),
        .div_ready(),
        .clk_out(clk_uart_clk_from_pll_800m),
        .count()
    );

    /* Clock output assignments */
    assign adc_clk = clk_adc_clk_from_osc_24m;
    assign uart_clk = clk_uart_clk_from_pll_800m;

endmodule
```

=== Syntax Summary
<soc-net-clock-syntax-summary>
Clock format supports two processing levels with distinct syntax patterns:

*Target Level* (key existence determines operation):
- `icg` - Map format with enable/polarity
- `div` - Map format with default/width/reset and optional dynamic signals
- `inv` - Key existence enables inversion
- `sta_guide` - Map format with foundry cell details
- `select` - String (required for ≥2 links)
- `reset` - String (auto-selects GF_MUX when present)
- `test_enable`, `test_clock` - String (GF_MUX DFT signals)

*Link Level* (key existence determines operation):
- `div` - Map format with default/width/reset and optional dynamic signals
- `inv` - Key existence enables inversion
- `sta_guide` - Map format with foundry cell details
- Pass-through: No attributes specified

== BEST PRACTICES
<soc-net-clock-practices>

=== Processing Strategy
<soc-net-clock-processing-strategy>
- Use link-level processing for per-source operations (individual clock conditioning)
- Use target-level processing for final output operations (common to all sources)
- Reset signal presence automatically selects mux type (GF_MUX vs STD_MUX)
- Include DFT signals (test_enable, test_clock) for glitch-free mux when needed

=== Syntax Guidelines
<soc-net-clock-syntax-guidelines>
- Always specify input frequencies for proper SDC generation
- Use clear clock names indicating purpose and frequency
- Specify attributes only when operations are needed
- Group related clocks in the same controller
- Reset signals default to active-low polarity
- Include individual `test_enable` signals for comprehensive DFT support

=== Design Guidelines
<soc-net-clock-design-guidelines>
- Ensure target frequencies match division ratios mathematically
- Use proper SI units (Hz, kHz, MHz, GHz)
- Consider clock domain crossing requirements for multi-clock systems
- Replace template cells with foundry-specific implementations for production
