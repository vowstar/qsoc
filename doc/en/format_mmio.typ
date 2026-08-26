= MMIO Generator
<mmio-generator>
The MMIO generator turns one register map in a module library into one
AXI4-Lite slave. Its source stays in the module's `.soc_mod` entry.

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

`data_width` and `address_width` are optional generator entries alongside
`bus`; both default to 32. `data_width` accepts 32 or 64. `address_width`
accepts values from `ceil(log2(data_width / 8))` through 64.

```yaml
wide_status:
  generator:
    kind: mmio
    bus: axi4_lite
    data_width: 64
    address_width: 13
    register:
      status:
        offset: 0x08
        field:
          count:
            lsb: 0
            width: 64
            access: ro
            input: count_i
```

Offsets are local byte offsets. Each register occupies one complete data beat,
so offsets must be aligned to `data_width / 8` bytes. A 64-bit register is
therefore 8-byte aligned; the generator does not pack two independently
addressed 32-bit registers into one beat.

Each register needs an explicit, unique `offset` and a non-empty `field` map.
Each field needs `lsb` and `access`; `width` defaults to 1. Register, field,
module, input, and output names must be Verilog identifiers. Optional scalar
`description` values do not affect RTL.

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

Fields may not overlap or cross bit `data_width - 1`. `reset` and `value` must
fit their field width. Sideband signals must be unique and may not collide
with fixed interface ports. A generated entry may not also contain manual
`parameter`, `port`, or `bus` sections. Unknown generator keys are errors.

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
present and ignored. Address ports use `address_width`; data and strobe ports
use `data_width` and `data_width / 8`. Addresses remain local byte offsets.

The slave accepts write address and data independently, permits one pending
read and one pending write, applies `s_axi_wstrb` to writable bytes, and holds
responses while backpressured. Unmapped or misaligned accesses return
`SLVERR`; mapped accesses return `OKAY`. Reserved bits read as zero and ignore
writes. A read and write to the same register on one clock edge returns the
old value.

== Formal Collateral
<mmio-formal-collateral>
Add `--with-formal` to generate a matching formal harness and SymbiYosys job:

```bash
qsoc generate module --with-formal -l <library> <module>
```

The command selects three files in the same output directory:
`<module>.v`, `<module>_formal.sv`, and `<module>_formal.sby`. Generation checks
all selected targets before opening or replacing a selected output file. If any
target exists, the command fails without replacing any selected file unless
`-f` or `--force` is present.

Generation does not run the job. From `output/<library>/<module>/`, run the
proof or cover task explicitly:

```bash
sby -f <module>_formal.sby prove
sby -f <module>_formal.sby cover
```

The harness resets itself from an `initial` value. A tool that ignores
`initial` blocks compiles the harness with `FORMAL_EXTERNAL_RESET` defined,
which adds a `formal_reset_ni` port for the tool's own reset handling.

The generated job also has a `bmc` task for bounded counterexamples; omit the
task name to run `prove`, `bmc`, and `cover`. The harness follows the selected
address width, data width, register layout, resets, sidebands, byte strobes,
responses, and backpressure. It covers the generated AXI4-Lite slave only. No
UVM testbench is generated. The harness holds reset active for two clock steps
before release; it does not explore reset reassertion during traffic.

== Current Limits
<mmio-current-limits>
This CLI slice supports one AXI4-Lite slave with a 32- or 64-bit data port and
one configurable local address port. Optional formal collateral targets that
same slave. It does not allocate a system address, create a netlist instance,
insert a bus bridge, cross clock domains, or generate register arrays and
extended access types. Use a wrapper for those functions.
