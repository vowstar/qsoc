= IOMUX Generator
<iomux-generator>
The IOMUX generator turns one sparse route table into a high-speed pin
multiplexer: an AXI4-Lite selector slave, a per-pin mux core, a connection
fabric, and one public wrapper. Its source stays in the module's `.soc_mod`
entry.

The first version covers four roles only: `input_value`, `input_enable`,
`output_value`, and `output_enable`. Pull control, bus keepers, oscillators,
register overrides, runtime inversion, and interrupts are not generated. A
design that needs those capabilities cannot replace its existing pin
multiplexer with this generator.

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
`FIXME`.

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
