= FILE FORMATS
<file-formats>
QSoC uses several YAML-based file formats to define modules, buses, and netlists. This document describes the structure and usage of these file formats, with a focus on the SOC_NET format for netlist description.

== SOC_NET FORMAT
<soc-net-format>
The SOC_NET format is a YAML-based netlist description format used to define SoC designs, including module instances, port connections, and bus mappings. It provides precise control over connections through features like bit selection.

=== STRUCTURE OVERVIEW
<soc-net-structure>
A SOC_NET file consists of several key sections:

#figure(
  align(center)[#table(
      columns: (0.2fr, 1fr),
      align: (auto, left),
      table.header([Section], [Description]),
      table.hline(),
      [port], [Defines top-level ports of the design],
      [instance], [Defines module instances and their parameters],
      [net], [Defines explicit connections between instance ports],
      [bus], [Defines bus interface connections (automatically expanded into nets)],
    )],
  caption: [SOC_NET FILE SECTIONS],
  kind: table,
)

=== PORT SECTION
<soc-net-port>
The `port` section defines the top-level ports of the design, specifying their direction and type:

```yaml
# Top-level ports
port:
  clk:
    direction: input
    type: logic          # Main system clock input
  reset_n:
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

=== INSTANCE SECTION
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
    )],
  caption: [INSTANCE PROPERTIES],
  kind: table,
)

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
      [uplink], [Create a net with the specified name, connect this port to it, AND create a top-level port with the same name],
    )],
  caption: [PORT ATTRIBUTES],
  kind: table,
)

=== NET SECTION
<soc-net-net>
The `net` section defines explicit connections between instance ports:

```yaml
# Net connections
net:
  sys_clk:               # System clock distribution
    cpu0:
      port: clk
    ram0:
      port: clk
    uart0:
      port: clk
  sys_rst_n:             # System reset (active low)
    cpu0:
      port: rst_n
    ram0:
      port: rst_n
    uart0:
      port: rst_n
  data_bus:              # Data communication bus
    cpu0:
      port: data_out
      bits: "[7:0]"      # Connect only lower 8 bits
    ram0:
      port: data_in      # Full width connection
```

Net connection properties include:

#figure(
  align(center)[#table(
      columns: (0.2fr, 1fr),
      align: (auto, left),
      table.header([Property], [Description]),
      table.hline(),
      [port], [Port name to connect],
      [bits], [Optional bit selection (e.g., "[7:0]" or "[5]")],
    )],
  caption: [NET CONNECTION PROPERTIES],
  kind: table,
)

=== BUS SECTION
<soc-net-bus>
The `bus` section defines bus interface connections that will be automatically expanded into individual net connections:

```yaml
bus:
  cpu_ram_bus:           # AXI bus connection between CPU and RAM
    cpu0:
      port: axi_master   # CPU acts as AXI master
    ram0:
      port: axi_slave    # RAM acts as AXI slave
```

During netlist processing, bus connections are expanded based on bus interface definitions in the module files, generating individual nets for each bus signal.

Bus connection properties include:

#figure(
  align(center)[#table(
      columns: (0.2fr, 1fr),
      align: (auto, left),
      table.header([Property], [Description]),
      table.hline(),
      [port], [Bus interface port name defined in the module],
    )],
  caption: [BUS CONNECTION PROPERTIES],
  kind: table,
)

=== LINK AND UPLINK ATTRIBUTES
<soc-net-link-uplink>
The `link` and `uplink` attributes provide convenient shortcuts for creating connections without explicitly defining nets in the `net` section.

==== Link Attribute
<soc-net-link>
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
    module_a:
      port: output_port  # Source of the signal
    module_b:
      port: input_port   # Destination of the signal
```

==== Uplink Attribute
<soc-net-uplink>
The `uplink` attribute creates both an internal net and a top-level port with the same name, then connects the instance port to both:

```yaml
instance:
  io_cell:
    module: io_pad
    port:
      PAD:
        uplink: spi_clk    # Create top-level port + internal net + connection
```

This automatically creates:
1. A top-level port named `spi_clk` with direction inferred from the module port (reversed for internal logic)
2. An internal net named `spi_clk`
3. A connection between the instance port and the net

The generated top-level port will have:
- Direction opposite to the module port (input→output, output→input, inout→inout)
- Same data type as the module port

==== Conflict Resolution
<soc-net-uplink-conflicts>
When using `uplink`, QSoC automatically handles conflicts:

- If a top-level port with the same name already exists and is compatible (same width, compatible direction), the connection is made
- If the existing port is incompatible, an error is reported
- Multiple `uplink` attributes with the same name are allowed as long as all connected ports are compatible

==== Usage Guidelines
<soc-net-link-uplink-guidelines>
Use `link` when you want to:
- Create simple internal connections between modules
- Avoid cluttering the `net` section with simple point-to-point connections

Use `uplink` when you want to:
- Connect internal modules directly to top-level ports (common for I/O pads)
- Automatically generate top-level ports without manually defining them
- Create chip-level pin assignments for SoC designs

=== BIT SELECTION
<soc-net-bit-selection>
Bit selection allows connecting specific bits of a port to a net. Two formats are supported:

#figure(
  align(center)[#table(
      columns: (0.3fr, 1fr),
      align: (auto, left),
      table.header([Format], [Description]),
      table.hline(),
      [[high:low]], [Range selection (e.g., "[7:3]" selects bits 7 through 3, 5 bits wide)],
      [[bit]], [Single bit selection (e.g., "[5]" selects only bit 5, 1 bit wide)],
    )],
  caption: [BIT SELECTION FORMATS],
  kind: table,
)

The width of a bit selection is calculated as `|high - low| + 1` for range selections, or 1 for single bit selections.

=== AUTOMATIC WIDTH CHECKING
<soc-net-width-checking>
QSoC performs automatic width checking for all connections:

1. It calculates the effective width of each port in a connection, considering bit selections
2. It compares widths of all ports connected to the same net
3. It generates warnings for width mismatches, including detailed information about port widths and bit selections

=== EXAMPLE FILE
<soc-net-example>
Below is a complete example of a SOC_NET file:

```yaml
# Top-level ports
port:
  clk:
    direction: input
    type: logic          # Main system clock input
  reset_n:
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
    cpu0:
      port: clk
    ram0:
      port: clk
    uart0:
      port: clk
  sys_rst_n:             # System reset (active low)
    cpu0:
      port: rst_n
    ram0:
      port: rst_n
    uart0:
      port: rst_n
  data_bus:              # Data communication bus
    cpu0:
      port: data_out
      bits: "[7:0]"      # Connect only lower 8 bits
    ram0:
      port: data_in      # Full width connection

# Bus interface connections
bus:
  cpu_ram_bus:           # AXI bus connection between CPU and RAM
    cpu0:
      port: axi_master   # CPU acts as AXI master
    ram0:
      port: axi_slave    # RAM acts as AXI slave
```

This example defines a simple SoC with a CPU, RAM, and UART controller, connected via individual nets and an AXI bus interface. It also demonstrates the use of `link` and `uplink` attributes for simplified I/O pad connections and internal signal routing. The `uplink` attributes automatically create top-level ports (spi_clk, spi_mosi) while `link` attributes create internal nets for clock distribution and internal connections.

=== PROCESSING FLOW
<soc-net-processing>
When QSoC processes a SOC_NET file, it follows this sequence:

1. Parse all module definitions referenced in the instance section
2. Validate port connections against module definitions
3. Process `link` and `uplink` attributes to generate nets and top-level ports
4. Expand bus connections into individual nets based on bus interface definitions
5. Calculate effective widths for all connections, considering bit selections
6. Check for width mismatches and generate appropriate warnings
7. Generate Verilog output based on the processed netlist

=== NOTE ON VERILOG PORT WIDTHS
<soc-net-verilog-widths>
QSoC correctly handles Verilog port width declarations where LSB is not zero. For example, a port declared as `output [7:3] signal` in Verilog has a width of 5 bits. The SOC_NET format and processing logic properly calculates this width as `|7-3|+1 = 5`. This ensures accurate width checking even with non-zero-based bit ranges.
</rewritten_file>
