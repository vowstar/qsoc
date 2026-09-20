= PRCM
<prcm-check>

PRCM declares a controller, each managed domain, and legal stable modes. Version 1 checks resource binding and stable modes. Circuit generation supports APB4 and AXI4-Lite with one management clock.

```sh
qsoc generate verilog --check prcm.soc_net
```

The command reads input files without loading a project or writing RTL. Paths are relative to the current directory. Each file is checked separately unless `--merge` is set. The check does not accept `--force` or `--format`.

Use `--merge` to combine resource files with one PRCM declaration:

```sh
qsoc generate verilog --check --merge clock.soc_net reset.soc_net prcm.soc_net
```

Each input file contains one YAML document. Use `--merge` for multiple files. Each resource has one definition. Duplicate declarations report both input locations. Merged diagnostics retain the original file, line, column, and field path.

The controller uses an always-on supply and an active-low reset. Each domain binds a direct positive-edge clock gate and an asynchronous reset with synchronous release. The domain request cannot replace the management reset. Gate controls and domain reset requests must not also control another target.

Supply, quiesce, and isolation feedback use the management clock. A request output cannot also serve as completion feedback. Test bypass, clock selection, division, asynchronous feedback, and composite `power` controllers are outside the current binding scope.

Optional `controller.reset.target` names a reset-tree output for runtime management reset. Its release uses the controller clock input, and its source list includes `controller.reset.source`. Domain control must not drive its reset requests. This command checks the connection, without a runtime reset proof.

Save this example as `prcm.soc_net`:

```yaml
clock:
  - name: clock
    input: {aon_clk: {}}
    target:
      periph_clk:
        icg: {enable: gate_en, reset: por_n}
        link: {aon_clk: {}}
reset:
  - name: reset
    source: {por_n: {active: low}, hold_n: {active: low}}
    target:
      periph_n:
        active: low
        async: {clock: periph_clk, stage: 2}
        link: {por_n: {}, hold_n: {}}
prcm:
  version: 1
  controller:
    clock: {controller: clock, input: aon_clk}
    reset: {controller: reset, source: por_n, stage: 2}
    supply: aon
  mmio: {bus: apb4, data_width: 32, address_width: 12}
  supply:
    aon: {always_on: true}
    periph:
      request: power_en
      valid: {signal: pgood, sample_clock: aon_clk}
  domain:
    periph:
      supply: periph
      clock: {controller: clock, target: periph_clk, stage: target.icg}
      reset: {controller: reset, source: hold_n, target: periph_n}
      quiesce:
        request: stop_req
        ack: {signal: idle, sample_clock: aon_clk}
      isolation:
        request: iso_req
        active: {signal: iso_active, sample_clock: aon_clk}
      reset_mode: 'OFF'
      mode:
        'OFF':
          code: 0
          power: 'off'
          clock: stopped
          reset: asserted
          isolation: enabled
        'RUN':
          code: 1
          power: 'on'
          clock: running
          reset: released
          isolation: disabled
      transition: [{from: 'OFF', to: 'RUN'}, {from: 'RUN', to: 'OFF'}]
```

`code` is the software mode value. `reset_mode` selects the initial request. It does not establish the physical reset state. Request and feedback signals use separate names.

The stable mode check enforces one mode per domain. A running clock requires valid power. An unpowered domain requires a stopped clock, asserted reset, and active isolation. Domains on one supply share the same power value. Service requirements constrain the provider mode.

MMIO uses the existing bus width limits. Register allocation, transitions, and RTL behavior are outside this check. A valid stable mode does not prove a safe path to that mode.

Conflicts report the input file, line, column, and field path. Each solver query has a ten-second limit. A conflict, timeout, unknown result, or input error returns a nonzero exit code.

== Circuit Generation

Generate a controller inside an existing project:

```sh
qsoc generate verilog -d demo prcm.soc_net
qsoc generate verilog -d demo --merge prcm.soc_net clock.soc_net reset.soc_net
qsoc generate verilog -d demo --with-formal prcm.soc_net
```

The first input basename selects the module name. Generation checks stable modes, each action model, and conditional service progress before writing files. Other circuit sections and additional clock or reset controllers require a later template and produce an error.

Set `controller.reset.stage` to at least two. This selects the controller reset receivers, independently of each resource target's `async.stage`. The check command permits omission, but circuit generation requires an explicit value.

The template accepts OFF, RESET, and RUN resource states. Mode names and software codes remain user-defined. It requires an OFF state for fault recovery and all directed transitions between declared modes. A shared circuit supports one service layer. A provider cannot depend on another service. Every running mode of a consumer must require the same service set.

For two domains with OFF and RUN modes, add these entries under `prcm`. Each domain still needs its own resource bindings and mode definitions.

```yaml
domain:
  fabric:
    service:
      online: {mode: RUN}
  periph:
    require:
      access: {service: fabric.online, mode: [RUN]}
chip:
  reset_mode: SLEEP
  mode:
    SLEEP:
      code: 0
      domain: {fabric: {target: 'OFF'}, periph: {target: 'OFF'}}
    NORMAL:
      code: 1
      domain: {fabric: {allow: ['OFF', RUN]}, periph: {allow: ['OFF', RUN]}}
```

For `prcm.soc_net`, the project output contains:

#table(
  columns: (auto, 1fr),
  [Path], [Content],
  [`prcm/rtl/prcm.v`], [Controller connections],
  [`prcm/rtl/prcm_register.v`], [MMIO register bank],
  [`prcm/rtl/prcm_clock.v`], [Clock controller],
  [`prcm/rtl/prcm_reset.v`], [Reset controller],
  [`prcm/rtl/qsoc_prcm_domain.v`], [Single-domain action FSM],
  [`prcm/rtl/qsoc_prcm_domain_service.v`], [Action FSM for a shared circuit],
  [`prcm/rtl/qsoc_prcm_service.v`], [Service handshake, when required],
  [`prcm/rtl/clock_cell.v`, `prcm/rtl/reset_cell.v`], [Replaceable resource cells],
  [`prcm/rtl/prcm.fl`], [RTL file list, relative to its directory],
  [`prcm/include/prcm.h`], [Register offsets, field masks, and mode codes],
  [`prcm/integration/prcm.json`], [Circuit bindings, interface conditions, and check scope],
  [`prcm/formal/prcm_formal.sv`], [Optional RTL assertions and environment],
  [`prcm/formal/check.sby`, `prcm/formal/prcm_formal.fl`], [Optional proof job and file list],
)

Ordinary outputs are regenerated. Existing resource cells remain unchanged unless `--force` is set. `--format` formats the top module before publication. Input or model failures leave existing outputs unchanged.

For a single-domain circuit, REQUEST, STATUS, and EVENT occupy three consecutive bus words. STATUS contains the raw request, done, invalid_mode, and fault. The mode code and three status bits must fit one data word. EVENT records power loss and uses write-one-to-clear semantics.

A shared circuit allocates DOMAIN_name_REQUEST, DOMAIN_name_STATUS, and DOMAIN_name_EVENT in domain-name order. Optional CHIP_REQUEST and CHIP_STATUS precede them. The generated header supplies each offset, mask, and qualified mode code.

A consumer keeps its service request until it stops using the provider. The provider executes its local shutdown target after the last consumer releases it. Each chip mode either fixes a domain target or permits every local mode. A chip policy that blocks a required provider is rejected.

STATUS adds blocked_by_chip for chip control, in_use for a provider, and wait_service for a consumer. EVENT adds service_lost for a consumer. Shared integration reports use version 2 and record feedback per domain. Single-domain reports retain version 1.

Management reset clears the software request to reset_mode and retains action state, accepted transactions, pending responses, and event history. A pending write can complete after reset and change the target again. Cold reset cancels transactions. Multiple software users must serialize a complete mode operation through the platform's normal locking and MMIO ordering rules.

A new mode request waits for an active reset release to complete before it can reassert reset. Fault protection remains immediate.

Sequence checks cover normal feedback. Progress requires a stable target and eventual feedback. Customer logic and cell replacements need separate checks.

`binding` records top-level instances and port connections. Receiver entries identify registers, inputs, clock edges, resets, and stage counts. Names are relative to the top module. Map them to the actual cell and netlist before applying physical constraints.

`--with-formal` emits checks for the actual circuit in a separate directory. Proof jobs select the synthesis branch of resource cells. The RTL file list contains only synthesis input. File generation does not run the RTL checks. The integration report records not_run.

#table(
  columns: (auto, 1fr),
  [Check], [Scope],
  [Power], [While the power request is off, reset stays active and the domain clock stops. Isolation and prior work drain complete before power removal within the same cold-reset interval.],
  [Bus], [Accepted request and response state, address errors, byte-masked REQUEST updates, STATUS flags, and EVENT history through management reset.],
  [Single-domain reachability], [Work admission, power loss, shutdown, and bus traffic during management reset after operation starts.],
  [Shared reachability], [Declared mode completion, software shutdown, invalid requests, management reset, service use and release, and faults after operation starts.],
)

The RTL checks compare register values and power, isolation, and quiesce requests with an independent phase model. Shared proof jobs separate normal operation from fault response and check actual clock and reset outputs. Hardware event set takes priority over software clear. Management reset retains event history.

Safety checks allow arbitrary feedback delay, write data, byte masks, and sampled power loss. Single-domain bounded reachability uses one cold start, a legal operating mode then OFF, full strobes, and one-cycle feedback. RUN and management-reset coverage require those features in the input. Reachability does not prove eventual completion. Physical timing and synchronization reliability need separate checks.

Shared reachability uses one cold start, full strobes, and one-cycle isolation and drain feedback. Normal cases use one-cycle power feedback. Fault cases permit power loss. A domain without RUN uses its powered RESET state. A shutdown witness requires an OFF write while active and local request completion. Management reset cancels this observation.

Cover failure reports a goal not reached within the search bound. Chip policy can block a declared local mode.
