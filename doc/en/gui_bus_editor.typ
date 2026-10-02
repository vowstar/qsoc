= GUI Bus Editor
<gui-bus-editor>
The Bus Editor manages project-local `.soc_bus` libraries with a table workflow.
Open it from `Tools > Bus Editor` or by double-clicking a `.soc_bus` file in the
project tree.

== Library Pane
<gui-bus-editor-library-pane>
The left pane lists project bus libraries and their buses. Each library row shows
enabled state, project-relative path, bus count, and load status. Empty pending
libraries stay in memory until a bus is saved. Duplicate bus names across loaded
libraries are rejected.

== Signal Table
<gui-bus-editor-signal-table>
The center table stores one `(signal, mode)` row per bus signal mode. Columns map
to `Signal`, `Mode`, `Direction`, `Width`, `Qualifier`, and `Description`.
`Mode` is editable and is not limited to `master` and `slave`. `Width` is scalar
text, so symbolic legacy widths can round-trip as warnings.

Rows can be added, duplicated, deleted, searched, saved, and reverted. The YAML
preview shows the exact definition that will be written.

== CSV Import
<gui-bus-editor-csv-import>
CSV import parses rows without saving first, shows a preview, then applies one of
three merge modes: replace the bus, append rows, or merge by signal and mode.
CSV columns map to the table columns, descriptions included.

== Validation and References
<gui-bus-editor-validation>
Save validates duplicate `(signal, mode)` rows, required signal and mode values,
direction values, width warnings, preserved attribute conflicts, and module
references. Errors block save, warnings do not.

The usage tab lists module library, module, interface, bus, mode, mapping count,
and compact problem state. Problem rows select the affected signal table row when
the issue belongs to a row.

== Undo
<gui-bus-editor-undo>
CSV import and each row operation are one undo step.

== Safe Renames
<gui-bus-editor-safe-renames>
Renaming a bus, signal, or mode lists the module interfaces that reference it
and, after confirmation, updates their module YAML. Delete Bus is blocked while
module interfaces reference the bus. Delete Library only removes empty libraries.
