= GUI BUS EDITOR
<gui-bus-editor>
The Bus Editor manages project-local `.soc_bus` libraries with a table workflow.
Open it from `Tools > Bus Editor` or by double-clicking a `.soc_bus` file in the
project tree.

== LIBRARY PANE
<gui-bus-editor-library-pane>
The left pane lists project bus libraries and their buses. Each library row shows
enabled state, project-relative path, bus count, and load status. Empty pending
libraries stay in memory until a bus is saved. Duplicate bus names across loaded
libraries are rejected because the manager indexes definitions by bus name.

== SIGNAL TABLE
<gui-bus-editor-signal-table>
The center table stores one `(signal, mode)` row per bus signal mode. Columns map
to `Signal`, `Mode`, `Direction`, `Width`, `Qualifier`, and `Description`.
`Mode` is editable and is not limited to `master` and `slave`. `Width` is scalar
text, so symbolic legacy widths can round-trip as warnings.

Rows can be added, duplicated, deleted, searched, saved, and reverted. The YAML
preview shows the exact definition that will be written.

== CSV IMPORT
<gui-bus-editor-csv-import>
CSV import parses rows without saving first, shows a preview, then applies one of
three merge modes:

- Replace bus
- Append rows
- Merge by signal and mode

CSV columns are mapped to signal, mode, direction, width, qualifier, and
description. Descriptions are preserved through preview and save.

== VALIDATION AND REFERENCES
<gui-bus-editor-validation>
Save validates duplicate `(signal, mode)` rows, required signal and mode values,
direction values, width warnings, preserved attribute conflicts, and module
references. Errors block save. Warnings are shown but do not block save.

The usage tab lists module library, module, interface, bus, mode, mapping count,
and compact problem state. Problem rows select the affected signal table row when
the issue belongs to a row.

== SAFE RENAMES
<gui-bus-editor-safe-renames>
Renaming a bus, signal, or mode scans module interfaces first. When references
exist, the editor shows the affected interfaces and can update the module YAML
after confirmation. Delete Bus is blocked while module interfaces still reference
the selected bus. Delete Library only removes empty libraries.
