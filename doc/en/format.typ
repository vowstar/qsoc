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
    align: (auto,left,),
    table.header([Section], [Description],),
    table.hline(),
    [port], [Defines top-level ports of the design],
    [instance], [Defines module instances and their parameters],
    [net], [Defines explicit connections between instance ports],
    [bus], [Defines bus interface connections (automatically expanded into nets)],
  )]
  , caption: [SOC_NET FILE SECTIONS]
  , kind: table
  )

=== PORT SECTION
<soc-net-port>
The `port` section defines the top-level ports of the design, specifying their direction and type:

```yaml
port:
  clk:
    direction: input
    type: logic
  data_out:
    direction: output
    type: logic[31:0]
  addr_bus:
    direction: inout
    type: logic[15:0]
```

Port properties include:

#figure(
  align(center)[#table(
    columns: (0.2fr, 1fr),
    align: (auto,left,),
    table.header([Property], [Description],),
    table.hline(),
    [direction], [Port direction (input, output, or inout)],
    [type], [Port data type and width (e.g., logic[7:0])],
  )]
  , caption: [PORT PROPERTIES]
  , kind: table
  )

=== INSTANCE SECTION
<soc-net-instance>
The `instance` section defines module instances and their optional parameters:

```yaml
instance:
  cpu0:
    module: cpu
    parameter:
      XLEN: 64
      CACHE_SIZE: 32768
  ram0:
    module: sram
    parameter:
      WIDTH: 32
      DEPTH: 1024
  uart0:
    module: uart_controller
    port:
      tx_enable:
        tie: 1
        invert: true
```

Instance properties include:

#figure(
  align(center)[#table(
    columns: (0.2fr, 1fr),
    align: (auto,left,),
    table.header([Property], [Description],),
    table.hline(),
    [module], [Module name (must exist in the module library)],
    [parameter], [Optional module parameters (name-value pairs)],
    [port], [Optional port-specific attributes like tie values],
  )]
  , caption: [INSTANCE PROPERTIES]
  , kind: table
  )

Port attributes within an instance can include:

#figure(
  align(center)[#table(
    columns: (0.2fr, 1fr),
    align: (auto,left,),
    table.header([Attribute], [Description],),
    table.hline(),
    [tie], [Tie the port to a specific value],
    [invert], [Invert the port signal (true/false)],
  )]
  , caption: [PORT ATTRIBUTES]
  , kind: table
  )

=== NET SECTION
<soc-net-net>
The `net` section defines explicit connections between instance ports:

```yaml
net:
  sys_clk:
    cpu0:
      port: clk
    ram0:
      port: clk
  data_bus:
    cpu0:
      port: data_out
      bits: "[7:0]"
    ram0:
      port: data_in
  addr_bus:
    cpu0:
      port: addr_out
    ram0:
      port: addr_in
      bits: "[15:0]"
```

Net connection properties include:

#figure(
  align(center)[#table(
    columns: (0.2fr, 1fr),
    align: (auto,left,),
    table.header([Property], [Description],),
    table.hline(),
    [port], [Port name to connect],
    [bits], [Optional bit selection (e.g., "[7:0]" or "[5]")],
  )]
  , caption: [NET CONNECTION PROPERTIES]
  , kind: table
  )

=== BUS SECTION
<soc-net-bus>
The `bus` section defines bus interface connections that will be automatically expanded into individual net connections:

```yaml
bus:
  cpu_ram_bus:
    cpu0:
      port: axi_master
    ram0:
      port: axi_slave
```

During netlist processing, bus connections are expanded based on bus interface definitions in the module files, generating individual nets for each bus signal.

Bus connection properties include:

#figure(
  align(center)[#table(
    columns: (0.2fr, 1fr),
    align: (auto,left,),
    table.header([Property], [Description],),
    table.hline(),
    [port], [Bus interface port name defined in the module],
  )]
  , caption: [BUS CONNECTION PROPERTIES]
  , kind: table
  )

=== BIT SELECTION
<soc-net-bit-selection>
Bit selection allows connecting specific bits of a port to a net. Two formats are supported:

#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto,left,),
    table.header([Format], [Description],),
    table.hline(),
    [[high:low]], [Range selection (e.g., "[7:3]" selects bits 7 through 3, 5 bits wide)],
    [[bit]], [Single bit selection (e.g., "[5]" selects only bit 5, 1 bit wide)],
  )]
  , caption: [BIT SELECTION FORMATS]
  , kind: table
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
    type: logic
  reset_n:
    direction: input
    type: logic
  data_out:
    direction: output
    type: logic[31:0]

# Module instances
instance:
  cpu0:
    module: cpu
  ram0:
    module: sram
    parameter:
      WIDTH: 32
      DEPTH: 1024
  uart0:
    module: uart_controller
    port:
      tx_enable:
        tie: 1
        invert: true

# Network connections
net:
  sys_clk:
    cpu0:
      port: clk
    ram0:
      port: clk
    uart0:
      port: clk
  sys_rst_n:
    cpu0:
      port: rst_n
    ram0:
      port: rst_n
    uart0:
      port: rst_n
  data_bus:
    cpu0:
      port: data_out
      bits: "[7:0]"
    ram0:
      port: data_in

# Bus interface connections
bus:
  cpu_ram_bus:
    cpu0:
      port: axi_master
    ram0:
      port: axi_slave
```

This example defines a simple SoC with a CPU, RAM, and UART controller, connected via individual nets and an AXI bus interface.

=== PROCESSING FLOW
<soc-net-processing>
When QSoC processes a SOC_NET file, it follows this sequence:

1. Parse all module definitions referenced in the instance section
2. Validate port connections against module definitions
3. Expand bus connections into individual nets based on bus interface definitions
4. Calculate effective widths for all connections, considering bit selections
5. Check for width mismatches and generate appropriate warnings
6. Generate Verilog output based on the processed netlist

=== NOTE ON VERILOG PORT WIDTHS
<soc-net-verilog-widths>
QSoC correctly handles Verilog port width declarations where LSB is not zero. For example, a port declared as `output [7:3] signal` in Verilog has a width of 5 bits. The SOC_NET format and processing logic properly calculates this width as `|7-3|+1 = 5`. This ensures accurate width checking even with non-zero-based bit ranges.
</rewritten_file>
