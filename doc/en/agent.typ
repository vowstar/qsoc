= AGENT MODE
<agent-overview>
QSoC provides an interactive AI agent for SoC design automation. The agent uses
LLM tool calling to execute multi-step workflows through natural language.

== AGENT COMMAND
<agent-command>

#figure(
  align(center)[#table(
    columns: (0.5fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [`-d`, `--directory <path>`], [Project directory path],
    [`-p`, `--project <name>`], [Project name],
    [`-q`, `--query <text>`], [Single query mode (non-interactive)],
    [`--max-tokens <n>`], [Maximum context tokens (default: 128000)],
    [`--temperature <n>`], [LLM temperature 0.0--1.0 (default: 0.2)],
    [`--no-stream`], [Disable streaming output],
    [`--effort <level>`], [Reasoning effort: low, medium, high],
    [`--model-reasoning <model>`], [Model to use when effort is set],
    [`--resume [id]`],
    [Resume a previous session; pick from list if id omitted],
    [`--continue`], [Continue the most recent session for this project],
  )],
  caption: [AGENT COMMAND OPTIONS],
  kind: table,
)

=== Interactive Mode
<agent-interactive>

```bash
qsoc agent
qsoc agent -d /path/to/project -p myproject
qsoc agent --effort high --model-reasoning deepseek-reasoner
qsoc agent --continue
qsoc agent --resume abc123
```

=== Single Query Mode
<agent-single-query>

```bash
qsoc agent -q "List all modules in the project"
qsoc agent -q "Import cpu.v and add AXI bus interface"
```

== INTERACTIVE COMMANDS
<agent-commands>
The following commands are available during an interactive session:

#figure(
  align(center)[#table(
    columns: (0.35fr, 1fr),
    align: (auto, left),
    table.header([Command], [Description]),
    table.hline(),
    [`exit`, `/exit`], [Exit the agent],
    [`/branch [name]`], [Fork the current session into a new one],
    [`/clear`], [Clear conversation history],
    [`/compact`], [Compact context and report tokens saved],
    [`/context`], [Show token usage breakdown and suggestions],
    [`/cost`], [Show session token totals and cost (if rates configured)],
    [`/cwd [path]`], [Show or change the agent working directory],
    [`/diff`], [Review file edits turn-by-turn],
    [`/effort [level]`], [Show or set effort (off/low/medium/high)],
    [`/model [id]`], [Show or switch the active model],
    [`/project <path>`],
    [Switch project root (reloads config, starts a new session)],
    [`/rename <title>`], [Set session title for the resume picker],
    [`/status`], [Show model, session, and endpoint info],
    [`/help`], [Show help message],
    [`!<command>`], [Execute a shell command directly],
  )],
  caption: [INTERACTIVE COMMANDS],
  kind: table,
)

== KEYBOARD AND INPUT
<agent-keyboard>
Editing and navigation in the prompt:

- *Left/Right*, *Ctrl+A/E* -- Move cursor / jump to start or end of line
- *Up/Down* -- Browse prompt history
- *Ctrl+K*, *Ctrl+U*, *Ctrl+W* -- Delete to end of line / start of line / previous word
- *Backspace* -- Delete character before cursor (CJK/emoji aware)
- *Ctrl+\_* -- Undo the last edit
- *`\` + Enter* -- Continue on next line; multi-line paste is preserved as one input

External editor and search:

- *Ctrl+X Ctrl+E* or *Ctrl+G* -- Edit current input in `$EDITOR`
- *Ctrl+R* -- Reverse-i-search through prompt history

View and selection:

- *Ctrl+T* -- Toggle TODO list visibility
- *Ctrl+L* -- Force a full screen repaint
- *Mouse drag* -- Select and auto-copy to clipboard (OSC 52)
- *Shift + drag* -- Native terminal selection (fallback)

Completion and interrupt:

- *`@<name>`* -- Fuzzy-complete a project file path
- *ESC* -- Abort the current operation

While the agent is executing, you can keep typing. Press *Enter* to submit; the
agent consumes queued requests at the start of the next iteration. Pressing
*ESC* aborts the current LLM stream, tool execution, and any pending tool
calls; conversation history is preserved.

== DECISION FLOW
<agent-decision-flow>
The agent follows a four-tier decision flow for every request:

+ *Tier 1 -- Skills*: Search for matching user-defined skills via `skill_find`.
  If a skill matches, read and follow its instructions.
+ *Tier 2 -- SoC Infrastructure*: If the request involves clock tree, reset
  network, power sequencing, or FSM generation, the agent queries built-in
  documentation (`query_docs`) for the YAML format, writes a `.soc_net` file,
  and calls `generate_verilog` to produce production-grade RTL. The agent never
  writes clock/reset/power/FSM Verilog by hand.
+ *Tier 3 -- Plan*: For tasks requiring 3+ steps, decompose into a TODO
  checklist before execution.
+ *Tier 4 -- Execute*: Use file, shell, generation, or other tools directly.

== SoC INFRASTRUCTURE
<agent-soc-infrastructure>
The `generate_verilog` tool produces production RTL from `.soc_net` YAML files
with four primitive generators:

- *Clock* -- ICG gating, static/dynamic/auto dividers, glitch-free MUX, STA
  guide buffers, test enable bypass
- *Reset* -- ARSR synchronizers (async assert / sync release), multi-source
  matrices, reset reason recording
- *Power* -- 8-state FSM per domain
  (OFF→WAIT\_DEP→TURN\_ON→CLK\_ON→ON→RST\_ASSERT→TURN\_OFF), hard/soft
  dependencies, fault recovery
- *FSM* -- Table-mode (Moore/Mealy) and microcode-mode, binary/onehot/gray
  encoding

The agent detects SoC infrastructure requests by keyword (clock, reset, power,
FSM, etc.) and routes them through Tier 2 automatically.

== CAPABILITIES
<agent-capabilities>
The agent provides the following tools through natural language:

- *Project* -- `project_list`, `project_show`, `project_create`
- *Module* -- `module_list`, `module_show`, `module_import`, `module_bus_add`
- *Bus* -- `bus_list`, `bus_show`, `bus_import` (AXI, APB, Wishbone, etc.)
- *Generation* -- `generate_verilog` (RTL from `.soc_net`), `generate_template`
  (Jinja2 rendering)
- *Files* -- `read_file`, `list_files`, `write_file`, `edit_file`; `path_context`
  reports and adjusts allowed write directories
- *Shell* -- `bash` with timeout, `bash_manage` for background processes
- *Documentation* -- `query_docs` by topic (about, commands, config, bus, clock,
  fsm, logic, netlist, power, reset, template, validation, overview, ...)
- *Memory* -- `memory_read`, `memory_write` for persistent notes across sessions
- *Todo* -- `todo_list`, `todo_add`, `todo_update`, `todo_delete` for multi-step
  workflows
- *Skills* -- `skill_find`, `skill_create` for user-defined prompt templates
  resolved across four layers (`$QSOC_HOME/skills`, `<project>/.qsoc/skills`,
  `~/.config/qsoc/skills`, and a platform-native system skills dir);
  see @config-files for the full layout
- *LSP* -- `lsp` for language server diagnostics and symbol lookup
- *Web* -- `web_fetch` for URL content, `web_search` via SearXNG (when configured)

== REASONING EFFORT
<agent-effort>
The `--effort` option enables extended reasoning for complex tasks. When set,
a `reasoning_effort` parameter is sent to the LLM API.

If `llm.model_reasoning` is configured, the agent automatically switches to that
model when effort is set, and switches back when effort is off. This allows
pairing a fast model for normal use with a reasoning model for hard problems.

The receiving side always parses `reasoning_content` and `reasoning_details`
fields from the SSE stream, regardless of the `--effort` setting. Reasoning
output is displayed in dim text.

#figure(
  align(center)[#table(
    columns: (0.3fr, 0.3fr, 1fr),
    align: (auto, auto, left),
    table.header([`--effort`], [`model_reasoning`], [Behavior]),
    table.hline(),
    [not set], [not set], [Primary model, no reasoning parameter],
    [not set], [set], [Primary model; reasoning model idle],
    [`high`], [not set], [Primary model + `reasoning_effort`],
    [`high`],
    [`deepseek-reasoner`],
    [Switch to reasoning model + `reasoning_effort`],
  )],
  caption: [MODEL SELECTION BEHAVIOR],
  kind: table,
)

== CONTEXT COMPACTION
<agent-context-compaction>
Long conversations are managed by a three-layer compaction system:

+ *Tool Output Pruning* (60% threshold) -- Old tool outputs are replaced with
  `[output pruned]`. Zero LLM calls.
+ *LLM Compaction* (80% threshold) -- Older messages are summarized by the LLM,
  preserving technical details (file paths, decisions, errors).
+ *Auto-Continue* -- After compaction during streaming, the agent automatically
  resumes the current task.

Use `/compact` to trigger compaction manually, and `/context` to inspect the
per-category token breakdown.

== SYSTEM PROMPT SOURCES
<agent-system-prompt>
On every turn, the system prompt is composed from:

- *Modular sections* -- built-in role, decision flow, and tool usage guidance
- *Project instructions* -- `AGENTS.md` and `AGENTS.local.md` in the project
  directory, injected verbatim
- *Memory* -- entries from the auto-memory store (see `memory_read` /
  `memory_write`), capped by `agent.memory_max_chars`
- *Skill listing* -- names and descriptions of installed skills so the agent
  can route to them via `skill_find`

Set `agent.system_prompt` in the config to replace the modular base with a
literal string (useful for testing or custom deployments).

== SECURITY
<agent-security>
The agent uses a read-unrestricted, write-restricted permission model:

- *Read*: Any path on the system
- *Write*: Project directory, working directory, user-added directories, `/tmp`
- *Shell*: Configurable timeout, no upper limit

== SESSIONS
<agent-persistence>
Each session is persisted as `.qsoc/sessions/<id>.jsonl` under the project
directory, one JSON event per line (messages plus metadata). This enables:

- `qsoc agent --continue` -- resume the most recent session
- `qsoc agent --resume [id]` -- pick a session from a list, or load one by id /
  unique prefix
- `/branch [name]` -- fork the current session into a new id, preserving the
  original
- `/rename <title>` -- set a human-readable title shown by the resume picker

== USAGE EXAMPLES
<agent-examples>

```
qsoc> Create a new project named "soc_design" in the current directory
qsoc> Import all Verilog files from ./rtl directory
qsoc> Add AXI4 slave interface to the cpu module
qsoc> Generate Verilog from netlist.yaml with output name "top"
```
