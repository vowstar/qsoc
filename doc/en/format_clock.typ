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
  - name: soc_clk_ctrl
    clock: clk_sys                    # Default synchronous clock
    test_en: test_en                  # Test enable bypass signal (optional)
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
        icg:                          # Target-level ICG
          enable: dbg_clk_en
        link:
          pll_800m:                   # Direct connection

      # Target-level divider
      uart_clk:
        freq: 200MHz
        div:                          # Target-level divider
          ratio: 4
          reset: rst_n
        link:
          pll_800m:                   # Direct connection

      # Link-level processing
      slow_clk_n:
        freq: 12MHz
        link:
          osc_24m:
            div:                      # Link-level divider
              ratio: 2
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
          ratio: 8
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
        test_enable: test_enable      # DFT test enable
        test_clock: test_clock        # DFT test clock
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
    [MUX], [✓], [N/A], [Multi-source selection (>1 link only)],
  )],
  caption: [PROCESSING LEVEL SUPPORT],
  kind: table,
)

=== Processing Order
<soc-net-clock-processing-order>
Signal processing follows a defined order at each level:

*Link Level*: `source` → `div` → `inv` → output to target

*Target Level*: `mux` → `icg` → `div` → `inv` → final output

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
      ratio: 4                      # Division ratio (≥2)
      reset: rst_n                  # Reset signal
    link:
      pll_clk:                      # Direct connection

# Target-level inverter
target:
  inv_clk:
    freq: 100MHz
    inv: true                       # Clock inversion
    link:
      pll_clk:                      # Direct connection

# Link-level processing (multiple syntax options)
target:
  processed_clk:
    freq: 50MHz
    link:
      pll_clk:
        div:
          ratio: 16                 # Link-level divider
          reset: rst_n
        inv: true                   # Link-level inverter (boolean)

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
      ratio: 2                    # Target-level divider
      reset: rst_n
    inv: true                     # Target-level inverter
    link:
      pll_clk:
        div:
          ratio: 16               # Link-level divider first
          reset: rst_n
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
    [test_en], [String], [Test enable bypass signal (optional)],
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
    [div], [Division configuration with ratio and reset (map format)],
    [inv], [Clock inversion (key existence enables inversion)],
  )],
  caption: [CLOCK LINK PROPERTIES],
  kind: table,
)

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
    parameter integer default_val = 0,
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

All templates use pure Verilog 2005 syntax with behavioral models:
- No SystemVerilog features (`always @(*)` instead of `always_comb`)
- Standard data types (`wire`/`reg` instead of `logic`)
- Integer parameters instead of typed parameters
- Active-low reset signals use `rst_n` naming convention
- Simplified parameter names for clarity (`width`, `default_val`, `enable_reset`)

Template cells must be replaced with foundry-specific implementations before production use.

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
    QSOC_CLKDIV_CELL #(
        .width(8),
        .default_val(4),
        .enable_reset(1'b0)
    ) u_uart_clk_div (
        .clk(pll_800m),
        .rst_n(rst_n),
        .en(1'b1),
        .test_en(test_en),
        .div(8'd4),
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
- `div` - Map format with ratio/reset
- `inv` - Key existence enables inversion
- `select` - String (required for ≥2 links)
- `reset` - String (auto-selects GF_MUX when present)
- `test_enable`, `test_clock` - String (GF_MUX DFT signals)

*Link Level* (key existence determines operation):
- `div` - Map format with ratio/reset
- `inv` - Key existence enables inversion
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
- Include `test_en` bypass for comprehensive DFT support

=== Design Guidelines
<soc-net-clock-design-guidelines>
- Ensure target frequencies match division ratios mathematically
- Use proper SI units (Hz, kHz, MHz, GHz)
- Consider clock domain crossing requirements for multi-clock systems
- Replace template cells with foundry-specific implementations for production
