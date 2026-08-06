= Clock Controller Format
<clock-format>
The clock section defines clock controller primitives using a simplified format that eliminates complex type configurations. Clock operations are specified through direct attributes, with automatic selection of mux types based on signal presence.

#block(
  fill: rgb("#fffce8"),
  inset: 8pt,
  radius: 4pt,
  stroke: rgb("#a08410") + 0.5pt,
)[
  *Standalone module:* a `clock:` block generates a self-contained
  Verilog module named after `clock.name`. The generated module is NOT
  auto-instantiated by any parent netlist: the user instantiates it
  manually (or via `qsoc module import` followed by an `_inst.soc_net`
  entry) at the top level. This is by design.

  *Auto-input pattern:* a target's `link:` source that is neither a
  declared input nor another target gets auto-promoted to a fresh
  input port on the controller. This is how you wire software
  controlled signals (e.g. `*_sw_en`, `pll_lockout`) from outside the
  controller. A typo in the source name surfaces downstream as an
  unwired controller pin, not as a qsoc warning.
]

== Clock Overview
<soc-net-clock-overview>
Clock controllers manage clock distribution with two processing levels: link-level and target-level. Each level supports specific operations in a defined order, providing clear signal flow without explicit type enumeration.

Key features include:
- Two-level processing: link-level (icg→div→inv) and target-level (mux→icg→div→inv)
- Automatic mux type selection based on reset signal presence
- Direct attribute specification without complex type parsing
- ETH Zurich glitch-free mux implementation with DFT support
- Template RTL cells replaceable with foundry-specific IP
- Complete processing chain from multiple sources to single target

== Clock Structure
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
            inv: ~                    # Link-level inverter (exists = enabled)

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

== Processing Levels
<soc-net-clock-levels>
Clock controllers operate at two distinct processing levels with defined operation support:

#figure(
  align(center)[#table(
    columns: (0.2fr, 0.2fr, 0.2fr, 0.4fr),
    align: (auto, center, center, left),
    table.header([Operation], [Target Level], [Link Level], [Description]),
    table.hline(),
    [ICG], [Yes], [Yes], [Clock gating with enable signal],
    [DIV], [Yes], [Yes], [Clock division with configurable ratio],
    [INV], [Yes], [Yes], [Clock signal inversion],
    [STA_GUIDE], [Yes], [Yes], [STA guide buffer for timing constraints],
    [MUX], [Yes], [N/A], [Multi-source selection (>1 link only)],
  )],
  caption: [PROCESSING LEVEL SUPPORT],
  kind: table,
)

=== Processing Order
<soc-net-clock-processing-order>
Signal processing follows a defined order at each level, with STA guide buffers insertable after any processing stage:

*Link Level*: `source` → `icg` (+ `sta_guide`) → `div` (+ `sta_guide`) → `inv` (+ `sta_guide`) → output to target

*Target Level*: `mux` (+ `sta_guide`) → `icg` (+ `sta_guide`) → `div` (+ `sta_guide`) → `inv` (+ `sta_guide`) → final output

Each processing stage can independently include an optional STA guide buffer in serial (not parallel) configuration.

=== Operation Examples
<soc-net-clock-operations>
Operations are specified through direct attributes without type enumeration:

```yaml
# Target-level ICG (clock gating) with optional STA guide
target:
  gated_clk:
    freq: 800MHz
    icg:
      enable: clk_enable            # Gate enable signal
      sta_guide:                    # Optional STA guide after ICG
        cell: BUF_X2
        in: I
        out: Z
      polarity: high                # Enable polarity (default: high)
      reset: rst_n                  # Reset signal (optional)
      clock_on_reset: false         # Clock disabled during reset (default)
    link:
      pll_clk:                      # Direct connection

# Target-level divider with optional STA guide
target:
  div_clk:
    freq: 200MHz
    div:
      default: 4                    # Default division value (≥1)
      sta_guide:                    # Optional STA guide after DIV
        cell: BUF_CLK
        in: CK
        out: CKO
      width: 3                      # Divider width in bits (required for auto/dynamic modes)
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
      reset: rst_n                  # Reset signal
      # No 'value' signal = static mode
    link:
      pll_800m:                     # Direct connection

# Target-level inverter with optional STA guide
target:
  inv_clk:
    freq: 100MHz
    inv:                            # Inverter configuration (exists = enabled)
      sta_guide:                    # Optional STA guide after INV
        cell: BUF_INV
        in: A
        out: Y
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
          reset: rst_n
        inv:                        # Link-level inverter (exists = enabled)
          sta_guide:                # STA guide after the inverter
            cell: FOUNDRY_GUIDE_BUF # Foundry-specific cell
            in: A                   # Input port
            out: Y                  # Output port
            instance: u_pll_sta     # Instance name

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
      clock_on_reset: true        # Clock enabled during reset
    div:
      default: 2                  # Target-level divider
      reset: rst_n
    inv:                          # Target-level inverter (exists = enabled)
      sta_guide:                  # STA guide after inverter
        cell: BUF_INV
        in: A
        out: Y
    link:
      pll_clk:
        icg:
          enable: clk_en          # Link-level ICG
          clock_on_reset: false   # Clock disabled during reset (default)
          sta_guide:              # STA guide after ICG
            cell: BUF_ICG
            in: I
            out: Z
        div:
          default: 16             # Link-level divider first
          reset: rst_n
          sta_guide:              # STA guide after divider
            cell: BUF_DIV
            in: CK
            out: CKO
```

== ICG Configuration
<soc-net-clock-icg-config>
Clock gating cells (ICG) control clock distribution by enabling or disabling clock output based on enable signals. The ICG primitive supports both target-level and link-level configurations.

An `enable` or `reset` name that is not already a controller signal becomes an
input port, at either level.

=== ICG Parameters
<soc-net-clock-icg-params>
#figure(
  align(center)[#table(
    columns: (0.25fr, 0.15fr, 0.6fr),
    align: (auto, center, left),
    table.header([Parameter], [Required], [Description]),
    table.hline(),
    [enable], [Yes], [Gate enable signal name],
    [polarity], [No], [Enable polarity: "high" (default) or "low"],
    [test_enable],
    [No],
    [Test enable bypass signal (uses controller-level if not specified)],
    [reset], [No], [Reset signal name (active-low)],
    [clock_on_reset], [No], [Enable clock output during reset (default: false)],
    [sta_guide], [No], [STA guide buffer configuration after ICG (optional)],
  )],
  caption: [ICG CONFIGURATION PARAMETERS],
  kind: table,
)

=== ICG with Clock During Reset
<soc-net-clock-icg-clock-during-reset>
The `clock_on_reset` parameter controls whether the ICG outputs clock during reset. When enabled, the clock passes through during reset regardless of the enable signal state:

```yaml
# ICG with clock enabled during reset
target:
  boot_clk:
    freq: 24MHz
    icg:
      enable: boot_clk_en
      reset: rst_n
      clock_on_reset: true          # Clock output enabled during reset
    link:
      osc_24m:

# ICG with clock disabled during reset (default behavior)
target:
  cpu_clk:
    freq: 800MHz
    icg:
      enable: cpu_clk_en
      reset: rst_n
      clock_on_reset: false         # Clock output disabled during reset (default)
    link:
      pll_800m:
```

Generated Verilog uses the `CLOCK_DURING_RESET` parameter:
```verilog
qsoc_tc_clk_gate #(
    .CLOCK_DURING_RESET(1'b1),      // Clock enabled during reset
    .POLARITY(1'b1)
) u_boot_clk_target_icg (
    .clk(osc_24m),
    .en(boot_clk_en),
    .test_en(test_en),
    .rst_n(rst_n),
    .clk_out(boot_clk_icg_out)
);
```

*Warning*: the ICG routes both the `test_en` bypass and the
`CLOCK_DURING_RESET` path through a plain combinational 2:1 mux, not a
glitch-free one. Toggling `test_en`, or releasing `rst_n` while the clock is
high, can produce a runt pulse on the gated clock. The divide-by-1 bypass in
`qsoc_clk_div` uses the same plain mux with `test_en` in its select term. Change
`test_en` or `clock_on_reset` only while the source clock is stopped. Note also
that with `test_en` high the ICG output is the raw clock, so the gating latch
itself gets no toggle coverage.

== Divider Configuration
<soc-net-clock-divider-config>
Clock dividers support three operational modes determined by the presence of `value` and `valid` signals:

Odd division ratios keep a 50% duty cycle by toggling a second flop on the
falling edge of the source clock. The generated divider therefore instantiates
negative-edge flops whenever an odd ratio is reachable, which affects scan
chain construction and mixed-edge timing closure.

#figure(
  align(center)[#table(
    columns: (0.2fr, 0.2fr, 0.2fr, 0.4fr),
    align: (auto, center, center, left),
    table.header([Mode], [value], [valid], [Description]),
    table.hline(),
    [Static], [absent], [absent], [Constant division tied to `default` value],
    [Auto],
    [present],
    [absent],
    [Automatic handshake control via `qsoc_clk_div_auto`],
    [Dynamic],
    [present],
    [present],
    [Manual handshake control via `qsoc_clk_div`],
  )],
  caption: [DIVIDER MODE SELECTION],
  kind: table,
)

=== Divider Parameters
<soc-net-clock-divider-params>
#figure(
  align(center)[#table(
    columns: (0.2fr, 0.15fr, 0.65fr),
    align: (auto, center, left),
    table.header([Parameter], [Required], [Description]),
    table.hline(),
    [default],
    [Yes],
    [Default division value (≥1), used as reset default and static constant],
    [width],
    [Mode],
    [Divider width in bits (required for auto/dynamic modes, auto-calculated for static mode)],
    [reset],
    [],
    [Reset signal name (active-low), divider uses default value during reset],
    [enable], [No], [Enable signal name, disables divider when inactive],
    [test_enable], [No], [Test enable bypass signal],
    [clock_on_reset], [No], [Enable clock output during reset (default: false)],
    [value], [No], [Dynamic division input signal (empty = static mode)],
    [valid],
    [],
    [Division value valid strobe signal (auto-generated for static mode)],
    [ready], [No], [Division ready output status signal],
    [count], [No], [Division counter output for debugging],
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
      reset: rst_n                  # Reset to division by 8
      clock_on_reset: false         # Clock disabled during reset
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

=== Auto Mode (Simplified Dynamic Control)
<soc-net-clock-divider-auto>
When `value` is specified but `valid` is omitted, the divider automatically uses `qsoc_clk_div_auto` for simplified control:

```yaml
# Auto mode divider example
target:
  gpu_clk:
    freq: 200MHz
    div:
      default: 4                    # Reset default: 800MHz / 4 = 200MHz
      width: 4                      # 4-bit divider (max value 15)
      value: gpu_div_ratio          # Runtime division control (auto-sync & self-strobe div_valid)
      reset: rst_n
    link:
      pll_800m:                     # Variable: 800MHz / gpu_div_ratio
```

Generated Verilog uses the automatic handshake module:
```verilog
qsoc_clk_div_auto #(
    .WIDTH(4),
    .DEFAULT_VAL(4)
) u_gpu_clk_target_div (
    .clk(source_clock),
    .rst_n(rst_n),
    .div(gpu_div_ratio),          // Auto-sync & self-strobe div_valid
    // Note: No div_valid/div_ready ports - handled internally
    // ...
);
```

=== Mode Behaviors
<soc-net-clock-divider-mode-rules>
- *Reset Behavior*: All modes use `default` value during reset condition
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
- ERROR generated if `default > (2^width - 1)` or `default < 1`
- Examples:
  - `default: 100, width: 8` → OK (1 ≤ 100 ≤ 255)
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
      default: 100                  # Valid: 1 ≤ 100 ≤ 255
      width: 8                      # Required for dynamic mode
      value: div_control

  invalid_dynamic_clk:
    div:
      default: 300                  # ERROR: 300 > 255 for 8-bit width
      width: 8                      # Width too small
      value: div_control
```

== Clock Multiplexing
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

For `STD_MUX`, `select` is the zero-based ordinal of each source in `link`.
Unused binary encodings drive the output low. A standard mux accepts at most
4096 linked sources. Targets may share one `select` name: the port is declared
at the widest target's width, and narrower targets read its low bits.

=== Multiplexer Configuration
<soc-net-clock-mux-config>
```yaml
# Standard mux (no reset signal)
target:
  func_clk:
    freq: 100MHz
    div:
      default: 8
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
    test_enable: test_en          # DFT test enable (optional)
    test_clock: test_clock        # DFT test clock (optional)
```

For a glitch-free mux, `test_enable` and `test_clock` form one DFT
connection. A complete target-level pair overrides the controller-level
`test_enable`; otherwise a target `test_clock` uses the controller-level
enable. `select` and `test_clock` must be identifiers. `test_enable` accepts
an identifier or the exact constants `1'b0` and `1'b1`; a test clock with no
enable rejects the controller. An explicit `1'b0` keeps the DFT path disabled.
Without a target `test_clock`, the DFT mux stays disabled even when the
controller enable is used by gates or dividers.
Standard muxes and single-link targets warn and ignore target-level DFT
keys without changing the generated artifacts.

== Clock Properties
<soc-net-clock-properties>
Clock controller properties define inputs, processing, and outputs:

#figure(
  align(center)[#table(
    columns: (0.25fr, 0.25fr, 0.5fr),
    align: (auto, left, left),
    table.header([Property], [Type], [Description]),
    table.hline(),
    [name], [String], [Clock controller instance name - *REQUIRED*],
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
    [icg.sta_guide], [STA guide after ICG (optional)],
    [div], [Target-level division configuration (optional)],
    [div.sta_guide], [STA guide after divider (optional)],
    [inv], [Target-level inversion configuration (optional)],
    [inv.sta_guide], [STA guide after inverter (optional)],
    [mux],
    [Deprecated mux configuration (use select/reset/test_enable instead)],
    [mux.sta_guide], [STA guide after multiplexer (optional)],
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
    [icg],
    [Clock gating configuration with enable/polarity/reset and optional sta_guide (map format)],
    [icg.sta_guide], [STA guide after link-level ICG (optional)],
    [div],
    [Division configuration with default/width/reset and optional dynamic control (map format)],
    [div.sta_guide], [STA guide after link-level divider (optional)],
    [inv],
    [Clock inversion configuration (map format with enabled flag and optional sta_guide)],
    [inv.sta_guide], [STA guide after link-level inverter (optional)],
  )],
  caption: [CLOCK LINK PROPERTIES],
  kind: table,
)

== STA Guide Buffers
<soc-net-clock-sta-guide>
STA (Static Timing Analysis) guide buffers are foundry-specific cells inserted *serially* in clock processing chains to assist with timing constraints during physical design.

=== Purpose and Usage
<soc-net-clock-sta-purpose>
STA guide buffers serve several purposes in physical design flows:
- Provide insertion points for timing constraints during place and route
- Help STA tools with accurate timing analysis at specific clock distribution points
- Allow foundry-specific timing models to be applied at critical clock nodes
- Enable better correlation between pre-layout and post-layout timing

*Serial Implementation*: STA guide buffers are inserted *in-line* after each processing stage, becoming part of the main signal path. Each processing stage (MUX, ICG, DIV, INV) can independently include an STA guide buffer. This ensures signal flows through the STA buffer rather than observing it in parallel.

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
    [Instance name for the generated buffer. If empty, automatically generates `u_{target}_target_sta` for target-level or `u_{target}_{source}_sta` for link-level guides. Explicit names provide deterministic results for tools requiring specific instance references.],
  )],
  caption: [STA GUIDE BUFFER PARAMETERS],
  kind: table,
)

=== Configuration Examples
<soc-net-clock-sta-examples>
```yaml
# New STA guide architecture - buffers can be placed after each stage
target:
  cpu_clk:
    freq: 800MHz
    icg:
      enable: clk_en
      sta_guide:                    # STA guide after ICG
        cell: TSMC_CKBUF_X2
        in: I
        out: Z
        instance: u_cpu_icg_buf
    div:
      default: 2
      width: 2
      sta_guide:                    # STA guide after divider
        cell: TSMC_CKBUF_X4
        in: CK
        out: CKO
    link:
      pll_800m:

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
          reset: rst_n
          sta_guide:                # STA guide at end of chain
            cell: FOUNDRY_GUIDE_BUF # Generic foundry cell
            in: A                   # Input port
            out: Y                  # Output port
            instance: u_pll_dsp_sta # Instance name

# Combined target and link STA guides
target:
  complex_clk:
    freq: 100MHz
    link:
      source_clk:
        div:
          default: 4                # Default division value
          reset: rst_n
          sta_guide:                # STA guide after the link divider
            cell: FOUNDRY_CKBUF_X1  # Link buffer
            in: I
            out: Z
            instance: u_complex_link_sta
```

=== Generated Verilog
<soc-net-clock-sta-verilog>
STA guide buffers generate direct foundry cell instantiations in *serial configuration*:

```verilog
// Serial STA guide architecture - inserted in main signal path
// ICG with STA guide (serial connection)
wire cpu_clk_icg_pre_sta;        // Temporary signal: ICG output
wire cpu_clk_icg_out;            // Final signal: STA guide output
qsoc_tc_clk_gate u_cpu_clk_icg (
    .clk(clk_source),
    .en(clk_en),
    .clk_out(cpu_clk_icg_pre_sta)   // ICG outputs to temporary signal
);
TSMC_CKBUF_X2 u_cpu_icg_buf (
    .I(cpu_clk_icg_pre_sta),        // STA guide inputs from temporary
    .Z(cpu_clk_icg_out)             // STA guide outputs to final signal
);

// Divider with STA guide (serial connection)
wire cpu_clk_div_pre_sta;        // Temporary signal: DIV output
wire cpu_clk_div_out;            // Final signal: STA guide output
qsoc_clk_div u_cpu_clk_div (
    .clk(cpu_clk_icg_out),          // Input from previous stage
    .clk_out(cpu_clk_div_pre_sta)   // DIV outputs to temporary signal
);
TSMC_CKBUF_X4 u_cpu_clk_div_sta (
    .CK(cpu_clk_div_pre_sta),       // STA guide inputs from temporary
    .CKO(cpu_clk_div_out)           // STA guide outputs to final signal
);
assign cpu_clk = cpu_clk_div_out; // Final output maintains consistent name

// Link-level STA guide example (serial connection)
wire u_dsp_clk_pll_800m_pre_sta;  // Temporary signal before STA
wire clk_dsp_clk_from_pll_800m;    // Final signal after STA
// ... processing stage outputs to pre_sta signal ...
FOUNDRY_GUIDE_BUF u_dsp_clk_pll_800m_sta (
    .A(u_dsp_clk_pll_800m_pre_sta), // Input from temporary signal
    .Y(clk_dsp_clk_from_pll_800m)   // Output to final signal name
);
```

*Key Differences from Legacy Implementation*:
- STA guide buffers are inserted *in the main signal path* (serial)
- Processing stages output to `*_pre_sta` temporary signals when STA guide present
- STA guide buffers output to the expected final signal name
- Signal flow: `Processing → *_pre_sta → STA_Guide → *_out`
- Downstream logic sees consistent signal names regardless of STA guide presence

== Template RTL Cells
<soc-net-clock-templates>
QSoC generates these templates:
- `qsoc_tc_clk_buf` - Test-controllable clock buffer
- `qsoc_tc_clk_gate` - Test-controllable clock gate
- `qsoc_tc_clk_gate_pos` - Positive-edge clock gate
- `qsoc_tc_clk_gate_neg` - Negative-edge clock gate
- `qsoc_tc_clk_inv` - Clock inverter
- `qsoc_tc_clk_or2` - Two-input clock OR
- `qsoc_tc_clk_mux2` - Two-input clock multiplexer
- `qsoc_tc_clk_xor2` - Two-input clock XOR
- `qsoc_clk_div` - Clock divider with FSM control
- `qsoc_clk_div_auto` - Auto-width clock divider
- `qsoc_clk_or_tree` - Parameterized clock OR tree
- `qsoc_clk_mux_gf` - Glitch-free clock multiplexer
- `qsoc_clk_mux_raw` - Parameterized clock multiplexer

Templates include:
- Full FSM implementations, not toy assign statements
- Parameter validation and error checking
- Reset handling and test mode support
- Dynamic configuration with handshaking
- Cycle counters and status outputs

`qsoc_clk_mux_raw.NUM_INPUTS` must be a power of two. Generated controllers
pad unused high lanes with zero.

*Read the generated `clock_cell.v` file for actual interfaces.*
Replace with foundry cells before production use.

=== Auto-generated Template File: clock_cell.v
<soc-net-clock-template-file>
When missing, QSoC creates `clock_cell.v` with every template module listed in
@soc-net-clock-templates.

File generation behavior:
- Creates a missing file atomically
- Preserves any existing file byte-for-byte
- Replaces an existing file only when `--force` is explicit

*Important Notes:*
+ Inspect the generated interfaces before integrating technology cells.
+ Replace the templates with technology-specific implementations before production use.

== Port Sharing
<soc-net-clock-signal-dedup>
Within a generated clock controller, a port name names exactly one
port:

- Exact input reuse is legal: when several consumers name the same
  input with identical direction, width, and packed shape, the port
  declares once and every consumer connects to it. This covers a
  divider `value`/`valid`/`enable` shared between dividers, an input
  clock doubling as `test_clock`, and a scalar control doubling as a
  scalar `select`.
- Any other collision rejects the controller: an output colliding with
  an input, two outputs sharing a name, a width mismatch, or a scalar
  meeting a packed `[0:0]` declaration. In particular, divider outputs
  (`ready`, `count`) never feed a control input by carrying the same
  name; route such signals through explicit topology instead.
- A gating control accepts the constants `1'b0` and `1'b1` in place of
  a signal name: `test_enable`, a divider `enable`, `valid`, or
  `reset`, an ICG `enable` or `reset`, and a mux `reset`. A constant
  stays inline in the RTL and forms no port. Every other name must be
  a plain Verilog identifier, including `select`, `test_clock`, all
  outputs, and the divider `value`; anything else rejects the
  controller.
- A divider `valid` only carries meaning together with a dynamic
  `value`, but a static divider still declares the `valid` port it has
  always declared, so parent connections keep working. A unity divider
  (`default: 1` with no `value`) declares no `valid`; its other control
  ports are unaffected.
- Different controllers are separate Verilog modules and may reuse
  names freely.

A rejected controller reports the collision once and generates
nothing; existing output files stay byte-identical.

== Code Generation
<soc-net-clock-generation>
Run the generator with `qsoc generate verilog` (@verilog-generation).
Connectivity and width problems are reported as described in
@validation-format.

Clock controllers generate standalone modules that provide clean clock management infrastructure.

=== Generated Code Structure
<soc-net-clock-code-structure>
The clock controller generates a dedicated `clkctrl` module with:
+ Template RTL cell definitions (if not already defined)
+ Clock and test enable inputs with frequency documentation
+ Clock input signal declarations with frequency specifications
+ Clock target signal outputs with frequency documentation
+ Internal wire declarations for intermediate clock signals
+ Clock logic instantiations using template cells or foundry IP
+ Output assignment logic with proper multiplexing and inversion

=== Generated Code Example
<soc-net-clock-code-example>
```verilog
// Clock controller module (template cells in separate clock_cell.v file)

module clkctrl (
    /* Default clock */
    input  clk_sys,       /* Default synchronous clock */
    /* Clock inputs */
    input  osc_24m,       /* Clock input: osc_24m (24MHz) */
    input  pll_800m,      /* Clock input: pll_800m (800MHz) */
    /* Clock targets */
    output adc_clk,       /* Clock target: adc_clk (24MHz) */
    output uart_clk,      /* Clock target: uart_clk (200MHz) */
    /* Test enable */
    input  test_en        /* Test enable signal */
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
        .div_ready(),
        .clk_out(clk_uart_clk_from_pll_800m),
        .count()
    );

    /* Clock output assignments */
    assign adc_clk = clk_adc_clk_from_osc_24m;
    assign uart_clk = clk_uart_clk_from_pll_800m;

endmodule
```

=== Diagram Output
<soc-net-clock-diagram>
Generates `.typ` circuit diagram alongside Verilog.

*Elements*: Inputs → MUX → ICG/DIV/INV → Outputs (with frequencies/parameters)

*Files*: `<module>.v`, `<module>.typ` (compile: `typst compile <module>.typ`)

=== Syntax Summary
<soc-net-clock-syntax-summary>
Clock format supports two processing levels with distinct syntax patterns:

*Target Level* (key existence determines operation):
- `icg` - Map format with enable/polarity/reset/clock_on_reset and optional sta_guide
- `div` - Map format with default/reset/clock_on_reset (width auto-calculated for static mode) and optional sta_guide
- `inv` - Map format with enabled flag and optional sta_guide (or boolean for compatibility)
- `select` - Identifier (required for ≥2 links)
- `reset` - String (auto-selects GF_MUX when present)
- `test_enable` - Identifier or exact `1'b0`/`1'b1` constant
- `test_clock` - Identifier (GF_MUX DFT clock)

*Link Level* (key existence determines operation):
- `icg` - Map format with enable/polarity/reset/clock_on_reset and optional sta_guide
- `div` - Map format with default/reset/clock_on_reset (width auto-calculated for static mode) and optional sta_guide
- `inv` - Map format with enabled flag and optional sta_guide (or boolean for compatibility)
- Pass-through: No attributes specified

A link accepts only `icg`, `div`, and `inv`, or the scalar `inv`. `inv: false`
and `enabled: false` disable the inverter; a present inverter is one cell, so
its `sta_guide` requires `cell`, `in`, and `out` together.

== Best Practices
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
