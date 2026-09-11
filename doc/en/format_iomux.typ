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
qsoc generate module -l <library> <module>        # -f replaces existing outputs
```

`create` writes a recognized but incomplete draft. `validate` reports the
missing `pin_count` and `integration` sections until the source is complete.

```yaml
iomux0:
  generator:
    kind: iomux
    bus: axi4_lite
    data_width: 32
    address_width: 14
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
Both are generation-time configuration, not Verilog parameters. `build` is a
number from 0 to 255 the design chooses, 0 when omitted, and reads back in
the low byte of the `version` word so firmware can tell one build of the
same layout from another. `data_width` is 32 or 64 and `address_width` is
the bus address width; both default to 32. An IOMUX entry may not also carry
manual `parameter`, `port`, or `bus` sections.

Each route names a `pin` below `pin_count`, a `slot` below `hs_slots`, and
non-empty `function` and `signal` labels used only for reports. A `(pin,
slot)` pair appears at most once, and an `input_value` sink (`link` plus
`bit`) is driven by at most one route. Role values are an endpoint map with
`link`, an optional `bit` up to 65535, and an optional boolean `invert`, or for every role
but `input_value` the integer `0` or `1`. An omitted output role drives `0`;
an omitted `input_value` declares no sink. An `input_value` endpoint may add
`tie: 0` or `tie: 1` under `option.rx_override`: the sink then leaves reset on
its override at that level, the way a bus arbitration input is held released
until firmware routes it, and the ordinary override bits hand it to the pad.
HDL expressions, slices, and concatenations
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

== Slow-Bus Pools
<iomux-ls-pools>
The slots of a pin are its fast group: a few peripherals bonded to that pin
at generation time, one small mux, no latency to speak of. `generator.ls`
adds the other tier, slow buses that reach any pin of a pool through one
larger mux. A pool names the pins it binds and the channels it carries; a
channel is a route without a pin, one number, the same four roles, the same
`link`, `bit`, `invert`, and `open_drain`. A channel with output roles is a
slow bus any pin of the pool can select; a channel with `input_value` is a
slow input that reads any pad of the pool.

```yaml
ls:
  spi_pool:
    pins: ["0-7", 12]
    reset: 1
    channel:
      - channel: 0
        function: spi4
        signal: sclk
        output_value: {link: spi4_sclk}
        output_enable: 1
      - channel: 1
        function: spi4
        signal: miso
        input_value: {link: spi4_miso}
      - channel: 4
        function: spi4
        signal: arb
        input_value: {link: spi4_arb, tie: 1}
  uart_pool:
    pins: ["16-23"]
    channel:
      - {channel: 2, function: uart5, signal: tx, output_value: {link: uart5_tx}, output_enable: 1}
      - {channel: 3, function: uart5, signal: rx, input_value: {link: uart5_rx}}
```

Channel numbers are one space across every pool, 0 to 255, and a pin belongs
to at most one pool. One pool over every pin is the plain crossbar; several
pools cut it into blocks, and a pool of eight pins pays for eight. Slot 0 of
every pool pin is the pool, so a route may not claim it; slot 0 of any other
pin stays an ordinary route. A pool name is a label for the report, the
wrapper ports are `ls_c<k>_<role>_i` and `ls_c<k>_input_value_o`.

Each pool pin owns an 8-bit `ls_select` lane holding a channel number. Its
slot 0 bundle is that channel's roles when the number is one of its pool's
channels, and all zero otherwise, exactly like an unrouted slot. The gpio
sources, inversion, and the safe row layer on top as on every slot. Each slow
input owns an 8-bit `ls_rx_pin` lane holding a pin number: it reads that pad
when the pin is in its pool and zero otherwise, then `ls_rx_src` and
`ls_rx_value` substitute under `option.rx_override` and `ls_rx_inv` inverts
under `option.invert`, as the fast sinks do. The substitution is per slow
input, so a channel no pad is selected for can be held at a level its
peripheral needs while every pad and every other sink stays as it is, and a
channel with `tie` starts that way. Every lane resets to 0, so a slow input
starts on pin 0 and a pool pin on channel 0, or on nothing when its pool has
no channel 0; a pool's `reset` names one of its channels instead, so each
pool can bring its pins up on the function the board expects. Nothing on the slow path is synchronised; only
the gpio `input_value` bank and the interrupt detectors sample through two
flip-flops. A channel carries the four roles only: the pull and the controls
of a pool pin come from `option.pad_control`.

== Pad Cell
<iomux-pad-cell>
`generator.pad_cell` names the pad cell the design uses and the generator
instantiates it, one per pin, in `<module>_io.v`, the pad shell. The shell
is a sibling of the wrapper, not a child: the wrapper stays digital and
drives the abstract pad bus out, the four role vectors and one 4-bit lane
per pin for the pull mode, each strength select, and each control, and the
shell turns that bus into cell pins. Reset and clock sources, the pads, and
everything physical live in the shell, which the chip top places like any
other block and a flow can replace whole. The integration fragment
instantiates both and wires the bus between them, so `integration.pad`
names only `io`, the net the shell's `pad_io` vector uplinks to. With a
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
matching direction and one bit wide, and every input of the cell must be named here, or
generation stops before it writes a file: an input the declaration forgets
would be left floating in a netlist that elaborates. A role that is absent
from `port` is a role the cell lacks, and a route that asks for it is an
error. The same holds for a missing `pull` section or an absent control.

`pull.table` maps mode names to the values the pull ports take, one entry per
port, transcribed from the databook. `none` is required and encodes as the
all-zero selector. `up`, `down`, `keeper`, and `oscillator` carry meaning to
the generator.
Any other name is a mode the cell documents, which a route asks for by that
name. A mode holds either one row or a map of strength labels to rows, so a
cell with two pull-up strengths and one pull-down strength is expressed as
`up: {"47k": [...], "100k": [...]}` and `down: [...]`. A route writes
`pull: up` for a single row and `pull: {mode: up, strength: "47k"}` for a
labelled one.

A mode name, a strength label, or a row label written twice is an error,
so one label never names two rows.

`control` declares every other input group of the cell: drive strength,
slew rate, Schmitt trigger, analog enable, an open-drain mode pin, a filter
enable, whatever the databook lists. Each control names its pins, a table of
labelled rows, and an optional `default` row, which is otherwise the first.
The control name is yours. It must be a Verilog identifier, because it
appears as is in ports, register fields, and the report, and it may not be
one of the names in the table below. Every cell pin is named once, by a
role, by `pull`, or by one control. A cell may declare up to 16 controls of
up to 16 rows each. A route asks for a row by label under `control`, as in
`control: {drive: high, slew: fast}`. A slot that names none, an unrouted
slot, and a selector value above the slot count all take the default. A
control with one row has nothing to select: its pins take that row, it owns
no field and no port, and a route may not put a link on it.

A row may also follow a net at bus speed. `control: {od: {link:
i3c0_sda_oe, on: pp, off: od}}` gives the slot the `on` row while the net is
high and the `off` row while it is low, with the usual `bit` and `invert` on
the link, and `pull: {link: sleep_n, invert: true, on: down, off: up}` does
the same for the pull, where `on` and `off` take a mode name or a `mode` and
`strength` map. The net becomes a wrapper input named
`hs_p<pin>_s<slot>_<group>_select_i`, where the group is `pull` or the
control name, and a link in the integration fragment.
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
Nothing outranks it. The priority of every pad output is: forced selects the
safe row, otherwise a set source bit selects the register, otherwise the
selected slot's link or constant. The keeper and oscillator weave still
runs on whatever mode wins, so a safe row may name `keeper`.

A name the generator gives behaviour to is fixed. Every other name, a pull
mode, a strength, a control, a row, is yours and is copied through
unchanged. The fixed names and the names a control may not take:

#figure(
  align(center)[#table(
    columns: 3,
    align: (left, left, left),
    table.header([Where], [Name], [Meaning]),
    table.hline(),
    [`port`, route, `safe`], [`input_value` `input_enable` `output_value` `output_enable`], [the four roles],
    [`pull.table`], [`none` `up` `down` `keeper` `oscillator`], [modes 0 to 4, `none` required, `up` and `down` graded],
    [`pull.kind`], [`resistor` `driver`], [whether the pulls are resistors],
    [control body], [`port` `table` `default`], [keys],
    [`safe`], [the roles, `pull`, control names], [keys],
    [control name], [the roles, `pull`, `pull_mode`, `up_sel`, `down_sel`, `select`, `rx_*`, `*_src`, `*_inv`, `*_detect`, `*_int_en`, `*_int_pend`], [taken, would collide with a generated port or field],
  )],
  caption: [FIXED NAMES],
)

Only `up` and `down` carry strength rows. Every other mode is one row. A
route may ask for `keeper` or `oscillator` when the cell has no row of that
name and the cell has neither. The generator then weaves the mode from `up`
and `down`: the keeper follows the pad and the oscillator opposes it, both
read the receiver, and the loop closes inside the pad module. The receiver
has to be on for the loop to run, so a cell without an `input_value` port
weaves nothing, a route that asks for a woven mode must raise
`input_enable`, and a woven mode written into the pad control word needs
the enable from the selected slot or the gpio register. A woven
mode keeps its strength: `pull: {mode: keeper, strength: "47k"}` selects that
row on each graded direction, and an absent strength selects the first row.
`pull.kind` is `resistor`, the default, or `driver`, which marks a cell whose
pulls are its output driver rather than resistors, and no mode is woven from
those. A woven mode is a
combinational loop through the pad. A zero-delay simulation of a floating pad
under a woven keeper does not settle, so a testbench drives the pad across a
mode change or gives it a delay.

A cell that gates its receiver on `input_enable` reads zero on any pin whose
sinks listen while no slot raises that enable. Generation refuses such a pin
unless `option.gpio` or `option.invert` is on, because either register path
can then raise it. A pool pin is read by every slow input of its pool, so the
pool needs a channel that raises the enable, which software selects on the
pin while the slow inputs point at it.

`constraint` lists properties over the cell ports, one per pin. `expr` is
combinational and `property` is sampled on the formal global clock, where
`$past`, `$rose`, and `$stable` are available. The body is not parsed:
identifiers that name cell ports are rewritten to the nets of each pin, and
any other identifier must be a SystemVerilog keyword or a system function.
The open engine has no SVA sequences, so write an implication as a boolean.
The generator settles `kind` when it is absent. A body over pull and drive
ports alone is a claim about logic this generator emits, so it is an
`assert`. A body that reaches a role port speaks about what routes and
registers will do, so it is an `assume`. `--with-formal` writes them into
the stub of the cell in `<module>_io_formal.sv`, built from the library port
table, so every pin that instantiates the cell carries a copy named by its
instance, and `<module>_io_formal.sby` proves them over the shell. `_io.v`
itself carries no verification code. A false claim fails by name.

== Pad Classes
<iomux-pad-classes>
A design whose pins do not all take the same cell declares each cell once
under `generator.pad_cells`, keyed by a class name, and says under
`generator.pin_cell` which class each pin instantiates: `default` for every
pin not named, a pin number, or a range `"a-b"`. Each class holds exactly
what `pad_cell` holds. `pad_cell` stays the short form for one class, named
after its cell, and the two keys do not mix. One class needs no `pin_cell`;
several need a `default` or every pin named.

```yaml
pad_cells:
  gpio_33: {cell: PDDW33, port: {...}, pull: {...}, control: {...}}
  gpio_18: {cell: PDDW18, port: {...}, pull: {...}}
pin_cell:
  default: gpio_33
  "40-47": gpio_18
  3: gpio_33
```

The register map is one for the block, the union of its classes. `pull_mode`
numbers every named mode of every class in name order, `up_sel`, `down_sel`
and each control lane are as wide as the widest table, and controls sit in
first appearance order, `pad_cells` order then the order inside each class,
so a class added later appends lanes and moves none. A class that lacks a
control or the pull table leaves the field in place: a write to it on such
a pin is kept and read back, the lane to the pad is driven low, the pad
takes the rows the class does have, and a route that asks that pin for it is
an error. A code still names the row of that pin's own class, so two classes
that share a control name should share its rows or take different names; the
report prints one table per class and the class of every pin. When one class
declares `safe`, every class must, so `pad_force_i` holds every pin.

`generator.pad_model` pins the order instead of leaving it to appearance:
`mode` lists named modes and `control` lists controls, each taking the
numbers or lanes in that order ahead of everything else, and a name no class
declares yet keeps its place with nothing in it, so a lane can be reserved
for a cell that arrives later. Names left out follow in appearance order as
before.

```yaml
pad_model:
  mode: [bus_hold]
  control: [drive, slew, od]
```

== IO Ring
<iomux-io-ring>
`generator.io_ring` places the pads and the cells around them: supplies,
corners, breakers, and cells the mux does not drive such as an oscillator or the
reset pad. It is optional, and a design without it generates exactly what it
did before; a design with it needs a `pad_cell`. `generator.io_lib` describes the cells it names: the `kind`
(`signal`, `power`, `corner`, `fill`, `other`), the `width` and `height` of
its own box in microns, and what happens across the two axes under
`variant`.
Neither block knows a net or a port of the mux; each side lists identities in
placement order and nothing else.

```yaml
io_lib:
  PDDW33:  {kind: signal, width: 40, variant: {west_east: PDDW33_H, north_south: PDDW33_V}}
  PVSS:    {kind: power, width: 20, variant: rotate}
  PVDD:    {kind: power, width: 20, variant: rotate}
  PRCUT:   {kind: other, width: 5}
  PCORNER: {kind: corner, width: 60}
io_ring:
  die: {width: 5100, height: 5200}
  corner: PCORNER
  power: {VSS: PVSS, VDDIO: PVDD}
  direct:
    rst: {cell: PDDW33, port: {PAD: pad_rst_n, C: rst_n, IE: "1'b1", OE: "1'b0"}}
  sides:
    west:  [{power: VSS}, {pin: 0}, {pin: 1}, {direct: rst}]
    south: [{power: VDDIO}, {pin: 2}, {cell: PRCUT}]
    east:  [{pin: 3}, {power: VSS, id: 7}]
    north: [{power: VDDIO}]
```

An item is one of `pin`, `power`, `cell`, or `direct`. Every pin appears on
exactly one side. A `power` item names a net declared under `power`, a
`cell` item any cell with no ports of its own, a breaker, a clamp, an
explicit filler, and a `direct` item a key declared under `direct`. A cell
`io_lib` describes has to be used as its kind says: a supply belongs under
`power`, where its net is named and its instances are counted per net, and
a corner under `corner`, so only an `other` or a `fill` cell is a `cell`
item.

The west and east sides are one axis, the north and south sides the other.
The two sides of an axis always sit 180 degrees apart, so a cell that lands
on both axes must say what happens across them: `variant: rotate` when one
layout serves both, or `variant: {west_east: A, north_south: B}` when the
library draws one cell per axis. Silence would not tell the two apart, so a
cell on both axes without either is an error, and a cell the library does not
describe at all is not, because there is no variant to pick. A cell on one
axis needs neither, and a map that names one axis and not the other is
refused. An axis entry may also carry its own box, `{cell: A, width: 90,
height: 45}`, measured in the cell's own frame like every other box here;
without it the axis keeps the box of the cell it belongs to. The rule
applies to every kind that sits on a side, so a supply, a breaker, or a fill
that comes in two forms takes the right one on each side. A fill reaches
every side it can, so it counts as sitting on both axes. A corner sits on
neither, and declaring an axis map on one is refused. A direct cell maps each of its
ports to a wrapper net or to a sized binary constant as wide as the port,
`1'b0` or `2'b10`; the nets become ports of the
wrapper with the direction of the cell port, an `inout` uplinks and the rest
link in the integration fragment, and every input of the cell must be
named.

The corners, supplies, plain cells, and direct cells join the signal pads in
`<module>_io.v`, and `<module>.ring.rpt` lists one table per side with each
instance, its cell, and what it stands for. A pin on a side whose class cell
has a `variant` for it instantiates that variant under the same instance
name. Instance names carry identity, never position: `u_pad_<pin>`,
`u_<net>_<k>` with `k` counting that net around the ring or the `id` an item
gives, `u_corner_<nw|sw|se|ne>`, `u_<cell>_<k>` counting that cell or the
`name` an item gives, and `u_<key>` for a direct cell. Moving a pad to
another side or reordering a side changes the report and, when the library
has variants, the module a pad instantiates, and nothing else.

With a `die`, a `corner`, and a `width` in `io_lib` for every cell on the
ring, generation also writes `<module>_io.def`: `DIEAREA`, and every
corner, pad, supply, plain cell, direct cell, and fill as a `FIXED`
component named by its path below `prefix`, which defaults to the shell
instance, `<instance>_io`. A `prefix` of your own is a path of Verilog
identifiers separated by `/`, and the separator before the instance is
added when you leave it off. Every length here is in microns, between a
nanometre, which the DEF grid is, and a metre, which no die is; a number
outside that is refused as a units slip. `die` is the area the ring occupies, the
inside of the seal ring, not the cut die. Corners, supplies, plain cells,
and fills carry `SOURCE DIST`, so a back end knows the netlist never drives
them. Each side packs from its first corner in the order written, going
around from the north-west corner: west top to bottom, south left to right,
east bottom to top, north right to left, which is counter-clockwise in DEF's
y-up coordinates. Every pin must appear on exactly one side. An item's `gap` leaves that
many microns before it and `offset` pins it at that distance from the
corner; fill cells of kind `fill`, widest first, close every gap and each
side's tail, and appear in `<module>_io.v` as `u_fill_<side>_<k>`. The
ring has one depth: a cell without a `height` takes the corner's, and a
corner without one is square. Orientations follow the usual convention, west `E`, south `N`, east `W`,
north `S`, and the corners `N`, `W`, `S`, `E` from the south-west round,
each overridable under `orient`. A cell lies along its edge, so the west and
east sides take a quarter turn, `E`, `W`, `FE` or `FW`, and the north and
south sides take none, `N`, `S`, `FN` or `FS`; a value from the wrong family
is refused. A corner lies along no edge and takes any of the eight. A flip keeps the box, a quarter turn shows it the other way
round, and the footprint of every cell and corner follows from that, so a
corner that is not square still lands flush and each side starts past the
extent its first corner actually shows. When something is missing the DEF is not written and the report's
`def:` line says what it needs, and the other artifacts are written as
usual. A contradiction is different: a side whose cells exceed its length,
or an `offset` that reaches back over the item before it, is a source error
and stops generation, because no extra input would fix it. So is a ring
that does not fit across the die: the two rows of an axis face each other,
so their depths together have to fit the die between them. Two ring cells
may not answer to one name either, whichever of the running count, an `id`
and an explicit `name` produced it, and none may take a name a pad or a
corner already owns.

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
    [0x0], [`version`], [layout contract: major `[31:24]`, minor `[23:16]`, patch `[15:8]`; the design's `build` `[7:0]`],
    [0x4], [`type`], [0x494F4D58, the letters IOMX read as one hex value],
    [0x8], [`capability`], [`pin_count` `[15:0]`, `hs_slots` `[23:16]`],
    [0xC], [`feature`], [one bit per option: 0 gpio, 1 interrupt, 2 pad_control, 3 invert, 4 rx_override, 5 ls],
    [0x10], [`ls_capability`], [with `ls` only: channel count `[15:0]`, one past the highest number, and pool count `[23:16]`],
  )],
  caption: [IOMUX IDENTITY WORDS],
)

Software reads `version` first, because that is where a driver written for
another instance of this type looks, then `type` to confirm the block, then
`capability` and `feature` to compute the rest of the map. The layout
contract is 2.1.0, which appended the slow-bus blocks to 2.0.0. The number steps against a layout that has shipped in
silicon or that firmware depends on. Until then fields may move under it.
After that, a block appended after the existing ones steps the minor number
and any existing offset that moves steps the major number. With
`pin_count`, `hs_slots`, and `data_width` every offset below is
computable, and the report prints them.

Every block has a fixed byte base, and a block whose option is off leaves
its region empty: an offset means the same thing on every design, so a
driver carries constants and reads `feature` only to learn which blocks
are present. Within the IOMUX window, any byte offset without a generated register is reserved. These addresses include gaps between registers and regions for options disabled at generation. Reads return zero and writes are ignored. Both reads and writes return AXI OKAY, with no register side effects. OKAY confirms transaction completion, not feature availability. Drivers use `feature` and `capability` to discover functions and access only their documented registers. Reserved locations can acquire functions in later versions, so drivers must not use them as scratch storage or probe them with writes.

The window spans at least 16 KB. Additional pad control words can extend it. The report gives its size as `aperture` bytes, covering local offsets zero through `aperture - 1`. `address_width` must cover the whole window and is at least 14. The interconnect must decode this window and return DECERR for addresses that reach no peripheral. With wider address ports, an out-of-window access delivered directly to IOMUX returns SLVERR without aliasing a register. This window policy does not change the generic MMIO generator's SLVERR response for unlisted addresses.

#figure(
  align(center)[#table(
    columns: 3,
    align: (left, left, left),
    table.header([Base], [Block], [Size at 256 pins]),
    table.hline(),
    [0x0000], [identity words], [16 bytes],
    [0x0100], [`hs_select` words], [`pin_count / 2` bytes, at most 0x80],
    [0x0200], [gpio banks, four], [at most 0x80],
    [0x0300], [`rx_value_sk` banks, one per slot], [at most 0x100],
    [0x0400], [interrupt banks, four enable then four pending], [at most 0x100],
    [0x0800], [inversion banks], [at most 0x400, 32 banks],
    [0x0C00], [`ls_select` lanes, 8 bits per pool pin], [at most 0x100],
    [0x0D00], [`ls_rx_pin` lanes, 8 bits per slow input], [at most 0x100],
    [0x0E00, 0x0E20, 0x0E40], [`ls_rx_src`, `ls_rx_value`, `ls_rx_inv` banks, one bit per slow input], [0x20 each],
    [0x1000], [`pin_src_ctrl`, one word per pin], [at most 0x800],
    [0x1800], [`pin_pad_ctrl`, one word per pin], [at most 0x800],
    [0x2000 + k × 0x800], [`pin_ctl_k`, one word per pin], [0x800 each],
  )],
  caption: [IOMUX REGISTER MAP],
)

Within a block the stride is the only arithmetic. A bank holds one bit per
pin, `data_width` pins per word, so bit `p` of a family sits at
`base + (p / data_width) × (data_width / 8)`, and one store flips the same
bit on a whole word of pins. A per-pin word sits at
`base + p × (data_width / 8)` and holds that pin's whole configuration, so
one store reconfigures a pin without a read-modify-write and without
touching its neighbours. A name that starts with `pin_` is indexed by pin;
any other suffix is a word index.

Every pin owns a fixed 4-bit lane in the selector words; the field uses the
low `ceil(log2(hs_slots))` bits and the remaining lane bits read zero and
ignore writes. A 32-bit word holds 8 pins and a 64-bit word holds 16, so no
field crosses a byte, one write strobe never splits a selector, and a hex
dump shows one pin per digit. The lane idles 2 bits per pin at the default 4
slots, 48 bytes on a 185-pin instance; that is the price of a selector
offset that depends on `pin_count` and `data_width` alone.

The slow lanes hold 4 pins or channels per 32-bit word and 8 per 64-bit
word; a word with no pool pin or no slow input behind it reads zero and
takes no write, as does the lane of a pin outside every pool.

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
    [16 + i], [`<control>_src`], [`pad_control`, i is the control's index in the block's control order, the same order as its lane; a single-row control keeps its index and has no bit],
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
holding the pull fields and the first four controls, then one `pin_ctl_k`
word per pin for each further group of eight controls, control `i` in the
4-bit lane at bit `4 * ((i - 4) mod 8)` of `pin_ctl_k`, `k = (i - 4) div 8`,
in the block's control order. A single-row control
keeps its lane empty, so its neighbours never move, and a group whose
controls are all single-row emits no word while `k` still counts it. The
fields below are present only when the cell has something for them to select
and each is as wide as its table needs; a table has at most 16 rows. Between
the core and `<module>_io` the same selects travel in one 4-bit lane per pin,
`[4 * pin + 3 : 4 * pin]`, whatever the table needs, so a pin's slice never
moves when a table grows.

#figure(
  align(center)[#table(
    columns: 3,
    align: (left, left, left),
    table.header([Bits], [Field], [Meaning]),
    table.hline(),
    [from 0], [`pull_mode`], [0 none, 1 up, 2 down, 3 keeper, 4 oscillator, then the cell's other modes from 5 in name order],
    [from 4], [`up_sel`], [strength row of `up`, table order, only when `up` has several rows],
    [from 8], [`down_sel`], [strength row of `down`, likewise],
    [from 16, 20, 24, 28], [control 0 to 3], [row of the control, table order, only when it has several rows],
  )],
  caption: [PIN_PAD_CTRL LAYOUT],
)

The mode values are fixed, so software reads the same field on every design. A
value the cell has no row for behaves as `none`, a strength select past the
table behaves as the first row, and a control value past its table behaves as
the control's default row. The register keeps what was written, so firmware can
read its own mistake back. A mode says whether and which way the pin pulls, a
select says how strongly, and the two never mix: the keeper and the oscillator
switch the mode between `up` and `down` from the pad level and leave both
selects alone, so they hold at whatever strength the selects name. The report
prints the mode numbering and each graded direction's strengths. `pull_src` at
0 keeps the pull the selected slot's route asked for, and at 1 hands mode and
selects to the word. Each `<control>_src` does the same for its control. All
reset to 0, so the words are inert until software claims them. A
register-driven keeper or oscillator is the same woven loop as a route request,
with the same simulation caveat.

`generator.option.invert` appends the banks `input_enable_inv`,
`output_value_inv`, `output_enable_inv`, one `rx_inv_sk` bank per slot k,
then `pull_inv` when the cell has a pull table and one `<control>_inv` bank
per control with several rows. Each role bit inverts its signal after the
source selector, so it inverts whichever source the pin currently uses. The
cross taps read the uninverted slot bundle. `input_value` and the interrupt
detectors read the pad and are not affected. A pull or control bit inverts
the select net of the slot's `{link, on, off}` request, so a fixed row and
the register path are not affected.

`generator.option.rx_override` appends one `rx_value_sk` bank per slot k.
While `rx_src_sk` of a pin is set, the sink of slot k on that pin reads the
register bit instead of the pad, which holds an unselected peripheral's input
at a known level. Substitution happens before inversion.

`generator.option.interrupt` appends four enable banks and four pending
banks, one bit per pin in each, for the high level, the low level, the
rising edge, and the falling edge. It also adds an `irq_o` output that
carries one line per `data_width` pins, which the fragment links to the net
`integration.interrupt` names.

A pending bit records its event whether or not the matching enable is set,
so a pin that reaches no interrupt line can still be polled. The enable
gates `irq_o` alone, so a bit that latched while its enable was clear raises
the line the moment the enable is written; clear it first when that event is
stale. Software clears a pending bit by writing one to it,
and a set that lands on the same cycle as that write wins, so an event
cannot vanish into its own acknowledgement. Edge detection compares the
second synchronizer stage against a third, so the pad must hold a level for
one bus cycle to register as an edge.

#figure(
  align(center)[#table(
    columns: 4,
    align: (left, right, right, left),
    table.header([Pins, width], [Selector regs], [Total regs], [Selector offsets]),
    table.hline(),
    [185, 32-bit], [24], [28], [0x100 to 0x15C],
    [185, 64-bit], [12], [14], [0x100 to 0x158],
    [256, 32-bit], [32], [36], [0x100 to 0x17C],
    [256, 64-bit], [16], [18], [0x100 to 0x178],
  )],
  caption: [IOMUX SELECTOR LAYOUT],
)

== Generated Artifacts
<iomux-generated-artifacts>
Generation writes six files under `output/<library>/<module>/`:
`<module>_regs.v`, `<module>_conn.v`, `<module>.v` with the private core and
the public wrapper, the `<module>.fl` file list, the `<module>.iomux.rpt`
route report, and the `<module>_integration.soc_net` fragment, plus
`<module>_io.v`, the pad shell, when a pad cell is declared. Every `.v`
is Verilog-2001 for synthesis and simulation and carries no verification
code; verification lives in the `_formal.sv` files and their `.sby` jobs,
listed by `<module>_formal.fl`, and never enters `<module>.fl`. Selector
sidebands stay inside the wrapper and never reach the public interface. Each
endpoint port carries a `function.signal` comment in the wrapper header. The
report shows each selector location and lists unused slots per pin.
Generation refuses a library that already holds a module named
`<module>_regs`, `<module>_conn`, `<module>_core`, or `<module>_io`.

== Integration
<iomux-integration>
`module validate` checks the IOMUX source only; link existence, widths,
directions, and drivers are checked when the fragment is merged. Assemble the
final design with the existing merge flow:

```bash
qsoc generate verilog --merge <base.soc_net> <module>_integration.soc_net
```

The fragment instantiates the public wrapper once and connects the clock, the
reset, the pad bus, the control bus, the interrupt lines when
`option.interrupt` is on, and every non-constant endpoint of every route and
slow channel exactly once. With a pad cell it also instantiates the shell as `<instance>_io`,
links the bus between the two on nets named `<instance>_pad_<signal>`,
uplinks `pad_io` and every `inout` net of a direct cell, and links the other
direct nets. The merge flow derives `<module>_io` from the source and the
cells it names in the module library, so nothing has to be imported. The control link must carry exactly one master before the merge
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
their source bits and the same force, and every receive sink after
substitution and inversion. With pools it also leaves every slow lane free
and asserts slot 0 of each pool pin over its channels and every slow input
over its pool's pads. A pad cell with constraints adds the pad proof
described above. `<module>_formal.fl` lists the design files the proofs
read followed by the harnesses, so another engine can take the whole set
in one go.

The routing proof is per pin, and one job over a whole design grows faster
than the pin count, so the job file cuts the pins into banks of
`--formal-bank` pins, 16 when not given. A design that fits one bank has the
tasks `prove`, `bmc`, and `cover`. A larger one has `prove_bN` and `bmc_bN`
per bank and `cover` on bank 0; each task sets the harness parameters
`PIN_LO` and `PIN_HI` and the other pins fall out of the job.

```bash
sby -f iomux0_hs_formal.sby            # every task, in parallel
sby -f iomux0_hs_formal.sby bmc_b3     # one bank
```

`--formal-bank 1` makes one task per pin, for proving a few pins after a
change to their routes.

== UVM Collateral
<iomux-uvm-collateral>
`--with-uvm` reuses the MMIO UVM testbench for `<module>_regs` only, writing
`<module>_regs_uvm_if.sv`, `<module>_regs_uvm_pkg.sv`,
`<module>_regs_uvm_tb.sv`, and `<module>_regs_uvm.fl`. It covers the
register slave and does not cover routing, the connection fabric, or the
pads; those are covered by the directed simulation and the formal routing
proof.
