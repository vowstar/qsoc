= File Formats Overview
<file-formats-overview>
QSoC uses several YAML-based file formats to define modules, buses, and netlists. This document provides an overview of these file formats, with a focus on the SOC_NET format for netlist description.

== Generated RTL
<soc-net-generated-rtl>
Generated RTL targets IEEE 1364-2001 Verilog. The generators stay inside a
2001-safe subset: ANSI port lists, `generate` blocks, `localparam` state
constants, and a hand-written `clog2` function in place of `$clog2`. No
SystemVerilog construct is emitted, so `always_ff`, `logic` declarations,
`typedef enum`, packages and interfaces never appear in the output.

Input is not restricted the same way. Module import parses full SystemVerilog
through slang, and SystemVerilog port types are reduced to `wire`/`reg` on the
way into the generated netlist.

Slang parses the modules you import. Default generation does not run
`verible-verilog-format`, so canonical output is independent of `PATH`. The
`generate verilog --format` option resolves that program from `PATH` and
reformats each generated top-level file; that post-processed text is
non-canonical. Neither operation simulates, lints, or synthesizes the
generated RTL. Validate generated RTL in your own flow before relying on it.

== SOC_NET Format
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

/* 3. Power controller modules (if power primitives are defined) */
module pwr_ctrl (...);
  // Power domain management logic
endmodule

/* 4. FSM controller modules (if FSM primitives are defined) */
module fsm_controller (...);
  // Finite state machine logic
endmodule

/* 5. Top-level design module (if ports, nets, instances, or comb/seq exist) */
module design (...);
  // Port declarations
  // Wire declarations
  // Module instantiations
  // Combinational logic (comb section)
  // Sequential logic (seq section)
endmodule
```

*Note*: Users are responsible for manually instantiating the generated primitive modules (reset, clock, FSM) in their design or other modules as needed.

=== Structure Overview
<soc-net-structure>
A SOC_NET file consists of several key sections:

#figure(
  align(center)[#table(
    columns: (0.2fr, 1fr),
    align: (auto, left),
    table.header([Section], [Description]),
    table.hline(),
    [port], [Defines top-level ports of the design (@soc-net-port)],
    [instance],
    [Defines module instances and their parameters (@soc-net-instance)],
    [net],
    [Defines explicit connections between instance ports (@soc-net-net)],
    [bus],
    [Defines bus interface connections, automatically expanded into nets
     (@soc-net-bus)],
    [comb],
    [Defines combinational logic blocks for behavioral descriptions
     (@soc-net-comb)],
    [seq],
    [Defines sequential logic blocks for register-based descriptions
     (@soc-net-seq)],
    [fsm],
    [Defines finite state machine blocks for complex control logic
     (@soc-net-fsm-overview)],
    [reset],
    [Defines reset controller primitives, generates standalone modules
     (@soc-net-reset-overview)],
    [clock],
    [Defines clock controller primitives, generates standalone modules
     (@soc-net-clock-overview)],
    [power],
    [Defines power controller primitives, generates standalone modules
     (@soc-net-power-overview)],
  )],
  caption: [SOC_NET FILE SECTIONS],
  kind: table,
)

=== Processing Flow
<soc-net-processing-flow>
The QSoC netlist processor follows a multi-stage processing flow:

+ *Parse*: Read and validate YAML structure
+ *Expand*: Process bus definitions into individual nets
+ *Connect*: Build connectivity graph from nets and instances
+ *Generate*: Create Verilog RTL output

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

power:
  # Power controllers
```

Each section has its own chapter: @netlist-format, @bus-interface-format,
@logic-description-format, @fsm-format, @reset-format, @clock-format,
@power-format and @template-format.
