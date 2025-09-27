= POWER CONTROLLER FORMAT
<power-format>
Manages voltage domain sequencing with dependency tracking and auto-healing.

== KEY FEATURES
- Domain type: no `depend` = AO, `depend: []` = root, `depend: [...]` = normal
- Hard depend timeout → FAULT (block), soft depend timeout → fault flag (proceed)
- Auto-heal: cooldown then retry when hard depends ready
- DFT: `test_en=1` forces all domains on

== YAML STRUCTURE
```yaml
power:
  - name: pwr0
    host_clock: clk_ao
    host_reset: rst_ao
    test_enable: test_en         # DFT force-on (optional)
    domain:
      # AO: no depend
      - name: ao
        v_mv: 900
        pgood: pgood_ao
        wait_dep: 0
        settle_on: 0
        settle_off: 0
        follow:
          clock: []
          reset: []

      # Root: depend: []
      - name: vmem
        depend: []
        v_mv: 1100
        pgood: pgood_vmem
        wait_dep: 50
        settle_on: 100
        settle_off: 50
        follow:
          clock: []
          reset: []

      # Normal: mixed depends
      - name: gpu
        depend:
          - name: ao
            type: hard           # block on timeout
          - name: vmem
            type: soft           # warn on timeout
        v_mv: 900
        pgood: pgood_gpu
        wait_dep: 200
        settle_on: 120
        settle_off: 80
        follow:
          clock: [clk_gpu]
          reset: [rst_gpu]
```

== DOMAIN TYPES
- No `depend` → AO (no switch, always on)
- `depend: []` → Root (has switch, no depends)
- `depend: [...]` → Normal (has switch, depends)

== FSM STATES
S_OFF → S_WAIT_DEP → S_TURN_ON → S_ON → S_TURN_OFF → S_OFF
Fault path: any timeout → S_FAULT → auto-heal after cooldown

== PORTS
Inputs: `clk_ao`, `rst_ao`, `test_en`, `pgood_<dom>`, `en_<dom>`, `clr_<dom>`
Outputs: `icg_en_<dom>`, `rst_allow_<dom>`, `sw_<dom>`, `rdy_<dom>`, `flt_<dom>`

Domain reset: `rst_<dom>_n = rst_sys_n & rst_allow_<dom>`

== POWER_FSM MODULE
```verilog
module power_fsm #(
    parameter integer HAS_SWITCH        = 1,
    parameter integer WAIT_DEP_CYCLES   = 100,
    parameter integer SETTLE_ON_CYCLES  = 100,
    parameter integer SETTLE_OFF_CYCLES = 50
) (
    input  wire clk,
    input  wire rst_n,
    input  wire test_en,
    input  wire ctrl_enable,
    input  wire fault_clear,
    input  wire dep_hard_all,
    input  wire dep_soft_all,
    input  wire pgood,
    output reg  clk_enable,
    output reg  rst_allow,
    output reg  pwr_switch,
    output reg  ready,
    output reg  valid,
    output reg  fault
);
```

Behaviors:
- Counter: load N-1, zero means no wait
- Hard timeout → FAULT + cooldown → auto-retry
- Soft timeout → fault flag, continue
- `test_en=1` → force all active

== DEPEND AGGREGATION
```verilog
wire dep_hard_all_<dom> = rdy_dep1 & rdy_dep2 & ...;  // all hard
wire dep_soft_all_<dom> = rdy_dep3 & ...;             // all soft
```
No depends → tie to 1'b1

== INSTANTIATION EXAMPLE
```verilog
/* AO domain */
power_fsm #(
    .HAS_SWITCH(0), .WAIT_DEP_CYCLES(0),
    .SETTLE_ON_CYCLES(0), .SETTLE_OFF_CYCLES(0)
) u_pwr_ao (
    .clk(clk_ao), .rst_n(rst_ao), .test_en(test_en),
    .ctrl_enable(1'b1), .fault_clear(1'b0),
    .dep_hard_all(1'b1), .dep_soft_all(1'b1),
    .pgood(pgood_ao),
    .clk_enable(icg_en_ao), .rst_allow(rst_allow_ao),
    .pwr_switch(), .ready(rdy_ao), .valid(), .fault()
);

/* Root domain */
power_fsm #(
    .HAS_SWITCH(1), .WAIT_DEP_CYCLES(50),
    .SETTLE_ON_CYCLES(100), .SETTLE_OFF_CYCLES(50)
) u_pwr_vmem (
    .clk(clk_ao), .rst_n(rst_ao), .test_en(test_en),
    .ctrl_enable(en_vmem), .fault_clear(clr_vmem),
    .dep_hard_all(1'b1), .dep_soft_all(1'b1),
    .pgood(pgood_vmem),
    .clk_enable(icg_en_vmem), .rst_allow(rst_allow_vmem),
    .pwr_switch(sw_vmem), .ready(rdy_vmem),
    .valid(), .fault(flt_vmem)
);

/* Normal domain */
wire dep_hard_all_gpu = rdy_ao;
wire dep_soft_all_gpu = rdy_vmem;
power_fsm #(
    .HAS_SWITCH(1), .WAIT_DEP_CYCLES(200),
    .SETTLE_ON_CYCLES(120), .SETTLE_OFF_CYCLES(80)
) u_pwr_gpu (
    .clk(clk_ao), .rst_n(rst_ao), .test_en(test_en),
    .ctrl_enable(en_gpu), .fault_clear(clr_gpu),
    .dep_hard_all(dep_hard_all_gpu),
    .dep_soft_all(dep_soft_all_gpu),
    .pgood(pgood_gpu),
    .clk_enable(icg_en_gpu), .rst_allow(rst_allow_gpu),
    .pwr_switch(sw_gpu), .ready(rdy_gpu),
    .valid(), .fault(flt_gpu)
);
```

== PROPERTIES
#figure(
  align(center)[#table(
    columns: (auto, auto, auto, auto),
    align: (left, left, left, left),
    table.header([*Property*], [*Type*], [*Required*], [*Description*]),
    [`name`], [String], [Yes], [Controller name],
    [`host_clock`], [String], [Yes], [Host clock],
    [`host_reset`], [String], [Yes], [Host reset],
    [`test_enable`], [String], [No], [DFT signal],
    [`domain`], [Array], [Yes], [Domain list],
  )],
  caption: [Power Controller Properties],
)

#figure(
  align(center)[#table(
    columns: (auto, auto, auto, auto),
    align: (left, left, left, left),
    table.header([*Property*], [*Type*], [*Required*], [*Description*]),
    [`name`], [String], [Yes], [Domain name],
    [`depend`], [Array], [No], [Absent=AO, []=root, list=normal],
    [`v_mv`], [Integer], [No], [Voltage mV],
    [`pgood`], [String], [Yes], [Power good signal],
    [`wait_dep`], [Integer], [Yes], [Depend wait cycles],
    [`settle_on`], [Integer], [Yes], [On settle cycles],
    [`settle_off`], [Integer], [Yes], [Off settle cycles],
    [`follow`], [Map], [No], [Clock/reset lists],
  )],
  caption: [Domain Properties],
)

#figure(
  align(center)[#table(
    columns: (auto, auto, auto, auto),
    align: (left, left, left, left),
    table.header([*Property*], [*Type*], [*Required*], [*Description*]),
    [`name`], [String], [Yes], [Depend domain name],
    [`type`], [String], [No], [hard or soft (default hard)],
  )],
  caption: [Depend Properties],
)
