= Command-line Overview
<cli-overview>
== Command Line Interface
<cli>
#figure(
  align(center)[#table(
    columns: (0.25fr, 0.25fr, 1fr),
    align: (auto, auto, left),
    table.header([Command], [Subcommand], [Description]),
    table.hline(),
    [project], [create], [Create a new QSoC project],
    [], [update], [Update an existing project],
    [], [remove], [Remove a project],
    [], [list], [List all projects],
    [], [show], [Show project details],
    [module], [create], [Create a generated module draft],
    [], [validate], [Validate a generated module],
    [], [import], [Import Verilog modules into module libraries],
    [], [remove], [Remove modules from specified libraries],
    [], [list], [List all modules within designated libraries],
    [], [show], [Show detailed information on a chosen module],
    [], [bus], [Manage bus interfaces of modules],
    [bus], [import], [Import buses into bus libraries],
    [], [remove], [Remove buses from specified libraries],
    [], [list], [List all buses within designated libraries],
    [], [show], [Show detailed information on a chosen bus],
    [schematic],
    [],
    [Reserved. Schematic editing is available in the GUI, not on the
     command line],
    [generate],
    [module],
    [Generate Verilog for a generated module],
    [],
    [verilog],
    [Generate Verilog code and unconnected port reports from netlist files],
    [],
    [template],
    [Generate files from Jinja2 templates using various data sources],
    [], [stub], [Generate Verilog and Liberty stub files for selected modules],
    [gui], [], [Start the GUI program `qsoc-gui` (@gui-overview)],
    [agent], [], [Start interactive AI agent for SoC design automation],
  )],
  caption: [COMMAND LINE INTERFACE],
  kind: table,
)

QSoC installs four programs side by side: `qsoc` (this command line),
`qsoc-gui` (the GUI), `qsoc-agentd` (the agent daemon, @agent-daemon) and
`qsoc-smt-worker` (the SMT solver worker). `qsoc` finds the others in its own
directory, or in the directory named by the `QSOC_BIN_DIR` environment
variable.

== Global Options
<global-options>
The following global options are available for all commands:

#figure(
  align(center)[#table(
    columns: (0.25fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [`-h`, `--help`], [Display help information for commands and options],
    [`--verbose <level>`],
    [Set verbosity level (0-5): \
      - 0=Silent - No output \
      - 1=Error - Only error messages \
      - 2=Warning - Error and warning messages \
      - 3=Info - Error, warning, and informational messages (default) \
      - 4=Debug - All messages including debug information \
      - 5 - Same as 4
    ],
    [`--color <when>`],
    [Colorize output: `auto` (default), `always`, `never`. `auto` honors
     `NO_COLOR` / `FORCE_COLOR` and whether the stream is a terminal],
    [`-v`, `--version`], [Display version information],
    [`--licenses [<name>...]`],
    [List the third-party components QSoC distributes and their licenses.
     With component names, print their full license texts],
  )],
  caption: [GLOBAL OPTIONS],
  kind: table,
)

== Project Command Options
<project-options>
=== Project Creation Options
<project-creation>
The `project create` command creates a new QSoC project.
It refuses an existing project file; use `project update` to change its paths.
Project names are file names, not paths.

#figure(
  align(center)[#table(
    columns: (0.5fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [`-d`, `--directory <path>`], [The path to the project directory],
    [`-b`, `--bus <path>`], [The path to the bus directory],
    [`-m`, `--module <path>`], [The path to the module directory],
    [`-s`, `--schematic <path>`], [The path to the schematic directory],
    [`-o`, `--output <path>`], [The path to the output file],
    [name], [The name of the project to be created],
  )],
  caption: [PROJECT CREATION OPTIONS],
  kind: table,
)

The project file also accepts `cell.target`, set by hand (@cell-declare).

=== Project Update, Remove, List and Show
<project-other>
`project update` takes the same options as `project create` and rewrites the
paths of an existing project. `project remove`, `project list` and `project show`
take only `-d, --directory` plus a project name or regex.

#figure(
  align(center)[#table(
    columns: (0.4fr, 1fr),
    align: (auto, left),
    table.header([Command], [Arguments]),
    table.hline(),
    [`project update <name>`], [Same options as `project create`],
    [`project remove <regex>`], [`-d`; removes every matching project file],
    [`project list [regex]`], [`-d`; lists project names],
    [`project show <name>`], [`-d`; prints the project file contents],
  )],
  caption: [PROJECT SUBCOMMANDS],
  kind: table,
)

== Module Command Options
<module-options>
=== Generated Module Lifecycle
<module-mmio>
`module create` and `module validate` manage the source of a generated module,
by exact library and module name. The generator is `mmio` or `iomux`. `create`
writes an empty draft. Edit the `.soc_mod` file before validation and
generation. The source formats are in @mmio-source-format and
@iomux-source-format.

#figure(
  align(center)[#table(
    columns: (0.45fr, 1fr),
    align: (auto, left),
    table.header([Command or option], [Effect]),
    table.hline(),
    [`module create --generator <mmio|iomux> -l <library> <module>`],
    [Creates an empty draft without replacing an existing module],
    [`module validate -l <library> <module>`],
    [Validates the current source without writing files],
    [`-d`, `--directory <path>`], [Selects the project directory],
    [`-p`, `--project <name>`], [Selects the project],
  )],
  caption: [GENERATED MODULE COMMANDS],
  kind: table,
)

=== Module Import Options
<module-import>
The `module import` command imports Verilog modules into module libraries.

#figure(
  align(center)[#table(
    columns: (0.5fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [`-d`, `--directory <path>`], [The path to the project directory],
    [`-p`, `--project <name>`], [The project name],
    [`-l`, `--library <name>`], [The library base name],
    [`-m`, `--module <regex>`], [The module name or regex],
    [`-f`, `--filelist <path>`],
    [The path where the file list is located, including a list of verilog files in order],
    [`-D`, `--define <macro>`],
    [Define macro as KEY or KEY=VALUE. Can be used multiple times to define multiple macros],
    [`-U`, `--undefine <macro>`],
    [Undefine macro KEY at the start of all source files. Can be used multiple times],
    [files], [The verilog files to be processed],
  )],
  caption: [MODULE IMPORT OPTIONS],
  kind: table,
)

=== Macro Definition Support
<module-macro-definitions>
The `module import` command supports Verilog preprocessor macro definitions and undefinitions:

*Define Macros (`-D`, `--define`)*:
- Define macros that will be available during Verilog parsing
- Supports both simple macros: `-D DEBUG` (defines DEBUG as empty)
- Supports value macros: `-D WIDTH=32` (defines WIDTH as 32)
- Can be used multiple times: `-D DEBUG -D WIDTH=32 -D MODE=FAST`

*Undefine Macros (`-U`, `--undefine`)*:
- Remove macro definitions at the start of all source files
- Useful for clearing previously defined macros
- Can be used multiple times: `-U OLD_MACRO -U DEPRECATED_FLAG`

*Usage Examples*:
```bash
# Define simple macros
qsoc module import -D SYNTHESIS -D FPGA_TARGET file.v

# Define macros with values
qsoc module import -D DATA_WIDTH=64 -D ADDR_WIDTH=32 cpu.v

# Combine define and undefine
qsoc module import -D NEW_FEATURE -U OLD_FEATURE module.v

# Use with other options
qsoc module import -p myproject -l stdlib -D DEBUG=1 -f filelist.txt
```

=== Module Remove, List and Show
<module-other>
These three share `-d, --directory`, `-p, --project`, and `-l, --library`
(base name or regex), and take a module name or regex as the argument.

#figure(
  align(center)[#table(
    columns: (0.4fr, 1fr),
    align: (auto, left),
    table.header([Command], [Effect]),
    table.hline(),
    [`module remove <regex>`], [Deletes matching modules from the libraries],
    [`module list [regex]`], [Lists module names],
    [`module show <regex>`], [Prints the stored module definition],
  )],
  caption: [MODULE SUBCOMMANDS],
  kind: table,
)

=== Module Bus Interfaces
<module-bus>
`module bus` attaches bus interfaces to a module already in the library. The
mapping between module ports and bus signals can be produced by an LLM.

#figure(
  align(center)[#table(
    columns: (0.4fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [`-d`, `--directory <path>`], [The path to the project directory],
    [`-p`, `--project <name>`], [The project name],
    [`-l`, `--library <regex>`], [Module library base name or regex],
    [`-m`, `--module <regex>`], [Module name or regex (required)],
    [`-b`, `--bus <name>`], [Bus name to attach (required)],
    [`-o`, `--mode <mode>`], [Bus mode, for example `master` or `slave` (required)],
    [`--bl`, `--bus-library <regex>`], [Bus library name or regex],
    [`--ai`], [Let the configured model propose the port mapping],
    [`<interface>`], [Name of the bus interface to create (required)],
  )],
  caption: [MODULE BUS ADD OPTIONS],
  kind: table,
)

`module bus explain` asks the model which bus interfaces a module plausibly
implements and prints the reasoning; it takes the same selection options plus
`-b` and `--bl`. `module bus remove`, `list` and `show` operate on interfaces
already attached and need only the selection options.

Both AI-assisted paths use the model selected per @llm-config. Without one,
`--ai` and `explain` fail; the other subcommands do not need it.

== Bus Command Options
<bus-options>
=== Bus Import Options
<bus-import>
The `bus import` command imports buses into bus libraries.

#figure(
  align(center)[#table(
    columns: (0.5fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [`-d`, `--directory <path>`], [The path to the project directory],
    [`-p`, `--project <name>`], [The project name],
    [`-l`, `--library <name>`], [The library base name],
    [`-b`, `--bus <name>`], [The specified bus name],
    [files], [The bus definition CSV files to be processed],
  )],
  caption: [BUS IMPORT OPTIONS],
  kind: table,
)

=== Bus Remove, List and Show
<bus-other>
These share `-d, --directory`, `-p, --project`, and `-l, --library` (base name
or regex) and take a bus name or regex.

#figure(
  align(center)[#table(
    columns: (0.4fr, 1fr),
    align: (auto, left),
    table.header([Command], [Effect]),
    table.hline(),
    [`bus remove <regex>`], [Deletes matching buses from the libraries],
    [`bus list [regex]`], [Lists bus names],
    [`bus show <regex>`], [Prints the stored bus definition],
  )],
  caption: [BUS SUBCOMMANDS],
  kind: table,
)

== Generate Command Options
<generate-options>
=== Generated Module Options
<generated-module-options>
`generate module` validates one generated module and writes it under
`output/<library>/<module>/`: the sources and their file list in `rtl/`, the
optional verification collateral in `formal/` and `uvm/`. IOMUX adds
`include/`, `reports/`, and `integration/` (@iomux-generated-artifacts).

#figure(
  align(center)[#table(
    columns: (0.45fr, 1fr),
    align: (auto, left),
    table.header([Command or option], [Effect]),
    table.hline(),
    [`generate module -l <library> <module>`],
    [Generates one module selected by exact name],
    [`--with-formal`],
    [Also generates the formal jobs supported by that generator],
    [`--formal-bank <pins>`],
    [Pins per IOMUX routing proof task, default 16 (@iomux-formal-collateral)],
    [`--with-uvm`],
    [Also generates the UVM interface, package, testbench, and file list],
    [`-f`, `--force`], [Replaces every selected output file],
    [`-d`, `--directory <path>`], [Selects the project directory],
    [`-p`, `--project <name>`], [Selects the project],
  )],
  caption: [GENERATED MODULE OPTIONS],
  kind: table,
)

When one selected output file already exists, generation writes nothing,
unless `--force` is given. `--force` replaces only the selected files, so an
unselected `formal/` or `uvm/` stays as it is. Generation writes the
verification collateral but does not run it.

=== Verilog Generation Options
<verilog-generation>
The `generate verilog` command generates Verilog code from netlist files. The
input format is documented in @soc-net-format. PRCM input and controller generation are described in @prcm-check.

#figure(
  align(center)[#table(
    columns: (0.5fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [`-d`, `--directory <path>`], [The path to the project directory],
    [`-p`, `--project <name>`], [The project name],
    [`-m`, `--merge`],
    [Merge multiple netlist files in order before processing],
    [`--check`],
    [Check PRCM resource binding and stable modes without a project or RTL output],
    [`--with-formal`],
    [Also write the cell formal checks, and PRCM RTL checks for a PRCM circuit],
    [`--format`],
    [Run `verible-verilog-format` from `PATH` on each generated top-level Verilog file],
    [files], [The netlist files to be processed],
  )],
  caption: [VERILOG GENERATION OPTIONS],
  kind: table,
)

`generate verilog` rewrites its outputs on every run and has no `--force`.
Default generation writes canonical QSoC output without consulting `PATH`.
`--format` is an explicit post-processing step; its bytes depend on the
installed `verible-verilog-format` and are not canonical.

==== Output Layout
<verilog-output-layout>
Each top or PRCM circuit is a unit under `output/<unit>/`. Every run also
writes the shared cell unit `output/qsoc_cell/` and the top file list
`output/qsoc.fl`.

#figure(
  align(center)[#table(
    columns: (auto, 1fr),
    align: (left, left),
    table.header([Path], [Content]),
    table.hline(),
    [`output/<top>/rtl/<top>.v`], [Top-level Verilog, with its controllers],
    [`output/<top>/rtl/<top>.fl`], [Unit file list],
    [`output/<top>/doc/<controller>.typ`], [Clock, reset, and power controller diagrams],
    [`output/<top>/reports/<top>.nc.rpt`], [Unconnected port report, when ports are left open],
    [`output/qsoc_cell/rtl/role/<role>.v`], [One role module per file (@cell-roles)],
    [`output/qsoc_cell/rtl/qsoc_cell_clock.v`], [Clock template cells],
    [`output/qsoc_cell/rtl/qsoc_cell_reset.v`], [Reset template cells],
    [`output/qsoc_cell/rtl/qsoc_cell_power.v`], [Power template cells],
    [`output/qsoc_cell/rtl/qsoc_cell.fl`], [Cell file list: the roles, then clock, reset, power],
    [`output/qsoc_cell/formal/`], [Cell formal checks, with `--with-formal`],
    [`output/qsoc_cell/model/<cell>.v`], [Model of each declared cell (@cell-declare)],
    [`output/qsoc_cell/model/qsoc_cell_model.fl`], [Model file list],
    [`output/qsoc_cell/qsoc_cell_role.rpt`], [Role binding report, `asic` target only],
    [`output/qsoc.fl`],
    [Concatenation of every unit file list],
  )],
  caption: [VERILOG OUTPUT LAYOUT],
  kind: table,
)

Every `.fl` entry is a path relative to `output/`, so every list works from
that directory:

```bash
cd output
iverilog -g2005 -s top -c qsoc.fl                 # generic target
iverilog -g2005 -s top -c qsoc.fl -c qsoc_cell/model/qsoc_cell_model.fl   # asic
```

`qsoc.fl` lists `qsoc_cell` first, then, in name order, every unit under
`output/` that has an `rtl/<unit>.fl`, both `output/<unit>/` and
`output/<library>/<module>/`. It is rebuilt after every generation, so a
deleted unit directory drops out of the next `qsoc.fl`. Rebuilding fails when a
list holds a path outside `output/`, or when two listed files define the same
module.

A failed run adds no unit. The `qsoc_cell` RTL is rewritten on every run, so
edit the netlist or the cell declarations, not these files. A top, PRCM
circuit, controller, or declared cell named `qsoc` or `qsoc_*`, in any case,
is rejected.

With `--with-formal`, every run also writes `output/qsoc_cell/formal/`: a
harness per cell file, `check.sby` with every task, and `qsoc_cell_formal.fl`.
Run `sby -f check.sby [task]` in that directory. Without `--with-formal`, an
existing `formal/` directory is left as it is.

===== Cell Roles
<cell-roles>
A role is a small clock-path module with fixed ports: a clock buffer,
inverter, logic gate, mux, clock gate, or synchronizer. Every such element in
the generated cells and controllers is a role instance, and each role is one
file in `output/qsoc_cell/rtl/role/`. In the default `generic` target the file
holds behavioral RTL with no `keep` or `dont_touch` attribute. In the `asic`
target it instantiates your technology cell instead (@cell-declare), so the
rest of the RTL never changes.

#figure(
  align(center)[#table(
    columns: (auto, 1fr),
    align: (left, left),
    table.header([Role], [Ports and function]),
    table.hline(),
    [`qsoc_ck_buf`], [`clk_in`, `clk_out`: buffer],
    [`qsoc_ck_inv`], [`clk_in`, `clk_out`: inverter],
    [`qsoc_ck_or2`], [`clk_in0`, `clk_in1`, `clk_out`: OR],
    [`qsoc_ck_xor2`], [`clk_in0`, `clk_in1`, `clk_out`: XOR],
    [`qsoc_ck_mux2`], [`clk_in0`, `clk_in1`, `clk_sel`, `clk_out`: `clk_sel ? clk_in1 : clk_in0`],
    [`qsoc_ck_icg_pos`],
    [`clk`, `en`, `test_en`, `clk_out`: latch `en | test_en` while `clk` is low, output low while disabled],
    [`qsoc_ck_icg_neg`],
    [`clk`, `en`, `test_en`, `clk_out`: latch `en | test_en` while `clk` is high, output high while disabled],
    [`qsoc_sync`],
    [`#(STAGES, RESET_VALUE)` with `clk`, `rst_n`, `d`, `q`: `STAGES` flops on `clk`, reset asynchronously to `RESET_VALUE`],
  )],
  caption: [CELL ROLES],
  kind: table,
)

`STAGES` defaults to 2 and must be at least 1. `RESET_VALUE` is one bit.

===== Declaring Cells
<cell-declare>
To use technology clock cells, describe them in a module library and select
the `asic` target. This library declares a made-up clock cell set. Save it as
`module/clkcells.soc_mod`, or add the keys to the entries that `module
import` wrote for the cells. A later `module import` keeps them.

```yaml
CKINV:
  port:
    A: {direction: input, type: logic}
    Y: {direction: output, type: logic}
  function: {Y: "!A"}                   # one row, the output an expression
CKBUF:
  port: {...}                           # A in, Y out
  function: {Y: A}
CKMUX2:
  port: {...}                           # D0, D1, S in, Y out
  function: [{S: 0, Y: D0}, {S: 1, Y: D1}]    # an absent pin is x
CKNAND2:
  port: {...}                           # A, B in, Y out
  function: [{A: 0, Y: 1}, {B: 0, Y: 1}, {A: 1, B: 1, Y: 0}]
CKGATE:
  port: {...}                           # CK, E, SE in, GCK out
  sequential: {type: icg_pos, clock: CK, enable: E, test: SE, output: GCK}
SYNC2:
  port: {...}                           # CK, D, RN, SI, SE in, Q out
  sequential: {type: sync, stages: 2, clock: CK, data: D, output: Q, reset: RN}
  tie: {SI: 0, SE: 0}                   # scan pins held inactive
```

Then set the target in the project file:

```yaml
cell:
  target: asic              # generic (default) or asic
  synth_rlimit: 200000000   # optional, solver budget per composed role, 0 for none
```

`qsoc generate verilog` now binds every role it can to a cell, composes the
others from the declared cells, and warns once for each composed role. The
binding lands in `output/qsoc_cell/qsoc_cell_role.rpt`, here shortened:

```yaml
target: asic
role:
  qsoc_ck_inv:
    cell: CKINV
    instance: u_cell
    pin: {A: clk_in, Y: clk_out}
  qsoc_ck_or2:
    synthesized:
      depth: 1
      cells: 1
      use: {CKMUX2: 1}
      instance: {u_cell_g0: CKMUX2}
      hazard: free for one input change at a time
  qsoc_ck_xor2:
    synthesized:
      depth: 2
      cells: 2
      use: {CKINV: 1, CKMUX2: 1}
      instance: {u_cell_g0: CKINV, u_cell_g1: CKMUX2}
      hazard: free for one input change at a time
  qsoc_ck_icg_neg:
    composed: [qsoc_ck_inv, qsoc_ck_icg_pos, qsoc_ck_inv]
    note: two extra inverter delays
  qsoc_sync:
    cell: SYNC2
    instance: g_cell[i].u_cell
    pin: {CK: clk, D: d, Q: q, RN: rst_n, SE: 1'b0, SI: 1'b0}
    stages: 2
```

No declared cell is an OR or an XOR, so QSoC builds them. The mux gives the
smallest networks, so `CKNAND2` stays unused. The negative clock gate is the
positive one between two inverters. A role bound to one cell wraps it, as in
`output/qsoc_cell/rtl/role/qsoc_ck_mux2.v`:

```verilog
(* keep_hierarchy = "yes" *)
module qsoc_ck_mux2 (
    input  wire clk_in0,
    input  wire clk_in1,
    input  wire clk_sel,
    output wire clk_out
);
    (* dont_touch = "true" *)
    CKMUX2 u_cell (.D0(clk_in0), .D1(clk_in1), .S(clk_sel), .Y(clk_out));
endmodule
```

*Unresolved roles.* A role that no cell binds and no network composes
instantiates `qsoc_role_unresolved_<name>`, such as
`qsoc_role_unresolved_ck_mux2`, a module that does not exist. With only
`CKBUF`, `CKGATE`, and `SYNC2` declared, generation still succeeds and warns:

```text
warning: cell roles without a declared cell, elaboration fails where they are used: qsoc_ck_inv, qsoc_ck_or2, qsoc_ck_xor2, qsoc_ck_mux2, qsoc_ck_icg_neg
warning: cell role qsoc_ck_mux2 not composed: proven impossible, no hazard-free network of at most 8 declared cells exists
```

Elaboration with the top set, such as `iverilog -s <top>`, fails only where
the design uses such a role. Declare a cell for the role, or declare basic
cells that QSoC can compose it from. Adding `CKINV` and `CKNAND2` to that
library resolves all five roles. When the warning names the solver budget,
raise `cell.synth_rlimit` or set it to 0. An `asic` project with no `icg_pos`
or `icg_neg` cell is rejected, since every clock controller uses a gate.

*Declaration rules.* A `function` is one row or a list of rows. A row maps
input pins to 0 or 1, and an absent pin is x. It maps output pins to 0, 1, or
an expression over input pins with `! ~ & | ^ && || ?:`. A `sequential`
template names pins by their use:

#figure(
  align(center)[#table(
    columns: (auto, 1fr),
    align: (left, left),
    table.header([`type`], [Keys and behavior]),
    table.hline(),
    [`icg_pos`],
    [`clock`, `enable`, optional `test`, `output`: latch `enable | test` while `clock` is low, output low while disabled],
    [`icg_neg`],
    [`clock`, `enable`, optional `test`, `output`: latch `enable | test` while `clock` is high, output high while disabled],
    [`sync`],
    [`stages`, `clock`, `data`, `output`, `reset`: `stages` flops, `reset` an active-low asynchronous clear],
  )],
  caption: [SEQUENTIAL CELL TEMPLATES],
  kind: table,
)

`tie` holds input pins at 0 or 1. A gate cell without `test` is allowed: its
role drives `enable` with `en | test_en`. Generation rejects a declaration
when:

- a pin is missing, has the wrong direction, is wider than 1 bit, or is inout
- two rows that overlap disagree, or an output is undefined for some inputs
- an input appears in no function, template key or `tie`, or an output has no function
- `type` is not one of the three above, a `sync` cell has no `reset` or `stages`
- a `tie` names an output or a value other than 0 or 1
- the name is `qsoc` or starts with `qsoc_`
- two declared cells match the same role, and the error names both

*Binding.* A combinational cell binds a role when its function, with its ties
applied, equals the role function under some pin mapping. A sequential cell
binds the role of its `type`. When only one gate role is bound and
`qsoc_ck_inv` is bound, the other gate is composed as inverter, gate,
inverter, which adds two inverter delays. A `qsoc_sync` of `STAGES` flops uses
`STAGES / stages` whole cells, then plain flops with the same reset up to
`STAGES`. `RESET_VALUE` 1 inverts the chain input and output.

#figure(
  align(center)[#table(
    columns: (auto, 1fr),
    align: (left, left),
    table.header([Target], [Role files]),
    table.hline(),
    [`generic`], [Behavioral bodies. Declarations are checked, and models written],
    [`asic`],
    [Every role, with `(* keep_hierarchy = "yes" *)`. A bound or composed role instantiates its cells with `(* dont_touch = "true" *)` and ties. An unresolved role instantiates `qsoc_role_unresolved_<name>`],
  )],
  caption: [CELL TARGETS],
  kind: table,
)

*Instance paths.* Declared cells sit at fixed paths below each role instance,
for SDC:

#figure(
  align(center)[#table(
    columns: (auto, 1fr),
    align: (left, left),
    table.header([Role], [Cell instance]),
    table.hline(),
    [Combinational, gate], [`<role instance>/u_cell`],
    [Composed gate], [`<role instance>/u_icg/u_cell`, inverters `u_inv_in/u_cell` and `u_inv_out/u_cell`],
    [Composed combinational], [`<role instance>/u_cell_g0` to `u_cell_g<n-1>`, the last one drives `clk_out`],
    [`qsoc_sync`], [`<role instance>/g_cell[<i>].u_cell`, plain flops `g_extra.tail`],
  )],
  caption: [DECLARED CELL PATHS],
  kind: table,
)

*Composition.* In the `asic` target, QSoC builds each of `qsoc_ck_buf`,
`qsoc_ck_inv`, `qsoc_ck_or2`, `qsoc_ck_xor2`, and `qsoc_ck_mux2` that no cell
binds from the declared combinational cells with one output and one to three
inputs left after their ties. It takes the network with the least depth, then
the fewest cells, up to 8 cells. The result does not depend on declaration
order, and nothing is written back to the library.

A composed network is free of static and dynamic hazards when one input
changes at a time and the network settles between changes, for any fixed
delay of each cell and wire. For `qsoc_ck_mux2` the data inputs can also
change together while `clk_sel` holds. QSoC does not check a `clk_sel` change
on a composed `qsoc_ck_mux2`, two inputs of `qsoc_ck_or2` or `qsoc_ck_xor2`
changing together, glitches inside a cell, or timing, which is left to STA.

#figure(
  align(center)[#table(
    columns: (auto, 1fr),
    align: (left, left),
    table.header([Search result], [Role]),
    table.hline(),
    [Found], [Instantiates the network as `u_cell_g<i>`. Generation warns with the depth and instances],
    [No network], [Stays unresolved. The warning says the role is proven impossible to compose within 8 cells],
    [Budget spent], [Stays unresolved. The warning gives the budget and names `cell.synth_rlimit`],
  )],
  caption: [ROLE COMPOSITION],
  kind: table,
)

`cell.synth_rlimit` is the solver budget for each composed role, an integer
from 0 to 4294967295, default 200000000, and 0 removes the limit. It counts
solver steps, not time, so the result does not depend on machine load.

*Report.* Besides the keys shown above, `qsoc_cell_role.rpt` gives
`synth_rlimit`, and for an unresolved role `unresolved` plus `synthesis` with
the reason when composition failed.

*Models.* Each declared cell gets a behavioral model in
`output/qsoc_cell/model/<cell>.v`, listed by `qsoc_cell_model.fl`, for
simulation and formal only. Neither `qsoc_cell.fl` nor `qsoc.fl` lists them,
so synthesis reads your library instead. A sequential model ignores its tied
pins.

*Contracts.* With `--with-formal` in the `asic` target,
`output/qsoc_cell/formal/contract/` holds one `<role>_contract.sv` per bound or
composed role, `qsoc_cell_contract.fl`, and `contract.sby`. Each task proves
that the role file with the cell models behaves like the generic role: for
every input of a combinational role, once the latch has loaded for a gate
role, and after reset for `qsoc_sync` with `STAGES` 1, 2, 3, and 5 and both
reset values. Run `sby -f contract.sby [task]` in that directory. The proof
covers your declaration, not the cell itself, so check the declaration
against the databook. In the `asic` target the cell checks in `formal/` and
the PRCM checks read generic role copies from `formal/role/`.

==== Upgrading Earlier Output
<output-upgrade>
Projects generated by QSoC 2.5.1 or earlier need these changes:

#figure(
  align(center)[#table(
    columns: (0.9fr, 1fr),
    align: (left, left),
    table.header([Before], [Now]),
    table.hline(),
    [`output/<top>.v`, `<top>.nc.rpt`, and diagrams in `output/`],
    [`output/<top>/rtl/`, `reports/`, and `doc/`. Delete the old files],
    [`clock_cell.v`, `reset_cell.v`, `power_cell.v`, kept unless `--force`],
    [`output/qsoc_cell/rtl/qsoc_cell_{clock,reset,power}.v`, rewritten on every run],
    [`generate verilog -f`, `--force`], [Removed. Drop the option from scripts],
    [`qsoc_tc_clk_*` cells], [Roles `qsoc_ck_*` and `qsoc_sync` (@cell-roles)],
    [`qsoc_clk_div_auto`], [`qsoc_clk_div` with `AUTO_UPDATE` (@soc-net-clock-divider-auto)],
    [`generate module` files directly in `output/<library>/<module>/`],
    [Subdirectories `rtl/`, `formal/`, `uvm/`. Generation refuses while an old file remains there, so move the old directory aside],
    [IOMUX `pull` and `control` with `port` and `table`],
    [`function` rows (@iomux-pad-cell)],
  )],
  caption: [OUTPUT CHANGES AFTER 2.5.1],
  kind: table,
)

==== Netlist Merge Semantics (`-m` / `--merge`)
<netlist-merge-semantics>
The `--merge` option loads two or more netlist files in command-line order
and folds them into a single netlist before generation. It is the standard
pattern for SoC top-level integration where each peripheral block lives in
its own `<block>_inst.soc_net` and the top is assembled from all of them.

Merge rules (applied recursively, file-by-file):

- *Map sections* (`instance`, `net`, `port`, `parameter`, `bus`):
  union of keys. When the same key appears in two files the values are
  merged recursively (deep merge).
- *List sections* (the connection list under each `net.<name>`):
  concatenation. A net listed in two files ends up with all connections
  from both files in the order they were loaded.
- *Scalar values*: the later file overrides the earlier one.

The output filename is derived from the *first* file's basename. Order
matters: pass the top-level / framework file first, then peripheral
instances, so any conflicting scalar in a later file is the override.

Example:

```bash
qsoc generate verilog --merge \
  output/soc_top.soc_net    \
  output/cpu_inst.soc_net   \
  output/peri_inst.soc_net
```

==== Unconnected Port Report
<unconnected-port-report>
The Verilog generation automatically creates an unconnected port report when unconnected ports are detected. The report is saved as `output/<top>/reports/<top>.nc.rpt` in YAML format containing:

- Summary statistics (total instances and ports)
- Detailed breakdown by instance and port
- Port type and direction information

Example report structure:
```yaml
# Unconnected port report - soc_top
# Generated by QSoC.

summary:
  total_instance: 2
  total_port: 3

instance:
  u_axi4_interconnect:
    module: axi4_interconnect
    port:
      araddr:
        type: logic[39:0]
        direction: input
```

=== Template Generation Options
<template-generation>
The `generate template` command generates files from Jinja2 templates using CSV, YAML, JSON, SystemRDL (RDL), and RCSV (Register-CSV) data sources.

#figure(
  align(center)[#table(
    columns: (0.5fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [`-d`, `--directory <path>`], [The path to the project directory],
    [`-p`, `--project <name>`], [The project name],
    [`--csv <file>`], [CSV data file (can be used multiple times)],
    [`--yaml <file>`], [YAML data file (can be used multiple times)],
    [`--json <file>`], [JSON data file (can be used multiple times)],
    [`--rdl <file>`], [SystemRDL data file (can be used multiple times)],
    [`--rcsv <file>`],
    [RCSV (Register-CSV) data file (can be used multiple times)],
    [templates], [The Jinja2 template files to be processed],
  )],
  caption: [TEMPLATE GENERATION OPTIONS],
  kind: table,
)

=== Template Generation Examples
<template-generation-examples>
The following examples demonstrate usage of different data sources with template generation:

==== SystemRDL Template Usage
```bash
# Generate from SystemRDL file
qsoc generate template --rdl registers.rdl template.h.j2

# Multiple SystemRDL files (independent namespaces)
qsoc generate template --rdl cpu_regs.rdl --rdl mem_regs.rdl system.h.j2
```

==== RCSV Template Usage
```bash
# Generate from RCSV file
qsoc generate template --rcsv chip_registers.csv template.h.j2

# Mixed data sources
qsoc generate template --csv config.csv --rdl registers.rdl --rcsv peripherals.csv template.h.j2
```

==== Data Source Namespacing
Each data file is a namespace named after its basename: `registers.rdl` is
`{{ registers.* }}` and `config.csv` is `{{ config.* }}` in a template.

==== SystemRDL Template Access Patterns
SystemRDL files generate simplified JSON format accessible in templates:
```jinja2
// Access addrmap information
{{ chip.addrmap.inst_name }}

// Iterate through registers
{% for reg in chip.registers %}
  Register: {{ reg.inst_name }} @ {{ reg.absolute_address }}
  {% for field in reg.fields %}
    Field: {{ field.inst_name }} [{{ field.msb }}:{{ field.lsb }}]
  {% endfor %}
{% endfor %}
```

A field `reset` is a lowercase hex string such as `"0xff"`, and so is
`reg.register_reset_value`. A field without a reset has no `reset` key, so
test for it first: `{% if existsIn(field, "reset") %}{{ field.reset }}{% endif %}`.

==== RCSV Processing
RCSV files expose the same template data structure as SystemRDL files.

=== Stub Generation Options
<stub-generation>
The `generate stub` command generates Verilog and Liberty stub files for selected modules.

#figure(
  align(center)[#table(
    columns: (0.5fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [`-d`, `--directory <path>`], [The path to the project directory],
    [`-p`, `--project <name>`], [The project name],
    [`-l`, `--library <regex>`],
    [The library base name or regex pattern to filter libraries],
    [`-m`, `--module <regex>`],
    [The module name or regex pattern to filter modules],
    [stubname],
    [The base name for the generated stub files (generates stubname.v and stubname.lib)],
  )],
  caption: [STUB GENERATION OPTIONS],
  kind: table,
)
