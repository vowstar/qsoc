= LOGIC DESCRIPTION FORMAT
<logic-description-format>
The logic description format provides high-level behavioral modeling capabilities through combinational and sequential logic blocks, allowing designers to describe complex digital behavior using intuitive YAML syntax.

== COMBINATIONAL LOGIC (COMB)
<soc-net-comb>
The `comb` section defines combinational logic blocks that generate pure combinational Verilog code. This section allows you to describe combinational logic using a high-level YAML DSL that is then translated to appropriate Verilog constructs.

=== Overview
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

=== Basic Structure
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

=== Simple Assignment (expr)
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

=== Conditional Logic (if)
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

=== Case Statement Logic (case)
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

=== Nested Conditional Logic
<soc-net-comb-nested>
The combinational logic supports nested structures by allowing `then` fields to contain nested `case` statements:

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
    default: "4'b0000"                   # NOP or unknown instruction
```

=== Complex Control Unit Example
<soc-net-comb-complex-example>
A more sophisticated example demonstrates comprehensive instruction decoding with multiple nested levels:

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

=== Best Practices
<soc-net-comb-best-practices>
- Always provide `default` values for `if` and `case` logic to avoid latch inference
- Use descriptive signal names that reflect their purpose
- Keep expressions simple and readable
- Prefer case statements over long if-else chains for discrete value switching

== SEQUENTIAL LOGIC (SEQ)
<soc-net-seq>
The `seq` section defines sequential logic blocks that generate pure sequential Verilog code with proper clock and reset handling.

=== Overview
<soc-net-seq-overview>
The `seq` section supports various types of sequential logic with comprehensive control over clocking, reset, enable, and next-state logic:

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
  )],
  caption: [SEQUENTIAL LOGIC FEATURES],
  kind: table,
)

=== Basic Structure
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

=== Simple Assignment
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

=== Clock Edge Control
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
always @(negedge clk) begin
    neg_edge_reg <= data_in;
end
```

=== Reset Logic
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

=== Enable Logic
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

=== Conditional Logic
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

=== Complex Nested Sequential Logic
<soc-net-seq-nested>
More sophisticated sequential logic can include nested case statements within conditional logic:

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

=== Design Guidelines
<soc-net-seq-guidelines>
- Always specify reset values for registers that need initialization
- Use descriptive register names that indicate their function
- Group related registers with the same clock and reset signals
- Use enable signals for conditional register updates
- Provide default values in conditional logic to maintain current state
- Consider clock edge requirements carefully for timing-critical designs

== MIXED COMBINATIONAL AND SEQUENTIAL LOGIC
<soc-net-mixed-logic>
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

=== Key Features Demonstrated
<soc-net-mixed-logic-features>
The generated examples showcase several important features:

1. *Verilog 2005 Compliance*: All output signals use internal register pattern with `_reg` suffix and continuous assign statements to ensure compatibility.

2. *FSM Naming Conventions*: FSM-generated signals use lowercase FSM names with underscores (e.g., `test_onehot_cur_state`), while state constants use uppercase (e.g., `TEST_ONEHOT_S0`).

3. *Internal Register Pattern*: Both combinational and sequential logic outputs use internal registers followed by assign statements for proper wire/reg type separation.

4. *Bit Width Inference*: The system automatically infers correct bit widths from port declarations and applies them to internal registers.

5. *ROM Initialization*: Microcode FSMs properly initialize ROM contents with correct bit width padding (e.g., `{5'd1, 2'd0, 8'h55}` for 15-bit ROM words).

6. *Expression Handling*: Sequential logic expressions correctly reference output ports (e.g., `shift_reg << 1` refers to the output port, not the internal register).
