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
    [`/cwd [path]`],
    [Show or change the working directory. Empty opens a picker. In remote
     mode, drives the remote cwd clamped to the workspace root.],
    [`/diff`], [Review file edits turn-by-turn],
    [`/effort [level]`], [Show or set effort (off/low/medium/high)],
    [`/local`],
    [Leave SSH remote mode and return to local workspace. The sticky binding
     stays on disk so the next `/ssh <same target>` or next startup can
     auto-reuse it.],
    [`/model [id]`], [Show or switch the active model],
    [`/project <path>`],
    [Switch project root (reloads config, starts a new session)],
    [`/rename <title>`], [Set session title for the resume picker],
    [`/ssh [[user\@]host[:port]]`],
    [Connect to an SSH remote workspace. Empty opens a picker of
     `~/.ssh/config` aliases plus the saved binding. The user defaults
     to the current OS user, the port defaults to 22. After the session
     comes up a two-column directory browser asks for the workspace; the
     choice is remembered in `<project>/.qsoc/remote.yml` and reused on
     later connects.],
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
  `~/.config/qsoc/skills`, and a platform-native system skills dir), plus
  any directory listed in `QSOC_SKILLS_PATH`; see @agent-skills for the
  SKILL.md format and @config-files for the full layout
- *LSP* -- `lsp` for language server diagnostics and symbol lookup
- *Web* -- `web_fetch` for URL content, `web_search` via SearXNG (when configured)

== SKILLS
<agent-skills>
A skill is a `SKILL.md` markdown file with a YAML frontmatter block. Skills
extend the agent without code changes. They are discovered across the four
configuration layers (see @config-files) plus any directory listed in the
`QSOC_SKILLS_PATH` environment variable. Same-name skills in higher layers
shadow lower ones.

=== File Layout
<agent-skill-layout>
Each skill lives in its own directory:

```
<root>/skills/<skill-name>/SKILL.md
```

Where `<root>` is one of: `$QSOC_HOME`, `<project>/.qsoc`,
`~/.config/qsoc`, the platform system root, or any entry in
`QSOC_SKILLS_PATH` (colon-separated on Unix, semicolon on Windows).

=== Frontmatter Fields
<agent-skill-frontmatter>
#figure(
  align(center)[#table(
    columns: (0.4fr, 1fr),
    align: (auto, left),
    table.header([Key], [Meaning]),
    table.hline(),
    [`name`], [Skill identifier (lowercase, digits, hyphens)],
    [`description`], [One-line summary shown in listings],
    [`when-to-use`], [Hint for the model on when to invoke],
    [`argument-hint`], [Shorthand for arguments shown in autocomplete],
    [`user-invocable`], [`true` (default) registers a `/name` slash command],
    [`disable-model-invocation`], [`true` hides the skill from `skill_find` and from the system-prompt listing while keeping `/name` dispatch available],
  )],
  caption: [SKILL.md FRONTMATTER FIELDS],
  kind: table,
)

=== Body and Placeholders
<agent-skill-placeholders>
The skill body is treated as a prompt prepended to the user message when
the skill is dispatched. Three placeholders are substituted before the
body is sent to the LLM:

- `${ARGS}`: text typed after `/name` (empty when no args were passed)
- `${CWD}`: the agent working directory
- `${PROJECT}`: the project directory

When `${ARGS}` is referenced anywhere in the body, the legacy
"Arguments passed: ..." suffix is suppressed so the same value does not
appear twice.

=== Discovery and Diagnostics
<agent-skill-diagnostics>
At REPL startup and after `/project`, the agent rescans every skill root
and prints a one-line warning for any `SKILL.md` whose frontmatter is
missing or unclosed. `/help` lists the user-invocable skills along with
their `argument-hint` so they are discoverable without grepping the
filesystem.

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

== REMOTE WORKSPACE
<agent-remote>
QSoC can drive a workspace on a remote host over SSH without installing
anything on that host. The transport is `libssh2` linked statically against
`mbedTLS 3.6 LTS` (shipped as git submodules); mbedTLS 4.x is not yet
supported by libssh2.

=== Connecting
<agent-remote-connect>

Use `/ssh` from the interactive REPL:

```bash
/ssh user@host
/ssh user@host:2222
/ssh host
```

User and port are optional: if omitted, user falls back to the current
OS user (`USERNAME` on Windows, `USER`/`LOGNAME` on POSIX) and port falls
back to 22. The workspace is never part of the command line; after the
session comes up a two-column directory browser opens starting at the
remote home directory, and the chosen path becomes both the initial cwd
and the sole writable root for remote file tools. The selection is
written to `<project>/.qsoc/remote.yml` and reused on subsequent
`/ssh <same target>` invocations without prompting the picker.

Bare `/ssh` opens a picker listing the saved binding plus concrete aliases
parsed from `~/.ssh/config`. `/local` returns to local mode but keeps the
binding on disk so the next session can auto-connect.

=== What Runs Where
<agent-remote-where>

Workspace tools operate on the remote host (SFTP + SSH exec):

- `read_file`, `write_file`, `list_files`, `edit_file`
- `bash` (with optional `background=true` for detached jobs)
- `bash_manage` (status/output/terminate/kill for backgrounded jobs)
- `path_context` (remote root, cwd, writable dirs)

Control-plane tools stay on the local machine regardless of mode:

- `query_docs`
- `web_fetch`, `web_search`

The following tools are intentionally unavailable in remote mode because
they depend on local QSoC managers:

- `project_*`, `module_*`, `bus_*`, `generate_*`, `lsp`

=== Authentication and Host Keys
<agent-remote-auth>

QSoC parses a deliberately small subset of `~/.ssh/config`: `Host`,
`HostName`, `User`, `Port`, `IdentityFile`, `IdentitiesOnly`,
`UserKnownHostsFile`, `StrictHostKeyChecking`, `AddKeysToAgent`, and bounded
`Include`. `Match`, `ProxyCommand`, port forwarding, certificates, and
complex token expansion are not supported.

Authentication order:

+ `ssh-agent` if available (unless `IdentitiesOnly=yes`)
+ Each `IdentityFile` from the config in order
+ If none configured, QSoC enumerates `~/.ssh/id_*` by filename and lets
  libssh2 try each key in turn

Host key verification uses `~/.ssh/known_hosts` by default with strict
checking enabled. `accept-new` is honored for first-contact hosts.

=== Private Key Safety
<agent-remote-keys>

QSoC code never opens, reads, copies, logs, or displays SSH private key
contents. IdentityFile paths are handed to libssh2 and only libssh2 (or
ssh-agent) reads the key material internally during authentication. Logs
refer to "configured IdentityFile" rather than literal paths where
practical, and passphrases are kept in memory only for the duration needed
to authenticate.

=== Background Jobs
<agent-remote-bg>

`bash` with `background=true` launches the command detached under
`<workspace>/.qsoc-agent/jobs/<id>/` and returns `job_id` immediately. The
wrapper writes `pid`, `output.log`, and `exit_code` files; `bash_manage`
reads those state files over SSH exec to report status, tail output, or
send `SIGTERM`/`SIGKILL`. Jobs survive SSH channel closes; QSoC does not
auto-kill them when the agent exits.

== SECURITY
<agent-security>
The agent uses a read-unrestricted, write-restricted permission model:

- *Read*: Any path on the system
- *Write*: Project directory, working directory, user-added directories,
  and the OS temp directory (`/tmp` on Linux, `/var/folders/...` on macOS,
  `%TEMP%` on Windows; resolved via Qt `QDir::tempPath()`)
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
