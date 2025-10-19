= NETLIST FORMAT
<netlist-format>
The netlist format defines the structural elements of a design: ports, instances, and connections. These sections form the foundation of any SOC_NET design description.

== PORT SECTION
<soc-net-port>
The `port` section defines the top-level ports of the design, specifying their direction and type:

```yaml
# Top-level ports
port:
  clk:
    direction: input
    type: logic          # Main system clock input
  rst_n:
    direction: input
    type: logic          # Active-low reset signal
  data_out:
    direction: output
    type: logic[31:0]    # 32-bit data output to external world
  addr_bus:
    direction: inout
    type: logic[15:0]    # Bidirectional 16-bit address bus
```

Port properties include:

#figure(
  align(center)[#table(
    columns: (0.2fr, 1fr),
    align: (auto, left),
    table.header([Property], [Description]),
    table.hline(),
    [direction], [Port direction (input, output, or inout)],
    [type], [Port data type and width (e.g., logic[7:0])],
  )],
  caption: [PORT PROPERTIES],
  kind: table,
)

== INSTANCE SECTION
<soc-net-instance>
The `instance` section defines module instances and their optional parameters:

```yaml
# Module instances
instance:
  cpu0:
    module: cpu          # Main processor core
  ram0:
    module: sram
    parameter:
      WIDTH: 32          # 32-bit data width
      DEPTH: 1024        # 1K words memory depth
  uart0:
    module: uart_controller
    port:
      tx_enable:
        tie: 1           # Always enable UART transmitter
        invert: true     # Invert signal (active low enable)
  io_cell0:
    module: io_cell
    port:
      PAD:
        uplink: spi_clk    # Creates top-level port and internal net
      C:
        link: int_clk      # Creates internal net only
  io_cell1:
    module: io_cell
    port:
      PAD:
        uplink: spi_mosi   # Another top-level port
      C:
        link: spi_mosi_int # Internal connection
  clock_gen:
    module: pll
    port:
      clk_out:
        link: int_clk      # Connects to same net as io_cell0.C
```

Instance properties include:

#figure(
  align(center)[#table(
    columns: (0.2fr, 1fr),
    align: (auto, left),
    table.header([Property], [Description]),
    table.hline(),
    [module], [Module name (must exist in the module library)],
    [parameter], [Optional module parameters (name-value pairs)],
    [port], [Optional port-specific attributes like tie values],
    [ifdef],
    [List of macros that must be defined (AND logic, alphabetically nested)],
    [ifndef],
    [List of macros that must be undefined (AND logic, alphabetically nested)],
  )],
  caption: [INSTANCE PROPERTIES],
  kind: table,
)

=== Conditional Compilation
<soc-net-instance-ifdef>
Instances support conditional compilation via `ifdef`/`ifndef` lists:

```yaml
instance:
  # Single ifdef
  u_pad_fpga:
    module: PAD_OUTER
    ifdef: [TECH_FPGA]

  # Multiple ifdef (AND + alphabetical nesting)
  u_pad_min:
    module: PAD_OUTER
    ifdef: [TECH_FPGA, USE_MIN_PADS]

  # ifdef + ifndef combination
  u_pad_asic:
    module: PAD_OUTER
    ifdef: [TECH_TSMC28HK_9T]
    ifndef: [TECH_FPGA]
```

*Semantics*:
- All macros in `ifdef` must be defined
- All macros in `ifndef` must be undefined
- Cross-list logic: AND (all conditions must hold)
- Nesting order: `ifdef` (alphabetical) → `ifndef` (alphabetical)

*Generated code*:
```verilog
`ifdef TECH_FPGA
    PAD_OUTER u_pad_fpga ();
`endif /*  TECH_FPGA */

`ifdef TECH_FPGA
`ifdef USE_MIN_PADS
    PAD_OUTER u_pad_min ();
`endif /*  USE_MIN_PADS */
`endif /*  TECH_FPGA */

`ifdef TECH_TSMC28HK_9T
`ifndef TECH_FPGA
    PAD_OUTER u_pad_asic ();
`endif /*  !TECH_FPGA */
`endif /*  TECH_TSMC28HK_9T */
```

Port attributes within an instance can include:

#figure(
  align(center)[#table(
    columns: (0.2fr, 1fr),
    align: (auto, left),
    table.header([Attribute], [Description]),
    table.hline(),
    [tie], [Tie the port to a specific value],
    [invert], [Invert the port signal (true/false)],
    [link], [Create a net with the specified name and connect this port to it],
    [uplink],
    [Create a net with the specified name, connect this port to it, AND create a top-level port with the same name],
  )],
  caption: [PORT ATTRIBUTES],
  kind: table,
)

== NET SECTION
<soc-net-net>
The `net` section defines explicit connections between instance ports using a standardized list format:

=== List Format
<soc-net-list-format>
The list format is the standard approach for all net connections:

```yaml
# Net connections (List Format)
net:
  sys_clk:               # System clock distribution
    - instance: cpu0
      port: clk
    - instance: ram0
      port: clk
    - instance: uart0
      port: clk
  sys_rst_n:             # System reset (active low)
    - instance: cpu0
      port: rst_n
    - instance: ram0
      port: rst_n
    - instance: uart0
      port: rst_n
  spi_sclk:              # SPI clock distribution
    - instance: u_spi_periph
      port: sclk
    - instance: u_spi_periph    # Same instance, different port
      port: clk
    - instance: u_spi_reg_0
      port: clk
    - instance: u_pwrclk_bridge
      port: ao_sclk
  data_bus:              # Data bus with bit selection
    - instance: cpu0
      port: data_out
      bits: "[7:0]"      # Connect only lower 8 bits
    - instance: ram0
      port: data_in      # Full width connection
```

The list format allows the same instance to appear multiple times with different ports, which is useful for complex SoC designs where a single module may have multiple ports connected to the same net.

=== Connection Properties
<soc-net-connection-properties>
Each connection in the list format supports the following properties:

#figure(
  align(center)[#table(
    columns: (0.2fr, 1fr),
    align: (auto, left),
    table.header([Property], [Description]),
    table.hline(),
    [instance], [Instance name to connect (required)],
    [port], [Port name to connect (required)],
    [bits], [Optional bit selection (e.g., "[7:0]" or "[5]")],
  )],
  caption: [NET CONNECTION PROPERTIES],
  kind: table,
)

== Link and Uplink Attributes
<soc-net-link-uplink>
The `link` and `uplink` attributes provide automatic net generation and connection:

=== Link Attribute
<soc-net-link>
The `link` attribute creates or connects to an internal net:

```yaml
instance:
  pll_inst:
    module: pll
    port:
      clk_out:
        link: sys_clk    # Creates or connects to net "sys_clk"

  cpu_inst:
    module: cpu
    port:
      clk:
        link: sys_clk    # Connects to the same "sys_clk" net
```

=== Uplink Attribute
<soc-net-uplink>
The `uplink` attribute creates both a top-level port and an internal net:

```yaml
instance:
  io_pad:
    module: io_cell
    port:
      PAD:
        uplink: ext_clk  # Creates top-level port "ext_clk" and net "ext_clk"
```

This is equivalent to:
1. Adding `ext_clk` to the `port` section as an input/output
2. Creating a net named `ext_clk`
3. Connecting `io_pad.PAD` to this net

=== Usage Guidelines
<soc-net-link-uplink-guidelines>
- Use `link` for internal connections between instances
- Use `uplink` when you need both a top-level port and internal connectivity
- Both attributes automatically handle net creation and prevent duplication
- Multiple instances can reference the same link/uplink net name for shared connections

== DETAILED LINK AND UPLINK ATTRIBUTES
<soc-net-detailed-link-uplink>
The `link` and `uplink` attributes provide convenient shortcuts for creating connections without explicitly defining nets in the `net` section. These attributes serve different purposes and have distinct design philosophies.

=== Overview
<soc-net-link-uplink-overview>
QSoC provides two primary connection attributes:

#figure(
  align(center)[#table(
    columns: (0.2fr, 0.4fr, 0.4fr),
    align: (auto, left, left),
    table.header([Attribute], [Purpose], [Use Cases]),
    table.hline(),
    [`link`],
    [Internal signal routing with flexible bit selection],
    [Module-to-module connections, bus segmentation, signal distribution],
    [`uplink`],
    [Direct I/O port mapping],
    [Chip pins, power/ground, clock/reset signals],
  )],
  caption: [LINK VS UPLINK COMPARISON],
  kind: table,
)

=== Link Attribute Details
<soc-net-link-detailed>

==== Basic Usage
<soc-net-link-basic>
The `link` attribute creates an internal net with the specified name and connects the port to it:

```yaml
instance:
  module_a:
    module: some_module
    port:
      output_port:
        link: internal_signal  # Create internal net for point-to-point connection
  module_b:
    module: another_module
    port:
      input_port:
        link: internal_signal  # Connect to the same internal net
```

This is equivalent to defining the net explicitly:

```yaml
net:
  internal_signal:       # Explicit net definition (same result as link)
    - instance: module_a
      port: output_port  # Source of the signal
    - instance: module_b
      port: input_port   # Destination of the signal
```

==== Bit Selection Support
<soc-net-link-bits>
The `link` attribute supports bit selection syntax for flexible signal routing:

```yaml
instance:
  amp_east:
    module: amplifier_stage
    port:
      VOUT_DATA:
        link: amp_bus[7:4]     # Connect to bits [7:4] of amp_bus net
  amp_west:
    module: amplifier_stage
    port:
      VOUT_DATA:
        link: amp_bus[3:0]     # Connect to bits [3:0] of amp_bus net
  mixer_core:
    module: mixer_unit
    port:
      CTRL_FLAG:
        link: control_net[5]   # Connect to bit [5] of control_net
```

Supported formats:
- Range selection: `signal_name[high:low]` (e.g., `data_bus[15:8]`)
- Single bit selection: `signal_name[bit]` (e.g., `control_flag[3]`)

=== Uplink Attribute Details
<soc-net-uplink-detailed>

==== Basic Usage
<soc-net-uplink-basic>
The `uplink` attribute creates both an internal net and a top-level port with the same name:

```yaml
instance:
  io_cell:
    module: io_pad
    port:
      PAD:
        uplink: spi_clk    # Create top-level port + internal net + connection
```

This automatically creates:
1. A top-level port named `spi_clk` with direction inferred from the module port
2. An internal net named `spi_clk`
3. A connection between the instance port and the net

==== Design Philosophy
<soc-net-uplink-philosophy>
The `uplink` attribute is designed with a specific philosophy:

*Direct I/O Mapping*: Uplink provides a one-to-one mapping between internal module ports and chip-level I/O pins. This design ensures:
- *Simplicity*: No complex routing or bit manipulation
- *Clarity*: Clear correspondence between internal signals and external pins
- *Reliability*: Minimal opportunity for connection errors

*Typical Applications*:
- I/O pad connections (`PAD` ports to chip pins)
- Power and ground distribution (`VDD`/`VSS` to power pins)
- Global signals (clocks, resets) to chip-level ports
- Test and debug interfaces

==== Limitations and Rationale
<soc-net-uplink-limitations>

*Bit Selection Not Supported*

The `uplink` attribute *intentionally does not support bit selection* for several important reasons:

*1. Design Philosophy Conflict*
```yaml
# This creates semantic ambiguity - what should be created?
instance:
  io_pad:
    port:
      PAD:
        uplink: data_bus[7:0]  # ✗ NOT SUPPORTED
```
- Should this create an 8-bit top-level port named `data_bus`?
- Or a full-width port with only bits [7:0] connected?
- The ambiguity violates uplink's principle of clear, direct mapping.

*2. Technical Complexity*
Supporting bit selection in uplink would require:
- Complex port width inference algorithms
- Handling of conflicting bit range specifications
- Ambiguous semantics for top-level port generation
- Additional error checking and validation logic

*3. Alternative Solutions Available*
For complex I/O scenarios requiring bit selection, use the `link` + explicit port definition approach:

```yaml
# ✓ CORRECT - Use link for bit selection
instance:
  io_pad_low:
    module: io_cell
    port:
      PAD:
        link: internal_data[7:0]
  io_pad_high:
    module: io_cell
    port:
      PAD:
        link: internal_data[15:8]

# Explicit top-level port definitions
port:
  data_out_low:
    direction: output
    type: logic[7:0]
    connect: internal_data[7:0]
  data_out_high:
    direction: output
    type: logic[7:0]
    connect: internal_data[15:8]
```

*4. Architectural Consistency*
The QSoC connection system maintains clear separation of concerns:
- *`link`*: Handles complex internal routing and bit manipulation
- *`uplink`*: Handles simple I/O port mapping
- *`bus`*: Handles protocol-level connections

Adding bit selection to uplink would blur these boundaries and reduce system clarity.

==== Conflict Resolution
<soc-net-uplink-conflicts>
When using `uplink`, QSoC automatically handles conflicts:

- If a top-level port with the same name already exists and is compatible (same width, compatible direction), the connection is made
- If the existing port is incompatible, an error is reported
- Multiple `uplink` attributes with the same name are allowed as long as all connected ports are compatible

=== Comparison and Usage Guidelines
<soc-net-link-uplink-comparison-guidelines>

==== When to Use Link
Use `link` when you need:
- *Internal signal routing* between modules
- *Bit selection* or signal manipulation
- *Complex connections* with width adaptation
- *Bus segmentation* across multiple modules

==== When to Use Uplink
Use `uplink` when you need:
- *Direct I/O port mapping* (chip pins)
- *Simple, one-to-one connections* to top-level
- *Automatic port generation* without manual definition
- *Clear correspondence* between internal and external signals

==== Best Practices
*For Simple I/O*:
```yaml
# ✓ Good - Direct I/O mapping
instance:
  reset_pad:
    module: io_cell
    port:
      PAD:
        uplink: chip_rst_n
```

*For Complex I/O*:
```yaml
# ✓ Good - Use link + explicit ports for complex scenarios
instance:
  data_pads:
    module: io_array
    port:
      DATA_OUT:
        link: internal_data[31:0]

port:
  chip_data:
    direction: output
    type: logic[31:0]
    connect: internal_data
```

== EXAMPLE SOC_NET FILE
<soc-net-example>
Below is a complete example of a SOC_NET file demonstrating various features:

```yaml
# Top-level ports
port:
  clk:
    direction: input
    type: logic          # Main system clock input
  rst_n:
    direction: input
    type: logic          # Active-low reset signal
  data_out:
    direction: output
    type: logic[31:0]    # 32-bit data output to external world

# Module instances
instance:
  cpu0:
    module: cpu          # Main processor core
  ram0:
    module: sram
    parameter:
      WIDTH: 32          # 32-bit data width
      DEPTH: 1024        # 1K words memory depth
  uart0:
    module: uart_controller
    port:
      tx_enable:
        tie: 1           # Always enable UART transmitter
        invert: true     # Invert signal (active low enable)
  io_cell0:
    module: io_cell
    port:
      PAD:
        uplink: spi_clk    # Creates top-level port and internal net
      C:
        link: int_clk      # Creates internal net only
  io_cell1:
    module: io_cell
    port:
      PAD:
        uplink: spi_mosi   # Another top-level port
      C:
        link: spi_mosi_int # Internal connection
  clock_gen:
    module: pll
    port:
      clk_out:
        link: int_clk      # Connects to same net as io_cell0.C

# Net connections
net:
  sys_clk:               # System clock distribution
    - instance: cpu0
      port: clk
    - instance: ram0
      port: clk
    - instance: uart0
      port: clk
  sys_rst_n:             # System reset (active low)
    - instance: cpu0
      port: rst_n
    - instance: ram0
      port: rst_n
    - instance: uart0
      port: rst_n
  data_bus:              # 32-bit data bus
    - instance: cpu0
      port: data_out
    - instance: ram0
      port: data_in

# Bus connections (automatically expanded)
bus:
  cpu_ram_axi:           # AXI bus connection
    - instance: cpu0
      port: axi_master
    - instance: ram0
      port: axi_slave

# Combinational logic examples
comb:
  # Simple address decoding
  - out: cs_ram
    expr: "(addr[31:28] == 4'h0)"

  # Multiplexer logic
  - out: chip_select
    case: addr[31:28]
    cases:
      "4'h0": "cs_ram"
      "4'h1": "cs_uart"
      "4'h2": "cs_gpio"
    default: "1'b0"

  # Complex ALU operation with nested logic
  - out: alu_result
    if:
      - cond: "alu_op == 2'b00"
        then:
          case: funct_code
          cases:
            "3'b000": "operand_a + operand_b"
            "3'b001": "operand_a - operand_b"
          default: "32'b0"
      - cond: "alu_op == 2'b01"
        then: "operand_a & operand_b"
    default: "32'hDEADBEEF"
```

This example defines a simple SoC with a CPU, RAM, and UART controller, connected via individual nets and an AXI bus interface. It also demonstrates the use of `link` and `uplink` attributes for simplified I/O pad connections and internal signal routing. The `uplink` attributes automatically create top-level ports (spi_clk, spi_mosi) while `link` attributes create internal nets for clock distribution and internal connections.
