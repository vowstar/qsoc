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

On Linux the AppImage needs the executable bit before it runs:

```bash
chmod +x QSoC-*.AppImage
./QSoC-*.AppImage --version
```

To build from source instead:

```bash
nix develop
cmake -B build -G Ninja
cmake --build build -j
```

A first session runs the commands described in @cli-overview:

```bash
qsoc project create mychip               # create a project in the current directory
qsoc project show                        # check what was created
qsoc module import rtl/*.v               # import Verilog modules into the library
qsoc module list                         # confirm the modules landed
qsoc generate verilog output/top.soc_net # generate output/top.v
```

Every command accepts `--help`.

The generators need no LLM configuration. The agent does: set `llm.url`,
`llm.key`, and `llm.model` before running `qsoc agent`. QSoC writes a template
user configuration on first start, so filling in three keys is enough:

```yaml
llm:
  url: https://api.example.com/chat/completions
  key: your-api-key
  model: your-model-id
```

@llm-config lists every endpoint option and @config-files explains which
configuration layer wins.

== Project Layout
<project-layout>
`project create` writes a project file and four directories into the target
directory. Every later command reads and writes inside that tree:

#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto, left),
    table.header([Path], [Contents]),
    table.hline(),
    [`<name>.soc_pro`],
    [Project file: directory paths and extension fields, without a tool version],
    [`bus/`], [Bus definition libraries],
    [`module/`], [Module libraries filled by `module import`],
    [`schematic/`], [Schematic sources],
    [`output/`],
    [Netlists (`.soc_net`) and generated output: `<netlist>.v` plus
     `<netlist>.nc.rpt` when ports are left unconnected],
    [`.qsoc.yml`],
    [Project-level configuration, overrides the user layer. Created when you
     add project settings],
    [`.qsoc/`],
    [Agent state: sessions, plans, sub-agent definitions, skills, memory, and
     the remote workspace binding. Created by the agent on first run],
  )],
  caption: [PROJECT LAYOUT],
  kind: table,
)

The `project create` options in @project-creation move any of the four
directories elsewhere.

== Your First Netlist
<first-netlist>
A netlist declares top-level ports, instantiates modules, and lists the nets
between them. This one is complete and generates:

```yaml
# output/top.soc_net
port:
  clk:
    direction: input
    type: logic
    connect: clk           # tie this top-level port to the net named clk
  rst_n:
    direction: input
    type: logic
    connect: rst_n
  data_out:
    direction: output
    type: logic[7:0]
    connect: data_bus

instance:
  u_src:
    module: source
  u_sink:
    module: sink

net:
  clk:
    - instance: u_src
      port: clk
    - instance: u_sink
      port: clk
  rst_n:
    - instance: u_src
      port: rst_n
    - instance: u_sink
      port: rst_n
  data_bus:
    - instance: u_src
      port: dout
    - instance: u_sink
      port: din
```

The `connect:` attribute is what joins a top-level port to a net. A net whose
name happens to match a port name is also wired to that port, but the generator
warns about it, so state the link explicitly. Nets with no `connect:` reference
stay internal and are declared as wires.

Then:

```bash
qsoc generate verilog output/top.soc_net
```

The modules named under `instance:` must already be in the module library, so
run `module import` on their Verilog sources first. @netlist-format documents
every section, and @soc-net-example shows a larger design.

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
