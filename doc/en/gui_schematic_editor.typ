= GUI Schematic Editor
<gui-schematic-editor>
The schematic editor assembles module instances into a diagram and exports the
result as a netlist (@soc-net-format). Two workflows share the same canvas:
place and wire modules by hand, or import an existing netlist and let the
editor lay it out.

Import and Auto Arrange each count as one step of undo: the editor records the
whole document before and after, so Ctrl+Z brings back exactly what was on the
canvas beforehand and Ctrl+Y puts the result back.

== Window Anatomy
<gui-schematic-anatomy>
The Module List dock holds the project's module libraries, grouped by library
name; modules with no library appear under `Unknown`. The Command History dock
shows the undo stack. Three toolbars carry file operations, the grid toggle,
and the two canvas modes.

The title bar shows the file name, prefixed with `*` while the document is
dirty. The status bar keeps the schematic path visible.

== Placing and Wiring
<gui-schematic-placing>
Click a library entry to drop it at the center of the view, or drag it onto the
canvas. Both paths are undoable. Instances are named `u_<module>_<n>`
automatically, and a name is only reassigned when it collides or still equals
the module name, so names loaded from a file are preserved. Double-click an
instance to rename it.

A symbol places inputs and bus interfaces on the left, outputs and inouts on
the right. Ports covered by a bus mapping are hidden unless the module marks
them `visible: true`. Symbol width and height follow the port count and label
widths.

Press *W* for wire mode and *Esc* to return to selection. Connected wires form
nets, and each new net is named `<instance>_<port>` after the port at its
starting point, with a numeric suffix on collision. A net that already carries
a name is never renamed automatically. Double-click a wire to rename its net.
Nets touching a bus port are drawn with a thicker blue underlay.

Zoom with Ctrl and the wheel or Ctrl+`+`/`-`/`0`, pan with the middle mouse
button, and press *F* to fit the whole drawing. The grid is fixed at 20 units
and every item snaps to it; View > Show Grid only controls whether it is drawn.

Delete removes the selected items through the undo stack. Cut, copy and paste
are not available.

Undo covers both kinds of change, but they are kept apart: a bulk edit clears
the item-level history, because item steps from the previous document no
longer apply to the new one. Undoing works back through the item steps first
and then through the bulk edits, which is the order they happened in.

== Importing a Netlist
<gui-schematic-import>
File > Import Netlist (Ctrl+I) accepts one or more `.soc_net` files and
defaults to the project output directory. Several files are merged with the
same rules as `generate verilog --merge` (@netlist-merge-semantics): maps
union, later files win on conflicting scalars.

The import reads the `instance:` section only, taking connectivity from each
instance's `link:` entries. A netlist that expresses its connectivity in the
top-level `net:` section imports as instances with no wires. Sections such as
`comb:`, `seq:`, `fsm:`, `clock:`, `reset:`, and `power:` are ignored, and the
editor never writes them back, so a netlist that round-trips through the editor
loses them.

Modules already in the project library are placed with their real symbols.
A module that is missing from the library gets a placeholder built from the
port names in the netlist, with directions guessed from the names. Since
direction drives the layout, a partially loaded library produces a plausible
but incorrect arrangement.

If the canvas already holds anything, the import asks whether to replace it and
then offers to save the current document before clearing it.

== How Auto Layout Works
<gui-schematic-layout>
The layout runs in stages, and knowing them explains most of what a drawing
looks like:

+ *Broadcast filter.* A scalar net with at least `max(8, sqrt(instances))`
  endpoints is treated as a distribution net and removed from the dependency
  graph, so clock and reset do not collapse the design into one column. Bus
  nets are never filtered.
+ *Columns.* Instances are layered by longest path from driver to consumer.
  Feedback loops are broken silently: one member is placed after its settled
  drivers. Instances with no connections stay in column 0.
+ *Order within a column.* Alphabetical to start, then three barycenter sweeps
  in each direction to reduce crossings.
+ *Wrapping.* A column deeper than 12 rows is split into sub-columns.
+ *Spacing.* The gap between columns is the stub length plus the widest
  outgoing label on the left plus the widest incoming label on the right, so
  long net names widen the drawing.
+ *Port placement.* Each module may be mirrored if that lowers the number of
  ports facing the wrong way, then ports are ordered by the average position of
  their remote endpoints and packed onto grid rows.
+ *Routing.* Two-endpoint nets between adjacent columns are drawn as real
  wires, three or four endpoint nets get a trunk with branches, and anything
  else, including buses and filtered nets, is drawn as short labelled stubs
  connected by name. A net that would cross more than three others is demoted
  to stubs as well.

Place > Auto Arrange (Ctrl+Shift+A) re-runs the whole import for the files
imported in this session. It asks for confirmation first, and one Ctrl+Z
restores the arrangement it replaced. The remembered file list is not saved,
so the action is unavailable after reopening a file.

== Exporting a Netlist
<gui-schematic-export>
Tools > Export Netlist (Ctrl+Shift+E) writes `<schematic>.soc_net` into the
project output directory. The output contains an `instance:` section only, with
`port.<name>.link` and `bus.<name>.link` entries, which is exactly what
`qsoc generate verilog` consumes (@verilog-generation).

What export cannot produce: top-level ports, since the editor has no
representation for `uplink:`; the geometry of the drawing, so re-importing an
exported netlist re-runs the layout from scratch; and bit selects, ties, or
inversions. Nets and ports with no name are dropped silently, so a wire whose
auto-naming failed is visible on the canvas but absent from the generated RTL.

== Files
<gui-schematic-files>
Schematics are saved as `.soc_sch` in the project schematic directory. The file
embeds a copy of each placed module's definition, so a schematic still renders
after the library changes, and equally does not follow those changes. A file
written in a different format version is refused with an explicit message and
left untouched, and a file that fails to load leaves the open document alone.

Print renders the whole drawing onto one page with no scaling or pagination.
