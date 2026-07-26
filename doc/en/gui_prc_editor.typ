= GUI Power, Reset and Clock Editor
<gui-prc-editor>
The PRC editor draws clock, reset, and power controllers and exports them as
the `clock:`, `reset:`, and `power:` sections of a netlist
(@soc-net-clock-overview, @soc-net-reset-overview, @soc-net-power-overview).
It is an authoring front end only: generation still runs through
`qsoc generate verilog` (@verilog-generation).

Read @gui-prc-limitations for what the editor can and cannot express.

== The Palette
<gui-prc-palette>
The PRC Library dock offers five primitives:

#figure(
  align(center)[#table(
    columns: (0.35fr, 0.25fr, 1fr),
    align: (auto, auto, left),
    table.header([Primitive], [Ports], [Becomes]),
    table.hline(),
    [Clock Input], [`out`], [One entry under `clock.input`],
    [Clock Target], [`in`, `out`], [One entry under `clock.target`],
    [Reset Source], [`out`], [One entry under `reset.source`],
    [Reset Target], [`in`], [One entry under `reset.target`],
    [Power Domain], [`dep`, `out`], [One entry in the `power.domain` list],
  )],
  caption: [PRC PRIMITIVES],
  kind: table,
)

There is no ICG, divider, inverter, or multiplexer item: those are properties
of a clock target or of a link between two items, set in the configuration
dialogs. The reset synchronizer is likewise a property of a reset target.

Dropping a primitive names it `clk_<n>`, `rst_<n>`, or `pd_<n>` and opens its
configuration dialog immediately. Clock and reset targets grow another input
port whenever all their inputs are connected.

Primitives assigned to the same controller are enclosed in a dashed frame.
Right-click inside a frame to edit that controller.

== Drawing and Configuring
<gui-prc-drawing>
Press *W* for wire mode and connect an `out` port to an `in` port. Only
compatible pairs are exported: clock input to clock target, reset source to
reset target, and power domain to power domain. Connections are detected
geometrically, so a wire that merely looks attached is not a link.

Double-click a primitive to reopen its dialog. The fields map directly onto the
netlist keys documented in the clock, reset, and power chapters: frequency,
select, reset and test clock for a clock target, plus optional ICG, divider,
inverter and STA guide blocks; active level for reset sources and targets, plus
the asynchronous synchronizer clock and stage count; voltage, power good and
the three cycle counts for a power domain.

The *Auto* button next to a field fills a suggested name, but only when the
field is still empty. Unchecking a group and confirming clears the fields
inside it rather than remembering them.

Double-click a wire to configure link-level operations. The wire label then
carries markers such as `[ICG]` or `[DIV/2]`, and the settings are stored with
the diagram.

A power domain is always-on until you clear *Always-on domain* in its dialog.
A controllable domain takes its dependencies from the wires drawn into its
`dep` port, and its reset synchronizers from the follow table in the same
dialog.

== From Diagram to RTL
<gui-prc-workflow>
+ Draw and configure the primitives, then wire them.
+ Save the drawing as `.soc_prc` (Ctrl+S).
+ Tools > Export Netlist (Ctrl+E) writes `<name>.soc_net` into the project
  output directory.
+ Review the exported YAML against @gui-prc-limitations.
+ Run `qsoc generate verilog <name>.soc_net`.

The exporter writes one controller per family, named after the controller the
primitives are assigned to, and falls back to `clock_ctrl`, `reset_ctrl` and
`power_ctrl` when nothing is assigned. Keys come from the name shown on each
box.

== Limitations
<gui-prc-limitations>
#figure(
  align(center)[#table(
    columns: (0.45fr, 1fr),
    align: (auto, left),
    table.header([Area], [Behavior in this release]),
    table.hline(),
    [Link operations], [Configured on a wire and saved with the diagram. The
      wire must carry a name for the settings to reach the netlist; unnamed
      wires are named after the connection they carry],
    [Controller names], [The export uses the controller each primitive is
      assigned to. A diagram with several controllers of one family still
      exports only the first],
    [Power host clock and reset], [Taken from the controller dialog. When they
      are left empty the export falls back to `ao_clk` and `ao_rst_n` and says
      so],
    [Power dependencies], [`depend:` comes from the wires drawn into a domain's
      dep port. Clear "Always-on domain" to make a domain controllable; an
      always-on domain omits the key entirely],
    [Reset synchronizers], [Only the asynchronous form is available; `sync` and
      `count` have no fields. The asynchronous settings are written onto every
      link of a target rather than after the combination],
    [Editing], [Cut, copy and paste are not available. Delete works from the
      keyboard],
  )],
  caption: [PRC EDITOR LIMITATIONS],
  kind: table,
)
