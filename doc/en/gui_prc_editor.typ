= GUI Power, Reset and Clock Editor
<gui-prc-editor>
The PRC editor draws clock, reset, and power controllers and exports them as
the `clock:`, `reset:`, and `power:` sections of a netlist
(@soc-net-clock-overview, @soc-net-reset-overview, @soc-net-power-overview).
It is an authoring front end only: generation still runs through
`qsoc generate verilog` (@verilog-generation).

Read @gui-prc-limitations before relying on the editor for a real controller.
Several fields shown in its dialogs do not reach the exported netlist in this
release.

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
carries markers such as `[ICG]` or `[DIV/2]`.

== From Diagram to RTL
<gui-prc-workflow>
+ Draw and configure the primitives, then wire them.
+ Save the drawing as `.soc_prc` (Ctrl+S).
+ Tools > Export Netlist (Ctrl+E) writes `<name>.soc_net` into the project
  output directory.
+ Review the exported YAML against @gui-prc-limitations and add whatever the
  editor could not express.
+ Run `qsoc generate verilog <name>.soc_net`.

The exporter writes exactly one controller per family, named `clock_ctrl`,
`reset_ctrl`, and `power_ctrl`, whatever the diagram calls them. Keys come from
the name shown on each box.

== Limitations
<gui-prc-limitations>
#figure(
  align(center)[#table(
    columns: (0.45fr, 1fr),
    align: (auto, left),
    table.header([Area], [Behavior in this release]),
    table.hline(),
    [Link operations], [ICG, divider, inverter and STA settings configured on a
      wire are not written to the `.soc_prc` file and are lost on reload. The
      label markers survive, so a link can look configured when it is not],
    [Link operations in export],
    [The same settings usually export as `null`, because the link is keyed by
      the wire net name and nets are not named automatically here],
    [Controller names], [Only the frame drawing uses them; the export always
      emits the three fixed controller names],
    [Controller test enable], [Not exported, so the generator falls back to
      `1'b0` and the DFT bypass is lost],
    [Power host clock and reset], [Not exported. `host_clock` and `host_reset`
      are required, so an exported power section does not generate until they
      are added by hand],
    [Power dependencies], [`depend:` and `follow:` have no editor fields and are
      never exported. Every exported domain therefore reads as always-on],
    [Reset synchronizers], [Only the asynchronous form is available; `sync` and
      `count` have no fields. The asynchronous settings are written onto every
      link of a target rather than after the combination],
    [Editing], [Cut, copy, paste and select all are not implemented. Delete
      works from the keyboard],
    [Files], [A `.soc_prc` written by an older release loads as an empty canvas
      without an error; saving then overwrites the original],
  )],
  caption: [PRC EDITOR LIMITATIONS],
  kind: table,
)
