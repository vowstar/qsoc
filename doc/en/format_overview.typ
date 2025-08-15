= FILE FORMATS OVERVIEW
<file-formats-overview>
QSoC uses several YAML-based file formats to define modules, buses, and netlists. This document provides an overview of these file formats, with a focus on the SOC_NET format for netlist description.

== SOC_NET FORMAT
<soc-net-format>
The SOC_NET format is a YAML-based netlist description format used to define SoC designs, including module instances, port connections, and bus mappings. It provides precise control over connections through features like bit selection.

=== Generated Verilog File Structure
<soc-net-verilog-structure>
When QSoC processes a SOC_NET file, it generates a single Verilog file with the following structure:

```verilog
/**
 * @file design.v
 * @brief RTL implementation of design
 * NOTE: Auto-generated file, do not edit manually.
 */

/* 1. Reset controller modules (if reset primitives are defined) */
module reset_ctrl (...);
  // Reset synchronization logic
endmodule

/* 2. Clock controller modules (if clock primitives are defined) */
module clk_ctrl (...);
  // Clock management logic
endmodule

/* 3. FSM controller modules (if FSM primitives are defined) */
module fsm_controller (...);
  // Finite state machine logic
endmodule

/* 4. Top-level design module (if ports, nets, instances, or comb/seq exist) */
module design (...);
  // Port declarations
  // Wire declarations
  // Module instantiations
  // Combinational logic (comb section)
  // Sequential logic (seq section)
endmodule
```

Note: Users are responsible for manually instantiating the generated primitive modules (reset, clock, FSM) in their design or other modules as needed.

=== Structure Overview
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
    [bus],
    [Defines bus interface connections (automatically expanded into nets)],
    [comb], [Defines combinational logic blocks for behavioral descriptions],
    [seq], [Defines sequential logic blocks for register-based descriptions],
    [fsm], [Defines finite state machine blocks for complex control logic],
    [reset],
    [Defines reset controller primitives (generates standalone modules)],
    [clock],
    [Defines clock controller primitives (generates standalone modules)],
  )],
  caption: [SOC_NET FILE SECTIONS],
  kind: table,
)

=== Processing Flow
<soc-net-processing-flow>
The QSoC netlist processor follows a multi-stage processing flow:

1. *Parse*: Read and validate YAML structure
2. *Expand*: Process bus definitions into individual nets
3. *Connect*: Build connectivity graph from nets and instances
4. *Generate*: Create Verilog RTL output

Each stage includes comprehensive validation and error checking to ensure design correctness.

=== Example File Structure
<soc-net-example-structure>
```yaml
# Example SOC_NET file structure
port:
  # Top-level interface definitions

instance:
  # Module instantiations

net:
  # Point-to-point connections

bus:
  # Bus interface connections

comb:
  # Combinational logic

seq:
  # Sequential logic

fsm:
  # State machines

reset:
  # Reset controllers

clock:
  # Clock controllers
```

The following chapters provide detailed specifications for each section of the SOC_NET format.
