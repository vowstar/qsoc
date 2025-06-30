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
      [comb], [Defines combinational logic blocks for behavioral descriptions],
      [seq], [Defines sequential logic blocks for register-based descriptions],
      [fsm], [Defines finite state machine blocks for complex control logic],
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
      [`if`], [Conditional logic with if-else chain], [`always @(*)` block with if-else],
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
      [`then`], [Value when condition is true (required, scalar or nested structure)],
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
module test_fsm_encodings (
    input  clk,
    input  rst_n,
    input  trigger,
    output onehot_output,
    output gray_output
);

    /* Wire declarations */
    /* Module instantiations */

    /* Finite State Machine logic */

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
FSM logic is generated after sequential logic in the Verilog output, providing structured control flow implementation.

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
      [`link`], [Internal signal routing with flexible bit selection], [Module-to-module connections, bus segmentation, signal distribution]),
      [`uplink`], [Direct I/O port mapping], [Chip pins, power/ground, clock/reset signals]),
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
        uplink: chip_reset_n
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
      [[high:low]], [Range selection (e.g., "[7:3]" selects bits 7 through 3, 5 bits wide)],
      [[bit]], [Single bit selection (e.g., "[5]" selects only bit 5, 1 bit wide)],
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
</rewritten_file>
