= MMIO Generator
<mmio-generator>
The MMIO generator turns one register map in a module library into one
32-bit AXI4-Lite slave. Its source stays in the module's `.soc_mod` entry.

== Source Format
<mmio-source-format>
Create an empty draft, edit the generated library file, then validate it:

```bash
qsoc module create --generator mmio -l peripheral timer_ctrl
qsoc module validate -l peripheral timer_ctrl
qsoc generate module -l peripheral timer_ctrl
```

`create` writes an empty `register` map. An empty map is a saved draft, not a
valid generator input.

```yaml
timer_ctrl:
  generator:
    kind: mmio
    bus: axi4_lite
    register:
      identification:
        offset: 0x00
        field:
          device_id:
            lsb: 0
            width: 8
            access: ro
            value: 0x2a
      control:
        offset: 0x04
        field:
          enable:
            lsb: 0
            access: rw
            reset: 0
            output: enable_o
      status:
        offset: 0x08
        field:
          busy:
            lsb: 0
            access: ro
            input: busy_i
```

Each register needs an explicit, unique, 4-byte-aligned `offset` and a
non-empty `field` map. Each field needs `lsb` and `access`; `width` defaults to
1. Register, field, module, input, and output names must be Verilog
identifiers. Optional scalar `description` values do not affect RTL.

#figure(
  align(center)[#table(
    columns: (0.14fr, 0.25fr, 0.28fr, 0.33fr),
    align: (auto, left),
    table.header([Access], [Required], [Optional], [Forbidden]),
    table.hline(),
    [`rw`], [`lsb`, `reset`], [`width`, `output`, `description`],
    [`input`, `value`],
    [`ro` input], [`lsb`, `input`], [`width`, `description`],
    [`reset`, `output`, `value`],
    [`ro` constant], [`lsb`, `value`], [`width`, `description`],
    [`reset`, `input`, `output`],
  )],
  caption: [MMIO FIELD FORMS],
  kind: table,
)

Fields may not overlap or cross bit 31. `reset` and `value` must fit their
field width. Sideband signals must be unique and may not collide with fixed
interface ports. A generated entry may not also contain manual `parameter`,
`port`, or `bus` sections. Unknown generator keys are errors.

== Generated Interface
<mmio-generated-interface>
Generation writes
`output/<library>/<module>/<module>.v`. It refuses to replace the file unless
`-f` or `--force` is present.

`module validate` checks only the source structure and values. Generation
writes RTL; neither command runs lint, simulation, synthesis, or formal proof.

The generated module has `clk_i`, active-low asynchronous `rst_ni`, the five
AXI4-Lite channels under the `s_axi_` prefix, and the sideband ports named by
field `input` and `output` bindings. `s_axi_awprot` and `s_axi_arprot` are
present and ignored. Addresses are full 32-bit local byte offsets.

The slave accepts write address and data independently, permits one pending
read and one pending write, applies `s_axi_wstrb` to writable bytes, and holds
responses while backpressured. Unmapped or misaligned accesses return
`SLVERR`; mapped accesses return `OKAY`. Reserved bits read as zero and ignore
writes. A read and write to the same register on one clock edge returns the
old value.

== Current Limits
<mmio-current-limits>
This CLI slice supports one 32-bit AXI4-Lite slave and emits Verilog only. It
does not allocate a system address, create a netlist instance, insert a bus
bridge, cross clock domains, or generate register arrays and extended access
types. Use a wrapper for those functions.
