= Command-line Overview
<cli-overview>
QSoC provides a comprehensive command-line interface for SoC development and management.
The following sections describe the available commands and options.

== Command Line Interface
<cli>
QSoC provides a comprehensive command-line interface for SoC development. The following
commands and subcommands are available:

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
    [module], [import], [Import Verilog modules into module libraries],
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
    [verilog],
    [Generate Verilog code and unconnected port reports from netlist files],
    [],
    [template],
    [Generate files from Jinja2 templates using various data sources],
    [], [stub], [Generate Verilog and Liberty stub files for selected modules],
    [gui], [], [Start the software in GUI mode],
    [agent], [], [Start interactive AI agent for SoC design automation],
  )],
  caption: [COMMAND LINE INTERFACE],
  kind: table,
)

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
      - 1=Error - Only error messages (default) \
      - 2=Warning - Error and warning messages \
      - 3=Info - Error, warning, and informational messages \
      - 4=Debug - All messages including debug information \
      - 5=Verbose - Maximum detail for all operations
    ],
    [`--color <when>`],
    [Colorize output: `auto` (default), `always`, `never`. `auto` honors
     `NO_COLOR` / `FORCE_COLOR` and whether the stream is a terminal],
    [`-v`, `--version`], [Display version information],
  )],
  caption: [GLOBAL OPTIONS],
  kind: table,
)

== Project Command Options
<project-options>
The project command provides functionality for managing QSoC projects.

=== Project Creation Options
<project-creation>
The `project create` command creates a new QSoC project.

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
The module command provides functionality for managing hardware modules.

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

Both AI-assisted paths use the endpoint from @llm-config. Without an endpoint
configured, `--ai` and `explain` fail; the other subcommands do not need one.

== Bus Command Options
<bus-options>
The bus command provides functionality for managing bus interfaces.

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
The generate command provides functionality for generating different types of outputs.

=== Verilog Generation Options
<verilog-generation>
The `generate verilog` command generates Verilog code from netlist files. The
input format is documented in @soc-net-format.

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
    [`-f`, `--force`],
    [Force overwrite existing primitive cell files (clock_cell.v, reset_cell.v)],
    [files], [The netlist files to be processed],
  )],
  caption: [VERILOG GENERATION OPTIONS],
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
The Verilog generation automatically creates an unconnected port report when unconnected ports are detected. The report is saved as `<module_name>.nc.rpt` in YAML format containing:

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
Each data file creates an independent namespace in templates using the file's basename:
- `registers.rdl` → accessible as `{{ registers.* }}` in templates
- `config.csv` → accessible as `{{ config.* }}` in templates
- `chip_regs.csv` → accessible as `{{ chip_regs.* }}` in templates

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

==== RCSV Processing
RCSV files are processed through two-stage conversion:
+ CSV to SystemRDL conversion using `csv_to_rdl()`
+ SystemRDL elaboration to simplified JSON using `elaborate_simplified()`
This ensures RCSV files follow the same template access patterns as RDL files.

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
