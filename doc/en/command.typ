= SYSTEM DESCRIPTION
<system-description>
QSoC provides a comprehensive command-line interface for SoC development and management.
The following sections describe the available commands and options.

== COMMAND LINE INTERFACE
<cli>
QSoC provides a comprehensive command-line interface for SoC development. The following
commands and subcommands are available:

#figure(
  align(center)[#table(
    columns: (0.25fr, 0.25fr, 1fr),
    align: (auto,auto,auto,),
    table.header([Command], [Subcommand], [Description],),
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
    [schematic], [], [Process schematic designs (not implemented yet)],
    [generate], [verilog], [Generate Verilog code from netlist files],
    [], [template], [Generate files from Jinja2 templates using various data sources],
    [gui], [], [Start the software in GUI mode],
  )]
  , caption: [COMMAND LINE INTERFACE]
  , kind: table
  )

== GLOBAL OPTIONS
<global-options>
The following global options are available for all commands:

#figure(
  align(center)[#table(
    columns: (0.25fr, 1fr),
    align: (auto,auto,),
    table.header([Option], [Description],),
    table.hline(),
    [-h, --help], [Display help information for commands and options],
    [--verbose <level>], [Set verbosity level (0-5): 0=silent, 1=error, 2=warning, 3=info, 4=debug, 5=verbose],
    [-v, --version], [Display version information],
  )]
  , caption: [GLOBAL OPTIONS]
  , kind: table
  )

== PROJECT COMMAND OPTIONS
<project-options>
The project command provides functionality for managing QSoC projects.

=== Project Creation Options
<project-creation>
The `project create` command creates a new QSoC project.

#figure(
  align(center)[#table(
    columns: (0.5fr, 1fr),
    align: (auto,auto,),
    table.header([Option], [Description],),
    table.hline(),
    [-d, --directory <path>], [The path to the project directory],
    [-b, --bus <path>], [The path to the bus directory],
    [-m, --module <path>], [The path to the module directory],
    [-s, --schematic <path>], [The path to the schematic directory],
    [-o, --output <path>], [The path to the output file],
    [name], [The name of the project to be created],
  )]
  , caption: [PROJECT CREATION OPTIONS]
  , kind: table
  )

== MODULE COMMAND OPTIONS
<module-options>
The module command provides functionality for managing hardware modules.

=== Module Import Options
<module-import>
The `module import` command imports Verilog modules into module libraries.

#figure(
  align(center)[#table(
    columns: (0.5fr, 1fr),
    align: (auto,auto,),
    table.header([Option], [Description],),
    table.hline(),
    [-d, --directory <path>], [The path to the project directory],
    [-p, --project <name>], [The project name],
    [-l, --library <name>], [The library base name],
    [-m, --module <regex>], [The module name or regex],
    [-f, --filelist <path>], [The path where the file list is located, including a list of verilog files in order],
    [files], [The verilog files to be processed],
  )]
  , caption: [MODULE IMPORT OPTIONS]
  , kind: table
  )

== BUS COMMAND OPTIONS
<bus-options>
The bus command provides functionality for managing bus interfaces.

=== Bus Import Options
<bus-import>
The `bus import` command imports buses into bus libraries.

#figure(
  align(center)[#table(
    columns: (0.5fr, 1fr),
    align: (auto,auto,),
    table.header([Option], [Description],),
    table.hline(),
    [-d, --directory <path>], [The path to the project directory],
    [-p, --project <name>], [The project name],
    [-l, --library <name>], [The library base name],
    [-b, --bus <name>], [The specified bus name],
    [files], [The bus definition CSV files to be processed],
  )]
  , caption: [BUS IMPORT OPTIONS]
  , kind: table
  )

== GENERATE COMMAND OPTIONS
<generate-options>
The generate command provides functionality for generating different types of outputs.

=== Verilog Generation Options
<verilog-generation>
The `generate verilog` command generates Verilog code from netlist files.

#figure(
  align(center)[#table(
    columns: (0.5fr, 1fr),
    align: (auto,auto,),
    table.header([Option], [Description],),
    table.hline(),
    [-d, --directory <path>], [The path to the project directory],
    [-p, --project <name>], [The project name],
    [files], [The netlist files to be processed],
  )]
  , caption: [VERILOG GENERATION OPTIONS]
  , kind: table
  )

=== Template Generation Options
<template-generation>
The `generate template` command generates files from Jinja2 templates using various data sources.

#figure(
  align(center)[#table(
    columns: (0.5fr, 1fr),
    align: (auto,auto,),
    table.header([Option], [Description],),
    table.hline(),
    [-d, --directory <path>], [The path to the project directory],
    [-p, --project <name>], [The project name],
    [--csv <file>], [CSV data file (can be used multiple times)],
    [--yaml <file>], [YAML data file (can be used multiple times)],
    [--json <file>], [JSON data file (can be used multiple times)],
    [templates], [The Jinja2 template files to be processed],
  )]
  , caption: [TEMPLATE GENERATION OPTIONS]
  , kind: table
  )

#pagebreak()
