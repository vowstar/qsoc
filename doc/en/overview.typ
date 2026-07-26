= Overview
<overview>
QSoC is a System-on-Chip (SoC) design tool with three front ends over one
project format: a GUI for interactive editing, a CLI for scripted and batch
flows, and a terminal agent for LLM-driven work. All three read and write the
same project files, so a design can move between them at any point.

A project holds module and bus libraries, netlists, and generated output.
Commands create and manage projects, import and update Verilog modules, manage
bus interfaces, and generate RTL, register maps, and stub files. Verbosity is
set per invocation, so the same command serves both interactive use and build
scripts.

== Getting Started
<getting-started>
Prebuilt binaries are attached to every release: an AppImage for Linux, a
`.dmg` for macOS, and a `.zip` for Windows. They are published at
#link("https://github.com/vowstar/qsoc/releases") together with this manual.

To build from source instead:

```bash
nix develop
cmake -B build -G Ninja
cmake --build build -j
```

A first session runs the commands described in @cli-overview:

```bash
qsoc project create mychip              # create a project in the current directory
qsoc module import rtl/*.v              # import Verilog modules into the library
qsoc generate verilog output/top.soc_net # generate RTL from a netlist
qsoc agent                              # or let the agent drive the same tools
```

Every command accepts `--help`. The agent is documented in @agent-overview, the
netlist format in @soc-net-format, and configuration in @config-overview.

== Terminology
<terminology>
The following terms are used in system descriptions.

#figure(
  align(center)[#table(
    columns: (0.25fr, 1fr),
    align: (auto, left),
    table.header([Terminology], [Description]),
    table.hline(),
    [SoC],
    [System-on-Chip, an integrated circuit that integrates all components of a computer or other electronic system],
    [RTL],
    [Register Transfer Level, a design abstraction which models a synchronous digital circuit in terms of the flow of digital signals between hardware registers],
    [GUI],
    [Graphical User Interface, a form of user interface that allows users to interact with electronic devices through graphical icons and visual indicators],
    [CLI],
    [Command Line Interface, a means of interacting with a computer program where the user issues commands to the program in the form of successive lines of text],
    [Verilog],
    [A hardware description language used to model electronic systems],
    [Bus],
    [A communication system that transfers data between components inside a computer or between computers],
    [SystemRDL],
    [A standard language for describing and specifying the behavior of register and memory structures within semiconductor IP],
    [RCSV],
    [Register-CSV format, a CSV-based approach for describing register structures following RCSV v0.3 specification],
  )],
  caption: [TERMINOLOGY OF SYSTEM],
  kind: table,
)

The following terms are used in command descriptions.

#figure(
  align(center)[#table(
    columns: (0.25fr, 1fr),
    align: (auto, left),
    table.header([Terminology], [Description]),
    table.hline(),
    [Command], [A primary operation in QSoC CLI],
    [Subcommand], [A secondary operation under a main command],
    [Option], [A parameter that modifies the behavior of a command],
    [Argument], [A value provided to a command or option],
    [Verbose], [Detailed output level for debugging and monitoring],
  )],
  caption: [TERMINOLOGY OF COMMANDS],
  kind: table,
)
