#import "datasheet.typ": datasheet

#datasheet(
  metadata: (
    organization: [QSoC],
    logo: "./image/logo.svg",
    website_url: "https://github.com/vowstar/qsoc",
    title: [QSoC],
    product: [QSoC],
    product_url: "https://github.com/vowstar/qsoc",
    revision: [v1.0.2],
    publish_date: [2025-09-15],
  ),
  features: [
    - GUI, CLI, and interactive terminal agent in one tool
    - Project, Verilog module library, and bus interface management
    - Netlist-driven RTL generation with connection validation
    - Clock, reset, power, FSM, and combinational logic generators
    - SystemRDL register templates and stub generation
    - Schematic import with automatic layout
    - LLM agent with sub-agents, skills, hooks, and MCP servers
    - Persistent agent memory with selective recall
    - Remote SSH workspaces and layered configuration
  ],
  applications: [
    - System-on-Chip (SoC) design
    - Hardware description and verification
    - RTL development and management
    - Bus interface design and implementation
    - Clock tree, reset tree, and power sequence generation
    - Register map and firmware header generation
    - AI-assisted RTL authoring and code review
    - SoC project organization and documentation
  ],
  description: [
    QSoC turns a declarative netlist into synthesizable RTL. Modules are imported
    from Verilog, bus interfaces are matched by definition, and generators emit
    clock trees, reset trees, power sequencers, state machines, and register maps
    that stay consistent with the source description.

    The GUI drives interactive editing; the CLI drives scripted and batch flows.
    Both read the same project files, so a design can move between them at any
    point.

    The terminal agent adds an LLM-driven workflow on top of the same tools: it
    reads the project, runs commands, and edits files under the same
    configuration, extended by MCP servers, sub-agents, and persistent memory.
  ],
  quickref: include "cheatsheet.typ",
  document: [
    #include "about.typ"
    #include "overview.typ"
    #include "command.typ"
    #include "agent.typ"
    #include "gui_bus_editor.typ"
    #include "gui_module_editor.typ"
    #include "format_overview.typ"
    #include "format_netlist.typ"
    #include "format_bus.typ"
    #include "format_logic.typ"
    #include "format_fsm.typ"
    #include "format_reset.typ"
    #include "format_clock.typ"
    #include "format_power.typ"
    #include "format_template.typ"
    #include "format_validation.typ"
    #include "tui_image_preview.typ"
    #include "config.typ"
  ],
)
