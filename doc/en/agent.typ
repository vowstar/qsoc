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

=== Skills
<agent-cap-skills>
User-defined prompt templates (SKILL.md) that extend agent capabilities without
code changes. Skills are stored in project (`.qsoc/skills/`) or user
(`~/.config/qsoc/skills/`) directories. Use the built-in skill discovery and
creation tools to bootstrap the skill ecosystem.

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

== CONTEXT COMPACTION
<agent-context-compaction>
The agent implements a three-layer context compaction system to manage long
conversations efficiently without losing critical information.

=== Layer 1 -- Tool Output Pruning
<agent-compact-prune>
When token usage reaches 60% of the context window, old tool outputs are replaced
with `[output pruned]`. Recent tool outputs (within the protection window) are
preserved. This is a zero-LLM-call operation that typically saves 50--80% of tokens
from verbose tool results like file reads and command outputs.

=== Layer 2 -- LLM Compaction
<agent-compact-llm>
When token usage reaches 80% of the context window, the agent calls the LLM to
generate a structured summary of older messages. The summary preserves all technical
details (file paths, decisions, error messages) in a compact format. Recent messages
are kept verbatim. If the LLM call fails, a mechanical fallback summary is used.

=== Layer 3 -- Auto-Continue
<agent-compact-continue>
After compaction occurs during an active streaming session, the agent automatically
injects a continuation prompt so the LLM resumes its current task without user
intervention.

#figure(
  align(center)[#table(
    columns: (0.5fr, 0.5fr, 1fr),
    align: (auto, auto, left),
    table.header([Parameter], [Default], [Description]),
    table.hline(),
    [`prune_threshold`], [0.6], [Token ratio to trigger tool output pruning],
    [`compact_threshold`], [0.8], [Token ratio to trigger LLM summary compaction],
    [`compaction_model`], [(empty)], [Model for compaction (empty = use primary model)],
  )],
  caption: [COMPACTION CONFIGURATION],
  kind: table,
)

In interactive mode, the `compact` command triggers compaction manually and reports
the number of tokens saved.

== INTERRUPT HANDLING
<agent-interrupt>
Press *ESC* during agent execution to abort the current operation. The interrupt
cascades through all active subsystems:

- *LLM Streaming*: The HTTP connection is aborted immediately
- *Tool Execution*: Running bash processes are killed, pending tools are skipped
- *Message Format*: Skipped tool calls receive `"Aborted by user"` placeholder
  responses to maintain API message format compliance

After interruption, the agent prints `(interrupted)` and returns to the input
prompt. Conversation history is preserved, so you can continue the session normally.

The ESC monitor uses `termios` raw mode with `QSocketNotifier` on stdin, which
works inside nested `QEventLoop` instances (e.g. during bash tool execution).

=== Input Queuing
<agent-input-queuing>
While the agent is executing (LLM streaming, tool calls), you can type follow-up
requests directly. The input line appears below the status bar:

```
\ Thinking... (1.2k/3.4k 1:54) [21 tools]
> your follow-up request here
```

Press *Enter* to submit the request into the queue. The agent consumes queued
requests at the start of the next iteration, so the LLM sees your new message in
the following round. Multiple requests can be queued.

Keyboard shortcuts during input:
- *Enter* -- Submit the current input to the queue
- *Backspace* -- Delete the last character (CJK and emoji are deleted as a unit)
- *Ctrl-U* -- Clear the entire input line
- *Ctrl-W* -- Delete the last word
- *ESC* -- Clear the input line and interrupt the agent

CJK and multilingual input via IME is fully supported. Raw mode operates at the
PTY/termios layer, which is architecturally below the terminal emulator's IME
composition. Characters arrive as complete UTF-8 sequences.

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
