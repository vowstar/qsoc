= PRCM Checks
<prcm-check>

PRCM declares a controller, its managed domain, and legal stable modes. Version 1 supports resource binding and stable mode queries.

```sh
qsoc generate verilog --check prcm.soc_net
```

The command reads input files without loading a project or writing RTL. Paths are relative to the current directory. Each file is checked separately. The check does not accept `--merge`, `--force`, or `--format`.

The controller uses an always-on supply and an active-low reset. Each domain binds a direct positive-edge clock gate and an asynchronous reset with synchronous release. The domain request cannot replace the management reset. Gate controls and domain reset requests must not also control another target.

Supply, quiesce, and isolation feedback use the management clock. A request output cannot also serve as completion feedback. Test bypass, clock selection, division, asynchronous feedback, and composite `power` controllers are outside the current binding scope.

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
    reset: {controller: reset, source: por_n}
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
