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
    [`--thinking <level>`], [Reasoning effort: low, medium, high],
    [`--model-reasoning <model>`], [Model to use when thinking is enabled],
  )],
  caption: [AGENT COMMAND OPTIONS],
  kind: table,
)

=== Interactive Mode
<agent-interactive>

```bash
qsoc agent
qsoc agent -d /path/to/project -p myproject
qsoc agent --thinking high --model-reasoning deepseek-reasoner
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
    [`/clear`], [Clear conversation history],
    [`/compact`], [Compact conversation context and report tokens saved],
    [`/thinking [level]`], [Show or set thinking level (off/low/medium/high)],
    [`/help`], [Show help message],
  )],
  caption: [INTERACTIVE COMMANDS],
  kind: table,
)

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

- *Project & Module* -- Create projects, import Verilog/SystemVerilog, configure bus interfaces
- *Bus Interfaces* -- Import and manage bus definitions (AXI, APB, Wishbone, etc.)
- *Code Generation* -- Generate Verilog RTL from `.soc_net` netlist files, render Jinja2 templates
- *File Operations* -- Read any file; write/edit within allowed directories
- *Shell Execution* -- Run bash commands with timeout; manage background processes
- *Path Management* -- Configure allowed directories for file write access
- *Memory & Tasks* -- Persistent notes across sessions; todo list for multi-step workflows
- *Documentation* -- Query built-in QSoC docs by topic (15 topics including clock, reset, power, fsm)
- *Skills* -- User-defined prompt templates (`SKILL.md`) in `.qsoc/skills/` or `~/.config/qsoc/skills/`
- *Web Access* -- Search the web via SearXNG (`web_search`) and fetch URL content (`web_fetch`)

== THINKING / REASONING
<agent-thinking>
The `--thinking` option enables extended reasoning for complex tasks. When set,
a `reasoning_effort` parameter is sent to the LLM API.

If `llm.model_reasoning` is configured, the agent automatically switches to that
model when thinking is enabled, and switches back when thinking is off. This
allows pairing a fast model for normal use with a reasoning model for hard
problems.

The receiving side always parses `reasoning_content` and `reasoning_details`
fields from the SSE stream, regardless of the `--thinking` setting. Reasoning
output is displayed in dim text.

#figure(
  align(center)[#table(
    columns: (0.3fr, 0.3fr, 1fr),
    align: (auto, auto, left),
    table.header([`--thinking`], [`model_reasoning`], [Behavior]),
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

Use `/compact` to trigger compaction manually.

== INPUT QUEUING
<agent-input-queuing>
While the agent is executing, you can type follow-up requests. The input line
appears below the status bar. Press *Enter* to submit; the agent consumes queued
requests at the start of the next iteration.

Keyboard shortcuts:
- *Enter* -- Submit input to queue
- *Backspace* -- Delete last character (CJK/emoji aware)
- *Ctrl-U* -- Clear line
- *Ctrl-W* -- Delete last word
- *ESC* -- Clear line and interrupt the agent

== INTERRUPT HANDLING
<agent-interrupt>
Press *ESC* to abort the current operation. The interrupt cascades through LLM
streaming, tool execution, and pending tool calls. Conversation history is
preserved.

== SECURITY
<agent-security>
The agent uses a read-unrestricted, write-restricted permission model:

- *Read*: Any path on the system
- *Write*: Project directory, working directory, user-added directories, `/tmp`
- *Shell*: Configurable timeout, no upper limit

== CONVERSATION PERSISTENCE
<agent-persistence>
Session state is saved to `.qsoc/conversation.json` in the project directory,
allowing conversations to resume across sessions.

== USAGE EXAMPLES
<agent-examples>

```
qsoc> Create a new project named "soc_design" in the current directory
qsoc> Import all Verilog files from ./rtl directory
qsoc> Add AXI4 slave interface to the cpu module
qsoc> Generate Verilog from netlist.yaml with output name "top"
```
