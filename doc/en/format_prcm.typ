= PRCM
<prcm-check>

PRCM declares a controller, its managed domain, and legal stable modes. Version 1 checks resource binding and stable modes. Circuit generation supports one switched domain with APB4 or AXI4-Lite.

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

The first input basename selects the module name. Generation checks stable modes and the single-domain action model before writing files. Other circuit sections and additional clock or reset controllers require a later template and produce an error.

Set `controller.reset.stage` to at least two. This selects the controller reset receivers, independently of each resource target's `async.stage`. The check command permits omission, but circuit generation requires an explicit value.

The template accepts OFF, RESET, and RUN resource states. Mode names and software codes remain user-defined. It requires an OFF state for fault recovery and all directed transitions between declared modes. Chip policy and domain service ownership do not yet have a circuit template.

For `prcm.soc_net`, the project output contains:

#table(
  columns: (auto, 1fr),
  [Path], [Content],
  [`prcm/rtl/prcm.v`], [Controller connections],
  [`prcm/rtl/prcm_register.v`], [MMIO register bank],
  [`prcm/rtl/prcm_clock.v`], [Clock controller],
  [`prcm/rtl/prcm_reset.v`], [Reset controller],
  [`prcm/rtl/qsoc_prcm_domain.v`], [Domain action FSM],
  [`prcm/rtl/clock_cell.v`, `prcm/rtl/reset_cell.v`], [Replaceable resource cells],
  [`prcm/rtl/prcm.fl`], [RTL file list, relative to its directory],
  [`prcm/include/prcm.h`], [Register offsets, field masks, and mode codes],
  [`prcm/integration/prcm.json`], [Interface conditions and model check scope],
  [`prcm/formal/prcm_formal.sv`], [Optional RTL assertions and environment],
  [`prcm/formal/check.sby`, `prcm/formal/prcm_formal.fl`], [Optional proof job and file list],
)

Ordinary outputs are regenerated. Existing resource cells remain unchanged unless `--force` is set. `--format` formats the top module before publication. Input or model failures leave existing outputs unchanged.

REQUEST, STATUS, and EVENT occupy three consecutive bus words. STATUS contains the raw request, done, invalid_mode, and fault. The mode code and three status bits must fit one data word. EVENT records power loss and uses write-one-to-clear semantics.

Management reset clears the software request to reset_mode and retains action state, accepted transactions, pending responses, and event history. A pending write can complete after reset and change the target again. Cold reset cancels transactions. Multiple software users must serialize a complete mode operation through the platform's normal locking and MMIO ordering rules.

Sequence checks cover the normal feedback model. Progress requires a stable target and eventual feedback. Customer logic, cell replacements, and physical timing need separate checks. The integration report leaves physical checks incomplete.

`--with-formal` emits checks for the actual circuit in a separate directory. The RTL file list contains only synthesis input. File generation does not run the RTL checks. The integration report records not_run.

#table(
  columns: (auto, 1fr),
  [Check], [Scope],
  [Power], [Isolation, reset, and stopped clock before power removal. Work admission requires drain before removal within the same cold-reset interval.],
  [Bus], [Accepted request and response state, address errors, byte-masked REQUEST updates, and REQUEST readback through management reset.],
  [Reachability], [Work admission, power loss, shutdown, and bus traffic during management reset after operation starts.],
)

The safety environment permits arbitrary feedback delay, write data, byte masks, and sampled power loss. Reachability uses one cold start, a legal operating mode followed by OFF, full write strobes, one-cycle feedback, and a finite depth. It does not prove eventual completion. RUN and management-reset coverage only apply when the input declares them. STATUS completion flags, EVENT values, physical reset timing, and synchronization reliability are outside these RTL assertions.
