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
    [`--workspace <path>`],
    [Working directory for tool execution. Local absolute path by default;
     paired with `--ssh` it names the remote workspace],
    [`--ssh <target>`],
    [Connect to a remote workspace before the first prompt. Accepts
     `[user@]host[:port]` or a `~/.ssh/config` alias. Requires `--workspace`],
  )],
  caption: [AGENT COMMAND OPTIONS],
  kind: table,
)

=== Interactive Mode
<agent-interactive>

```bash
qsoc agent
qsoc agent -d /path/to/project -p myproject
qsoc agent --effort high --model-reasoning deepseek-v4-pro
qsoc agent --continue
qsoc agent --resume abc123
```

=== Single Query Mode
<agent-single-query>

```bash
qsoc agent -q "List all modules in the project"
qsoc agent -q "Import cpu.v and add AXI bus interface"
```

=== Workspace Override
<agent-workspace-flag>

`--workspace` switches the directory tools operate in without changing
the project metadata directory selected by `-d`. Useful when the project
is checked out in one place but tool runs (shell, file read/write, path
context) should target a build directory or scratch space:

```bash
qsoc agent -d ~/work/proj --workspace ~/work/proj/build -q "list build outputs"
qsoc agent --workspace /tmp/scratch -q "summarize files here"
```

The hook payload's `cwd` field reflects the new working directory so
hook scripts see the same paths the tools do.

=== Remote Workspace via SSH
<agent-ssh-flag>

`--ssh` opens an SSH session and mounts a remote directory as the
workspace before the first prompt, so single-query mode and scripted
runs can target a remote host without entering the REPL. `--workspace`
must accompany it and name an absolute remote path:

```bash
qsoc agent --ssh user@host --workspace /home/user/proj -q "show host disk usage"
qsoc agent --ssh myalias --workspace /home/me/proj          # picker skipped, REPL starts remote
```

The workspace is created on demand via SFTP `mkdir -p`. Tool calls (shell,
file, path) execute remotely; hooks still run on the local host but
their JSON payload includes a `remote` section so scripts can branch on
it. `/local` inside the REPL returns to the local workspace; the sticky
binding (`<project>/.qsoc/remote.yml`) is *not* written for one-shot
`--ssh` runs.

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
    [`/agents`],
    [List sub-agent definitions by scope (builtin, user, project) and any
     parse errors. See @agent-subagents.],
    [`/agents-history`],
    [Show prior backgrounded sub-agent runs with status and transcript tail.],
    [`/loop [cron] <prompt>`],
    [Schedule a recurring prompt. Subforms: `/loop list`, `/loop stop <id>`,
     `/loop clear`. See @agent-loop.],
    [`!<command>`], [Execute a shell command directly],
  )],
  caption: [INTERACTIVE COMMANDS],
  kind: table,
)

== KEYBOARD AND INPUT
<agent-keyboard>
Editing and navigation in the prompt:

- *Left/Right*, *Ctrl+A/E*: Move cursor / jump to start or end of line
- *Up/Down*: Browse prompt history
- *Ctrl+K*, *Ctrl+U*, *Ctrl+W*: Delete to end of line / start of line / previous word
- *Backspace*: Delete character before cursor (CJK/emoji aware)
- *Ctrl+\_*: Undo the last edit
- *`\` + Enter*: Continue on next line; multi-line paste is preserved as one input

External editor and search:

- *Ctrl+X Ctrl+E* or *Ctrl+G*: Edit current input in `$EDITOR`
- *Ctrl+R*: Reverse-i-search through prompt history

View and selection:

- *Ctrl+T*: Toggle TODO list visibility
- *Ctrl+B*: Open the background-task overlay (see @agent-tasks)
- *Down* on the prompt's first row: park focus on the task pill;
  *Enter* opens the overlay, *Up* returns to the prompt
- *Ctrl+L*: Force a full screen repaint
- *Mouse drag*: Select and auto-copy to clipboard (OSC 52)
- *Shift + drag*: Native terminal selection (fallback)

Completion and interrupt:

- *`@<name>`*: Fuzzy-complete a project file path
- *ESC*: Abort the current operation

While the agent is executing, you can keep typing. Press *Enter* to submit; the
agent consumes queued requests at the start of the next iteration. Pressing
*ESC* aborts the current LLM stream, tool execution, and any pending tool
calls; conversation history is preserved.

== DECISION FLOW
<agent-decision-flow>
The agent follows a four-tier decision flow for every request:

+ *Tier 1: Skills*: Search for matching user-defined skills via `skill_find`.
  If a skill matches, read and follow its instructions.
+ *Tier 2: SoC Infrastructure*: If the request involves clock tree, reset
  network, power sequencing, or FSM generation, the agent queries built-in
  documentation (`query_docs`) for the YAML format, writes a `.soc_net` file,
  and calls `generate_verilog` to produce production-grade RTL. The agent never
  writes clock/reset/power/FSM Verilog by hand.
+ *Tier 3: Plan*: For tasks requiring 3+ steps, decompose into a TODO
  checklist before execution.
+ *Tier 4: Execute*: Use file, shell, generation, or other tools directly.

== SoC INFRASTRUCTURE
<agent-soc-infrastructure>
The `generate_verilog` tool produces production RTL from `.soc_net` YAML files
with four primitive generators:

- *Clock*: ICG gating, static/dynamic/auto dividers, glitch-free MUX, STA
  guide buffers, test enable bypass
- *Reset*: ARSR synchronizers (async assert / sync release), multi-source
  matrices, reset reason recording
- *Power*: 8-state FSM per domain
  (OFF→WAIT\_DEP→TURN\_ON→CLK\_ON→ON→RST\_ASSERT→TURN\_OFF), hard/soft
  dependencies, fault recovery
- *FSM*: Table-mode (Moore/Mealy) and microcode-mode, binary/onehot/gray
  encoding

The agent detects SoC infrastructure requests by keyword (clock, reset, power,
FSM, etc.) and routes them through Tier 2 automatically.

== CAPABILITIES
<agent-capabilities>
The agent provides the following tools through natural language:

- *Project*: `project_list`, `project_show`, `project_create`
- *Module*: `module_list`, `module_show`, `module_import`, `module_bus_add`
- *Bus*: `bus_list`, `bus_show`, `bus_import` (AXI, APB, Wishbone, etc.)
- *Generation*: `generate_verilog` (RTL from `.soc_net`), `generate_template`
  (Jinja2 rendering)
- *Files*: `read_file`, `list_files`, `write_file`, `edit_file`; `path_context`
  reports and adjusts allowed write directories
- *Shell*: `bash` (synchronous, or `background=true` for detached jobs),
  `bash_manage` to inspect, tail, or kill backgrounded jobs
- *Monitors*: `monitor` starts a line-oriented watcher whose output wakes
  the agent; `monitor_stop` terminates a watcher
- *Sub-agents*: `agent` to spawn a child run, `agent_status` to poll a
  backgrounded run, `agent_resume` to pick up a prior run from disk,
  `send_message` to post live input to a streaming child; see
  @agent-subagents
- *Documentation*: `query_docs` by topic (about, commands, config, bus, clock,
  fsm, logic, netlist, power, reset, template, validation, overview, ...)
- *Memory*: `memory_read`, `memory_write` for persistent notes across sessions
- *Todo*: `todo_list`, `todo_add`, `todo_update`, `todo_delete` for multi-step
  workflows
- *Skills*: `skill_find`, `skill_create` for user-defined prompt templates
  resolved across four layers (`$QSOC_HOME/skills`, `<project>/.qsoc/skills`,
  `~/.config/qsoc/skills`, and a platform-native system skills dir), plus
  any directory listed in `QSOC_SKILLS_PATH`; see @agent-skills for the
  SKILL.md format and @config-files for the full layout
- *LSP*: `lsp` for language server diagnostics and symbol lookup
- *Web*: `web_fetch` for URL content, `web_search` via SearXNG (when configured)

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
    [`deepseek-v4-pro`],
    [Switch to reasoning model + `reasoning_effort`],
  )],
  caption: [MODEL SELECTION BEHAVIOR],
  kind: table,
)

== CONTEXT COMPACTION
<agent-context-compaction>
Long conversations are managed by a three-layer compaction system:

+ *Tool Output Pruning* (60% threshold): Old tool outputs are replaced with
  `[output pruned]`. Zero LLM calls.
+ *LLM Compaction* (80% threshold): Older messages are summarized by the LLM,
  preserving technical details (file paths, decisions, errors).
+ *Auto-Continue*: After compaction during streaming, the agent automatically
  resumes the current task.

Use `/compact` to trigger compaction manually, and `/context` to inspect the
per-category token breakdown.

Each compaction also produces a rolling anchored summary that is preserved
across subsequent compactions. The earliest decisions, file paths, and
constraints survive even after multiple rounds, so long sessions retain
their starting context instead of drifting.

== SUB-AGENTS
<agent-subagents>
A sub-agent is a child run with its own message history, its own tool
allowlist, and its own system prompt. The parent dispatches through the
`agent` tool; the child's final output is returned as the tool result.

=== Spawning
<agent-subagents-spawn>

The `agent` tool accepts:

#figure(
  align(center)[#table(
    columns: (0.32fr, 1fr),
    align: (auto, left),
    table.header([Field], [Meaning]),
    table.hline(),
    [`subagent_type`],
    [Definition slug. Empty or `fork` selects fork mode.],
    [`description`],
    [3-7 word label shown in the task overlay.],
    [`prompt`],
    [Full instructions for the child.],
    [`run_in_background`],
    [`false` blocks until the child finishes; `true` returns a `task_id`
     immediately and the child runs detached.],
    [`isolation`],
    [`worktree` runs the child in its own `git worktree --detach` under
     `<runtime>/qsoc-worktrees/<task_id>`. Default is none.],
  )],
  caption: [`agent` TOOL FIELDS],
  kind: table,
)

`description` and `prompt` are required. The concurrency cap defaults to
four in-flight children; the limit is enforced before the LLM is touched
so a capped spawn fails fast.

=== Fork Mode
<agent-subagents-fork>

When `subagent_type` is empty or set to `fork`, the child inherits the
parent's full message history up to the spawn point. The fork point is
marked with the `<!-- qsoc-fork-tag -->` HTML comment so further `agent`
calls inside the child cannot recurse into another fork from the same
anchor.

The child's prefix is byte-identical to the parent's, so the prompt
cache hit on the first child turn is preserved.

=== Definitions
<agent-subagents-defs>

Three scopes exist, in shadowing order from highest to lowest:

- *Project*: `<project>/.qsoc/agents/*.md`. In remote-workspace mode the
  files are scanned over SFTP from the same path under the remote
  workspace, so a remote project's definitions are seen without
  uploading anything from the local host.
- *User*: `~/.config/qsoc/agents/*.md`
- *Builtin*: compiled in. The shipped names are `general-purpose`
  (full tool set), `explore` (read-only), and `verification` (adds
  `bash` and `lsp_*`).

Same-name definitions in higher scopes shadow lower ones. Files whose
frontmatter fails to parse appear in `/agents` under an "Errors"
group with the offending path.

=== Frontmatter Fields
<agent-subagents-frontmatter>

#figure(
  align(center)[#table(
    columns: (0.32fr, 1fr),
    align: (auto, left),
    table.header([Key], [Meaning]),
    table.hline(),
    [`name`],
    [Definition slug. Becomes the `subagent_type` value the parent passes.],
    [`description`],
    [One-line summary shown in the parent's system prompt and `/agents`.],
    [`tools`],
    [Allowlist of tool names the child may call. Empty inherits the parent
     set.],
    [`disallowed_tools`],
    [Denylist applied after the allowlist. Pipe-separated alternates.],
    [`max_turns`],
    [Hard ceiling on the child's iteration count. Unset inherits the
     parent's limit.],
    [`critical_reminder`],
    [Text re-injected as a system message at the head of every child turn.],
    [`skills`],
    [Skill names preloaded into the child's system prompt as if
     `skill_find` returned them.],
    [`hooks`],
    [Inline hook overrides. Same shape as the global `agent.hooks` block,
     single level deep.],
    [`inject_memory`],
    [`true` adds the auto-memory store. Default `false` for sub-agents.],
    [`inject_skills`],
    [`true` adds the skill listing. Default `false`.],
    [`inject_project_md`],
    [`true` (default) injects `AGENTS.md`. `false` skips it.],
    [`model`],
    [Override model id for this child. Empty inherits the parent.],
  )],
  caption: [SUB-AGENT DEFINITION FRONTMATTER],
  kind: table,
)

The body of the file is the child's base system prompt.

=== Runtime State
<agent-subagents-state>

Each run produces:

- `<runtime>/qsoc/agents/<task_id>.jsonl`: structured event stream
  (one JSON event per line: prompt, tool calls, tool results,
  content chunks, final output).
- `<task_id>.meta.json`: sidecar with label, `subagent_type`, status,
  isolation mode, and worktree path.

At startup, runs whose meta says `running` but whose owning process is
gone are rewritten to `failed`, and worktrees older than one hour
without a live owner are swept.

=== Polling and Resuming
<agent-subagents-poll>

While a backgrounded run is alive:

- `agent_status` returns the current status, transcript tail, and the
  final output once the run completes.
- `send_message` queues a line of user input into the child's stream.
- `/agents-history` lists prior runs with their final results.
- `agent_resume` reads the meta sidecar plus the transcript tail and
  synthesizes a `resume_prompt` that can be passed to a fresh `agent`
  call to continue where a prior run left off, e.g. across a process
  restart.

== BACKGROUND TASKS
<agent-tasks>
A unified task panel lists every long-running activity attached to the
current agent: backgrounded `bash` jobs, scheduled `/loop` prompts, and
detached sub-agent runs. Monitors created by `monitor` also appear here
while they are alive.

=== Status Pill
<agent-tasks-pill>

While any task is in flight, a status pill is rendered just above the
prompt with the count and the most recent activity. *Down* on the
prompt's first row parks focus on the pill; *Enter* opens the overlay;
*Up* returns to the prompt. *Ctrl+B* opens the overlay from anywhere.

=== Overlay
<agent-tasks-overlay>

The overlay is two columns: a list on the left, the highlighted task's
tail on the right.

#figure(
  align(center)[#table(
    columns: (0.25fr, 1fr),
    align: (auto, left),
    table.header([Key], [Action]),
    table.hline(),
    [`Up`/`Down`, `j`/`k`], [Move selection],
    [`Enter`], [Open the task's detail tail],
    [`x`],
    [Abort or kill the highlighted task with no confirm prompt],
    [`q`, `ESC`], [Close the overlay],
  )],
  caption: [TASK OVERLAY KEYS],
  kind: table,
)

`x` sends `SIGTERM` to bash jobs, calls `abort()` on streaming
sub-agents, stops monitors, and removes a `/loop` job from the schedule.
Completed rows linger for a short window so their tail can still be
inspected before being evicted.

=== Output Monitors
<agent-task-monitors>

`monitor` runs a shell command in the background and watches stdout/stderr
as a line stream. Each output line becomes a `<task-notification>` user
message queued for the current agent turn; partial trailing lines are
flushed on exit, and high-volume bursts are coalesced before injection.
The full stream is also written to an `output.log` path returned by the
tool and shown in the task overlay.

Users normally do not need to configure monitors manually. Ask the agent
to watch a log, poll a status endpoint, wait for CI, or react to a file
change; the agent should write the watcher command itself and call
`monitor` when event output needs to wake the session. Use `monitor_stop`
with the returned `task_id` to stop it.

== SCHEDULED PROMPTS
<agent-loop>
`/loop` runs a prompt on a cron schedule. Jobs are persisted to
`<project>/.qsoc/loop.yml` and gated by a per-project file lock so only
one qsoc session fires them at a time.

=== Subcommands
<agent-loop-cmds>

#figure(
  align(center)[#table(
    columns: (0.45fr, 1fr),
    align: (auto, left),
    table.header([Form], [Effect]),
    table.hline(),
    [`/loop <cron> <prompt>`],
    [Add a job. Cron is a 5-field expression (`m h dom mon dow`).],
    [`/loop <prompt>`],
    [Add a job at the default cadence (`*/10 * * * *`).],
    [`/loop list`],
    [Show all jobs with id, cron, next-run time, and prompt head.],
    [`/loop stop <id>`], [Remove a job.],
    [`/loop clear`], [Remove every job for this project.],
  )],
  caption: [/LOOP SUBCOMMANDS],
  kind: table,
)

Each fire is queued as a normal user prompt; the agent processes it
between turns of any in-flight conversation. If another qsoc session
already holds the loop lock, `/loop add/stop/clear` reports the conflict
and exits without modifying state.

== SYSTEM PROMPT SOURCES
<agent-system-prompt>
On every turn, the system prompt is composed from:

- *Modular sections*: built-in role, decision flow, and tool usage guidance
- *Project instructions*: `AGENTS.md` and `AGENTS.local.md` in the project
  directory, injected verbatim
- *Memory*: entries from the auto-memory store (see `memory_read` /
  `memory_write`), capped by `agent.memory_max_chars`
- *Skill listing*: names and descriptions of installed skills so the agent
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
- `monitor`, `monitor_stop` (remote command, local notification stream)
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

- `qsoc agent --continue`: resume the most recent session
- `qsoc agent --resume [id]`: pick a session from a list, or load one by id /
  unique prefix
- `/branch [name]`: fork the current session into a new id, preserving the
  original
- `/rename <title>`: set a human-readable title shown by the resume picker

== USAGE EXAMPLES
<agent-examples>

```
qsoc> Create a new project named "soc_design" in the current directory
qsoc> Import all Verilog files from ./rtl directory
qsoc> Add AXI4 slave interface to the cpu module
qsoc> Generate Verilog from netlist.yaml with output name "top"
```

== MCP SERVERS
<agent-mcp>
The agent can connect to external Model Context Protocol (MCP) servers and
expose their tools to the LLM alongside the built-in tool set. MCP is an
open JSON-RPC 2.0 protocol; any compliant server works.

=== Configuration
<agent-mcp-config>

Add an `mcp:` section to `.qsoc.yml` (project-level) or to the user-level
config. Two transports are supported in this release:

```yaml
mcp:
  servers:
    - name: fs
      type: stdio
      command: /usr/local/bin/mcp-fs-server
      args: ["--root", "/tmp"]
      env:
        LOG_LEVEL: info

    - name: search
      type: http
      url: http://127.0.0.1:8080/mcp
      headers:
        Authorization: "Bearer placeholder"
      request_timeout_ms: 60000
      connect_timeout_ms: 30000

    - name: archived
      type: stdio
      command: /opt/legacy/mcp
      enabled: false

    - name: lan_only
      type: http
      url: http://10.0.0.50/mcp
      proxy: none           # bypass any qsoc-wide / system proxy
```

#figure(
  align(center)[#table(
    columns: (0.4fr, 1fr),
    align: (auto, left),
    table.header([Field], [Description]),
    table.hline(),
    [`name`], [Logical server name; appears in `mcp__<name>__<tool>` and
       `/mcp list`. Must be unique.],
    [`type`], [`stdio` (child process) or `http` (Streamable HTTP).
       Defaults to `stdio` if omitted.],
    [`command`], [stdio: executable to launch.],
    [`args`], [stdio: list of arguments passed to the executable.],
    [`env`], [stdio: extra environment variables for the child process.],
    [`url`], [http: endpoint URL. Both immediate JSON and Server-Sent
       Events responses are accepted on the same endpoint.],
    [`headers`], [http: extra request headers, e.g. `Authorization`.],
    [`proxy`], [http: per-server proxy override. Accepts the same flat
       string form as the LLM endpoint `proxy:` field (`none` /
       `system` / `http://host:port` / `socks5://host:port`). Empty or
       `system` falls back to the qsoc-wide `proxy:` block; see
       @proxy-config.],
    [`connect_timeout_ms`], [Connection timeout in milliseconds.
       Default 30000.],
    [`request_timeout_ms`], [Per-request timeout in milliseconds.
       Default 60000.],
    [`enabled`], [Set to `false` to keep the entry in the config but skip
       it at startup.],
  )],
  caption: [MCP SERVER FIELDS],
  kind: table,
)

=== Tool naming
<agent-mcp-naming>
Each tool exposed by an MCP server is registered as
`mcp__<server>__<tool>`, with non-alphanumeric characters in either segment
replaced by underscore (collapsed and trimmed). Two servers can therefore
expose tools with the same short name without colliding. Examples:

- server `fs`, tool `read_file` becomes `mcp__fs__read_file`
- server `my server`, tool `Create Issue` becomes
  `mcp__my_server__Create_Issue`

=== Slash commands
<agent-mcp-commands>

#figure(
  align(center)[#table(
    columns: (0.4fr, 1fr),
    align: (auto, left),
    table.header([Command], [Description]),
    table.hline(),
    [`/mcp` or `/mcp list`], [List configured MCP servers with state and
       tool count.],
    [`/mcp reconnect <name>`], [Force a disconnect of the named server;
       the manager rebuilds it on the standard backoff.],
  )],
  caption: [MCP SLASH COMMANDS],
  kind: table,
)

=== Lifecycle
<agent-mcp-lifecycle>
At startup the agent gives every configured server a short window
(roughly 1.5 seconds total across servers) to complete the
initialize / capabilities handshake. Servers that respond on time
contribute their tools immediately; tools registered later via
`notifications/tools/list_changed` are picked up at runtime.

If a server closes unexpectedly the manager schedules a rebuild on
exponential backoff (1 s, 2 s, 4 s, capped at 30 s). After three
failed attempts the server is marked failed and dropped until the
next agent restart.

=== Security notes
<agent-mcp-security>
- MCP tools call out to processes (stdio) or remote endpoints (http) you
  configured yourself. Treat them with the same care as any other piece of
  third-party code.
- Tools inherit the agent's permission rails (read unrestricted, write
  restricted to allowlisted directories). The MCP server can still touch
  resources outside the agent (a stdio server may write anywhere it has
  permission to). Configure stdio servers with the narrowest filesystem
  scope your task needs.
- `headers` may contain bearer tokens or other secrets. Keep `.qsoc.yml`
  out of version control if it carries credentials, or substitute
  environment variables and reference them from your shell.

== HOOKS
<agent-hooks>
QSoC agent fires user-defined commands at well-known lifecycle points so
projects can layer their own policy, audit trail, or context injection
on top of the built-in agent loop. Hooks are configured in YAML, run
locally via `/bin/bash`, and communicate with the agent over stdin
JSON, stdout JSON (optional), and process exit codes.

=== Events
<agent-hooks-events>

#figure(
  table(
    columns: (auto, 1fr, auto),
    align: (left, left, left),
    table.header([Event], [When it fires], [Can block]),
    table.hline(),
    [`pre_tool_use`],
    [Before the agent dispatches a tool call. The matcher is tested
     against the tool name.],
    [yes (exit 2)],

    [`post_tool_use`],
    [Right after the tool returns and the result is emitted. Matcher tested
     against the tool name. Fire-and-forget; the result is not mutated.],
    [no],

    [`user_prompt_submit`],
    [Before a user prompt enters the conversation. Fires for the initial
     query, queued requests, and synchronous `run()` calls.],
    [yes (exit 2)],

    [`session_start`],
    [Once per agent lifetime, the first time `runStream` is invoked.],
    [no],

    [`stop`],
    [Just before the agent emits `runComplete` with the final assistant
     content. Fire-and-forget.],
    [no],
  ),
  caption: [Hook events],
  kind: table,
)

=== Configuration
<agent-hooks-config>

Add an `agent.hooks` section to `.qsoc.yml` (project-level) or to the
user-level config. Each event maps to a list of *matcher groups*; each
group has a matcher pattern plus the commands to run when it matches:

```yaml
agent:
  hooks:
    pre_tool_use:
      - matcher: "bash|file_write"   # exact name, or pipe-separated alternates
        hooks:
          - type: command
            command: /usr/local/bin/qsoc-audit
            timeout: 10               # seconds; default 10
    post_tool_use:
      - hooks:                        # matcher omitted = always matches
          - type: command
            command: logger -t qsoc-tool
    user_prompt_submit:
      - hooks:
          - type: command
            command: /usr/local/bin/qsoc-inject-context
    session_start:
      - hooks:
          - type: command
            command: /usr/local/bin/qsoc-init
    stop:
      - hooks:
          - type: command
            command: notify-send "QSoC agent done"
```

Matcher rules:
- Empty string or `*`: always matches.
- All-alphanumeric/underscore plus `|`: exact match against the subject,
  with `|` separating alternates.
- Anything else: regular expression (anchored full match). Invalid regex
  fails closed (the matcher is treated as no-match).

Multiple matchers can be configured for the same event. Every group
whose matcher matches contributes its commands; matched commands run in
parallel and the outcome is aggregated (any command returning Block
makes the whole event blocked).

=== JSON protocol
<agent-hooks-protocol>

The agent serializes the event payload to JSON and writes it on the
hook's stdin (one line, terminated by `\n`). The hook may write a
single-line JSON object on stdout to influence the outcome; anything
else on stdout is captured but ignored for control purposes.

Common payload fields:

#figure(
  table(
    columns: (auto, 1fr),
    align: (left, left),
    table.header([Field], [Meaning]),
    table.hline(),
    [`event`],            [Event key (`pre_tool_use`, `post_tool_use`, ...).],
    [`tool_name`],        [Tool name, for tool-related events.],
    [`tool_input`],       [Parsed tool arguments (JSON object).],
    [`response`],         [Tool result (text), for `post_tool_use` only.],
    [`prompt`],           [User prompt text, for `user_prompt_submit` only.],
    [`final_content`],    [Final assistant content, for `stop` only.],
    [`cwd`],              [Local working directory at fire time.],
    [`remote`],           [Present only when the agent is in remote mode;
                           carries `display`, `workspace`, `cwd`.],
  ),
  caption: [Hook payload fields],
  kind: table,
)

Hooks always run on the local host. In remote-workspace mode the agent
includes a `remote` section so scripts can branch on it; if you want to
inspect remote state, your hook script can `ssh` back into the host
itself.

Optional stdout fields (parsed only when the first stdout line is a
JSON object):

#figure(
  table(
    columns: (auto, 1fr),
    align: (left, left),
    table.header([Field], [Meaning]),
    table.hline(),
    [`reason`],           [Human-readable block reason. Used by
                           `pre_tool_use` and `user_prompt_submit` when the
                           hook also exits with code 2.],
    [`updatedInput`],     [Replacement for `tool_input` (JSON object).
                           Honored by `pre_tool_use` on success.],
    [`context`],          [String prepended to the user prompt. Honored by
                           `user_prompt_submit` on success.],
  ),
  caption: [Hook stdout JSON fields],
  kind: table,
)

Exit codes:
- `0`: success; the agent applies any optional fields above and
       continues normally.
- `2`: block. Only meaningful for `pre_tool_use` and
       `user_prompt_submit`; for other events it degrades to a
       non-blocking error.
- any other non-zero: non-blocking error; stderr is surfaced to the
       console, the agent continues.

If a hook does not finish within its `timeout` seconds the agent kills
the child process and treats the result as a timeout (non-blocking
except for `pre_tool_use`/`user_prompt_submit` where the hook's
contribution is dropped). The default timeout is 10 seconds; set
`timeout` per command to override.

=== Security
<agent-hooks-security>
- Hook commands run with the privileges of the qsoc user. Treat any
  hook source path as you would any locally executed script.
- Hooks always run locally, even when the agent operates a remote
  workspace; nothing is uploaded to the remote host.
- Hooks have full access to the local filesystem, environment, and
  network. Keep them small, single-purpose, and review their source
  whenever you change `.qsoc.yml`.
- Invalid YAML entries (unknown event, missing `command`, unsupported
  `type`) are dropped at load time with a console warning rather than
  crashing the agent.
