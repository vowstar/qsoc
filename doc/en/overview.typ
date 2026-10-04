= Overview
<overview>
QSoC writes SoC integration RTL from YAML descriptions. You import Verilog
modules into a project library and describe the top level in a netlist
(`.soc_net`). QSoC then writes Verilog-2001 with the instances, the wiring, and
the clock, reset, and power controllers that the netlist declares. The same
project opens in the CLI, the GUI, and a terminal LLM agent.

#figure(
  align(center)[#table(
    columns: (1fr, auto, auto),
    align: (left, left, left),
    table.header([Task], [Command], [Chapter]),
    table.hline(),
    [Wire modules and buses into a top level], [`generate verilog`], [@netlist-format],
    [Add combinational, sequential, and FSM logic], [`generate verilog`], [@soc-net-comb],
    [Add clock, reset, and power controllers], [`generate verilog`], [@soc-net-reset-overview],
    [Generate a PRCM controller with APB4 or AXI4-Lite control], [`generate verilog`], [@prcm-check],
    [Map clock-path cells to your technology cells], [`generate verilog`], [@cell-declare],
    [Generate an MMIO register block], [`generate module`], [@mmio-generator],
    [Generate an IOMUX pin multiplexer], [`generate module`], [@iomux-generator],
    [Write formal or UVM collateral], [`--with-formal` \ `--with-uvm`], [@generated-module-options],
    [Render files from Jinja2 templates and register data], [`generate template`], [@template-generation],
    [Write Verilog and Liberty stubs of library modules], [`generate stub`], [@stub-generation],
    [Edit schematics and controller diagrams], [`gui`], [@gui-overview],
    [Drive these tools by prompt, local or over SSH], [`agent`], [@agent-overview],
  )],
  caption: [WHAT QSOC DOES],
  kind: table,
)

Generation reports multiple drivers, undriven nets, and width mismatches
(@validation-format). It does not simulate, lint, or synthesize the output, and
it writes formal and UVM collateral without running it. Check the generated RTL
in your own flow (@soc-net-generated-rtl).

== Getting Started
<getting-started>
Each release at #link("https://github.com/vowstar/qsoc/releases") has a binary
for three platforms and this manual as a PDF:

#figure(
  align(center)[#table(
    columns: (auto, 1fr),
    align: (left, left),
    table.header([Platform], [Release file]),
    table.hline(),
    [Linux x86-64], [`QSoC-*-x86_64.AppImage`. Run `chmod +x` on it first],
    [macOS Apple silicon], [`QSoC-*-macos-arm64.dmg`],
    [Windows x64], [`QSoC-*-windows-x64.zip`],
  )],
  caption: [RELEASE BINARIES],
  kind: table,
)

Releases also provide `qsoc-X.Y.Z.tar.xz` and its `.sha256` checksum file.
This archive includes recursive submodule sources, which GitHub's automatic source downloads omit.
The `source-revisions.json` file records the main and dependency commits.
Qt and other system build dependencies still need installation.

With Nix, `nix shell github:vowstar/qsoc#qsoc` gives a shell with `qsoc` on
the `PATH`. To build from a clone of the repository:

```bash
nix develop
cmake -B build -G Ninja
cmake --build build
```

Every command and subcommand accepts `--help`.

== First Run
<first-run>
This example builds a two-module top level from an empty directory. Create the
project and two Verilog sources:

```bash
mkdir demo && cd demo
qsoc project create demo
mkdir rtl
```

```verilog
// rtl/counter.v
module counter (input clk, input rst_n, output reg [7:0] count);
  always @(posedge clk or negedge rst_n)
    if (!rst_n) count <= 8'd0;
    else        count <= count + 8'd1;
endmodule

// rtl/sink.v
module sink (input clk, input [7:0] din, output reg [7:0] dout);
  always @(posedge clk) dout <= din;
endmodule
```

Import both modules into a library named `demo`:

```bash
qsoc module import -l demo rtl/counter.v rtl/sink.v
qsoc module list
```

Save this netlist as `output/top.soc_net`:

```yaml
port:
  clk:
    direction: input
    connect: clk        # tie this top-level port to the net named clk
  rst_n:
    direction: input
    connect: rst_n
  data_out:
    direction: output
    type: logic[7:0]
    connect: data_q

instance:
  u_counter:
    module: counter
  u_sink:
    module: sink

net:
  clk:
    - { instance: u_counter, port: clk }
    - { instance: u_sink, port: clk }
  rst_n:
    - { instance: u_counter, port: rst_n }
  data:
    - { instance: u_counter, port: count }
    - { instance: u_sink, port: din }
  data_q:
    - { instance: u_sink, port: dout }
```

`connect:` joins a top-level port to a net. A net that no port names stays
internal and becomes a wire. Generate the top level:

```bash
qsoc generate verilog output/top.soc_net
```

The run writes these files:

```text
output/top/rtl/top.v        the top level
output/top/rtl/top.fl       its file list
output/qsoc_cell/           shared cells that controllers use
output/qsoc.fl              the list of every generated file
```

`qsoc.fl` lists generated files only, with paths relative to `output/`. Add
your own module sources when you compile:

```bash
cd output && iverilog -g2005 -s top -c qsoc.fl ../rtl/counter.v ../rtl/sink.v
```

@netlist-format documents every netlist section, and @verilog-output-layout
lists every output file.

== Agent Setup
<agent-setup>
The generators need no LLM. The agent needs one model entry. QSoC writes a
commented template to `~/.config/qsoc/qsoc.yml` on the first start. Declare an
entry under `llm.models` and point `llm.model` at it:

```yaml
llm:
  model: my-model
  models:
    my-model:
      url: https://api.example.com/chat/completions
      key: your-api-key
      model: your-model-id
```

Then run `qsoc agent` in the project directory. @llm-config lists every model
field, and @config-files shows which configuration layer wins.

== Project Layout
<project-layout>
`project create` writes a project file and four directories. Every later
command reads and writes inside this tree:

#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto, left),
    table.header([Path], [Contents]),
    table.hline(),
    [`<name>.soc_pro`], [Project file: the directory paths],
    [`bus/`], [Bus definition libraries (`.soc_bus`)],
    [`module/`], [Module libraries (`.soc_mod`)],
    [`schematic/`], [GUI drawings (`.soc_sch`, `.soc_prc`)],
    [`output/`], [Netlists, generated units, and `qsoc.fl` (@verilog-output-layout)],
    [`.qsoc.yml`], [Project configuration. Created by the first project setting],
    [`.qsoc/`],
    [Agent sessions, plans, sub-agents, skills, memory, and remote workspace
     binding. Created when the agent first saves state],
  )],
  caption: [PROJECT LAYOUT],
  kind: table,
)

The `project create` options in @project-creation move any of the four
directories.

== Terminology
<terminology>
#figure(
  align(center)[#table(
    columns: (0.25fr, 1fr),
    align: (auto, left),
    table.header([Term], [Meaning]),
    table.hline(),
    [Netlist], [A `.soc_net` YAML file: top-level ports, instances, nets, and controller sections (@netlist-format)],
    [Unit], [One generated block with its own directory under `output/`: a top, a PRCM circuit, a generated module, or the shared `qsoc_cell` (@verilog-output-layout)],
    [Generated module], [A module that QSoC writes from a `.soc_mod` generator source, such as MMIO or IOMUX (@generated-module-options)],
    [Cell role],
    [A clock-path module with fixed ports, such as a clock gate or a synchronizer, that the generated controllers instantiate (@cell-roles)],
    [Cell target],
    [`generic` for behavioral roles, `asic` for roles that instantiate declared technology cells (@cell-declare)],
    [PRCM], [Power, reset, and clock management: a controller that sequences domains under register control (@prcm-check)],
    [SystemRDL, RCSV], [Register description formats that `generate template` reads (@template-generation)],
  )],
  caption: [TERMINOLOGY],
  kind: table,
)
