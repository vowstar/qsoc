= About This Guide
<about>
QSoC is a comprehensive System-on-Chip (SoC) design tool that provides both
graphical user interface (GUI) and command-line interface (CLI) for creating,
managing, and generating SoC components. This tool enables efficient SoC design
by providing features for project management, module library management, bus
interface handling, schematic processing, and RTL generation.

The tool is designed to streamline the SoC development process by offering
integrated capabilities for importing and managing Verilog modules, handling
bus interfaces, and generating RTL code. It supports both interactive GUI-based
design and automated command-line operations, making it suitable for various
development workflows and automation scenarios.

== Disclaimer
<disclaimer>
Information in this document, including URL references, is subject to change
without notice. *This document is provided as is with no warranties whatsoever,
including any warranty of merchantability, non-infringement, fitness for any
particular purpose, or any warranty otherwise arising out of any proposal,
specification or sample.*

All liability, including liability for infringement of any proprietary rights,
relating to use of information in this document is disclaimed. No licenses
express or implied, by estoppel or otherwise, to any intellectual property
rights are granted herein.

All trade names, trademarks, and registered trademarks mentioned in this
document are property of their respective owners, and are hereby acknowledged.

== Revision History

#figure(
  align(center)[#table(
    columns: (0.25fr, 0.25fr, 0.25fr, 1fr),
    align: (auto, auto, auto, auto),
    table.header([Revision], [Date], [Author], [Description]),
    table.hline(),
    [1.0.0], [2025-04-08], [Huang Rui], [Initial release of QSoC Manual],
    [1.0.1],
    [2025-06-29],
    [Huang Rui],
    [Updated QSoC Manual with netlist improvements],
    [1.0.2],
    [2025-08-06],
    [Huang Rui],
    [Added SystemRDL support for template generation],
    [1.0.3],
    [2026-04-19],
    [Huang Rui],
    [Generator validation hardening, QSocConsole logging, and slang LSP backend],
    [1.0.4],
    [2026-04-22],
    [Huang Rui],
    [Schematic netlist import with auto-layout and cwd/project REPL commands],
    [1.0.5],
    [2026-04-23],
    [Huang Rui],
    [Layered resource roots via QSocPaths],
    [1.0.6],
    [2026-04-25],
    [Huang Rui],
    [Remote SSH workspace with ProxyJump, Windows port, and two-column path picker],
    [1.0.7],
    [2026-04-27],
    [Huang Rui],
    [MCP server integration, skill placeholders, and tiered proxy resolution],
    [1.0.8],
    [2026-04-28],
    [Huang Rui],
    [Hook system, --ssh and --workspace flags, packaging polish],
    [1.0.9],
    [2026-04-30],
    [Huang Rui],
    [Background-task overlay, /loop scheduler, and rolling compaction summary],
    [1.0.10],
    [2026-04-30],
    [Huang Rui],
    [Sub-agent system with spawn, resume, fork, and isolation worktrees],
  )],
  kind: table,
)

#pagebreak()
