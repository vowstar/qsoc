= GUI MODULE EDITOR
<gui-module-editor>
The Module Editor manages project-local `.soc_mod` libraries with a table-first
workflow. Open it from `Tools > Module Editor` or by double-clicking a
`.soc_mod` file in the project tree.

== LIBRARY PANE
<gui-module-editor-library-pane>
The left pane lists module libraries and modules. Duplicate module names across
loaded libraries are allowed, but they are shown as overlay warnings because
generation resolves the active module from library load order.

The toolbar can create empty module libraries, create unsaved modules in a
selected library, duplicate or rename the selected module, delete unused modules,
import Verilog modules, and delete empty libraries. Deleting a module is blocked
when project netlists or schematic copies still name it.

== MODULE TABLES
<gui-module-editor-tables>
Ports and parameters are edited as structured rows. Port `Visible` controls
whether a port remains visible when it is also mapped into a bus interface.
The row actions operate on the focused table; deleting a port warns when bus
mappings still reference it.

The Bus Interfaces table owns `module.bus.<interface>` entries. It edits the
interface name, selected bus, mode, mapping count, empty mapping count, and row
status. Rename and delete actions show affected netlist and schematic usages
before changing the library definition. Bus protocol definitions remain owned by
the Bus Editor.

== MAPPING TABLE
<gui-module-editor-mapping>
The Mapping table is generated from the selected bus definition and mode. It
shows bus signal, expected direction, expected width, expected qualifier, module
port, actual direction, actual type, and problem state.

Changing bus or mode rebuilds the generated rows while preserving stale mapping
keys so the user can inspect and remove them deliberately. Empty module-port
targets are warnings. Unknown bus signals or module ports are errors.

Mapping tools can auto-match by plain signal name or by interface prefix, clear
stale bus-signal rows, create missing module ports for unresolved valid bus
signals, and filter the mapping table to problem or empty rows.

== BUS EDITOR HANDOFF
<gui-module-editor-bus-editor-handoff>
Use Open Bus Editor to edit the selected bus definition. The Module Editor only
edits module-to-bus interfaces and mapping values, then saves the affected module
library explicitly.

== USAGE TRACKING
<gui-module-editor-usage-tracking>
The Usages tab lists project netlist references and placed schematic copies that
name the selected module. The scan reports source type, project-relative file,
instance name, module name, port count, bus interface count, and status.

Placed schematic items remain copies. The usage list is informational; library
edits do not rewrite schematic files automatically.

== PREVIEW AND SAVE
<gui-module-editor-preview-save>
The preview pane renders the module with the schematic module item path. It is
not the canonical editor; the tables are the source of truth. Saving validates
the module definition, writes only the selected module library, and does not
silently update already placed schematic instances.
