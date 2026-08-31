= IOMUX Generator
<iomux-generator>
The IOMUX generator turns one sparse route table into a high-speed pin
multiplexer: an AXI4-Lite selector slave, a per-pin mux core, a connection
fabric, and one public wrapper. Its source stays in the module's `.soc_mod`
entry.

Every design gets the four roles `input_value`, `input_enable`,
`output_value`, and `output_enable`. Software control of a pin, pin
interrupts, and pad pull and drive control are options that a source turns on
one by one. Register overrides of the receive path and runtime inversion are
not generated.

== Source Format
<iomux-source-format>
Create a draft, edit the generated library file, then validate it:

```bash
qsoc module create --generator iomux -l <library> <module>
qsoc module validate -l <library> <module>
qsoc generate module -l <library> <module>
```

`create` writes a recognized but incomplete draft. `validate` reports the
missing `pin_count` and `integration` sections until the source is complete.

```yaml
iomux0:
  generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 8
    pin_count: 2
    hs_slots: 2
    option:
      gpio: true
    integration:
      instance: u_iomux0
      clock: clk_iomux
      reset: rst_iomux_n
      control: iomux_control
      pad:
        input_value: pad_input_value
        input_enable: pad_input_enable
        output_value: pad_output_value
        output_enable: pad_output_enable
    route:
      - pin: 0
        slot: 0
        function: gpio0
        signal: data0
        input_value: {link: gpio0_input, bit: 0}
        input_enable: 1
        output_value: {link: gpio0_output, bit: 0}
        output_enable: {link: gpio0_enable, bit: 0}
      - pin: 0
        slot: 1
        function: uart0
        signal: tx
        output_value: {link: uart0_tx}
        output_enable: 1
```

`pin_count` is required and ranges from 1 to 256. It is never inferred from
the routes or the pad width. `hs_slots` ranges from 2 to 8 and defaults to 4
when omitted; an explicit 4 and the default generate byte-identical files.
Both are generation-time configuration, not Verilog parameters.

Each route names a `pin` below `pin_count`, a `slot` below `hs_slots`, and
non-empty `function` and `signal` labels used only for reports. A `(pin,
slot)` pair appears at most once. Role values are an endpoint map with `link`,
an optional `bit`, and an optional boolean `invert`, or for the three output
roles the integer `0` or `1`. An omitted output role drives `0`; an omitted
`input_value` declares no sink. HDL expressions, slices, and concatenations
are rejected.

== Behavior
<iomux-behavior>
Each pin owns one selector field. The selected slot drives `input_enable`,
`output_value`, and `output_enable` as one bundle. Undeclared slots and
selector codes at or above `hs_slots` drive an all-zero bundle. Every
selector resets to 0 and therefore selects slot 0. The pad input value
broadcasts to every declared `input_value` sink of that pin regardless of the
selector. `invert` applies one XOR inside the connection fabric.

`rst_ni` is asynchronous, so a selector takes its reset value as soon as the
reset asserts. Before the reset source itself drives `rst_ni`, and while an
inserted scan chain shifts, the selector registers hold no defined value and
`pad_output_enable_o` follows them. Slot 0 also carries whatever the route
table declares for it, not a guaranteed-safe bundle. A design that must not
drive its pads before firmware runs takes that guarantee from the pad cell
power-on state or from isolation outside this module; the generator emits
neither.

== Pad Cell
<iomux-pad-cell>
`generator.pad_cell` names the pad cell the design uses and the generator
instantiates it, one per pin, in `<module>_pad.v`. The public wrapper then
exposes a single `pad_io` vector in place of the four pad vectors, and
`integration.pad` names only `io`, the net that vector uplinks to.

```yaml
pad_cell:
  cell: gpio_pad_ps
  port:
    pad: PAD
    input_value: C
    input_enable: IE
    output_value: I
    output_enable: OE
  pull:
    port: [PE, PS]
    table:
      none: ["0", "x"]
      up: ["1", "1"]
      down: ["1", "0"]
  drive:
    port: [DS]
    table:
      low: ["0"]
      high: ["1"]
  constraint:
    - name: pull_select_needs_enable
      expr: "!PS || PE"
```

Every port named here must exist on the cell in the module library with a
matching direction, and every input of the cell must be named here, or
generation stops before it writes a file: an input the declaration forgets
would be left floating in a netlist that elaborates. A role that is absent
from `port` is a role the cell lacks, and a route that asks for it is an
error. The same holds for a missing `pull` or `drive` section.

`pull.table` maps mode names to the values the pull ports take, one entry per
port, transcribed from the databook. `none` is required and encodes as the
all-zero selector. `up`, `down`, and `keeper` carry meaning to the generator.
Any other name is a mode the cell documents, which a route asks for by that
name. A mode holds either one row or a map of strength labels to rows, so a
cell with two pull-up strengths and one pull-down strength is expressed as
`up: {"47k": [...], "100k": [...]}` and `down: [...]`. A route writes
`pull: up` for a single row and `pull: {mode: up, strength: "47k"}` for a
labelled one. `drive.table` has the same shape without the mode level.

A route may ask for `keeper` or `oscillator` when the cell has no row of that
name. The generator then weaves the mode from the first `up` and first `down`
rows: the keeper follows the pad and the oscillator opposes it, and both read
the pad itself rather than the receiver, so the loop closes inside the pad
module. `pull.kind: driver` marks a cell whose pulls are its output driver
rather than resistors, and no mode is woven from those. A woven mode is a
combinational loop through the pad. A zero-delay simulation of a floating pad
under a woven keeper does not settle, so a testbench drives the pad across a
mode change or gives it a delay.

A cell that gates its receiver on `input_enable` reads zero on any pin whose
sinks listen while no slot raises that enable. Generation refuses such a pin
unless `option.gpio` is on, because the register path can then raise it.

`constraint` lists properties over the cell ports, one per pin. `expr` is
combinational and `property` is sampled on the formal global clock, where
`$past`, `$rose`, and `$stable` are available. The body is not parsed:
identifiers that name cell ports are rewritten to the nets of each pin, and
any other identifier must be a SystemVerilog keyword or a system function.
The open engine has no SVA sequences, so write an implication as a boolean.
The generator settles `kind` when it is absent. A body over pull and drive
ports alone is a claim about logic this generator emits, so it is an
`assert`. A body that reaches a role port speaks about what routes and
registers will do, so it is an `assume`. The constraints live in `_pad.v`
under `ifdef FORMAL`, and `--with-formal` emits `<module>_pad_formal.sby`
that proves them against a stub of the cell built from the library port
table. A false claim fails by name.

== Register Layout
<iomux-register-layout>
Offset 0 is a read-only `capability` register: bits `[15:0]` hold
`pin_count` and bits `[23:16]` hold `hs_slots`. Selector registers follow
from the next data beat. Every pin owns a fixed 4-bit lane; the field uses
the low `ceil(log2(hs_slots))` bits and the remaining lane bits read zero and
ignore writes. A 32-bit word holds 8 pins and a 64-bit word holds 16, so no
field crosses a byte and one write strobe never splits a selector. Selector
offsets depend only on `pin_count` and `data_width`, never on `hs_slots`.
Generation fails when `2^address_width` cannot hold the aperture and reports
the minimum usable width.

`generator.option.gpio` appends the registers that let software drive and
read a pin. Four banked vectors follow the selectors, each holding one bit
per pin: read-only `input_value`, then `input_enable`, `output_value`, and
`output_enable`. One `pin_src_ctrl` word per pin follows them, so one store
reconfigures a pin without a read-modify-write and without touching its
neighbours. That word holds `input_enable_src` at bit 0, `output_value_src`
at bits `[3:2]`, and `output_enable_src` at bits `[5:4]`.

A source field selects where the pad signal comes from. `output_value_src`
takes 0 for the selected slot, 1 for the register, 2 for the slot input
enable, and 3 for the slot output enable. `output_enable_src` takes 0 for
the selected slot, 1 for the register, 2 for the slot output value, and 3
to stop driving. A cross tap reads the slot output and never the source mux
output, so no combination of the two fields closes a loop. Setting
`output_enable_src` to 2 ties the drive enable to the slot output value, so
the pad drives a one and releases a zero. That is open source, not open
drain. Open drain needs the enable to follow the inverted value, which no
source encoding reaches because there is no runtime inversion. A route that
declares `output_value: 0` with an inverted `output_enable` link gives open
drain, fixed at generation time.

Every source field resets to 0, so a design that enables `gpio` and writes
nothing behaves exactly as if the option were absent. `input_value` reads
the pad through two flip-flops in the bus clock domain, so a pad edge takes
two bus cycles to become readable.

`generator.option.interrupt` appends four enable banks and four pending
banks, one bit per pin in each, for the high level, the low level, the
rising edge, and the falling edge. It also adds an `irq_o` output that
carries one line per `data_width` pins.

A pending bit records its event whether or not the matching enable is set,
so a pin that reaches no interrupt line can still be polled. The enable
gates `irq_o` alone. Software clears a pending bit by writing one to it,
and a set that lands on the same cycle as that write wins, so an event
cannot vanish into its own acknowledgement. Edge detection compares the
second synchronizer stage against a third, so the pad must hold a level for
one bus cycle to register as an edge.

#figure(
  align(center)[#table(
    columns: 5,
    align: (left, right, right, left, right),
    table.header([Pins, width], [Selector regs], [Total regs], [Selector offsets], [Aperture]),
    table.hline(),
    [185, 32-bit], [24], [25], [0x04 to 0x60], [100 bytes],
    [185, 64-bit], [12], [13], [0x08 to 0x60], [104 bytes],
    [256, 32-bit], [32], [33], [0x04 to 0x80], [132 bytes],
    [256, 64-bit], [16], [17], [0x08 to 0x80], [136 bytes],
  )],
  caption: [IOMUX SELECTOR LAYOUT],
)

== Generated Artifacts
<iomux-generated-artifacts>
Generation writes six files under `output/<library>/<module>/`:
`<module>_regs.v`, `<module>_conn.v`, `<module>.v` with the private core and
the public wrapper, the `<module>.f` file list, the `<module>.iomux.rpt`
route report, and the `<module>_integration.soc_net` fragment. Selector
sidebands stay inside the wrapper and never reach the public interface. Each
endpoint port carries a `function.signal` comment in the wrapper header. The
report shows each selector location and lists unused slots per pin.

== Integration
<iomux-integration>
`module validate` checks the IOMUX source only; link existence, widths,
directions, and drivers are checked when the fragment is merged. Assemble the
final design with the existing merge flow:

```bash
qsoc generate verilog --merge <base.soc_net> <module>_integration.soc_net
```

The fragment instantiates the public wrapper once and connects the clock, the
reset, the four pad vectors, the control bus, and every non-constant endpoint
exactly once. The control link must carry exactly one master before the merge
and exactly one master and one slave after it. An invalid generator source
blocks the whole netlist instead of falling back to a stale module view. A
generated IOMUX instance name may not already exist in another merged input.
Any failed generated-module check leaves an existing top-level output untouched.
Routes may cover individual bits of a wider vector; unlisted bits remain outside
the generated IOMUX connections, and partial coverage does not emit a width
`FIXME`. Those bits stay undriven unless another merged input drives them, and
read as `z` in simulation.

== Formal Collateral
<iomux-formal-collateral>
`--with-formal` writes two jobs: the register slave proof
(`<module>_regs_formal.sv`, `<module>_regs_formal.sby`) and a routing proof
(`<module>_hs_formal.sv`, `<module>_hs_formal.sby`) that asserts the selected
bundle per slot, the all-zero bundle for invalid codes, the pad input
broadcast, and the inversion of each routed endpoint.

== UVM Collateral
<iomux-uvm-collateral>
`--with-uvm` reuses the MMIO UVM testbench for `<module>_regs` only. It
covers the register slave and does not cover routing, the connection fabric,
or the pads; those are covered by the directed simulation and the formal
routing proof.
