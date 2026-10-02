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
    - Verilog-2001 top levels from a YAML netlist
    - Connection checks: drivers, undriven nets, widths
    - Clock, reset, and power controllers, FSMs, and glue logic
    - PRCM controllers with APB4 or AXI4-Lite control
    - MMIO register block and IOMUX pin multiplexer generators
    - Optional formal (SymbiYosys) and UVM collateral
    - Clock-path cell roles bound to declared technology cells
    - Module libraries from Verilog, bus libraries from CSV
    - Jinja2 templates fed by SystemRDL, RCSV, CSV, YAML, and JSON
    - GUI schematic, module, bus, and controller editors
    - Terminal LLM agent with local or SSH workspaces and saved sessions
  ],
  applications: [
    - SoC top-level integration
    - Clock, reset, and power controller generation
    - Register blocks and pin multiplexers
    - Formal and UVM starting points for generated blocks
    - Prompt-driven project work in a terminal
  ],
  description: [
    QSoC writes SoC integration RTL from YAML descriptions. You import Verilog
    modules into a project library, describe the top level in a netlist, and
    QSoC writes the instances, the wiring, and the controllers that the netlist
    declares.

    Generated RTL is plain Verilog-2001 with file lists for your simulator and
    synthesis flow. QSoC does not simulate, lint, or synthesize it. Check the
    output in your own flow.

    The CLI, the GUI, and the terminal agent work on the same project files.
    The generators need no LLM.
  ],
  quickref: include "quick_reference.typ",
  document: [
    #include "about.typ"
    #include "overview.typ"
    #include "config.typ"
    #include "command.typ"
    #include "format_overview.typ"
    #include "format_netlist.typ"
    #include "format_bus.typ"
    #include "format_mmio.typ"
    #include "format_iomux.typ"
    #include "format_logic.typ"
    #include "format_fsm.typ"
    #include "format_reset.typ"
    #include "format_clock.typ"
    #include "format_power.typ"
    #include "format_prcm.typ"
    #include "format_template.typ"
    #include "format_validation.typ"
    #include "agent.typ"
    #include "tui_image_preview.typ"
    #include "gui_overview.typ"
    #include "gui_bus_editor.typ"
    #include "gui_module_editor.typ"
    #include "gui_schematic_editor.typ"
    #include "gui_prc_editor.typ"
  ],
)
