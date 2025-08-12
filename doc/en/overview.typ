= OVERVIEW
<overview>
QSoC is a versatile System-on-Chip (SoC) design tool that provides a comprehensive
solution for SoC development. The tool features both a graphical user interface
(GUI) and a command-line interface (CLI), offering flexibility in how users
interact with the system. At its core, QSoC provides powerful capabilities for
project management, module library handling, bus interface management, schematic
processing, and RTL generation.

The tool's architecture is designed to support efficient SoC development workflows
by providing integrated features for managing Verilog modules, handling bus
interfaces, and generating RTL code. The GUI mode offers an intuitive interface
for interactive design, while the CLI mode enables automation and integration
with other development tools and scripts.

The operation of QSoC is managed through various commands and subcommands, where
users can create and manage projects, import and update modules, handle bus
interfaces, process schematics, and generate RTL code. The tool provides
comprehensive feedback through its status reporting and verbose output options,
enabling efficient monitoring and management of the development process.

By providing a unified platform for SoC development, QSoC significantly reduces
the complexity of managing different aspects of SoC design, from module
management to RTL generation. This integrated approach ensures consistency and
efficiency throughout the development process.

== TERMINOLOGY
<terminology>
The following terms are used in system descriptions.

#figure(
  align(center)[#table(
      columns: (0.25fr, 1fr),
      align: (auto, left),
      table.header([Terminology], [Description]),
      table.hline(),
      [SoC], [System-on-Chip, an integrated circuit that integrates all components of a computer or other electronic system],
      [RTL], [Register Transfer Level, a design abstraction which models a synchronous digital circuit in terms of the flow of digital signals between hardware registers],
      [GUI], [Graphical User Interface, a form of user interface that allows users to interact with electronic devices through graphical icons and visual indicators],
      [CLI], [Command Line Interface, a means of interacting with a computer program where the user issues commands to the program in the form of successive lines of text],
      [Verilog], [A hardware description language used to model electronic systems],
      [Bus], [A communication system that transfers data between components inside a computer or between computers],
      [SystemRDL], [A standard language for describing and specifying the behavior of register and memory structures within semiconductor IP],
      [RCSV], [Register-CSV format, a CSV-based approach for describing register structures following RCSV v0.3 specification],
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

== QSoC
<qsoc>
QSoC is primarily used for SoC design and development.

#quote(block: true)[
  QSoC provides both GUI and CLI interfaces for SoC development. While the GUI
  offers an intuitive interface for interactive design, the CLI enables
  automation and integration with other development tools. Users can choose the
  interface that best suits their workflow and requirements.
]

#pagebreak()
