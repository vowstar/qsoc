= GUI Overview
<gui-overview>
`qsoc gui` opens the main window: a launcher for the four editors, a project
tree, and the project-wide file operations. The editors read and write the same
project files the CLI uses (@project-layout), so a design can move between them
freely.

If the current directory holds exactly one `.soc_pro` file, that project is
opened automatically at startup. With zero or several project files, open one
with File > Open Project.

== Main Window
<gui-overview-layout>
The central panel holds four launchers, each also reachable from the Tools
menu:

#figure(
  align(center)[#table(
    columns: (0.4fr, 0.25fr, 1fr),
    align: (auto, auto, left),
    table.header([Editor], [Shortcut], [Edits]),
    table.hline(),
    [Module Editor], [Ctrl+L], [Module libraries and bus mappings (@gui-module-editor)],
    [Bus Editor], [Ctrl+B], [Bus definition libraries (@gui-bus-editor)],
    [Schematic Editor], [Ctrl+E], [Instance diagrams (@gui-schematic-editor)],
    [Power/Reset/Clock Editor], [Ctrl+P], [Controller diagrams (@gui-prc-editor)],
  )],
  caption: [GUI EDITORS],
  kind: table,
)

The left dock lists the project contents grouped by kind: Bus (`.soc_bus`),
Module (`.soc_mod`), Schematic (`.soc_sch`), and Output (`.soc_net`, Verilog
sources, CSV). The tree is a snapshot, not a watcher: files created outside the
application, including a netlist just exported from an editor, appear after
View > Refresh (F5).

Double-clicking a file opens the editor that owns its extension: `.soc_sch`,
`.soc_mod`, `.soc_bus`, and `.soc_prc`. Other entries, including `.soc_net` and
Verilog sources, are listed but not openable from the tree.

The left toolbar carries the project operations plus Open In File Explorer,
which has no menu equivalent. The status bar keeps the current project path
visible.

== Editor Windows
<gui-overview-editors>
Each editor is a single window, not one window per file. Opening a second file
of the same kind closes the first, with the usual save prompt. Launching an
editor from the Tools menu always starts an untitled document; to open an
existing file, double-click it in the project tree or use File > Open inside
the editor.

Without a valid project the Module and Bus editors open read-only, and the
Schematic and PRC editors open with an empty module library.
