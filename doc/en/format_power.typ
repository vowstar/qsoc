= Power Controller Format
<power-format>
The `power` section defines power domains, sequencing, and dependencies.

For MMIO control and checked domain sequencing, see @prcm-check.

#block(
  fill: rgb("#fffce8"),
  inset: 8pt,
  radius: 4pt,
  stroke: rgb("#a08410") + 0.5pt,
)[
  *Standalone module:* a `power:` block generates a self-contained
  Verilog module named after `power.name`. The generated module is NOT
  auto-instantiated by any parent netlist: the user instantiates it
  manually (or via `qsoc module import` followed by an `_inst.soc_net`
  entry) at the top level.

  Unlike clock/reset, the power controller's `depend:` field
  references *only* domains declared in the same controller (no
  auto-input pattern). A `depend` entry with a missing or undeclared
  name is an error, and nothing is generated.
]

== Power Overview
<soc-net-power-overview>
Power controllers manage voltage domain sequencing through a three-domain architecture: AO (always-on), root (controllable without dependencies), and normal (controllable with dependencies). Each domain follows a strict power-up sequence: switch → pgood → clock enable → reset release, with configurable timing and automatic fault recovery.

Supported behavior:
- Three domain types with automatic inference from dependency configuration
- Hard dependencies (block on timeout) and soft dependencies (warn on timeout)
- Automatic fault recovery with cooldown and retry mechanisms
- DFT test mode bypass for all domains (test_en intended for static scan/test operations; deassert only when system is quiescent)
- FSM-based power sequencing with standardized timing
- Template RTL cells in the regenerated `qsoc_cell` unit

== Power Structure
<soc-net-power-structure>
Each domain specifies timing and dependencies. Dependency presence determines its type:

```yaml
# Power controller with three domain types
power:
  - name: soc_pwr_ctrl               # Controller instance name (required)
    host_clock: clk_ao               # AO host clock (required)
    host_reset: rst_ao_n             # AO host reset, active-low (required)
    test_enable: test_en             # Test enable bypass signal (optional)
    domain:
      # AO domain: no depend key = always-on type
      - name: ao
        v_mv: 900                    # Voltage level (informational)
        pgood: pgood_ao              # Power good input signal (optional)
        wait_dep: 0                  # Dependency wait cycles
        settle_on: 0                 # Power-on settle cycles
        settle_off: 0                # Power-off settle cycles
        follow: []                   # Reset synchronizer entries (typically empty for AO)

      # Root domain: depend: [] = controllable root type
      - name: vmem
        depend: []                   # Empty dependency list = root domain
        v_mv: 1100
        pgood: pgood_vmem
        wait_dep: 0
        settle_on: 100
        settle_off: 50
        follow: []                   # Reset synchronizer entries (typically empty for root)

      # Normal domain: depend: [...] = dependent type
      - name: gpu
        depend:                      # Dependency list = normal domain
          - name: ao                 # Hard dependency (default)
            type: hard               # Block on timeout
          - name: vmem
            type: soft               # Warn on timeout, continue
        v_mv: 900
        pgood: pgood_gpu
        wait_dep: 200
        settle_on: 120
        settle_off: 80
        follow:                      # Reset synchronizer entries
          - clock: clk_gpu           # Domain clock (typically post-ICG)
            reset: rst_gpu_n         # Synchronized reset output
            stage: 4                 # Synchronizer stages (optional, default: 4)
```

== Power Domains
<soc-net-power-domains>
Power controllers automatically infer domain types from configuration structure, eliminating the need for explicit type specification:

=== AO Domain Type
<soc-net-power-ao>
Always-on domains have no dependency key and remain permanently active:
- No power switch control (HAS_SWITCH=0)
- Always enabled (`ctrl_enable=1'b1`)
- Zero timing parameters (no wait or settle cycles)
- Used for essential infrastructure like AO power rails
- If pgood signal absent, the generator ties the FSM input to `1'b1`

*Warning*: a domain without `pgood` cannot complete a power-down. `S_TURN_OFF`
leaves only through `!pgood`, so a tied-high input makes the FSM enter
`S_FAULT` once `settle_off` expires and latch the sticky fault bit. Give every
controllable domain a real `pgood`, or keep the domain permanently on. Nothing
in the generator enforces this, and `wait_dep`, `settle_on` and `settle_off`
all default to zero even though the property table marks them required.

=== Root Domain Type
<soc-net-power-root>
Root domains have empty dependency arrays and operate independently:
- Power switch control enabled (HAS_SWITCH=1)
- No dependency wait requirements
- Controllable through enable/clear inputs
- Used for primary power domains like memory controllers

=== Normal Domain Type
<soc-net-power-normal>
Normal domains have dependency lists and wait for prerequisite domains:
- Power switch control enabled (HAS_SWITCH=1)
- Hard dependencies: timeout causes FAULT state (blocks operation)
- Soft dependencies: timeout sets fault flag (allows continuation)
- When wait_dep=0: hard dependency failure enters FAULT immediately, soft dependency failure warns but continues
- Automatic dependency aggregation with AND gates
- Used for peripheral domains with power sequencing requirements

== Power FSM Operation
<soc-net-power-fsm>
Each domain uses a standardized 8-state FSM for power sequencing:

State sequence: S_OFF → S_WAIT_DEP → S_TURN_ON → S_CLK_ON → S_ON → S_RST_ASSERT → S_TURN_OFF → S_OFF
Fault handling: any timeout → S_FAULT → auto-heal after cooldown

Power-up sequence timing: switch → pgood/settle → clock enable → reset release
Power-down sequence timing: reset assert → clock disable → switch off → pgood drop/settle

`pgood` must rise within `settle_on` cycles of the switch turning on and fall
within `settle_off` cycles of it turning off, counting the two synchronizer
cycles. Otherwise the domain enters `S_FAULT`.

The qsoc_power_fsm module provides the core sequencing logic:
```verilog
module qsoc_power_fsm
#(
    parameter integer HAS_SWITCH        = 1,   /**< 1=drive power switch        */
    parameter integer WAIT_DEP_CYCLES   = 100, /**< depend wait window cycles   */
    parameter integer SETTLE_ON_CYCLES  = 100, /**< power-on settle cycles      */
    parameter integer SETTLE_OFF_CYCLES = 50   /**< power-off settle cycles     */
)
(
    input  wire clk,              /**< AO host clock                        */
    input  wire rst_n,            /**< AO host reset, active-low            */

    input  wire test_en,          /**< DFT enable to force on               */
    input  wire ctrl_enable,      /**< target state 1:on, 0:off             */
    input  wire fault_clear,      /**< pulse to clear sticky fault          */

    input  wire dep_hard_all,     /**< AND of all hard-depend ready inputs  */
    input  wire dep_soft_all,     /**< AND of all soft-depend ready inputs  */
    input  wire pgood,            /**< power good of this domain            */

    output wire clk_enable,       /**< ICG enable for this domain clock     */
    output wire rst_gate_n,       /**< reset gate to synchronizer, active-low */
    output wire pwr_switch,       /**< power switch control                 */

    output wire ready,            /**< domain usable clock on reset off     */
    output wire valid,            /**< voltage stable                       */
    output reg  fault             /**< sticky fault indicator               */
);
```

Key behaviors:
- Reset: `host_reset` asserts the FSM at once and releases it on the second
  rising edge of `host_clock` through a `qsoc_sync`, so it needs no external
  synchronization
- Input synchronization: `pgood`, `ctrl_enable` and `fault_clear` each pass a
  two-stage `qsoc_sync` on `clk`, so the FSM sees them two cycles late.
  `dep_hard_all` and `dep_soft_all` come from FSMs on the same host clock and
  are used directly
- Registered outputs: `clk_enable`, `rst_gate_n`, `pwr_switch`, `ready` and
  `valid` are flops loaded from the decode of the next state, reset to the
  `S_OFF` values (all low). They change on the same edge as the state, never
  between edges
- `valid` outside `S_ON` follows `pgood` three cycles late: two in the
  synchronizer, one in its output flop
- Counter load: N-1 (zero means no wait)
- Hard timeout: enter FAULT state, block until auto-heal
- Soft timeout: set fault flag, continue operation
- Clock-reset sequencing: S_CLK_ON provides one cycle for clock stability before reset release
- Reset-clock sequencing: S_RST_ASSERT provides one cycle for reset assertion before clock disable
- DFT override: test_en=1 forces outputs active (pwr_switch=1, clk_enable=1, rst_gate_n=1, ready=1, valid=1) while preserving FSM state. The override is an OR after the output flops, so it holds while scan shifts through them
- With test_en=1, ready=1 for all domains, so dep_hard_all/dep_soft_all evaluate to 1 and dependency checks are bypassed
- Auto-heal works without fault_clear; fault remains sticky until cleared or reset
- Fault clear: `fault_clear` clears `fault` in any state. Entering `S_FAULT` or
  a soft timeout sets it, and a set on the same edge wins. A write that lowers
  `ctrl_enable` and pulses `fault_clear` together ends in the same state
  whichever synchronizer passes its bit first; the cell formal job proves this
  with each bit on time or one cycle late
- Auto-heal: automatic retry after cooldown when dependencies ready
- Cooldown source: auto-heal cooldown uses WAIT_DEP_CYCLES
- All cycle parameters are counted on host_clock (AO clock domain)
- Reset release is synchronized to clock to meet recovery/removal timing requirements

Also included in `qsoc_cell_power.v` is qsoc_power_rst_sync for domain reset synchronization:
```verilog
module qsoc_power_rst_sync #(parameter integer STAGE=4)(
    input  wire clk_dom,      /**< domain clock source                   */
    input  wire rst_gate_n,   /**< async assert, sync deassert           */
    input  wire test_en,      /**< DFT force release                     */
    output wire rst_dom_n     /**< synchronized domain reset, active-low */
);
```

Every run writes `output/qsoc_cell/rtl/qsoc_cell_power.v`, replacing any
existing file (@verilog-output-layout).

qsoc_power_rst_sync provides async assert, sync deassert reset synchronization with a `qsoc_sync` instance `u_sync`. Assert does not require clock, deassert requires STAGE edges on clk_dom. Default STAGE=4 provides better metastability protection. `STAGE` must be at least 1. The `test_en` override is a `qsoc_ck_or2` instance `u_test_or` (@cell-roles).

*Warning*: `test_en` here forces `rst_dom_n` permanently released, so a domain
reset cannot be applied while test mode is active. This is the opposite of the
reset controller cells (@soc-net-reset-components), where `test_en` bypasses
the synchronizer but leaves the reset controllable. Scan patterns that rely on
a reset-based initialization of domain flops are not possible under this cell.

`rst_gate_n` is a flop output ORed with `test_en`, and it drives the
asynchronous reset pin of this synchronizer. Keep `test_en` static while the
domain is live.

`clk_enable` leaves the controller as `icg_en_<domain>` in the host clock
domain and reaches the domain clock gate with no synchronizer. It changes only
while `rst_gate_n` is low on both sides of the change, so the domain is held in
reset whenever its gate enable moves; the cell formal job proves this
(`icg_in_reset`).

== Generated Interfaces
<soc-net-power-interfaces>
Power controllers generate standardized interfaces with predictable naming:

Inputs: `host_clock`, `host_reset`, `rst_sys_n`, `test_en`, `pgood_<domain>`,
`en_<domain>`, `clr_<domain>`, and the domain clock named by each `follow`
entry

Outputs: `icg_en_<domain>`, `sw_<domain>`, `rdy_<domain>`, `flt_<domain>`, and
one synchronized reset per `follow` entry

`en_<domain>` and `clr_<domain>` are generated for every controllable domain
and for no AO domain: an AO domain gets `ctrl_enable` tied to `1'b1` and
`fault_clear` tied to `1'b0` inside the controller. `sw_<domain>` is likewise
absent for AO domains.

*Note*: `pgood_<domain>`, `en_<domain>` and `clr_<domain>` are synchronized to
`host_clock` inside the FSM. Hold a `clr_<domain>` pulse for at least one host
clock cycle. `test_en` must be static

Signal semantics:
- `ready`: Asserted when FSM state = S_ON, equivalent to domain fully operational
- `valid`: Equals 1 in S_ON; equals the synchronized pgood in
  S_TURN_ON, S_CLK_ON, S_RST_ASSERT and S_TURN_OFF; 0 otherwise.
  The controller leaves this port unconnected, so it is observable only inside
  the FSM instance
- `rst_gate_n`: active-low reset gate, an internal wire rather than a
  controller port. It feeds the synchronizer as `rst_sys_n & rst_gate_<domain>_n`
- Dependency aggregation uses `ready_<dep>` signals exclusively

An AO domain has `fault_clear` tied low, so once its sticky `fault` bit is set
only `host_reset` clears it. `flt_<domain>` then stays high for the rest of the
session.

Dependency aggregation is automatic:
```verilog
/* Generated for normal domains with dependencies */
wire dep_hard_all_gpu = rdy_ao;              /**< Hard dependencies only */
wire dep_soft_all_gpu = rdy_vmem;            /**< Soft dependencies only */
/* No dependencies = tie to 1'b1 */
```

== Reset Synchronization
<soc-net-power-reset-sync>
Power controllers support domain-specific reset synchronization through follow entries. Each entry mechanically maps to a qsoc_power_rst_sync instance using KISS (Keep It Simple) principle:

```yaml
follow:                          # Reset synchronizer array (optional)
  - clock: clk_gpu               # Domain clock input (required)
    reset: rst_gpu_n             # Synchronized reset output (required)
    stage: 4                     # Synchronizer stages, at least 1 (optional, default: 4)
  - clock: clk_gpu_dsp           # Additional synchronizers for same domain
    reset: rst_gpu_dsp_n
    stage: 6                     # Different stage count
```

Key characteristics:
- Direct array format eliminates ambiguous clock/reset pairing from previous versions
- Each entry becomes one qsoc_power_rst_sync instance with dedicated ports
- Reset gate signal: `rst_sys_n & rst_gate_domain_n` (async assert, sync
  deassert). This AND is a reset network gate: `rst_sys_n` holds the domain
  resets asserted without changing the FSM, so a system reset does not power
  the domain down
- FSM outputs `rst_gate_n` (internal permission), the synchronizer output is
  named by the entry's `reset` key verbatim
- Test enable forces the domain reset released; it does not preserve
  reset controllability in test mode
- Stage parameter controls synchronizer depth (at least 1)
- Empty follow array generates no synchronizers (common for AO/root domains)

Generated RTL pattern per entry:
```verilog
qsoc_power_rst_sync #(.STAGE(4)) u_rst_sync_gpu_0 (
    .clk_dom     (clk_gpu),
    .rst_gate_n  (rst_sys_n & rst_gate_gpu_n),
    .test_en     (test_en),
    .rst_dom_n   (rst_gpu_n)
);
```

== Code Generation
<soc-net-power-generation>
Run the generator with `qsoc generate verilog` (@verilog-generation).
Connectivity and width problems are reported as described in
@validation-format.


=== Diagram Output
<soc-net-power-diagram>
Generates a `.typ` circuit diagram in the `doc/` directory of the top unit.

*Elements*: Domains → FSM → SYNC → Ready (with dependencies/timing/parameters)

*Files*: `output/<top>/doc/<module>.typ` (compile: `typst compile <module>.typ`)

== Properties
<soc-net-power-properties>

#figure(
  align(center)[#table(
    columns: (0.3fr, 0.2fr, 0.2fr, 1fr),
    align: (left, left, left, left),
    table.header([Property], [Type], [Required], [Description]),
    table.hline(),
    [`name`], [String], [Yes], [Controller instance name],
    [`host_clock`], [String], [Yes], [AO host clock signal],
    [`host_reset`], [String], [Yes], [AO host reset signal, active-low],
    [`test_enable`], [String], [No], [DFT test enable signal],
    [`domain`], [Array], [Yes], [Power domain definitions],
  )],
  caption: [POWER CONTROLLER PROPERTIES],
  kind: table,
)

#figure(
  align(center)[#table(
    columns: (0.3fr, 0.2fr, 0.2fr, 1fr),
    align: (left, left, left, left),
    table.header([Property], [Type], [Required], [Description]),
    table.hline(),
    [`name`], [String], [Yes], [Domain instance name],
    [`depend`], [Array], [No], [Absent=AO, []=root, list=normal],
    [`v_mv`], [Integer], [No], [Voltage level in millivolts],
    [`pgood`], [String], [No], [Power good input signal (ties `1'b1` if absent)],
    [`wait_dep`], [Integer], [Yes], [Dependency wait cycles],
    [`settle_on`], [Integer], [Yes], [Power-on settle cycles],
    [`settle_off`], [Integer], [Yes], [Power-off settle cycles],
    [`follow`], [Array], [No], [Reset synchronizer entry array],
  )],
  caption: [DOMAIN PROPERTIES],
  kind: table,
)

#figure(
  align(center)[#table(
    columns: (0.3fr, 0.2fr, 0.2fr, 1fr),
    align: (left, left, left, left),
    table.header([Property], [Type], [Required], [Description]),
    table.hline(),
    [`name`], [String], [Yes], [Name of a domain in the same controller],
    [`type`], [String], [No], [hard or soft (default: hard)],
  )],
  caption: [DEPENDENCY PROPERTIES],
  kind: table,
)
