= FILE FORMATS
<file-formats>
QSoC uses several YAML-based file formats to define modules, buses, and netlists. This document describes the structure and usage of these file formats, with a focus on the SOC_NET format for netlist description.

== SOC_NET FORMAT
<soc-net-format>
The SOC_NET format is a YAML-based netlist description format used to define SoC designs, including module instances, port connections, and bus mappings. It provides precise control over connections through features like bit selection.

=== GENERATED VERILOG FILE STRUCTURE
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

=== PORT SECTION
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
    [uplink],
    [Create a net with the specified name, connect this port to it, AND create a top-level port with the same name],
  )],
  caption: [PORT ATTRIBUTES],
  kind: table,
)

=== NET SECTION
<soc-net-net>
The `net` section defines explicit connections between instance ports using a standardized list format:

==== List Format
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

==== Connection Properties
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

=== COMB SECTION
<soc-net-comb>
The `comb` section defines combinational logic blocks that generate pure combinational Verilog code. This section allows you to describe combinational logic using a high-level YAML DSL that is then translated to appropriate Verilog constructs.

==== Overview
<soc-net-comb-overview>
The `comb` section is a sequence of combinational logic items, each describing one output assignment. The system supports three main types of combinational logic:

#figure(
  align(center)[#table(
    columns: (0.2fr, 0.4fr, 0.4fr),
    align: (auto, left, left),
    table.header([Type], [Description], [Verilog Output]),
    table.hline(),
    [`expr`], [Simple assignment with expression], [`assign` statement],
    [`if`],
    [Conditional logic with if-else chain],
    [`always @(*)` block with if-else],
    [`case`], [Case statement logic], [`always @(*)` block with case statement],
  )],
  caption: [COMBINATIONAL LOGIC TYPES],
  kind: table,
)

==== Basic Structure
<soc-net-comb-structure>
Each combinational logic item must have an `out` field specifying the output signal name, and exactly one logic specification field (`expr`, `if`, or `case`):

```yaml
comb:
  - out: output_signal_name
    expr: "logic_expression"    # Simple assignment
  - out: another_signal
    if:                         # Conditional logic
      - cond: "condition1"
        then: "value1"
      - cond: "condition2"
        then: "value2"
    default: "default_value"
  - out: case_output
    case: switch_expression     # Case statement
    cases:
      "value1": "result1"
      "value2": "result2"
    default: "default_result"
```

==== Simple Assignment (expr)
<soc-net-comb-expr>
Simple assignments generate `assign` statements in Verilog:

```yaml
comb:
  - out: y
    expr: "a & b"
```

Generates:
```verilog
assign y = a & b;
```

The expression can be any valid Verilog expression using input signals, constants, and operators.

==== Conditional Logic (if)
<soc-net-comb-if>
Conditional logic generates `always @(*)` blocks with if-else chains:

```yaml
comb:
  - out: result
    if:
      - cond: "sel == 2'b00"
        then: "a"
      - cond: "sel == 2'b01"
        then: "b"
    default: "32'b0"
```

Generates:
```verilog
/* Internal reg declarations for combinational logic */
reg [31:0] result_reg;

/* Assign internal regs to outputs */
assign result = result_reg;

/* Combinational logic */
always @(*) begin
    result_reg = 32'b0;
    if (sel == 2'b00)
        result_reg = a;
    else if (sel == 2'b01)
        result_reg = b;
end
```

===== Conditional Logic Properties
<soc-net-comb-if-properties>

#figure(
  align(center)[#table(
    columns: (0.2fr, 1fr),
    align: (auto, left),
    table.header([Field], [Description]),
    table.hline(),
    [`cond`], [Condition expression (required, scalar)],
    [`then`],
    [Value when condition is true (required, scalar or nested structure)],
  )],
  caption: [CONDITIONAL LOGIC FIELDS],
  kind: table,
)

The `default` field is strongly recommended to avoid accidental latch inference.

==== Case Statement Logic (case)
<soc-net-comb-case>
Case statements generate `always @(*)` blocks with case statements:

```yaml
comb:
  - out: alu_op
    case: funct
    cases:
      "6'b100000": "4'b0001"
      "6'b100010": "4'b0010"
      "6'b100100": "4'b0011"
    default: "4'b0000"
```

Generates:
```verilog
/* Internal reg declarations for combinational logic */
reg [3:0] alu_op_reg;

/* Assign internal regs to outputs */
assign alu_op = alu_op_reg;

/* Combinational logic */
always @(*) begin
    alu_op_reg = 4'b0000;
    case (funct)
        6'b100000: alu_op_reg = 4'b0001;
        6'b100010: alu_op_reg = 4'b0010;
        6'b100100: alu_op_reg = 4'b0011;
        default:   alu_op_reg = 4'b0000;
    endcase
end
```

===== Case Statement Properties
<soc-net-comb-case-properties>

#figure(
  align(center)[#table(
    columns: (0.2fr, 1fr),
    align: (auto, left),
    table.header([Field], [Description]),
    table.hline(),
    [`case`], [Expression to switch on (required, scalar)],
    [`cases`], [Map of match values to result expressions (required, map)],
    [`default`], [Default value to avoid latches (recommended, scalar)],
  )],
  caption: [CASE STATEMENT FIELDS],
  kind: table,
)

==== Nested Conditional Logic
<soc-net-comb-nested>
The combinational logic supports nested structures by allowing `then` fields to contain nested `case` statements, enabling complex ALU decoders, control units, and multiplexed logic:

```yaml
comb:
  - out: alu_op
    if:
      - cond: "opcode == 6'b000000"      # R-type instructions
        then:
          case: funct                    # Nested case based on function field
          cases:
            "6'b100000": "4'b0001"       # ADD operation
            "6'b100010": "4'b0010"       # SUB operation
            "6'b100100": "4'b0011"       # AND operation
          default: "4'b1111"             # Undefined function
      - cond: "opcode == 6'b001000"      # I-type instruction
        then: "4'b0101"                  # Immediate operation
      - cond: "opcode == 6'b000010"      # J-type instruction
        then: "4'b0110"                  # Jump operation
    default: "4'b0000"                   # NOP or unknown instruction
```

Generates:
```verilog
always @(*) begin
    alu_op = 4'b0000;
    if (opcode == 6'b000000) begin
        case (funct)
            6'b100000: alu_op = 4'b0001;
            6'b100010: alu_op = 4'b0010;
            6'b100100: alu_op = 4'b0011;
            default:   alu_op = 4'b1111;
        endcase
    end else if (opcode == 6'b001000) begin
        alu_op = 4'b0101;
    end else if (opcode == 6'b000010) begin
        alu_op = 4'b0110;
    end
end
```

===== Nested Structure Syntax
<soc-net-comb-nested-syntax>
In nested structures, the `then` field can contain either:
- A scalar value (simple assignment): `then: "value"`
- A nested case structure with required `case` and `cases` fields:
  ```yaml
  then:
    case: expression_to_switch_on
    cases:
      "match_value1": "result_value1"
      "match_value2": "result_value2"
    default: "default_value"      # Optional but recommended
  ```

===== Complex Nested Example
<soc-net-comb-nested-complex>
More complex nested structures for sophisticated control logic:

```yaml
comb:
  - out: control_signals
    if:
      - cond: "instruction_type == 3'b000"
        then:
          case: "alu_funct[2:0]"
          cases:
            "3'b000": "8'b00010001"     # ADD control
            "3'b001": "8'b00010010"     # SUB control
            "3'b010": "8'b00100001"     # AND control
            "3'b011": "8'b00100010"     # OR control
          default: "8'b00000000"
      - cond: "instruction_type == 3'b001"
        then:
          case: "mem_op[1:0]"
          cases:
            "2'b00": "8'b01000100"      # LOAD control
            "2'b01": "8'b10000100"      # STORE control
          default: "8'b00000000"
      - cond: "instruction_type == 3'b010"
        then: "8'b00001000"             # BRANCH control
    default: "8'b00000000"              # NOP control
```

This generates a comprehensive control unit with nested decoding for different instruction types and their specific operations.

==== Validation Rules
<soc-net-comb-validation>
The system performs comprehensive validation of combinational logic to ensure correct synthesis:

===== Required Fields
- Each item must have an `out` field specifying the output signal
- Each item must have exactly one logic specification (`expr`, `if`, or `case`)
- For `case` logic: both `case` (scalar) and `cases` (map) fields are required

===== Logic Type Validation
- `expr`: Must be a scalar expression string
- `if`: Must be a sequence of condition-value pairs with `cond` and `then` fields
- `case`: The `case` field must be scalar, `cases` must be a map of scalar key-value pairs

===== Nested Structure Validation
- Nested structures are only supported in `if` logic within `then` fields
- Nested `then` field must contain a valid case structure with `case` and `cases` fields
- Nested case expressions (`case` field) must be scalar
- All case match values (keys in `cases`) must be scalar strings
- All case result values (values in `cases`) must be scalar strings
- Nested structures cannot contain further nesting (only one level deep)

===== Signal and Expression Validation
- Output signals specified in `out` fields must be valid Verilog identifiers
- All expressions in `expr`, `cond`, `then`, and case values must be valid Verilog expressions
- Signal names used in expressions should match port or net declarations
- Bit selection syntax in expressions must be properly formatted

===== Best Practices
- Always provide `default` values for `if` and `case` logic to avoid latch inference
- Use descriptive signal names that reflect their purpose (e.g., `alu_control`, `mux_select`)
- Keep expressions simple and readable to aid debugging and maintenance
- Group logically related combinational blocks together in the YAML
- Comment complex expressions using YAML comments for clarity
- Use consistent coding style for similar logic patterns
- Prefer case statements over long if-else chains when switching on discrete values

==== Code Generation
<soc-net-comb-generation>
Combinational logic is generated after module instantiations in the Verilog output, providing clear separation between structural and behavioral code.

===== Generated Code Structure
1. Wire declarations (from nets)
2. Module instantiations (from instances)
3. Combinational logic (from comb section)

This ordering ensures that:
- All signals are properly declared before use
- Structural connections are visible before behavioral logic
- Combinational logic appears as "glue logic" connecting modules

=== SEQ SECTION
<soc-net-seq>
The `seq` section defines sequential logic blocks that generate pure sequential Verilog code. This section allows you to describe register-based logic using a high-level YAML DSL that is then translated to appropriate Verilog `always` blocks with proper clock and reset handling.

==== Overview
<soc-net-seq-overview>
The `seq` section is a sequence of sequential logic items, each describing one register. The system supports various types of sequential logic with comprehensive control over clocking, reset, enable, and next-state logic.

#figure(
  align(center)[#table(
    columns: (0.3fr, 0.7fr),
    align: (auto, left),
    table.header([Feature], [Description]),
    table.hline(),
    [`reg`], [Register name (required)],
    [`clk`], [Clock signal (required)],
    [`edge`], [Clock edge type: "pos" (default) or "neg"],
    [`rst`], [Reset signal (optional)],
    [`rst_val`], [Reset value (required when rst is present)],
    [`enable`], [Enable signal (optional)],
    [`next`], [Simple next-state expression],
    [`if`], [Conditional next-state logic],
    [`default`], [Default value for conditional logic],
  )],
  caption: [SEQUENTIAL LOGIC FEATURES],
  kind: table,
)

==== Basic Structure
<soc-net-seq-structure>
Each sequential logic item must have `reg` and `clk` fields, and exactly one logic specification field (`next` or `if`):

```yaml
seq:
  - reg: register_name
    clk: clock_signal
    edge: pos              # Optional: "pos" (default) or "neg"
    rst: reset_signal      # Optional reset
    rst_val: reset_value   # Required when rst is present
    enable: enable_signal  # Optional enable
    next: "expression"     # Simple assignment
  - reg: another_register
    clk: clock_signal
    if:                    # Conditional logic
      - cond: "condition1"
        then: "value1"
      - cond: "condition2"
        then: "value2"
    default: "default_value"
```

==== Simple Assignment
<soc-net-seq-simple>
Simple register assignments use the `next` field:

```yaml
seq:
  - reg: data
    clk: clk
    rst: rst_n
    rst_val: "8'h00"
    next: data_in
```

Generates:
```verilog
/* Internal reg declarations for sequential logic */
reg [7:0] data_reg;

/* Assign internal regs to outputs */
assign data = data_reg;

/* Sequential logic */
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        data_reg <= 8'h00;
    end else begin
        data_reg <= data_in;
    end
end
```

==== Clock Edge Control
<soc-net-seq-edge>
Control the clock edge using the `edge` field:

```yaml
seq:
  - reg: neg_edge
    clk: clk
    edge: neg
    next: data_in
```

Generates:
```verilog
/* Internal reg declarations for sequential logic */
reg neg_edge_reg;

/* Assign internal regs to outputs */
assign neg_edge = neg_edge_reg;

/* Sequential logic */
always @(negedge clk) begin
    neg_edge_reg <= data_in;
end
```

==== Reset Logic
<soc-net-seq-reset>
Add asynchronous reset using `rst` and `rst_val` fields:

```yaml
seq:
  - reg: counter
    clk: clk
    rst: rst_n
    rst_val: "16'h0000"
    next: "counter + 1"
```

Generates:
```verilog
/* Internal reg declarations for sequential logic */
reg [15:0] counter_reg;

/* Assign internal regs to outputs */
assign counter = counter_reg;

/* Sequential logic */
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        counter_reg <= 16'h0000;
    end else begin
        counter_reg <= counter + 1;
    end
end
```

==== Enable Logic
<soc-net-seq-enable>
Add enable control using the `enable` field:

```yaml
seq:
  - reg: enabled_counter
    clk: clk
    rst: rst_n
    rst_val: "8'h00"
    enable: count_en
    next: "enabled_counter + 1"
```

Generates:
```verilog
/* Internal reg declarations for sequential logic */
reg [7:0] enabled_counter_reg;

/* Assign internal regs to outputs */
assign enabled_counter = enabled_counter_reg;

/* Sequential logic */
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        enabled_counter_reg <= 8'h00;
    end else begin
        if (count_en) begin
            enabled_counter_reg <= enabled_counter + 1;
        end
    end
end
```

==== Conditional Logic
<soc-net-seq-conditional>
Use conditional logic with the `if` field for complex state machines:

```yaml
seq:
  - reg: state
    clk: clk
    rst: rst_n
    rst_val: "2'b00"
    if:
      - cond: "start_signal"
        then: "2'b01"
      - cond: "state == 2'b01 && done_signal"
        then: "2'b10"
      - cond: "state == 2'b10"
        then: "2'b00"
    default: "state"
```

Generates:
```verilog
/* Internal reg declarations for sequential logic */
reg [1:0] state_reg;

/* Assign internal regs to outputs */
assign state = state_reg;

/* Sequential logic */
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        state_reg <= 2'b00;
    end else begin
        state_reg <= state;
        if (start_signal)
            state_reg <= 2'b01;
        else if (state == 2'b01 && done_signal)
            state_reg <= 2'b10;
        else if (state == 2'b10)
            state_reg <= 2'b00;
    end
end
```

===== Conditional Logic Properties
<soc-net-seq-conditional-properties>

#figure(
  align(center)[#table(
    columns: (0.2fr, 1fr),
    align: (auto, left),
    table.header([Field], [Description]),
    table.hline(),
    [`cond`], [Condition expression (required, scalar)],
    [`then`], [Value when condition is true (required, scalar)],
  )],
  caption: [CONDITIONAL SEQUENTIAL LOGIC FIELDS],
  kind: table,
)

The `default` field is strongly recommended to ensure predictable behavior when no conditions are met.

==== Nested Conditional Logic
<soc-net-seq-nested>
The sequential logic supports nested structures by allowing `then` fields to contain nested `case` statements, enabling complex state machines and control logic:

```yaml
seq:
  - reg: state_machine
    clk: clk
    rst: rst_n
    rst_val: "8'h00"
    if:
      - cond: "ctrl == 2'b00"
        then: "8'h01"
      - cond: "ctrl == 2'b01"
        then:
          case: sub_ctrl
          cases:
            "2'b00": "8'h10"
            "2'b01": "8'h20"
            "2'b10": "8'h30"
          default: "8'h0F"
      - cond: "ctrl == 2'b10"
        then: "data_in"
    default: "state_machine"
```

Generates:
```verilog
/* Internal reg declarations for sequential logic */
reg [7:0] state_machine_reg;

/* Assign internal regs to outputs */
assign state_machine = state_machine_reg;

/* Sequential logic */
always @(posedge clk or negedge rst_n) begin
    if (!rst_n) begin
        state_machine_reg <= 8'h00;
    end else begin
        state_machine_reg <= state_machine;
        if (ctrl == 2'b00) begin
            state_machine_reg <= 8'h01;
        end else if (ctrl == 2'b01) begin
            case (sub_ctrl)
                2'b00: state_machine_reg <= 8'h10;
                2'b01: state_machine_reg <= 8'h20;
                2'b10: state_machine_reg <= 8'h30;
                default: state_machine_reg <= 8'h0F;
            endcase
        end else if (ctrl == 2'b10) begin
            state_machine_reg <= data_in;
        end
    end
end
```

===== Nested Structure Properties
<soc-net-seq-nested-properties>

#figure(
  align(center)[#table(
    columns: (0.2fr, 1fr),
    align: (auto, left),
    table.header([Field], [Description]),
    table.hline(),
    [`then`], [Can be scalar expression OR nested case structure],
    [`case`], [Expression to switch on (required in nested structure)],
    [`cases`], [Map of case values to result expressions (required)],
    [`default`], [Default case value (recommended in nested structure)],
  )],
  caption: [NESTED SEQUENTIAL LOGIC FIELDS],
  kind: table,
)

Nested case structures within if conditions allow for:
- Complex state machine implementations
- Multi-level control logic
- Hierarchical decision trees
- Optimized Verilog case statement generation

==== Validation Rules
<soc-net-seq-validation>
The system performs comprehensive validation of sequential logic:

===== Required Fields
- Each item must have a `reg` field specifying the register name
- Each item must have a `clk` field specifying the clock signal
- Each item must have exactly one logic specification (`next` OR `if`, not both)

===== Optional Fields Validation
- `edge`: Must be "pos" or "neg" if present
- `rst`: Must be a scalar if present
- `rst_val`: Required when `rst` is present, must be scalar
- `enable`: Must be a scalar if present

===== Logic Type Validation
- `next`: Must be a scalar expression
- `if`: Must be a sequence of condition-value pairs
- Each `if` condition must have both `cond` and `then` fields
- `cond`: Must be a scalar expression
- `then`: Can be either a scalar expression OR a nested case structure

===== Nested Structure Validation
- When `then` is a nested structure, it must contain `case` and `cases` fields
- `case`: Must be a scalar expression for the case switch
- `cases`: Must be a map of case values to result expressions
- All case values and result expressions must be scalars
- `default` in nested structures is recommended but optional

===== Best Practices
- Always provide reset logic for registers in real designs
- Use meaningful register and signal names
- Provide `default` values for conditional logic to avoid latches
- Group related sequential logic together
- Use enable signals for conditional register updates

==== Code Generation
<soc-net-seq-generation>
Sequential logic is generated after combinational logic in the Verilog output, providing clear separation between combinational and sequential code.

===== Generated Code Structure
1. Wire declarations (from nets)
2. Module instantiations (from instances)
3. Combinational logic (from comb section)
4. Sequential logic (from seq section)
5. Finite state machine logic (from fsm section)

This ordering ensures that:
- All signals are properly declared before use
- Structural connections are visible before behavioral logic
- Combinational logic appears before sequential logic
- Sequential logic represents the state-holding elements
- FSM logic provides structured control flow with clear state management

===== Always Block Generation
The system generates optimized `always` blocks based on the specified features:
- Clock edge: `@(posedge clk)` or `@(negedge clk)`
- With reset: `@(posedge clk or negedge rst)`
- Reset logic: Asynchronous reset with `if (!rst)` structure
- Enable logic: Conditional assignment within the main logic

==== Generated Verilog Examples
<soc-net-generated-examples>
The following examples demonstrate the actual Verilog code generated by the QSoC system. These examples showcase the internal register pattern used for Verilog 2005 compliance, where all output signals are implemented using internal registers with `_reg` suffix followed by continuous assign statements.

===== FSM with Multiple Encoding Types
Here's an example showing two FSMs with different state encodings operating together:

```yaml
fsm:
  - name: test_onehot
    clk: clk
    rst: rst_n
    rst_state: S0
    encoding: onehot
    trans:
      S0: [{cond: trigger, next: S1}]
      S1: [{cond: trigger, next: S2}]
      S2: [{cond: trigger, next: S0}]
    moore:
      S1: {onehot_output: 1}
  - name: test_gray
    clk: clk
    rst: rst_n
    rst_state: A
    encoding: gray
    trans:
      A: [{cond: trigger, next: B}]
      B: [{cond: trigger, next: C}]
      C: [{cond: trigger, next: A}]
    moore:
      B: {gray_output: 1}
```

Generated Verilog:
```verilog
module test_onehot (
    /* FSM clock and reset */
    input  clk,        /**< FSM clock input */
    input  rst_n,      /**< FSM reset input */
    /* Input signals */
    input  trigger,    /**< Input signal */
    /* Output signals */
    output onehot_output /**< Output signal */
);

    /* test_onehot : Table FSM generated by YAML-DSL */
    /* test_onehot state registers */
    reg [2:0] test_onehot_cur_state, test_onehot_nxt_state;

    localparam TEST_ONEHOT_S0 = 3'd1;
    localparam TEST_ONEHOT_S1 = 3'd2;
    localparam TEST_ONEHOT_S2 = 3'd4;

    /* test_onehot next-state logic */
    always @(*) begin
        test_onehot_nxt_state = test_onehot_cur_state;
        case (test_onehot_cur_state)
            TEST_ONEHOT_S0: if (trigger) test_onehot_nxt_state = TEST_ONEHOT_S1;
            TEST_ONEHOT_S1: if (trigger) test_onehot_nxt_state = TEST_ONEHOT_S2;
            TEST_ONEHOT_S2: if (trigger) test_onehot_nxt_state = TEST_ONEHOT_S0;
            default: test_onehot_nxt_state = test_onehot_cur_state;
        endcase
    end

    /* test_onehot state register w/ async reset */
    always @(posedge clk or negedge rst_n)
        if (!rst_n) test_onehot_cur_state <= TEST_ONEHOT_S0;
        else test_onehot_cur_state <= test_onehot_nxt_state;

    /* test_onehot Moore outputs */
    reg test_onehot_onehot_output_reg;

    assign onehot_output = test_onehot_onehot_output_reg;

    always @(*) begin
        test_onehot_onehot_output_reg = 1'b0;
        case (test_onehot_cur_state)
            TEST_ONEHOT_S1: begin
                test_onehot_onehot_output_reg = 1'b1;
            end
            default: begin
                test_onehot_onehot_output_reg = 1'b0;
            end
        endcase
    end

endmodule

module test_gray (
    /* FSM clock and reset */
    input  clk,        /**< FSM clock input */
    input  rst_n,      /**< FSM reset input */
    /* Input signals */
    input  trigger,    /**< Input signal */
    /* Output signals */
    output gray_output /**< Output signal */
);

    /* test_gray : Table FSM generated by YAML-DSL */
    /* test_gray state registers */
    reg [1:0] test_gray_cur_state, test_gray_nxt_state;

    localparam TEST_GRAY_A = 2'd0;
    localparam TEST_GRAY_B = 2'd1;
    localparam TEST_GRAY_C = 2'd3;

    /* test_gray next-state logic */
    always @(*) begin
        test_gray_nxt_state = test_gray_cur_state;
        case (test_gray_cur_state)
            TEST_GRAY_A: if (trigger) test_gray_nxt_state = TEST_GRAY_B;
            TEST_GRAY_B: if (trigger) test_gray_nxt_state = TEST_GRAY_C;
            TEST_GRAY_C: if (trigger) test_gray_nxt_state = TEST_GRAY_A;
            default: test_gray_nxt_state = test_gray_cur_state;
        endcase
    end

    /* test_gray state register w/ async reset */
    always @(posedge clk or negedge rst_n)
        if (!rst_n) test_gray_cur_state <= TEST_GRAY_A;
        else test_gray_cur_state <= test_gray_nxt_state;

    /* test_gray Moore outputs */
    reg test_gray_gray_output_reg;

    assign gray_output = test_gray_gray_output_reg;

    always @(*) begin
        test_gray_gray_output_reg = 1'b0;
        case (test_gray_cur_state)
            TEST_GRAY_B: begin
                test_gray_gray_output_reg = 1'b1;
            end
            default: begin
                test_gray_gray_output_reg = 1'b0;
            end
        endcase
    end

endmodule

module test_fsm_encodings (
    input  clk,
    input  rst_n,
    input  trigger,
    output onehot_output,
    output gray_output
);

    /* Wire declarations */
    /* Module instantiations */
    test_onehot u_test_onehot (
        .clk(clk),
        .rst_n(rst_n),
        .trigger(trigger),
        .onehot_output(onehot_output)
    );

    test_gray u_test_gray (
        .clk(clk),
        .rst_n(rst_n),
        .trigger(trigger),
        .gray_output(gray_output)
    );

endmodule
```

===== Microcode FSM with ROM-based Control
Example of a microcode FSM using constant ROM initialization:

```yaml
fsm:
  - name: mseq_fixed
    clk: clk
    rst: rst_n
    rst_state: 0
    rom_mode: parameter
    fields:
      ctrl: [0, 7]
      branch: [8, 9]
      next: [10, 14]
    rom:
      0: {ctrl: "8'h55", branch: next, next: 1}
      1: {ctrl: "8'h3C", branch: branch_if, next: 4}
      2: {ctrl: "8'h18", branch: next, next: 3}
      3: {ctrl: "8'h80", branch: jump, next: 0}
      4: {ctrl: "8'hA0", branch: branch_if_not, next: 3}
```

Generated Verilog:
```verilog
module test_microcode_fixed_fsm (
    input        clk,
    input        rst_n,
    input        cond,
    output [7:0] ctrl_bus
);

    /* Wire declarations */
    /* Module instantiations */

    /* Finite State Machine logic */

    /* mseq_fixed : microcode FSM with constant ROM */
    localparam MSEQ_FIXED_AW = 3;
    localparam MSEQ_FIXED_DW = 14;

    /* mseq_fixed program counter */
    reg [MSEQ_FIXED_AW-1:0] mseq_fixed_pc, mseq_fixed_nxt_pc;

    /* mseq_fixed ROM array */
    reg [MSEQ_FIXED_DW:0] mseq_fixed_rom[0:(1<<MSEQ_FIXED_AW)-1];

    /* mseq_fixed reset-time ROM initialization */
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            mseq_fixed_rom[0] <= {5'd1, 2'd0, 8'h55};
            mseq_fixed_rom[1] <= {5'd4, 2'd1, 8'h3C};
            mseq_fixed_rom[2] <= {5'd3, 2'd0, 8'h18};
            mseq_fixed_rom[3] <= {5'd0, 2'd3, 8'h80};
            mseq_fixed_rom[4] <= {5'd3, 2'd2, 8'hA0};
        end
    end

    /* mseq_fixed branch decode */
    always @(*) begin
        mseq_fixed_nxt_pc = mseq_fixed_pc + 1'b1;
        case (mseq_fixed_rom[mseq_fixed_pc][9:8])
            2'd0: mseq_fixed_nxt_pc = mseq_fixed_pc + 1'b1;
            2'd1: if (cond) mseq_fixed_nxt_pc = mseq_fixed_rom[mseq_fixed_pc][14:10][MSEQ_FIXED_AW-1:0];
            2'd2: if (!cond) mseq_fixed_nxt_pc = mseq_fixed_rom[mseq_fixed_pc][14:10][MSEQ_FIXED_AW-1:0];
            2'd3: mseq_fixed_nxt_pc = mseq_fixed_rom[mseq_fixed_pc][14:10][MSEQ_FIXED_AW-1:0];
            default: mseq_fixed_nxt_pc = mseq_fixed_pc + 1'b1;
        endcase
    end

    /* mseq_fixed pc register */
    always @(posedge clk or negedge rst_n)
        if (!rst_n) mseq_fixed_pc <= 3'd0;
        else mseq_fixed_pc <= mseq_fixed_nxt_pc;

    /* mseq_fixed control outputs */
    assign ctrl_bus = mseq_fixed_rom[mseq_fixed_pc][7:0];

endmodule
```

===== Mixed Combinational and Sequential Logic
Example showing comprehensive mixed logic with internal register pattern:

```yaml
port:
  clk:
    direction: input
    type: logic
  rst_n:
    direction: input
    type: logic
  sel:
    direction: input
    type: logic
  a:
    direction: input
    type: logic[7:0]
  b:
    direction: input
    type: logic[7:0]
  c:
    direction: input
    type: logic[7:0]
  mux_out:
    direction: output
    type: logic[7:0]
  and_out:
    direction: output
    type: logic[7:0]
  reg_out:
    direction: output
    type: logic[7:0]
  shift_reg:
    direction: output
    type: logic[7:0]

comb:
  - out: mux_out
    if:
      - cond: "sel"
        then: "a"
    default: "b"
  - out: and_out
    expr: "a & c"

seq:
  - reg: reg_out
    clk: clk
    rst: rst_n
    rst_val: "8'h00"
    next: mux_out
  - reg: shift_reg
    clk: clk
    rst: rst_n
    rst_val: "8'hAA"
    next: "shift_reg << 1"
```

Generated Verilog:
```verilog
module test_mixed_merge1 (
    input        clk,
    input        rst_n,
    input        sel,
    input  [7:0] a,
    input  [7:0] b,
    input  [7:0] c,
    output [7:0] mux_out,
    output [7:0] and_out,
    output [7:0] reg_out,
    output [7:0] shift_reg
);

    /* Wire declarations */
    /* Module instantiations */

    /* Internal reg declarations for combinational logic */
    reg [7:0] mux_out_reg;

    /* Assign internal regs to outputs */
    assign mux_out = mux_out_reg;

    /* Combinational logic */
    always @(*) begin
        mux_out_reg = b;
        if (sel) begin
            mux_out_reg = a;
        end
    end

    assign and_out = a & c;

    /* Internal reg declarations for sequential logic */
    reg [7:0] reg_out_reg;
    reg [7:0] shift_reg_reg;

    /* Assign internal regs to outputs */
    assign reg_out   = reg_out_reg;
    assign shift_reg = shift_reg_reg;

    /* Sequential logic */
    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            reg_out_reg <= 8'h00;
        end else begin
            reg_out_reg <= mux_out;
        end
    end

    always @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            shift_reg_reg <= 8'hAA;
        end else begin
            shift_reg_reg <= shift_reg << 1;
        end
    end

endmodule
```

===== Key Features Demonstrated
The generated examples showcase several important features:

1. *Verilog 2005 Compliance*: All output signals use internal register pattern with `_reg` suffix and continuous assign statements to ensure compatibility.

2. *FSM Naming Conventions*: FSM-generated signals use lowercase FSM names with underscores (e.g., `test_onehot_cur_state`), while state constants use uppercase (e.g., `TEST_ONEHOT_S0`).

3. *Internal Register Pattern*: Both combinational and sequential logic outputs use internal registers followed by assign statements for proper wire/reg type separation.

4. *Bit Width Inference*: The system automatically infers correct bit widths from port declarations and applies them to internal registers.

5. *ROM Initialization*: Microcode FSMs properly initialize ROM contents with correct bit width padding (e.g., `{5'd1, 2'd0, 8'h55}` for 15-bit ROM words).

6. *Expression Handling*: Sequential logic expressions correctly reference output ports (e.g., `shift_reg << 1` refers to the output port, not the internal register).

=== FSM SECTION
<soc-net-fsm>
The `fsm` section defines finite state machine blocks that generate structured Verilog FSM code. This section supports both Table-mode FSMs with Moore/Mealy outputs and Microcode-mode FSMs with ROM-based control, providing powerful tools for implementing complex control logic.

==== FSM Overview
<soc-net-fsm-overview>
The `fsm` section is a sequence of FSM items, each describing one finite state machine. The system supports two main FSM architectures:

#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto, left),
    table.header([FSM Type], [Description]),
    table.hline(),
    [Table-mode], [State transition tables with Moore/Mealy outputs],
    [Microcode-mode], [ROM-based sequencer with branch decoding],
  )],
  caption: [FSM ARCHITECTURES],
  kind: table,
)

Both architectures support multiple state encodings (binary, onehot, gray) and generate proper three-stage FSM structure:
1. Next-state combinational logic
2. State register with async reset
3. Output combinational logic (Moore/Mealy/Control signals)

==== FSM Structure
<soc-net-fsm-structure>
Each FSM item must have `name`, `clk`, `rst`, and `rst_state` fields, plus architecture-specific fields:

```yaml
fsm:
  - name: controller        # FSM instance name (used for signal prefixes)
    clk: clk               # Clock signal
    rst: rst_n             # Reset signal
    rst_state: IDLE        # Reset state name
    encoding: binary       # State encoding: binary/onehot/gray (optional)
    # Table-mode specific fields:
    trans: { ... }         # State transition table
    moore: { ... }         # Moore output assignments (optional)
    mealy: [ ... ]         # Mealy output assignments (optional)
    # Microcode-mode specific fields:
    fields: { ... }        # ROM bit field definitions
    rom_mode: parameter    # ROM type: parameter/port
    rom: { ... }           # ROM initialization (for parameter mode)
    rom_depth: 32          # ROM depth (for port mode)
```

==== Table-mode FSMs
<soc-net-fsm-table>
Table-mode FSMs use explicit state transition tables and are ideal for traditional state machines with clear state flow:

===== Simple Moore FSM
```yaml
fsm:
  - name: cpu_ctrl
    clk: clk
    rst: rst_n
    rst_state: IDLE
    trans:
      IDLE: [{cond: start, next: LOAD}]
      LOAD: [{cond: done_load, next: RUN}]
      RUN: [{cond: done, next: IDLE}]
    moore:
      IDLE: {busy: 0}
      LOAD: {busy: 1}
      RUN: {busy: 1}
```

===== Moore and Mealy FSM
```yaml
fsm:
  - name: spi_rx
    clk: clk
    rst: rst_n
    rst_state: IDLE
    trans:
      IDLE:
        - {cond: "cs_n==0", next: SHIFT}
      SHIFT:
        - {cond: "bit_cnt==7", next: DONE}
        - {cond: "1", next: SHIFT}
      DONE:
        - {cond: "cs_n==1", next: IDLE}
    moore:
      SHIFT: {shift_en: 1}
    mealy:
      - {cond: "spi_rx_cur_state==SPI_RX_DONE && cs_n==1",
         sig: byte_ready, val: 1}
```

===== State Encodings
```yaml
fsm:
  - name: test_onehot
    encoding: onehot       # One-hot encoding
    # ... other fields
  - name: test_gray
    encoding: gray         # Gray code encoding
    # ... other fields
```

==== Microcode-mode FSMs
<soc-net-fsm-microcode>
Microcode-mode FSMs use ROM-based sequencers with branch decoding, ideal for complex control units and microprocessors:

===== Fixed ROM (Parameter mode)
```yaml
fsm:
  - name: mseq_fixed
    clk: clk
    rst: rst_n
    rst_state: 0           # Numeric state for microcode
    fields:
      ctrl: [0, 7]         # Control field bits [7:0]
      branch: [8, 9]       # Branch field bits [9:8]
      next: [10, 14]       # Next address field bits [14:10]
    rom_mode: parameter    # ROM implemented as parameters
    rom:
      0: {ctrl: 0x55, branch: 0, next: 1}
      1: {ctrl: 0x3C, branch: 1, next: 4}
      2: {ctrl: 0x18, branch: 0, next: 3}
```

===== Programmable ROM (Port mode)
```yaml
fsm:
  - name: mseq_prog
    clk: clk
    rst: rst_n
    rst_state: 0
    rom_mode: port         # ROM with write port
    rom_depth: 32          # 32-word ROM
    fields:
      ctrl: [0, 7]
      branch: [8, 9]
      next: [10, 14]
    # ROM programmed via external ports:
    # mseq_prog_rom_we, mseq_prog_rom_addr, mseq_prog_rom_wdata
```

==== FSM Properties
<soc-net-fsm-properties>

===== Required Fields
- `name`: FSM instance name (string) - used for signal prefixes
- `clk`: Clock signal name (string)
- `rst`: Reset signal name (string)
- `rst_state`: Reset state name (string/number)

===== Optional Fields
- `encoding`: State encoding type (`binary`/`onehot`/`gray`, default: `binary`)

===== Table-mode Fields
- `trans`: State transition table (map of state -> list of transitions)
  - Each transition: `{cond: "condition", next: "next_state"}`
- `moore`: Moore output assignments (map of state -> output assignments)
- `mealy`: Mealy output assignments (list of conditional assignments)
  - Each assignment: `{cond: "condition", sig: "signal", val: "value"}`

===== Microcode-mode Fields
- `fields`: ROM bit field definitions (map of field -> `[low_bit, high_bit]`)
- `rom_mode`: ROM implementation (`parameter`/`port`)
- `rom`: ROM initialization data (for parameter mode)
- `rom_depth`: ROM depth in words (for port mode)

==== FSM Validation
<soc-net-fsm-validation>
The system performs comprehensive validation of FSM specifications:

===== Common Validation
- FSM name must be unique within the design
- Clock and reset signals must be declared in ports
- Reset state must exist in the state space

===== Table-mode Validation
- All states referenced in transitions must be defined
- Transition conditions must be valid Verilog expressions
- Moore/Mealy output signals must be declared in ports
- No duplicate state names within an FSM

===== Microcode-mode Validation
- ROM fields must not overlap in bit ranges
- ROM addresses must be within the specified depth
- Field values must fit within their bit ranges
- Branch decoding must reference valid condition signals

==== Best Practices
<soc-net-fsm-practices>
===== Naming Conventions
- Use descriptive FSM names: `cpu_ctrl`, `spi_master`, `dma_engine`
- Use meaningful state names: `IDLE`, `FETCH`, `DECODE`, `EXECUTE`
- Group related FSMs with consistent prefixes

===== Design Guidelines
- Use Table-mode for clear state-based control
- Use Microcode-mode for complex instruction sequences
- Choose appropriate encoding: binary (compact), onehot (fast), gray (low power)
- Keep state count reasonable (< 16 states for table-mode)
- Use meaningful condition expressions

==== Code Generation
<soc-net-fsm-generation>
FSM controllers generate standalone modules that are placed at the beginning of the Verilog file, providing structured control flow implementation and module reusability.

===== Generated Code Structure
1. State type definitions (for table-mode)
2. State registers and next-state signals
3. ROM arrays and initialization (for microcode-mode)
4. Next-state combinational logic
5. State register with async reset
6. Output combinational logic (Moore/Mealy/Control)

===== Naming Conventions
All generated signals use the FSM name as prefix:
- `{fsm_name}_cur_state`, `{fsm_name}_nxt_state` (table-mode)
- `{fsm_name}_pc`, `{fsm_name}_nxt_pc` (microcode-mode)
- `{fsm_name}_rom` (microcode-mode)

=== RESET SECTION
<soc-net-reset>
The `reset` section defines reset controller primitives that generate proper reset signaling throughout the SoC. Reset primitives provide comprehensive reset management with support for multiple reset sources, component-based processing, signal polarity handling, and standardized module generation.

==== Reset Overview
<soc-net-reset-overview>
Reset controllers are essential for proper SoC operation, ensuring that all logic blocks start in a known state and can be reset reliably. QSoC supports sophisticated reset topologies with multiple reset sources mapping to multiple reset targets through a clear source → target → link relationship structure.

Key features include:
- Component-based reset processing architecture
- Signal polarity normalization (active high/low)
- Multi-source to multi-target reset matrices
- Structured YAML configuration without string parsing
- Test mode bypass support
- Standalone reset controller module generation

==== Reset Structure
<soc-net-reset-structure>
Reset controllers use a modern structured YAML format that eliminates complex string parsing and provides component-based processing:

```yaml
# Modern component-based reset controller format
reset:
  - name: main_reset_ctrl          # Reset controller instance name
    clock: clk_sys                 # Clock for synchronous reset operations
    test_enable: test_en           # Test enable bypass signal (optional)
    source:                        # Reset source definitions (singular)
      por_rst_n: low               # Active low reset source
      i3c_soc_rst: high            # Active high reset source
      trig_rst: low                # Trigger-based reset (active low)
    target:                        # Reset target definitions (singular)
      cpu_rst_n:
        active: low                # Active low target output
        link:                      # Link definitions for each source
          por_rst_n:
            source: por_rst_n
            async:                 # Component: qsoc_rst_sync
              clock: clk_sys
              stage: 4             # 4-stage synchronizer
          i3c_soc_rst:
            source: i3c_soc_rst    # Direct assignment (no components)
      peri_rst_n:
        active: low
        link:
          por_rst_n:
            source: por_rst_n      # Direct assignment (no components)
```

==== Reset Components
<soc-net-reset-components>
Reset controllers use component-based architecture with three standard reset processing modules. Each link can specify different processing attributes, automatically selecting the appropriate component:

==== Reset Properties
<soc-net-reset-properties>
Reset controller properties provide structured configuration:

#figure(
  align(center)[#table(
    columns: (0.2fr, 0.3fr, 0.5fr),
    align: (auto, left, left),
    table.header([Property], [Type], [Description]),
    table.hline(),
    [name], [String], [Reset controller instance name (required)],
    [clock], [String], [Clock signal name for sync operations (required)],
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

===== Source Properties
Reset sources define input reset signals with simple polarity specification:

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

===== Target Properties
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


==== Reset Reason Recording
<soc-net-reset-reason>
Reset controllers can optionally record the source of the last reset using sync-clear async-capture sticky flags with bit vector output. This implementation provides reliable narrow pulse capture and flexible software decoding.

===== Configuration
Enable reset reason recording with the simplified configuration format:
```yaml
reset:
  - name: my_reset_ctrl
    source:
      por_rst_n: low              # Root reset (excluded from bit vector)
      ext_rst_n: low              # bit[0]
      wdt_rst_n: low              # bit[1]
      i3c_soc_rst: high           # bit[2]

    # Simplified reason configuration
    reason:
      clock: clk_32k               # Always-on clock for recording logic
      output: reason               # Output bit vector name
      valid: reason_valid          # Valid signal name
      clear: reason_clear          # Software clear signal
      root_reset: por_rst_n        # Root reset signal for async clear (explicitly specified)
```

===== Implementation Details
The reset reason recorder uses *sync-clear async-capture* sticky flags to avoid S+R register timing issues:
- Each non-POR reset source gets a dedicated sticky flag (async-set on event, sync-clear during clear window)
- Clean async-set + sync-clear architecture avoids problematic S+R registers that cause STA difficulties
- Event normalization converts all sources to LOW-active format for consistent handling
- 2-cycle clear window after POR release or software clear pulse ensures proper initialization
- Output gating with valid signal prevents invalid data during initialization
- Always-on clock ensures operation even when main clocks are stopped
- Root reset signal explicitly specified in `reason.root_reset` field
- *Generate statement optimization*: Uses Verilog `generate` blocks to reduce code duplication for multiple sticky flags

===== Generated Logic
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

==== Code Generation
<soc-net-reset-generation>
Reset controllers generate standalone modules that are instantiated in the main design, providing clean separation and reusability.

===== Generated Code Structure
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

===== Variable Naming Conventions
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

===== Generated Modules
The reset controller generates dedicated modules with component-based implementations:
- Component instantiation using qsoc_rst_sync, qsoc_rst_pipe, and qsoc_rst_count modules
- Async reset synchronizer (qsoc_rst_sync) when async attribute is specified
- Sync reset pipeline (qsoc_rst_pipe) when sync attribute is specified
- Counter-based reset release (qsoc_rst_count) when count attribute is specified
- Custom combinational logic for signal routing and polarity handling

===== Generated Code Example
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

===== Reset Component Modules
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

==== Best Practices
<soc-net-reset-practices>
===== Design Guidelines
- Use `ASYNC_SYNC` for most digital logic requiring synchronized reset release
- Use `ASYNC_DIRECT` only for simple pass-through or clock-independent paths
- Implement power-on-reset with `ASYNC_COUNT` for reliable startup timing
- Group related resets in the same controller for better organization
- Use descriptive reset source and target names

===== YAML Structure Guidelines
- Always use singular forms (`source`, `target`) instead of plurals
- Specify clear type names instead of cryptic abbreviations
- Use structured parameters instead of string parsing
- Maintain consistent polarity naming (`low`/`high`)
- Include test_enable bypass for DFT compliance

=== CLOCK SECTION
<soc-net-clock>
The `clock` section defines clock controller primitives using a simplified format that eliminates complex type configurations. Clock operations are specified through direct attributes, with automatic selection of mux types based on signal presence.

==== Clock Overview
<soc-net-clock-overview>
Clock controllers manage clock distribution with two processing levels: link-level and target-level. Each level supports specific operations in a defined order, providing clear signal flow without explicit type enumeration.

Key features include:
- Two-level processing: link-level (div→inv) and target-level (mux→icg→div→inv)
- Automatic mux type selection based on reset signal presence
- Direct attribute specification without complex type parsing
- ETH Zurich glitch-free mux implementation with DFT support
- Template RTL cells replaceable with foundry-specific IP
- Complete processing chain from multiple sources to single target

==== Clock Structure
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

==== Processing Levels
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

===== Processing Order
Signal processing follows a defined order at each level:

*Link Level*: `source` → `div` → `inv` → output to target

*Target Level*: `mux` → `icg` → `div` → `inv` → final output

===== Operation Examples
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

==== Clock Multiplexing
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

===== Multiplexer Configuration
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

==== Clock Properties
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

===== Target Properties
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

===== Link Properties
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


==== Template RTL Cells
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

===== Auto-generated template file: clock_cell.v
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


==== Code Generation
<soc-net-clock-generation>
Clock controllers generate standalone modules that provide clean clock management infrastructure.

===== Generated Code Structure
The clock controller generates a dedicated `clkctrl` module with:
1. Template RTL cell definitions (if not already defined)
2. Clock and test enable inputs with frequency documentation
3. Clock input signal declarations with frequency specifications
4. Clock target signal outputs with frequency documentation
5. Internal wire declarations for intermediate clock signals
6. Clock logic instantiations using template cells or foundry IP
7. Output assignment logic with proper multiplexing and inversion

===== Generated Code Example
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

===== Syntax Summary
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

==== Best Practices
<soc-net-clock-practices>
===== Processing Strategy
- Use link-level processing for per-source operations (individual clock conditioning)
- Use target-level processing for final output operations (common to all sources)
- Reset signal presence automatically selects mux type (GF_MUX vs STD_MUX)
- Include DFT signals (test_enable, test_clock) for glitch-free mux when needed

===== Syntax Guidelines
- Always specify input frequencies for proper SDC generation
- Use clear clock names indicating purpose and frequency
- Specify attributes only when operations are needed
- Group related clocks in the same controller
- Reset signals default to active-low polarity
- Include `test_en` bypass for comprehensive DFT support

===== Design Guidelines
- Ensure target frequencies match division ratios mathematically
- Use proper SI units (Hz, kHz, MHz, GHz)
- Consider clock domain crossing requirements for multi-clock systems
- Replace template cells with foundry-specific implementations for production

=== BUS SECTION
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

==== Width Information Preservation
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

=== LINK AND UPLINK ATTRIBUTES
<soc-net-link-uplink>
The `link` and `uplink` attributes provide convenient shortcuts for creating connections without explicitly defining nets in the `net` section. These attributes serve different purposes and have distinct design philosophies.

==== Overview
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

==== Link Attribute
<soc-net-link>

===== Basic Usage
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

===== Bit Selection Support
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

==== Uplink Attribute
<soc-net-uplink>

===== Basic Usage
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

===== Design Philosophy
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

===== Limitations and Rationale
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

===== Conflict Resolution
<soc-net-uplink-conflicts>
When using `uplink`, QSoC automatically handles conflicts:

- If a top-level port with the same name already exists and is compatible (same width, compatible direction), the connection is made
- If the existing port is incompatible, an error is reported
- Multiple `uplink` attributes with the same name are allowed as long as all connected ports are compatible

==== Comparison and Usage Guidelines
<soc-net-link-uplink-guidelines>

===== When to Use Link
Use `link` when you need:
- *Internal signal routing* between modules
- *Bit selection* or signal manipulation
- *Complex connections* with width adaptation
- *Bus segmentation* across multiple modules

===== When to Use Uplink
Use `uplink` when you need:
- *Direct I/O port mapping* (chip pins)
- *Simple, one-to-one connections* to top-level
- *Automatic port generation* without manual definition
- *Clear correspondence* between internal and external signals

===== Best Practices
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

=== BIT SELECTION
<soc-net-bit-selection>
Bit selection allows connecting specific bits of a port to a net, enabling flexible signal routing and bus segmentation. This feature is supported by the `link` attribute and explicit `net` definitions.

==== Syntax and Formats
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

==== Implementation Details
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

==== Use Cases
<soc-net-bit-selection-use-cases>

===== Data Bus Segmentation
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

===== Control Signal Mapping
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

===== Mixed Width Connections
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

==== Limitations
<soc-net-bit-selection-limitations>

===== Attribute Support
- *`link`*: ✓ Full bit selection support
- *`uplink`*: ✗ No bit selection support (see uplink limitations for rationale)
- *`net`*: ✓ Full bit selection support via `bits` attribute

===== Width Validation
The system performs automatic validation:
- Bit selections exceeding port width generate warnings
- Undriven nets (no source connections) are flagged
- Width mismatches between connected ports are reported

==== Best Practices
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

# Net connections (using list format for flexibility)
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
  data_bus:              # Data communication bus
    - instance: cpu0
      port: data_out
      bits: "[7:0]"      # Connect only lower 8 bits
    - instance: ram0
      port: data_in      # Full width connection

# Bus interface connections
bus:
  cpu_ram_bus:           # AXI bus connection between CPU and RAM
    - instance: cpu0
      port: axi_master   # CPU acts as AXI master
    - instance: ram0
      port: axi_slave    # RAM acts as AXI slave

# Combinational logic
comb:
  # Simple AND gate
  - out: enable_signal
    expr: "cpu_ready & ram_ready"

  # Address decoder using case statement
  - out: chip_select
    case: addr_high
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

=== PROCESSING FLOW
<soc-net-processing>
When QSoC processes a SOC_NET file, it follows this sequence:

1. Parse all module definitions referenced in the instance section
2. Validate port connections against module definitions
3. Process `link` and `uplink` attributes to generate nets and top-level ports
4. Expand bus connections into individual nets based on bus interface definitions
5. Process and validate combinational logic (`comb`) section
6. Process and validate sequential logic (`seq`) section
7. Process and validate finite state machine (`fsm`) section
8. Calculate effective widths for all connections, considering bit selections
9. Check for width mismatches and generate appropriate warnings
10. Generate Verilog output based on the processed netlist

The Verilog generation follows this structure:
1. Module declaration with ports and parameters
2. Wire declarations (from processed nets)
3. Module instantiations (from instance section)
4. Combinational logic blocks (from comb section)
5. Sequential logic blocks (from seq section)
6. Finite state machine blocks (from fsm section)
7. Module termination

=== NOTE ON VERILOG PORT WIDTHS
<soc-net-verilog-widths>
QSoC correctly handles Verilog port width declarations where LSB is not zero. For example, a port declared as `output [7:3] signal` in Verilog has a width of 5 bits. The SOC_NET format and processing logic properly calculates this width as `|7-3|+1 = 5`. This ensures accurate width checking even with non-zero-based bit ranges.

== NETLIST VALIDATION FEATURES
<soc-net-validation>
QSoC includes comprehensive netlist validation capabilities to ensure design integrity and catch potential issues early in the design process.

=== PORT DIRECTION CHECKING
<soc-net-port-direction>
The netlist processor performs sophisticated port direction validation to detect connectivity issues:

*Top-Level Port Handling*:
- Correctly recognizes that top-level `input` ports should drive internal logic
- Correctly recognizes that top-level `output` ports should be driven by internal logic
- Prevents false warnings about top-level port direction conflicts
- Properly handles bidirectional (`inout`) top-level ports

*Multiple Driver Detection*:
- Identifies nets with multiple output drivers that could cause conflicts
- Allows legitimate multiple drivers on non-overlapping bit ranges
- Reports potential bus contention issues with detailed diagnostic information

*Undriven Net Detection*:
- Identifies nets that have no driving source (all input ports)
- Helps catch incomplete connections and missing driver assignments
- Provides clear error messages indicating which nets need attention

=== BIT-LEVEL OVERLAP DETECTION
<soc-net-bit-overlap>
Advanced bit-level analysis prevents conflicts in multi-driver scenarios:

*Bit Range Analysis*:
- Analyzes bit selections like `[7:4]` and `[3:0]` for overlap detection
- Allows multiple drivers on non-overlapping bit ranges of the same net
- Detects conflicts when bit ranges overlap between different drivers

*Supported Bit Selection Formats*:
- Range selections: `signal[7:0]`, `signal[15:8]`
- Single bit selections: `signal[3]`, `signal[0]`
- Mixed range scenarios with proper overlap validation

*Example Scenarios*:
```yaml
# Valid: Non-overlapping bit ranges
net:
  data_bus:
    - { instance: cpu, port: data_out[7:4] }    # Upper nibble
    - { instance: mem, port: data_out[3:0] }    # Lower nibble

# Invalid: Overlapping bit ranges (will generate warning)
net:
  addr_bus:
    - { instance: cpu, port: addr_out[7:4] }    # Bits 7-4
    - { instance: dma, port: addr_out[5:2] }    # Bits 5-2 overlap with 5-4
```

=== VALIDATION DIAGNOSTICS
<soc-net-diagnostics>
QSoC provides detailed diagnostic information for all validation issues:

*Comprehensive Error Reports*:
- Exact instance and port names involved in conflicts
- Bit range information for overlap detection
- Clear descriptions of the nature of each problem
- Suggestions for resolving connectivity issues

*Warning Categories*:
- `Multiple Drivers`: Multiple outputs driving the same net or overlapping bits
- `Undriven Nets`: Nets with no output drivers
- `Width Mismatches`: Port width incompatibilities
- `Direction Conflicts`: Improper port direction usage

*Integration with Generation Flow*:
- Validation occurs during Verilog generation process
- Issues are reported without preventing generation (when possible)
- Allows iterative design refinement with immediate feedback
