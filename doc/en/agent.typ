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
    columns: (0.5fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [`-d`, `--directory <path>`], [The path to the project directory],
    [`-p`, `--project <name>`], [The name of the project to use],
    [`-q`, `--query <text>`], [Single query mode (non-interactive)],
    [`--max-tokens <n>`], [Maximum context tokens (default: 128000)],
    [`--temperature <n>`], [LLM temperature 0.0-1.0 (default: 0.2)],
    [`--no-stream`], [Disable streaming output (streaming enabled by default)],
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

== CAPABILITIES
<agent-capabilities>
The agent provides the following capabilities through natural language interaction:

=== Project & Module Management
<agent-cap-project>
Create and manage SoC projects, import Verilog/SystemVerilog modules, configure
bus interfaces, and browse module libraries.

=== Bus Interface Management
<agent-cap-bus>
Import, browse, and manage bus definitions (AXI, APB, Wishbone, etc.) from CSV
bus libraries.

=== Code Generation
<agent-cap-generate>
Generate Verilog RTL code from netlist files (clock trees, reset networks, FSMs,
interconnects) and render Jinja2 templates with CSV, YAML, JSON, SystemRDL, or
RCSV data sources.

=== File Operations
<agent-cap-file>
Read any file on the system. Write and edit files within allowed directories
(project, working, user-added, and temporary directories). List directory contents
with pattern filtering.

=== Shell Execution
<agent-cap-shell>
Execute bash commands with configurable timeout. Manage long-running background
processes (check status, read output, terminate).

=== Path & Directory Management
<agent-cap-path>
Configure allowed directories for file write access. Add, remove, and list
registered paths at runtime.

=== Memory & Task Management
<agent-cap-memory>
Persistent agent memory for notes across sessions. Built-in task tracking with
todo lists to manage complex multi-step workflows.

=== Documentation
<agent-cap-docs>
Query built-in QSoC documentation by topic (commands, bus formats, clock trees,
reset networks, netlist syntax, templates, etc.).

== CONFIGURATION
<agent-config>
The agent requires LLM API configuration. Set the following environment variables
or configure in the project YAML config file (`.qsoc/config.yaml`):

#figure(
  align(center)[#table(
    columns: (0.5fr, 1fr),
    align: (auto, left),
    table.header([Variable], [Description]),
    table.hline(),
    [`QSOC_AI_PROVIDER`], [AI provider name (e.g., openai, deepseek)],
    [`QSOC_API_KEY`], [API key for the AI provider],
    [`QSOC_AI_MODEL`], [Model name to use],
    [`QSOC_API_URL`], [Base URL for API endpoint],
    [`QSOC_AGENT_TEMPERATURE`], [LLM temperature 0.0-1.0 (default: 0.2)],
    [`QSOC_AGENT_MAX_TOKENS`], [Maximum context tokens (default: 128000)],
    [`QSOC_AGENT_MAX_ITERATIONS`], [Maximum agent iterations (default: 100)],
    [`QSOC_AGENT_SYSTEM_PROMPT`], [Custom system prompt override],
  )],
  caption: [AGENT CONFIGURATION],
  kind: table,
)

== SECURITY
<agent-security>
The agent implements a read-unrestricted, write-restricted permission model:

- *File Read Access*: File reading and directory listing can access any path on the system
- *File Write Access*: File writing and editing are restricted to allowed directories only:
  - Project directory
  - Working directory
  - User-added directories (managed at runtime)
  - System temporary directory (`/tmp`)
- *Shell Commands*: Bash commands have configurable timeout with no upper limit.
  Timed-out processes continue running in the background and can be managed separately
- *Output Truncation*: Large command outputs are truncated to prevent memory issues

== CONVERSATION PERSISTENCE
<agent-persistence>
The agent automatically saves and loads conversation history. Session state is stored
in `.qsoc/conversation.json` within the project directory, allowing conversations to
resume across sessions.

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
