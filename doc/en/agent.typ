= AGENT MODE
<agent-overview>
QSoC provides an interactive AI agent mode that enables natural language interaction
for SoC design tasks. The agent leverages LLM (Large Language Model) capabilities
with tool calling to automate complex workflows.

== AGENT COMMAND
<agent-command>
The agent command starts an interactive AI assistant or executes a single query.

#figure(
  align(center)[#table(
    columns: (0.4fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [-d, --directory <path>], [The path to the project directory],
    [-p, --project <name>], [The name of the project to use],
    [-q, --query <text>], [Single query mode (non-interactive)],
    [--max-tokens <n>], [Maximum context tokens (default: 128000)],
    [--temperature <n>], [LLM temperature 0.0-1.0 (default: 0.2)],
  )],
  caption: [AGENT COMMAND OPTIONS],
  kind: table,
)

=== Interactive Mode
<agent-interactive>
Start the agent in interactive mode for continuous conversation:

```bash
qsoc agent
qsoc agent -d /path/to/project
qsoc agent -p myproject
```

In interactive mode, the following commands are available:
- `exit` or `quit` - Exit the agent
- `clear` - Clear conversation history
- `help` - Show help message

=== Single Query Mode
<agent-single-query>
Execute a single query without entering interactive mode:

```bash
qsoc agent -q "List all modules in the project"
qsoc agent -q "Import cpu.v and add AXI bus interface"
```

== AVAILABLE TOOLS
<agent-tools>
The agent has access to the following tools for performing SoC design tasks:

=== Project Tools
<agent-tools-project>
#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto, left),
    table.header([Tool], [Description]),
    table.hline(),
    [project_list], [List all projects in the project directory],
    [project_show], [Show detailed information about a specific project],
    [project_create], [Create a new QSoC project],
  )],
  caption: [PROJECT TOOLS],
  kind: table,
)

=== Module Tools
<agent-tools-module>
#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto, left),
    table.header([Tool], [Description]),
    table.hline(),
    [module_list], [List all modules in the module library],
    [module_show], [Show detailed information about a specific module],
    [module_import], [Import Verilog/SystemVerilog modules from files],
    [module_bus_add], [Add bus interface to a module using LLM matching],
  )],
  caption: [MODULE TOOLS],
  kind: table,
)

=== Bus Tools
<agent-tools-bus>
#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto, left),
    table.header([Tool], [Description]),
    table.hline(),
    [bus_list], [List all bus definitions in the bus library],
    [bus_show], [Show detailed information about a specific bus],
    [bus_import], [Import bus definitions from CSV files],
  )],
  caption: [BUS TOOLS],
  kind: table,
)

=== Generate Tools
<agent-tools-generate>
#figure(
  align(center)[#table(
    columns: (0.35fr, 1fr),
    align: (auto, left),
    table.header([Tool], [Description]),
    table.hline(),
    [generate_verilog], [Generate Verilog RTL code from netlist files],
    [generate_template], [Render Jinja2 templates with data files],
  )],
  caption: [GENERATE TOOLS],
  kind: table,
)

=== File Tools
<agent-tools-file>
#figure(
  align(center)[#table(
    columns: (0.25fr, 1fr),
    align: (auto, left),
    table.header([Tool], [Description]),
    table.hline(),
    [read_file], [Read file contents (restricted to project directory)],
    [list_files], [List files in a directory (restricted to project directory)],
    [write_file], [Write content to a file (restricted to project directory)],
    [edit_file], [Edit file with string replacement (restricted to project directory)],
  )],
  caption: [FILE TOOLS],
  kind: table,
)

=== Shell Tools
<agent-tools-shell>
#figure(
  align(center)[#table(
    columns: (0.15fr, 1fr),
    align: (auto, left),
    table.header([Tool], [Description]),
    table.hline(),
    [bash], [Execute bash commands with timeout protection],
  )],
  caption: [SHELL TOOLS],
  kind: table,
)

=== Documentation Tools
<agent-tools-doc>
#figure(
  align(center)[#table(
    columns: (0.25fr, 1fr),
    align: (auto, left),
    table.header([Tool], [Description]),
    table.hline(),
    [query_docs], [Query QSoC documentation by topic],
  )],
  caption: [DOCUMENTATION TOOLS],
  kind: table,
)

Available documentation topics: `about`, `commands`, `config`, `datasheet`, `bus`,
`clock`, `fsm`, `logic`, `netlist`, `format_overview`, `power`, `reset`, `template`,
`validation`, `overview`.

== CONFIGURATION
<agent-config>
The agent requires LLM API configuration. Set the following environment variables
or configure in the settings:

#figure(
  align(center)[#table(
    columns: (0.4fr, 1fr),
    align: (auto, left),
    table.header([Variable], [Description]),
    table.hline(),
    [OPENAI_API_KEY], [API key for OpenAI-compatible services],
    [OPENAI_API_BASE], [Base URL for API endpoint (optional)],
  )],
  caption: [AGENT CONFIGURATION],
  kind: table,
)

== SECURITY
<agent-security>
The agent implements several security measures:

- *File Access Restriction*: File operations (read, write, edit, list) are
  restricted to the project directory only
- *Command Timeout*: Bash commands have configurable timeout (default: 60s, max: 300s)
- *Output Truncation*: Large command outputs are truncated to prevent memory issues

== USAGE EXAMPLES
<agent-examples>

=== Create and Configure a Project
```
qsoc> Create a new project named "soc_design" in the current directory
qsoc> Import all Verilog files from ./rtl directory
qsoc> Add AXI4 slave interface to the cpu module
```

=== Generate RTL
```
qsoc> Show the netlist format documentation
qsoc> Generate Verilog from netlist.yaml with output name "top"
```

=== Explore the Codebase
```
qsoc> List all modules that match "axi.*"
qsoc> Show details of the dma_controller module
qsoc> Read the configuration file config.yaml
```
