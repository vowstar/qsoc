= POWER CONTROLLER FORMAT
<power-format>
The power section defines power controller primitives that manage voltage domain sequencing with dependency tracking and automatic fault recovery. Power primitives provide comprehensive power management with support for multiple domain types, hard/soft dependencies, and DFT override capabilities.

== POWER OVERVIEW
<soc-net-power-overview>
Power controllers manage voltage domain sequencing through a three-domain architecture: AO (always-on), root (controllable without dependencies), and normal (controllable with dependencies). Each domain follows a strict power-up sequence: switch → pgood → clock enable → reset release, with configurable timing and automatic fault recovery.

Key features include:
- Three domain types with automatic inference from dependency configuration
- Hard dependencies (block on timeout) and soft dependencies (warn on timeout)
- Automatic fault recovery with cooldown and retry mechanisms
- DFT test mode bypass for all domains (test_en intended for static scan/test operations; deassert only when system is quiescent)
- FSM-based power sequencing with standardized timing
- Template RTL cells replaceable with foundry-specific implementations

== POWER STRUCTURE
<soc-net-power-structure>
Power controllers use domain-based configuration with automatic type inference based on dependency presence. Each domain specifies timing parameters and dependency relationships without complex type enumeration:

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
            reset: rst_req_gpu_n     # Synchronized reset output (rst_req_*)
            stage: 4                 # Synchronizer stages (optional, default: 4)
```

== POWER DOMAINS
<soc-net-power-domains>
Power controllers automatically infer domain types from configuration structure, eliminating the need for explicit type specification:

=== AO Domain Type
<soc-net-power-ao>
Always-on domains have no dependency key and remain permanently active:
- No power switch control (HAS_SWITCH=0)
- Always enabled (ctrl_enable=1'b1)
- Zero timing parameters (no wait or settle cycles)
- Used for essential infrastructure like AO power rails
- If pgood signal absent, generator ties to 1'b1 and relies on settle cycles
- If pgood is absent, at least one of settle_on/settle_off must be non-zero

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

== POWER FSM OPERATION
<soc-net-power-fsm>
Each domain uses a standardized 8-state FSM for power sequencing:

State sequence: S_OFF → S_WAIT_DEP → S_TURN_ON → S_CLK_ON → S_ON → S_RST_ASSERT → S_TURN_OFF → S_OFF
Fault handling: any timeout → S_FAULT → auto-heal after cooldown

Power-up sequence timing: switch → pgood/settle → clock enable → reset release
Power-down sequence timing: reset assert → clock disable → switch off → pgood drop/settle

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

    output reg  clk_enable,       /**< ICG enable for this domain clock     */
    output reg  rst_allow,        /**< active-high reset allow for domain   */
    output reg  pwr_switch,       /**< power switch control                 */

    output reg  ready,            /**< domain usable clock on reset off     */
    output reg  valid,            /**< voltage stable                       */
    output reg  fault             /**< sticky fault indicator               */
);
```

Key behaviors:
- Counter load: N-1 (zero means no wait)
- Hard timeout: enter FAULT state, block until auto-heal
- Soft timeout: set fault flag, continue operation
- Clock-reset sequencing: S_CLK_ON provides one cycle for clock stability before reset release
- Reset-clock sequencing: S_RST_ASSERT provides one cycle for reset assertion before clock disable
- DFT override: test_en=1 forces outputs active (pwr_switch=1, clk_enable=1, rst_allow=1, ready=1, valid=1) while preserving FSM state
- With test_en=1, ready=1 for all domains, so dep_hard_all/dep_soft_all evaluate to 1 and dependency checks are bypassed
- Auto-heal works without fault_clear; fault remains sticky until cleared or reset
- Auto-heal: automatic retry after cooldown when dependencies ready
- Cooldown source: auto-heal cooldown uses WAIT_DEP_CYCLES
- All cycle parameters are counted on host_clock (AO clock domain)
- Reset release is synchronized to clock to meet recovery/removal timing requirements

Also included in power_cell.v is qsoc_power_rst_sync for domain reset synchronization:
```verilog
module qsoc_power_rst_sync #(parameter integer STAGE=4)(
    input  wire clk_dom,      /**< domain clock source                   */
    input  wire rst_gate_n,   /**< async assert, sync deassert           */
    input  wire test_en,      /**< DFT force release                     */
    output wire rst_dom_n     /**< synchronized domain reset, active-low */
);
```

qsoc_power_rst_sync provides async assert, sync deassert reset synchronization. Assert does not require clock, deassert requires STAGE edges on clk_dom. Default STAGE=4 provides better metastability protection.

== GENERATED INTERFACES
<soc-net-power-interfaces>
Power controllers generate standardized interfaces with predictable naming:

Inputs: `clk_ao`, `rst_ao_n`, `test_en`, `pgood_<domain>`, `en_<domain>`, `clr_<domain>`

Note: `clr_<domain>` is optional; when absent tie low and rely on auto-heal
Outputs: `icg_en_<domain>`, `rst_allow_<domain>`, `sw_<domain>`, `rdy_<domain>`, `flt_<domain>`

Note: `test_en`, `en_<domain>`, `clr_<domain>`, and `pgood_<domain>` must be synchronized into host_clock domain

Signal semantics:
- `ready`: Asserted when FSM state = S_ON, equivalent to domain fully operational
- `valid`: Equals 1 in S_ON; equals pgood in S_TURN_ON/S_TURN_OFF; 0 otherwise
- `rst_allow`: Active-high reset permission, domain reset = `rst_sys_n & rst_allow`
- Dependency aggregation uses `ready_<dep>` signals exclusively

Domain reset composition: `rst_<domain>_n = rst_sys_n & rst_allow_<domain>`

Dependency aggregation is automatic:
```verilog
/* Generated for normal domains with dependencies */
wire dep_hard_all_gpu = rdy_ao;              /**< Hard dependencies only */
wire dep_soft_all_gpu = rdy_vmem;            /**< Soft dependencies only */
/* No dependencies = tie to 1'b1 */
```

== RESET SYNCHRONIZATION
<soc-net-power-reset-sync>
Power controllers support domain-specific reset synchronization through follow entries. Each entry mechanically maps to a qsoc_power_rst_sync instance using KISS (Keep It Simple) principle:

```yaml
follow:                          # Reset synchronizer array (optional)
  - clock: clk_gpu               # Domain clock input (required)
    reset: rst_gpu_n             # Synchronized reset output (required)
    stage: 4                     # Synchronizer stages (optional, default: 4)
  - clock: clk_gpu_dsp           # Additional synchronizers for same domain
    reset: rst_gpu_dsp_n
    stage: 6                     # Different stage count
```

Key characteristics:
- Direct array format eliminates ambiguous clock/reset pairing from previous versions
- Each entry becomes one qsoc_power_rst_sync instance with dedicated ports
- Reset gate signal: `rst_sys_n & rst_gate_domain_n` (async assert, sync deassert)
- FSM outputs `rst_gate_n` (internal permission), synchronizer outputs `rst_req_*_n` (final reset)
- Test enable bypass preserves DFT capability
- Stage parameter controls synchronizer depth (1-16 stages typical)
- Empty follow array generates no synchronizers (common for AO/root domains)

Generated RTL pattern per entry:
```verilog
qsoc_power_rst_sync #(.STAGE(4)) u_rst_sync_gpu_0 (
    .clk_dom     (clk_gpu),
    .rst_gate_n  (rst_sys_n & rst_gate_gpu_n),
    .test_en     (test_en),
    .rst_dom_n   (rst_req_gpu_n)
);
```

== CODE GENERATION
<soc-net-power-generation>

=== Diagram Output
<soc-net-power-diagram>
Generates `.typ` circuit diagram alongside Verilog.

*Elements*: Domains → FSM → SYNC → Ready (with dependencies/timing/parameters)

*Files*: `<module>.v`, `<module>.typ` (compile: `typst compile <module>.typ`)

== PROPERTIES
<soc-net-power-properties>

#figure(
  align(center)[#table(
    columns: (auto, auto, auto, auto),
    align: (left, left, left, left),
    table.header([*Property*], [*Type*], [*Required*], [*Description*]),
    [`name`], [String], [Yes], [Controller instance name],
    [`host_clock`], [String], [Yes], [AO host clock signal],
    [`host_reset`], [String], [Yes], [AO host reset signal, active-low],
    [`test_enable`], [String], [No], [DFT test enable signal],
    [`domain`], [Array], [Yes], [Power domain definitions],
  )],
  caption: [Power Controller Properties],
)

#figure(
  align(center)[#table(
    columns: (auto, auto, auto, auto),
    align: (left, left, left, left),
    table.header([*Property*], [*Type*], [*Required*], [*Description*]),
    [`name`], [String], [Yes], [Domain instance name],
    [`depend`], [Array], [No], [Absent=AO, []=root, list=normal],
    [`v_mv`], [Integer], [No], [Voltage level in millivolts],
    [`pgood`], [String], [No], [Power good input signal (ties 1'b1 if absent)],
    [`wait_dep`], [Integer], [Yes], [Dependency wait cycles],
    [`settle_on`], [Integer], [Yes], [Power-on settle cycles],
    [`settle_off`], [Integer], [Yes], [Power-off settle cycles],
    [`follow`], [Array], [No], [Reset synchronizer entry array],
  )],
  caption: [Domain Properties],
)

#figure(
  align(center)[#table(
    columns: (auto, auto, auto, auto),
    align: (left, left, left, left),
    table.header([*Property*], [*Type*], [*Required*], [*Description*]),
    [`name`], [String], [Yes], [Dependency domain name],
    [`type`], [String], [No], [hard or soft (default: hard)],
  )],
  caption: [Dependency Properties],
)
