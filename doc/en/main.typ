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
    - Comprehensive SoC design and development environment
    - Both GUI and CLI interfaces for flexible workflow
    - Project management and organization
    - Verilog module library management
    - Bus interface handling and management
    - Schematic processing capabilities
    - RTL code generation
    - Easy integration with existing development tools
  ],
  applications: [
    - System-on-Chip (SoC) design
    - Hardware description and verification
    - RTL development and management
    - Bus interface design and implementation
    - Schematic-based design
    - Hardware module library management
    - SoC project organization and documentation
  ],
  description: [
    QSoC is a comprehensive System-on-Chip (SoC) design tool that provides both
    graphical user interface (GUI) and command-line interface (CLI) for creating,
    managing, and generating SoC components. This tool enables efficient SoC design
    by providing features for project management, module library management, bus
    interface handling, schematic processing, and RTL generation.

    The tool's architecture is designed to support efficient SoC development workflows
    by providing integrated features for managing Verilog modules, handling bus
    interfaces, and generating RTL code. The GUI mode offers an intuitive interface
    for interactive design, while the CLI mode enables automation and integration
    with other development tools and scripts.

    By providing a unified platform for SoC development, QSoC significantly reduces
    the complexity of managing different aspects of SoC design, from module
    management to RTL generation. This integrated approach ensures consistency and
    efficiency throughout the development process.
  ],
  document: [
    #include "about.typ"
    #include "overview.typ"
    #include "command.typ"
    #include "agent.typ"
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
    #include "config.typ"
  ],
)
