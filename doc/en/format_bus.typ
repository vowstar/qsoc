= BUS INTERFACE FORMAT
<bus-interface-format>
The bus interface format provides high-level connectivity abstractions for protocol-based connections and advanced signal routing through bit selection capabilities.

== BUS SECTION
<soc-net-bus>
The `bus` section defines bus interface connections that will be automatically expanded into individual net connections. Bus connections use the list format:

```yaml
bus:
  cpu_ram_bus:           # AXI bus connection between CPU and RAM
    - instance: cpu0
      port: axi_master   # CPU acts as AXI master
    - instance: ram0
      port: axi_slave    # RAM acts as AXI slave
  spi_bus:               # SPI bus with multiple slaves
    - instance: spi_master
      port: spi_master_if
    - instance: spi_slave0
      port: spi_slave_if
    - instance: spi_slave1
      port: spi_slave_if
```

During netlist processing, bus connections are expanded based on bus interface definitions in the module files, generating individual nets for each bus signal.

=== Width Information Preservation
<soc-net-bus-width-preservation>
When QSoC expands bus connections into individual nets, it preserves the original port width specifications from module definitions. This ensures that signals with specific bit ranges (e.g., `logic[21:2]`) maintain their exact width in the generated Verilog, rather than being converted to a standard `[msb:0]` format.

For example, if a module defines:
```yaml
port:
  addr_port:
    type: logic[21:2]
    direction: out
```

The generated Verilog wire declaration will correctly preserve the range:
```verilog
wire [21:2] bus_addr_signal;  // Preserves [21:2] range
```

Rather than incorrectly expanding to:
```verilog
wire [21:0] bus_addr_signal;  // Incorrect [21:0] format
```

This preservation is critical for hardware designs that rely on specific address ranges, bit alignments, or have hardware-imposed constraints on signal indexing.

Bus connection properties include:

#figure(
  align(center)[#table(
    columns: (0.2fr, 1fr),
    align: (auto, left),
    table.header([Property], [Description]),
    table.hline(),
    [instance], [Instance name to connect (required)],
    [port], [Bus interface port name defined in the module (required)],
  )],
  caption: [BUS CONNECTION PROPERTIES],
  kind: table,
)

== BIT SELECTION
<soc-net-bit-selection>
Bit selection allows connecting specific bits of a port to a net, enabling flexible signal routing and bus segmentation. This feature is supported by the `link` attribute and explicit `net` definitions.

=== Syntax and Formats
<soc-net-bit-selection-syntax>

#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto, left),
    table.header([Format], [Description]),
    table.hline(),
    [[high:low]],
    [Range selection (e.g., "[7:3]" selects bits 7 through 3, 5 bits wide)],
    [[bit]],
    [Single bit selection (e.g., "[5]" selects only bit 5, 1 bit wide)],
  )],
  caption: [BIT SELECTION FORMATS],
  kind: table,
)

=== Implementation Details
<soc-net-bit-selection-details>
The system follows these principles when processing bit selection:

1. *Wire Width Determination*: Generated wires use the *full width of the source port*, not the bit selection width
2. *Connection Generation*: Bit selection is applied at the *port connection level* in generated Verilog
3. *Width Safety*: Automatic validation ensures bit selections don't exceed port width

Example processing:
```yaml
# Input netlist
instance:
  processor:
    module: cpu_core
    port:
      DATA_OUT:
        link: data_bus[7:4]  # Connect to bits [7:4] of data_bus
```

```verilog
// Generated Verilog
wire [31:0] data_bus;  // Uses full port width (DATA_OUT is 32-bit)
cpu_core processor (
    .DATA_OUT(data_bus[7:4])  // Bit selection in connection
);
```

=== Use Cases
<soc-net-bit-selection-use-cases>

==== Data Bus Segmentation
Divide wide data buses among multiple modules:
```yaml
instance:
  proc_high:
    module: processor
    port:
      DATA_OUT:
        link: system_bus[31:16]  # Upper 16 bits
  proc_low:
    module: processor
    port:
      DATA_OUT:
        link: system_bus[15:0]   # Lower 16 bits
```

==== Control Signal Mapping
Map individual control signals to specific bits:
```yaml
instance:
  ctrl_unit:
    module: controller
    port:
      ENABLE:
        link: control_reg[0]     # Enable bit
      RESET:
        link: control_reg[1]     # Reset bit
      MODE_SELECT:
        link: control_reg[7:4]   # Mode selection bits
```

==== Mixed Width Connections
Connect different width ports to the same net:
```yaml
instance:
  wide_driver:
    module: wide_output    # 32-bit output
    port:
      DATA:
        link: shared_signal
  narrow_receiver:
    module: narrow_input   # 8-bit input
    port:
      DATA:
        link: shared_signal[7:0]  # Only use lower 8 bits
```

=== Limitations
<soc-net-bit-selection-limitations>

==== Attribute Support
- *`link`*: ✓ Full bit selection support
- *`uplink`*: ✗ No bit selection support (design philosophy requires direct I/O mapping)
- *`net`*: ✓ Full bit selection support via `bits` attribute

==== Width Validation
The system performs automatic validation:
- Bit selections exceeding port width generate warnings
- Undriven nets (no source connections) are flagged
- Width mismatches between connected ports are reported

=== Best Practices
<soc-net-bit-selection-best-practices>

1. *Clear Naming*: Use descriptive names for bit-selected signals
```yaml
# ✓ Good
link: addr_bus_high[31:16]
link: ctrl_flags[7:4]

# ✗ Avoid
link: bus[31:16]
link: sig[7:4]
```

2. *Logical Grouping*: Group related bits together
```yaml
# ✓ Good - logical bit grouping
instance:
  alu:
    port:
      FLAGS:
        link: cpu_status[3:0]    # ALU flags: [3:0]
  fpu:
    port:
      FLAGS:
        link: cpu_status[7:4]    # FPU flags: [7:4]
```

3. *Avoid Overlapping Assignments*: Prevent multiple drivers
```yaml
# ✗ Avoid - overlapping bit assignments
instance:
  module_a:
    port:
      OUT:
        link: shared_bus[7:4]
  module_b:
    port:
      OUT:
        link: shared_bus[6:3]    # Overlaps with [7:4]!
```

== AUTOMATIC WIDTH CHECKING
<soc-net-width-checking>
QSoC performs automatic width checking for all connections:

1. It calculates the effective width of each port in a connection, considering bit selections
2. It compares widths of all ports connected to the same net
3. It generates warnings for width mismatches, including detailed information about port widths and bit selections

This automatic checking helps catch design errors early in the development process and ensures signal integrity across the design hierarchy.
