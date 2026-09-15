= MMIO Generator
<mmio-generator>
The MMIO generator turns one register map in a module library into one
AXI4-Lite or APB4 slave. Its source stays in the module's `.soc_mod` entry.

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

`identity` names the block for software. `type` is a number, or four
characters from `!` to `~` packed with the first in the top byte, so `TMRC`
reads as 0x544D5243 in a register view; a value that parses as a number is
taken as the number. `version` is `major.minor.patch`, each part below 256.
The generator places `version` at 0x0 with major `[31:24]`, minor `[23:16]`,
patch `[15:8]`, and `type` at 0x4, both read-only. A 64-bit instance holds
both in its first beat. A user register on offsets 0x0 to 0x7, or named
`version` or `type`, is an error, so user registers start at 0x8. `validate`
warns when `identity` is absent, because a block software cannot recognise is
a block software cannot refuse.

```yaml
timer_ctrl:
  generator:
    kind: mmio
    bus: axi4_lite
    identity:
      type: TMRC
      version: 1.0.0
    register:
      control:
        offset: 0x08
        field:
          enable:
            lsb: 0
            access: rw
            reset: 0
            output: enable_o
      status:
        offset: 0x0C
        field:
          busy:
            lsb: 0
            access: ro
            input: busy_i
```

`data_width` and `address_width` are optional generator entries alongside
`bus`; both default to 32. With `bus: axi4_lite`, `data_width` accepts
32 or 64 and `address_width` ranges from `ceil(log2(data_width / 8))` to 64.
With `bus: apb4`, data widths are 8, 16, or 32 and address widths range from
`max(1, ceil(log2(data_width / 8)))` to 32.

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

Offsets are local byte offsets below `2^address_width`. Each register
occupies one complete data beat, so offsets must be aligned to
`data_width / 8` bytes. A 64-bit register is therefore 8-byte aligned; the
generator does not pack two independently addressed 32-bit registers into one
beat. Numbers are unsigned decimal or `0x` hexadecimal.

Each register needs an explicit, unique `offset` and a non-empty `field` map.
Each field needs `lsb` and `access`; `width` defaults to 1. Register, field,
module, input, and output names must be Verilog identifiers. A register or
field may carry a scalar `description`; it does not affect RTL.

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
    [`w1c`], [`lsb`, `reset`, `input`], [`output`, `description`],
    [`width` other than 1, `value`],
  )],
  caption: [MMIO FIELD FORMS],
  kind: table,
)

A `w1c` field is one bit that hardware sets through `input` and software
clears by writing one. A zero leaves it. A set that lands on the cycle of the
clearing write wins, so an event cannot vanish into its own acknowledgement.

Fields may not overlap. AXI4-Lite fields must fit in one bus word. APB4 fields
start within the addressed word and may span subsequent words, up to 64 bits
per field. Each byte strobe updates only its addressed field bits. Separate
writes take effect separately, including intermediate field values. `reset` and `value` must
fit their field width. Sideband signals must be unique and may not collide
with fixed interface ports or with the generator's own names such as
`write_fire`, `aw_take`, or `mmio_field_<n>_q`; the error names the clash. A
generated entry may not also contain manual
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

With `bus: apb4`, the control ports are `s_apb_paddr`, `s_apb_pselx`,
`s_apb_penable`, `s_apb_pwrite`, `s_apb_pwdata`, `s_apb_pstrb`, `s_apb_pprot`,
`s_apb_prdata`, `s_apb_pready`, and `s_apb_pslverr`. Protection attributes are
accepted without access filtering. The slave has no wait states. Writes take
effect on the ACCESS completion edge; SETUP does not change register state.
Read data reflects the addressed fields during ACCESS. Unmapped or misaligned
accesses complete with `PSLVERR`. Reserved bits read zero and ignore writes.
Reset clears stored fields and suppresses completion.

== Formal Collateral
<mmio-formal-collateral>
Add `--with-formal` to generate a matching formal harness and SymbiYosys job:

```bash
qsoc generate module --with-formal -l <library> <module>
```

The command selects four files in the same output directory:
`<module>.v`, `<module>_formal.sv`, `<module>_formal.sby`, and
`<module>_formal.fl`, the list of what the proof reads. The design file
carries no verification code. Generation checks
all selected targets before opening or replacing a selected output file. If any
target exists, the command fails without replacing any selected file unless
`-f` or `--force` is present.

Generation does not run the job. The generated tasks are `prove`, `bmc`, and
`cover`. Define `FORMAL_EXTERNAL_RESET` to expose `formal_reset_ni` for an
external reset controller. The harness follows the selected bus, register
layout, field access rules, and byte strobes.

== UVM Testbench
<mmio-uvm-testbench>
Add `--with-uvm` to generate a deterministic, self-checking UVM testbench:

```bash
qsoc generate module --with-uvm -l <library> <module>
```

The command selects `<module>.v`, `<module>_uvm_if.sv`,
`<module>_uvm_pkg.sv`, `<module>_uvm_tb.sv`, and `<module>_uvm.fl`. The file
list contains relative generated sources; the UVM library remains an external
dependency. Compile the listed sources with a UVM library and select
`<module>_uvm_tb` as the top module. Generation does not run the testbench.
The testbench checks the selected register interface and reports mismatches
as UVM errors. An error or fatal report makes the simulation fail.

`--with-formal` and `--with-uvm` are independent and may be combined. The
combined command selects eight files. Generation locks and checks every
selected target before writing; `--force` replaces only the selected set.

== Current Limits
<mmio-current-limits>
Each module exposes one AXI4-Lite or APB4 slave and one local address port.
Optional formal and UVM collateral target the selected interface. It does not allocate a system address, create a netlist
instance, insert a bus bridge, cross clock domains, or generate register arrays
and extended access types. Use a wrapper for those functions.
