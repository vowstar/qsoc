= IOMUX Generator
<iomux-generator>
The IOMUX generator turns one sparse route table into a high-speed pin
multiplexer: an AXI4-Lite selector slave, a per-pin mux core, a connection
fabric, and one public wrapper. Its source stays in the module's `.soc_mod`
entry.

Every design gets the four roles `input_value`, `input_enable`,
`output_value`, and `output_enable`. Software control of a pin, pin
interrupts, register control of the pad pull and of every other pad control,
runtime inversion, and register overrides of the receive path are options
that a source turns on one by one.

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

An `output_value` endpoint may add `open_drain: true`. It then stands for
both output roles: the value becomes the constant 0 and the enable follows
the inverted link, so the pad drives low on a 0 and releases on a 1. Such a
route declares no `output_enable`. The report and the generated files show
the expanded form.

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
`integration.pad` names only `io`, the net that vector uplinks to. With a
`safe` row the wrapper also takes `pad_force_i`, linked from
`integration.force`.

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
  control:
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
error. The same holds for a missing `pull` section or an absent control.

`pull.table` maps mode names to the values the pull ports take, one entry per
port, transcribed from the databook. `none` is required and encodes as the
all-zero selector. `up`, `down`, and `keeper` carry meaning to the generator.
Any other name is a mode the cell documents, which a route asks for by that
name. A mode holds either one row or a map of strength labels to rows, so a
cell with two pull-up strengths and one pull-down strength is expressed as
`up: {"47k": [...], "100k": [...]}` and `down: [...]`. A route writes
`pull: up` for a single row and `pull: {mode: up, strength: "47k"}` for a
labelled one.

`control` declares every other input group of the cell: drive strength,
slew rate, Schmitt trigger, analog enable, an open-drain mode pin, a filter
enable, whatever the databook lists. Each control names its pins, a table of
labelled rows, and an optional `default` row, which is otherwise the first.
The control name is yours. It must be a Verilog identifier, because it
appears as is in ports, register fields, and the report, and it may not be
one of the names the generator owns: the four roles, `pull`, `pull_mode`,
`up_sel`, `down_sel`, `keep`, `osc`, `select`, `io`, anything starting with
`rx_`, or anything ending in `_src` or `_inv`. A cell may declare up to 16
controls of up to 256 rows each. A route asks for a row by label under
`control`, as in `control: {drive: high, slew: fast}`, and a slot that
names none takes the default. A control with one row has nothing to select:
its pins take that row and it owns no field and no port.

A row may also follow a net at bus speed. `control: {mode: {link:
i3c0_sda_oe, on: pp, off: od}}` gives the slot the `on` row while the net is
high and the `off` row while it is low, with the usual `bit` and `invert` on
the link, and `pull: {link: sleep_n, invert: true, on: down, off: up}` does
the same for the pull, where `on` and `off` take a mode name or a `mode` and
`strength` map. The net becomes a wrapper input named
`hs_p<pin>_s<slot>_<group>_select_i` and a link in the integration fragment.
It is what an open-drain mode pin of an I3C controller or a pull that
changes for sleep needs. The register source bit still wins over the net,
and a slot that is not selected contributes nothing, exactly as for a fixed
row.

`safe` names the row every pin takes while the wrapper input `pad_force_i`
is high: `input_enable`, `output_value`, and `output_enable` as 0 or 1, `pull`
as a mode or a `mode` and `strength` map, and any control by its row label.
Anything left out is 0, `none`, or the control's default. Declaring `safe`
adds the `pad_force_i` port and requires `integration.force`, the net that
drives it, typically the isolation or test signal of the power domain.
Nothing outranks it: not a register, not a slot, not a select net. That is
the whole priority order of a pad output, and it reads in one line: forced
selects the safe row, otherwise a set source bit selects the register,
otherwise the selected slot's link or constant.

The rule behind the names is short. A name the generator gives behaviour
to is fixed: the roles, `none`, `up`, `down`, `keeper`, `oscillator`. Every
other name, a pull mode, a strength, a control, a row, is yours and is
copied through unchanged.

Only `up` and `down` carry strength rows. Every other mode is one row. A
route may ask for `keeper` or `oscillator` when the cell has no row of that
name. The generator then weaves the mode from `up` and `down`: the keeper
follows the pad and the oscillator opposes it, and both read the pad itself
rather than the receiver, so the loop closes inside the pad module. A woven
mode keeps its strength: `pull: {mode: keeper, strength: "47k"}` selects that
row on each graded direction, and an absent strength selects the first row.
`pull.kind: driver` marks a cell whose pulls are its output driver rather
than resistors, and no mode is woven from those. A woven mode is a
combinational loop through the pad. A zero-delay simulation of a floating pad
under a woven keeper does not settle, so a testbench drives the pad across a
mode change or gives it a delay.

A cell that gates its receiver on `input_enable` reads zero on any pin whose
sinks listen while no slot raises that enable. Generation refuses such a pin
unless `option.gpio` or `option.invert` is on, because either register path
can then raise it.

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
The first 16 bytes identify the block. The byte map is the same for both
data widths: a 64-bit instance packs each pair into one beat. All four words
are read-only and ignore writes.

#figure(
  align(center)[#table(
    columns: 3,
    align: (left, left, left),
    table.header([Offset], [Word], [Content]),
    table.hline(),
    [0x0], [`version`], [layout contract: major `[31:24]`, minor `[23:16]`, patch `[15:8]`],
    [0x4], [`type`], [0x494F4D58, the letters IOMX read as one hex value],
    [0x8], [`capability`], [`pin_count` `[15:0]`, `hs_slots` `[23:16]`],
    [0xC], [`feature`], [one bit per option: 0 gpio, 1 interrupt, 2 pad_control, 3 invert, 4 rx_override],
  )],
  caption: [IOMUX IDENTITY WORDS],
)

Software reads `version` first, because that is where a driver written for
another instance of this type looks, then `type` to confirm the block, then
`capability` and `feature` to compute the rest of the map. The layout
contract is 2.0.0. A block appended after the existing ones steps the minor
number. Any existing offset that moves steps the major number. With
`pin_count`, `hs_slots`, `data_width`, and the feature bits every block
offset below is computable, and the report prints them.

Selector registers start at 0x10. Every pin owns a fixed 4-bit lane; the
field uses the low `ceil(log2(hs_slots))` bits and the remaining lane bits
read zero and ignore writes. A 32-bit word holds 8 pins and a 64-bit word
holds 16, so no field crosses a byte and one write strobe never splits a
selector. Selector offsets depend only on `pin_count` and `data_width`,
never on `hs_slots`. Generation fails when `2^address_width` cannot hold
the aperture and reports the minimum usable width.

Options append register blocks after the selectors in a fixed order: the
four gpio banks, one `pin_src_ctrl` word per pin, one `pin_pad_ctrl` word
per pin, the inversion banks, the receive override banks, and the interrupt
banks. A block whose option is off is absent and the next block moves up, so
the report is the authority for offsets. A bank holds one bit per pin,
`data_width` pins per word, so one store flips the same bit on a whole word
of pins. A per-pin word holds that pin's whole configuration, so one store
reconfigures a pin without a read-modify-write and without touching its
neighbours.

`pin_src_ctrl` exists when any option owns a field in it, and every field
keeps a fixed position whatever else is on, so software reads the same word
on every design. Absent fields read zero.

#figure(
  align(center)[#table(
    columns: 3,
    align: (left, left, left),
    table.header([Bits], [Field], [Option]),
    table.hline(),
    [0], [`input_enable_src`], [`gpio`],
    [3:2], [`output_value_src`], [`gpio`],
    [5:4], [`output_enable_src`], [`gpio`],
    [6], [`pull_src`], [`pad_control`, when the cell has a pull table],
    [8 + k], [`rx_src_sk`], [`rx_override`, one bit per slot k],
    [16 + i], [`<control>_src`], [`pad_control`, one bit per selectable control i in declaration order],
  )],
  caption: [PIN_SRC_CTRL LAYOUT],
)

`generator.option.gpio` appends the registers that let software drive and
read a pin: the banks `input_value` (read-only), `input_enable`,
`output_value`, and `output_enable`, and the three source fields above.

A source field selects where the pad signal comes from. `output_value_src`
takes 0 for the selected slot, 1 for the register, 2 for the slot input
enable, and 3 for the slot output enable. `output_enable_src` takes 0 for
the selected slot, 1 for the register, 2 for the slot output value, and 3
to stop driving. A cross tap reads the slot output and never the source mux
output, so no combination of the two fields closes a loop. Setting
`output_enable_src` to 2 ties the drive enable to the slot output value, so
the pad drives a one and releases a zero. That is open source, not open
drain. Open drain needs the enable to follow the inverted value: at
generation time through `open_drain: true` on the route, at run time through
`output_enable_src` at 2 together with the pin's `output_enable_inv` bit.

Every source field resets to 0, so a design that enables `gpio` and writes
nothing behaves exactly as if the option were absent. `input_value` reads
the pad through two flip-flops in the bus clock domain, so a pad edge takes
two bus cycles to become readable.

`generator.option.pad_control` needs a `pad_cell` with a pull table or a
control of more than one row. It appends one `pin_pad_ctrl` word per pin
for the pull, then one `pin_ctl_k` word per pin for each group of four
controls, each control in an 8-bit slot at bit `8 * (i mod 4)` in
declaration order. A single-row control keeps its slot empty, so its
neighbours never move. The fields below are present only when the cell has
something for them to select and each is as wide as its table needs.

#figure(
  align(center)[#table(
    columns: 3,
    align: (left, left, left),
    table.header([Bits], [Field], [Meaning]),
    table.hline(),
    [from 0], [`pull_mode`], [0 none, 1 up, 2 down, 3 keeper, 4 oscillator, then the cell's other modes from 5 in name order],
    [from 8], [`up_sel`], [strength row of `up`, table order, only when `up` has several rows],
    [from 16], [`down_sel`], [strength row of `down`, likewise],
  )],
  caption: [PIN_PAD_CTRL LAYOUT],
)

The mode values are fixed, so software reads the same field on every design.
A value the cell has no row for behaves as `none`, a strength select past
the table behaves as the first row, and a control value past its table
behaves as the control's default row. The register keeps what was written, so
firmware can read its own mistake back. A mode says whether and which way
the pin pulls, a select says how strongly, and the two never mix: the keeper
and the oscillator switch the mode between `up` and `down` from the pad
level and leave both selects alone, so they hold at whatever strength the
selects name. The report prints the mode numbering and each graded
direction's strengths. `pull_src` at 0 keeps the pull the selected slot's
route asked for, and at 1 hands mode and selects to the word. Each
`<control>_src` does the same for its control. All reset to 0, so the words
are inert until software claims them. A register-driven keeper or oscillator is the same woven
loop as a route request, with the same simulation caveat.

`generator.option.invert` appends the banks `input_enable_inv`,
`output_value_inv`, `output_enable_inv`, and one `rx_inv_sk` bank per slot
k. Each bit inverts its signal after the source selector, so it inverts
whichever source the pin currently uses. The cross taps read the uninverted
slot bundle. `input_value` and the interrupt detectors read the pad and are
not affected. Pull and drive codes are not bits and have no inversion.

`generator.option.rx_override` appends one `rx_value_sk` bank per slot k.
While `rx_src_sk` of a pin is set, the sink of slot k on that pin reads the
register bit instead of the pad, which holds an unselected peripheral's input
at a known level. Substitution happens before inversion.

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
    [185, 32-bit], [24], [28], [0x10 to 0x6C], [112 bytes],
    [185, 64-bit], [12], [14], [0x10 to 0x68], [112 bytes],
    [256, 32-bit], [32], [36], [0x10 to 0x8C], [144 bytes],
    [256, 64-bit], [16], [18], [0x10 to 0x88], [144 bytes],
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
(`<module>_hs_formal.sv`, `<module>_hs_formal.sby`). The routing proof leaves
every option register free and asserts, per slot and for invalid codes, the
pad bundle after source selection, inversion, and the safe row under
`pad_force_i`, the pull mode, strength selects, and every control row after
their source bits and the same force, and every receive sink after substitution and
inversion. A pad cell adds the constraint proof described above.

== UVM Collateral
<iomux-uvm-collateral>
`--with-uvm` reuses the MMIO UVM testbench for `<module>_regs` only. It
covers the register slave and does not cover routing, the connection fabric,
or the pads; those are covered by the directed simulation and the formal
routing proof.
