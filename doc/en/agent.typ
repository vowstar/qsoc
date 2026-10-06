= Agent Mode
<agent-overview>
QSoC provides an interactive AI agent for SoC design automation. The agent uses
LLM tool calling to execute multi-step workflows through natural language.

== Agent Command
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
    [`--resources`], [Print a local daemon resource snapshot as JSON without opening an agent session],
    [`--max-tokens <n>`], [Maximum context tokens (default: 128000)],
    [`--temperature <n>`], [LLM temperature 0.0--1.0 (default: 0.2)],
    [`--no-stream`], [Disable streaming output],
    [`--effort <level>`], [Reasoning effort: low, medium, high],
    [`--resume [id]`],
    [Resume a session by id prefix, title, or branch; pick from list if
     omitted. A short title is auto-generated after the first turn (see
     `agent.session_title`); `/rename` always overrides it],
    [`--continue`], [Continue the most recent session for this project],
    [`--workspace <path>`],
    [Working directory for tool execution. Local absolute path by default;
     paired with `--ssh` it names the remote workspace],
    [`--ssh <target>`],
    [Connect to a remote workspace before the first prompt. Accepts
     `[user@]host[:port]` or a `~/.ssh/config` alias. Requires `--workspace`],
    [`--connect <socket>`],
    [Attach to a running `qsoc-agentd` instead of starting a private one
     (@agent-daemon)],
  )],
  caption: [AGENT COMMAND OPTIONS],
  kind: table,
)

=== Interactive Mode
<agent-interactive>

```bash
qsoc agent
qsoc agent -d /path/to/project -p myproject
qsoc agent --effort high
qsoc agent --continue
qsoc agent --resume abc123
```

=== Single Query Mode
<agent-single-query>

```bash
qsoc agent -q "List all modules in the project"
qsoc agent -q "Import cpu.v and add AXI bus interface"
```

Ctrl-C cancels an in-flight query with or without streaming. One press prints
`(interrupted)` and exits with status 0; a second press within two seconds
exits immediately with status 130. `--no-stream` still sends a synchronous
request and does not add terminal control sequences to its output.

=== Agent Daemon
<agent-daemon>

`qsoc agent` starts a private `qsoc-agentd` and stops it on exit. A daemon started separately accepts up to 64 simultaneous connections:

```bash
qsoc-agentd                               # default local endpoint
qsoc-agentd -s /path/to/agent.sock        # explicit endpoint
qsoc agent --connect /path/to/agent.sock  # attach to that daemon
```

On Windows, use a pipe name such as `qsoc-agent` for `--socket` and `--connect`.

An agent request starts a separate session process for its connection. Sessions have separate event loops and share the daemon's SMT task budget (@agent-smt). Closing an attached TUI cancels its session's work and leaves the daemon running. Saved sessions remain available through `--resume` and `--continue`.

Different OS users can run their own daemons. One user can run multiple daemons on distinct endpoints. Only processes of the same user can connect. Windows also requires matching process integrity levels. Linux and macOS use Unix domain sockets. Windows uses local named pipes. All three platforms use the same protocol: eight hexadecimal length bytes followed by a UTF-8 JSON object. The length counts JSON bytes. The greeting identifies `qsoc-agentd`, protocol version `1`, and available capabilities. Requests carry `id`, `method`, and `params`. Replies carry the same `id` and either `result` or `error`. Events use an `event` object. A turn the session starts by itself, for a scheduled prompt or a background notification (@agent-task-wake), replies with `id` `0`. A `task_notification` event carries a one-line summary in `text` and the model-facing envelope in `json.body`.

When the greeting advertises `smt`, clients can submit `smt.solve` without opening an agent session or configuring an LLM. Its `params` object accepts the same fields as `z3_solve` (@agent-smt). Multiple requests can remain outstanding, and results can arrive out of order. Keep each outstanding request ID unique within its connection.

```json
{"id":1,"method":"smt.solve","params":{"mode":"check","smtlib":"(assert true)"}}
{"id":2,"method":"smt.cancel","params":{"request_id":1}}
```

`smt.cancel` targets a request on the same connection. Its `result.canceled` field reports whether cancellation was accepted. The solve request receives a separate final result. Disconnecting cancels that connection's SMT requests. The daemon also cancels tasks belonging to a session when its session process exits. Failed or disconnected requests are not automatically replayed.

JSON payloads are limited to 16 MiB. The daemon shares a 64 MiB transport budget across connections and closes a connection that exceeds the budget.

=== Resource Usage
<agent-resources>

Use `qsoc agent --resources` for a JSON snapshot or `/resources` in the TUI. Add `--connect` to query an existing daemon. These queries do not contact the model or enter its conversation history.

The `system_resources` tool returns a snapshot only when the model calls it. Its definition stays fixed throughout the session. Samples are appended as tool results, without refreshing previous messages or the system prompt. Resource changes do not select a different model or reasoning effort.

Snapshots describe the local daemon host, its current descendant processes, and requested local workspace storage. SSH workspace resources are not included. CPU counters are cumulative nanoseconds. Memory and storage fields use bytes. `null` means the measurement is unavailable, not zero or unlimited. `sampled_at_utc` records collection start and `collection_duration_ms` records its duration. Old snapshots do not establish current capacity or reserve resources.

Resident, proportional, private committed, and footprint memory measure different things. Summing resident memory across processes counts shared pages more than once. Process ancestry is a best-effort observation and can miss processes that exit or detach during collection.

Linux reports host memory and, when readable, the remaining memory under visible cgroup v2 ancestors. Windows reports available physical memory. macOS estimates available memory from free and inactive pages. Container limits or parent job limits that cannot be read remain unknown. Storage reports available bytes and mount read-only status. It does not check directory access rights or reserve space.

When the greeting advertises `resources`, a client can request the same snapshot without opening a session:

```json
{"id":3,"method":"resources","params":{"paths":[]}}
```

`paths` accepts at most eight absolute local paths. An empty list skips storage queries. The daemon allows one probe at a time and stops waiting after three seconds. A busy, timed-out, or unavailable probe returns an explicit status. Resource probes do not change limits.

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
it. `/local` inside the REPL returns to the local workspace. `--ssh`
also accepts a catalog alias, and an `--ssh` run never changes the
remembered binding (@agent-remote-connect).

=== Host Catalog
<agent-host-catalog>

The host catalog supplements `~/.ssh/config` with two things SSH-config
does not carry: the remote workspace path and a free-form *capability*
description used by the parent agent to dispatch sub-agents.

The catalog lives in two YAML files:

- User scope: `~/.config/qsoc/host.yml`
- Project scope: `<project>/.qsoc/host.yml` (overrides user by alias)

```yaml
hostList:
  - alias: fpga-build           # matches Host fpga-build in ~/.ssh/config
    workspace: /home/bob/build
    capability: |
      FPGA synthesis, bitstream generation, and JTAG programming
  - alias: gpu-sim              # not in ssh-config; uses fallback target
    target: alice@gpu01:22
    workspace: /home/alice/sim
    capability: |
      RTL simulation
    shell: sh                   # optional: auto (default), bash, sh, or a Git Bash path
```

`shell:` chooses the interpreter for commands on that host (see
@agent-shell-discovery). It accepts `auto`, `bash`, `sh`, or an absolute
Windows path to a `bash.exe` such as `C:\Program Files\Git\bin\bash.exe`,
which applies to Windows hosts only. Any other value makes `/ssh <alias>`
fail with an error naming the entry.

The catalog (`hostList:`) holds named entries. Add, update, and remove
are driven by natural-language dialogue with the agent through three
tools; no slash command edits the list. The current binding is not kept
in either file (@agent-remote-connect). An `active:` key is ignored. In a
project file it prints a one-line notice at startup.

The three LLM-facing catalog tools edit `hostList:` atomically:

- `host_register` creates an entry. Required: `alias`, `workspace`.
  Optional: `capability`, `target` (only needed when `alias` is not in
  `~/.ssh/config`).
- `host_update` applies an op-list of `capability_append`,
  `capability_remove`, `capability_replace`, `set_workspace`,
  `set_target`. Ops apply atomically; partial failure leaves the file
  unchanged.
- `host_remove` drops an entry.

Bare `/ssh` opens a searchable menu: catalog aliases first (with a
`catalog` hint and the first capability line), then concrete
`~/.ssh/config` aliases (with the `ssh-config` hint). Wildcard and
negated patterns are not listed. A catalog alias hides an ssh-config
alias of the same name. The last row prints the `/ssh` usage.

`/ssh <alias>` resolves the name like the `agent` tool's `host`
parameter: an alias in `~/.ssh/config` is dialed through it, and a
catalog alias that is not there dials its `target`. A catalog
`workspace` is used without asking.

When spawning a sub-agent through the `agent` tool, set the optional
`host` parameter to run the child on another machine. Without
`agent.dispatch.hosts` every catalog alias is a valid `host`; with it, only
the hosts it lists are (@agent-dispatch).
A per-parent-run SSH session cache keeps sibling spawns to the same
alias and workspace on the same session, so a second child to the same host
does not re-authenticate. The child's
workspace tools act on that host; its other tools are the same local tools
a remote main session has (@agent-remote-where), except `monitor`.

Sub-agent dispatch never prompts for credentials. If a host needs an
interactive secret (encrypted private-key passphrase or `password`
method) and the per-parent cache does not already hold a session, the
spawn returns an error asking the user to authenticate once via
`/ssh <alias>` in the main session; the cached session then services
all subsequent sub-agent spawns to that host.

Agent definitions may declare `preferred_host:` in their frontmatter to
default sub-agent spawns of that type to a specific host. It passes the same
checks as the `host` parameter.

The `capability` field is injected verbatim into the parent agent's
system prompt and is therefore visible to the LLM provider. Treat it as
human-facing description text; do not record secrets, tokens, internal
hostnames, or other sensitive operational details there.

=== Project Goal
<agent-project-goal>

A *project goal* lets the user say "keep working on X until you've
actually finished it" and the agent keeps taking turns automatically
until either the goal is marked complete or the token budget runs out.

The catalog lives in two files under the project's `.qsoc/` directory:

- `goal.yml` carries the active goal record (single entry, atomic
  temp+rename writes via QSaveFile).
- `goal_log.jsonl` is an append-only timeline of lifecycle events
  (created, status_changed, objective_updated, usage_accounted,
  continued, cleared, discarded). One line per event so `tail -f` and
  `jq` work directly.

```yaml
goal:
  id: 8a7c6b3e-f1d2-4a9b-c8e7-d6f5a4b3c2d1
  objective: |
    Build top-level RTL and verify against the reference simulation
  status: active
  token_budget: 20000
  tokens_used: 1850
  seconds_used: 412
  created_at: 2026-05-16T22:00:00+08:00
  updated_at: 2026-05-16T22:06:52+08:00
  binding:
    target: sim1
    workspace: /work/proj
```

Status values: `active`, `paused`, `budget_limited`, `complete`. The
runtime auto-trips `budget_limited` when `tokens_used` reaches
`token_budget`; `complete` is terminal and drops the record so a fresh
goal can be set without an extra clear step.

A goal belongs to the binding it was set on: this machine (empty `target`
and `workspace`), or the `/ssh` alias and remote workspace. While another
binding is live, the goal does not auto-continue. The first turn that would
continue it prints `Goal paused: it was set on <binding> ...`, and `/goal`
adds `(waits for <binding>)`. It continues when its binding is live again. A
`goal.yml` without `binding` continues on every binding.

==== Lifecycle

When a goal is `active` and no user input is pending at the end of a
turn, the run loop appends a `<goal_context>` user message to the
conversation and takes another iteration. Three template variants live
in `QSocGoalPrompt`:

- *continuation* runs on every auto-continuation turn; it restates the
  objective, demands a "completion audit" before marking done, and
  prints the live token usage.
- *budget_limit* runs once when the budget threshold trips; it tells
  the LLM to wrap up and stops further auto-continuations.
- *objective_updated* runs once after the user replaces the objective
  on an in-flight goal.

All three are injected at the user role (not the system prompt) so the
prompt cache stays warm.

==== Slash commands

- `/goal` — show the current goal status; usage hint when none.
- `/goal <objective>` — set a new goal. If one is already active the
  user is asked to confirm replacement through a two-option menu.
- `/goal pause` / `/goal resume` — flip between `active` and `paused`;
  paused goals do not auto-continue.
- `/goal clear` — drop the active goal immediately (no confirmation).
- `/goal budget <N>` — set or change the token budget (`0` to disable).

==== goal_complete tool

Goal completion is *LLM-driven*. The agent exposes a single tool,
`goal_complete`, whose schema enum restricts the status payload to
`"complete"`. The LLM calls it only when its completion audit proves
the objective has been met. The agent gate hides and rejects this shared
tool for sub-agents, so only the parent can declare the goal complete.

The status-line chip surfaces the live state at all times:

```text
[E:high] [Qwen3.6-35B-A3B-FP8] [Goal: Build top-level RTL 1850/20000|active]
```

==== Audit

Inspect goal events with:

```bash
tail -20 .qsoc/goal_log.jsonl | jq -c '{ts,event,goal_id}'
```

== Interactive Commands
<agent-commands>
The following commands are available during an interactive session:

#figure(
  align(center)[#table(
    columns: (0.35fr, 1fr),
    align: (auto, left),
    table.header([Command], [Description]),
    table.hline(),
    [`exit`, `quit`, `/exit`, `/quit`], [Exit the agent],
    [`/branch [name]`], [Fork the current session into a new one],
    [`/btw <question>`],
    [Ask a quick side question with the full conversation context. The
     answer streams into the scrollback but is not added to the session
     history, so the main context is untouched. The fork uses the
     configured model and effort, has no tools, and answers in a single
     turn; *Esc* cancels.],
    [`/clear`], [Start a fresh session; the previous session and its file
      checkpoints remain available to resume],
    [`/compact`], [Compact context and report tokens saved],
    [`/context`], [Show token usage breakdown and suggestions],
    [`/cost`], [Show session token totals and cost (if rates configured)],
    [`/cache`], [Show local request and cache diagnostics],
    [`/cwd [path]`],
    [Show or change the working directory. Empty opens a picker. In remote
     mode, drives the remote cwd: the host resolves the path first, so a
     directory reached through a symlink is judged by where it really is and
     refused when that is outside the workspace root.],
    [`/diff`], [Review file edits turn-by-turn],
    [`/effort [level]`], [Show or set effort (off/low/medium/high)],
    [`/local`],
    [Leave SSH remote mode and return to local workspace. The remembered
     binding stays, so the next `/ssh <same target>` or the next interactive
     startup reuses it.],
    [`/memory [name|rm name]`],
    [List memory topics; `/memory <name>` edits the body in `$EDITOR`;
     `/memory rm <name>` deletes a topic. `#<fact>` saves a project memory
     directly without an LLM turn.],
    [`/model [id]`], [Show or switch the active model],
    [`/plan [on|off]`],
    [Toggle read-only plan mode (or press *Shift+Tab*). While on, the agent
     may only explore and ask questions until it calls `exit_plan_mode` and
     you approve a plan. Bare `/plan` toggles. The model can also enter plan
     mode itself.],
    [`/project <path>`],
    [Switch project root (reloads config, starts a new session)],
    [`/rename <title>`], [Set session title for the resume picker],
    [`/resume [id]`],
    [Switch to another saved session of this project (@agent-persistence)],
    [`/ssh [target]`],
    [Connect to an SSH remote workspace. The target is a catalog alias, a
     `~/.ssh/config` alias, or `[user\@]host[:port]`. Empty opens a menu of
     catalog and `~/.ssh/config` hosts. The workspace comes from the catalog
     or the remembered binding, else a directory browser asks for it
     (@agent-remote-connect).],
    [`/status`],
    [Show the selected model, the name it sends in requests, effort, and
     session. In remote mode it also
     reports the bound target, the workspace, and whether the link is
     still usable.],
    [`/dispatch [reload]`],
    [Show the hosts and models sub-agents may use, or read `agent.dispatch`
     again (@agent-dispatch).],
    [`/help`], [Show help message],
    [`/agents`],
    [List sub-agent definitions by scope (builtin, user, project, remote) and any
     parse errors. See @agent-subagents.],
    [`/agents-history`],
    [Show prior backgrounded sub-agent runs with status and transcript tail.],
    [`/loop [cron] <prompt>`],
    [Schedule a recurring prompt. Subforms: `/loop list`, `/loop stop <id>`,
     `/loop clear`. See @agent-loop.],
    [`!<command>`],
    [Run a shell command in the working directory and show its output,
     non-zero exit code, and the shell that ran it. The same rule applies
     locally and on a remote host (@agent-shell-discovery). In remote mode
     it runs only when the working directory still resolves inside the
     workspace. The command and its output enter the conversation; see
     @agent-shell-context.],
    [`!!<command>`],
    [Run a shell command the same way and show its output, without adding
     anything to the conversation or the session.],
  )],
  caption: [INTERACTIVE COMMANDS],
  kind: table,
)

=== Shell Command Context
<agent-shell-context>

After a `!` line, the command, its exit code, its duration and its output
are added to the conversation as one user message, and the model reads it
with the next request. The output is sent to the model provider and saved in
the session file. A dim line after the result says so and names `!!`, which
runs a command without either. The model is told that this block is command
output, not instructions. Output larger than a tool result keeps its head and
tail in the conversation; the full text is saved as an artifact that
`tool_output_read` reads. A `!` line in a new session saves the session the
same way a first prompt does, so it then appears in `/resume`. While a turn
runs or the session waits for recovery input, the result is only shown, and
the dim line says it was not added. A `!` or `!!` command never starts a turn
and never sends a background notification. Set `agent.shell_command_context`
to `false` to keep every `!` result on screen only. CRLF line ends in the
output are stored and shown as LF.

The directory picker shared by `/cwd`, `/ssh`, and workspace selection
navigates with Up/Down plus Enter/Right to descend and Left to go up.
Press `/` to open a locate prompt: type a path and Enter jumps to the
deepest existing directory along it, so deep targets need no click-by-
click descent. A leading `~` expands to the home directory.

=== Plan Mode
<agent-plan-mode>

Plan mode is a read-only brake for non-trivial or hard-to-undo work
(RTL edits, remote synthesis runs). Toggle it with *Shift+Tab* or
`/plan on|off`, or let the model enter it itself with the
`enter_plan_mode` tool. While active
the agent may only take read-only actions: read files, search, query
LSP, run read-only shell, and spawn read-only sub-agents. File writes,
mutating shell, commits, and config changes are rejected, and the status
line shows a `⏸ PLAN` chip. The main agent's tool list stays the same in
and out of plan mode; a call to a write tool returns an error until a plan
is approved.

Plan-mode sub-agents return findings, supporting evidence, unresolved
ambiguities, and a proposed plan to the parent agent.

Shell commands are not gated by a fixed allowlist; each one is judged by
a separate LLM safety classifier and blocked (with a reason) if it could
change state. The parent agent explores and clarifies with `ask_user`
across as many rounds as needed, then calls `exit_plan_mode` to present a
plan. You approve it or keep planning. On approval the agent leaves plan
mode, the plan is saved to `<project>/.qsoc/plans/<session>.md`, and a budget-capped
copy is added at the start of the next turn. It is added again after context
compaction removes it. A newer approved plan replaces the old one.

If the parent model ends a plan-mode turn by writing prose instead of calling
`exit_plan_mode` or `ask_user` (some models call tools less reliably),
qsoc first sends it one automatic reminder to route the text through
the proper tool, so a ready plan arrives via the normal approval prompt
and a question via the normal `ask_user` flow. If the parent model still ends
in prose, the final text is treated as the proposed plan and the same
approval prompt is shown, so you are never left in plan mode with no
way to approve. On approval qsoc leaves plan mode and immediately
starts executing.

=== Focus-Aware Prompting

When the terminal supports focus reporting (DECSET 1004), qsoc tracks
whether its window is the active one. If you switch away mid-run, the
agent is steered to stop pausing for non-critical `ask_user` questions
and instead proceed on reasonable, reversible defaults, so an unattended
run keeps moving instead of blocking on a prompt nobody answers; a
`[away]` chip shows in the status bar. A focus or plan-mode change during a
turn reaches the model with the next tool result. Terminals or multiplexers that do
not report focus (e.g. tmux without `focus-events on`) are treated as
focused, so behavior is unchanged there.

If the window stays unfocused past `agent.away_summary_delay_seconds`
(default 300), qsoc prints a one-line "while you were away" recap marked
with `※`: the high-level task and the next step, so you can pick up where
you left off on return. It is generated once per away period using your
configured model (`agent.away_summary_model` overrides; empty = primary),
and disabled with `agent.away_summary: false`.

In remote mode the bar carries an `[SSH:<target>]` chip, which gains a `✗`
once the link can no longer serve calls. It refreshes when a tool call
checks the workspace, not on a timer, so a link that dies while the agent is
idle shows as broken on the next call rather than the moment it drops. It is
deliberately separate from the status text beside it: that text says what the
*agent* is doing
("Ready", the running tool), so on its own it would leave a dead workspace
sitting behind an unchallenged "Ready". `/status` carries the same state in
full, including the reason and how to recover.

== Keyboard and Input
<agent-keyboard>
Editing and navigation in the prompt:

- *Left/Right*, *Ctrl+A/E*: Move cursor / jump to start or end of line
- *Up/Down*: Browse prompt history (global across projects, current project
  surfaced first)
- *Ctrl+K*, *Ctrl+U*, *Ctrl+W*: Delete to end of line / start of line / previous word
- *Backspace*: Delete character before cursor (CJK/emoji aware)
- *Ctrl+\_*: Undo the last edit
- *Shift+Enter* or *Ctrl+J*: Insert a newline for multi-line input. Shift+Enter
  needs a terminal that reports modified keys (most modern terminals do);
  Ctrl+J and *`\` + Enter* work everywhere. Multi-line paste is preserved as
  one input

External editor and search:

- *Ctrl+X Ctrl+E* or *Ctrl+G*: Edit current input in `$EDITOR`
- *Ctrl+R*: Reverse-i-search through prompt history (across all projects)

Save and close the editor to return the edited text to the prompt.
Press Enter in QSoC to submit it.

View and selection:

- *Ctrl+T*: Toggle TODO list visibility
- *Ctrl+B*: Open the background-task overlay (see @agent-tasks)
- *Alt+Left/Right*: Switch the view between the main agent and each live
  sub-agent (see @agent-subagents-talk)
- *Down* on the prompt's first row: park focus on the task pill;
  *Enter* opens the overlay, *Up* returns to the prompt
- *Ctrl+L*: Force a full screen repaint
- *Mouse drag*: Select and auto-copy to clipboard (OSC 52)
- *Shift + drag*: Native terminal selection (fallback)

Mathematical expressions use `$...$` or `\(...\)` inline, and `$$...$$` or `\[...\]`
for display formulas. Inline formulas stay on one line. Display formulas use
Unicode rows for fractions, roots, matrices, and aligned equations. Code and link
content retain literal formula syntax.

Supported matrix environments are `matrix`, `pmatrix`, `bmatrix`, `vmatrix`, and
`Vmatrix`, inside display formulas. The `array` environment accepts `l`, `c`, and
`r` column alignment and `|` or `||` separators for augmented matrices. Use
`\left[` and `\right]` to surround an array. The `aligned` environment aligns
equation columns. Separate columns with `&` and rows with `\\`.
Each environment requires equal column counts and preserves empty cells.
Each environment has at most 8 rows and 8 columns. Each formula has at most 64 cells.
Nested environments, custom macros, and custom row spacing are unsupported.

Formulas support Greek letters, common operators, and subscripts and superscripts.
Scripts use Unicode when available; other scripts retain explicit grouping, such
as `x^{1/n}`. Compound bases keep parentheses around their scope.
Fractions accept `\frac`, `\dfrac`, `\tfrac`, and `\cfrac`, including nested fractions.
Roots accept `\sqrt{x}` and `\sqrt[n]{x}`. Common functions include `\sin`, `\cos`,
`\log`, `\exp`, and `\lim`.

Use `\text{...}` for literal text, `\operatorname{...}` for operator names,
and `\quad` or `\qquad` for spacing. Text arguments can contain Chinese and escaped
punctuation, but cannot contain nested groups or commands.
`\mathrm` uses the terminal's upright font. Formulas containing `\mathbf`,
`\mathit`, `\mathsf`, or `\mathtt` retain their source because the renderer
cannot preserve those font distinctions. This also applies inside scripts,
such as `A^{\mathsf{T}}`. `\mathbb` supports ASCII uppercase and lowercase
letters and digits;
`\mathcal` supports uppercase Latin letters.

Display formulas support the two-column `cases` environment with a left brace.
They also support `\hat`, `\vec`, and `\overline` above an expression. Accents
inside inline formulas retain their source because they need a separate row.

Display formulas support `\boxed`, `\overset`, `\underset`, and stacked limits for
`\sum`, `\prod`, and `\int`. Inline annotations retain explicit grouping.
`\substack` keeps its rows in a parenthesized, semicolon-separated list.

Each expression is limited to
4096 UTF-8 bytes, 32 nesting levels, 16 display rows, and 256 columns. The available
terminal width can reduce that column limit. Unsupported input and oversized
formulas retain their source text. Display formulas inside table cells also
retain their source. Narrowing the terminal can cause this fallback.

History, logs, and Markdown copies retain the original formula source. Mouse
selection copies the displayed characters, including the rows of a display
formula. Supplementary Unicode characters remain intact when rendered, wrapped,
and copied, including single-character emoji and mathematical letters.
Formula output uses terminal fonts and does not require an external
TeX installation.

Closed `mermaid` code fences can render as Unicode diagrams:

- `flowchart` and `graph`: four directions, rectangles, decisions, rounded nodes,
  solid or dashed links, arrows at either end, labels, `&` node groups, cycles, and self-links.
- `sequenceDiagram`: participants and actors, messages, self-messages, notes,
  and nested `loop`, `alt`, `opt`, `critical`, and `break` fragments.
- `stateDiagram` and `stateDiagram-v2`: flat states, aliases, descriptions,
  labeled transitions, and initial and final states.
- `classDiagram`: members, associations, inheritance, composition, aggregation,
  dependencies, and endpoint cardinalities.
- `erDiagram`: entity attributes, keys and comments, relationship labels,
  cardinalities, and identifying or non-identifying links.

Graph nodes follow declaration or first-reference order in the requested
`TD`/`TB`, `BT`, `LR`, or `RL` direction. Each link has a separate route;
`╪` crossings do not join links. Sequence messages retain source order.
A diagram shows the supplied relationships; rendering does not validate them.

These are limited subsets. Subgraphs, compound states, sequence activation and
parallel fragments, styling, directives, HTML, and entity escapes retain the
source. Labels accept ordinary Unicode, including Chinese and single-character
emoji. Combining characters, joined emoji, and unsupported character widths
retain the source. Diagrams are limited to 16 KiB of source, 16 graph nodes,
24 links, 16 members per node, and 40 columns per label. Sequences allow
8 participants, 64 events, and 4 fragment levels. A canvas has at most
65,536 cells. Unknown syntax, limits, an unfinished fence, or insufficient
terminal width show the complete source instead of a partial diagram.

History, logs, whole-block copies, and Markdown copies retain the diagram source.
Mouse selection copies displayed diagram characters. Widening the terminal can
restore a diagram that previously needed source fallback.

Completion and interrupt:

- `@<name>`: Fuzzy-complete a project file path
- *ESC*: Request cancellation of the current operation

Input typed while the agent is still starting, *Enter* included, is kept.
While the agent is executing, *Enter* submits input for the next iteration and
*ESC* requests cancellation. Queued input waits below the transcript and enters
it as a separate prompt when the agent reads it. With queued input, the agent
stops the current step and continues with that input; otherwise it stops the
run. Conversation history and completed tool results are preserved. The active
tool may already have changed external state.

*ESC* also stops a running remote `bash` call, `!` line or SFTP transfer
within about a second. A stopped remote `bash` command is killed on the host;
when the host refuses the signal, the result gives a `job_id` for
`bash_manage`. A stop that arrives while an SSH request is still unanswered
leaves the session unusable until it reconnects.

== Decision Flow
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

== SoC Infrastructure
<agent-soc-infrastructure>
The `generate_verilog` tool produces production RTL from `.soc_net` YAML files
with four primitive generators. Its optional Boolean `force` parameter applies
only to the current call; omitted or `false` preserves any existing user cell
file.

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

== Capabilities
<agent-capabilities>
The agent provides the following tools through natural language:

- *Project*: `project_list`, `project_show`, `project_create`
- *Module*: `module_list`, `module_show`, `module_import`, `module_bus_add`
- *Bus*: `bus_list`, `bus_show`, `bus_import` (AXI, APB, Wishbone, etc.)
- *Generation*: `generate_verilog` (RTL from `.soc_net`), `generate_template`
  (Jinja2 rendering)
- *Files*: `read_file`, `list_files`, `write_file`, `edit_file`; `path_context`
  reports and adjusts allowed write directories. `edit_file` and overwriting
  `write_file` require the file to have been read first and reject a file
  changed on disk since that read (local and remote)
- *Shell*: `bash` (synchronous, or `background=true` for background jobs),
  `bash_manage` to inspect, tail, wait for, or stop backgrounded jobs. Both
  take the same parameters locally and on a remote host: `command`,
  `timeout` (milliseconds, default 60000; `timeout_ms` is accepted for it),
  `working_directory`, `background` and `max_output` for `bash`; `action`
  (`status`, `wait`, `output`, `kill`, `terminate`), `timeout` and
  `max_lines` for `bash_manage`, with `process_id` locally and `job_id`
  remotely. A result starts with `status:` and `exit_code:` lines, then the
  merged output. A command that reads standard input sees end of file. A
  second blocking shell call from the same agent is rejected; different
  sub-agents can block independently. Use `background=true` for background
  concurrency. A terminal status, wait, or stop returns final output and
  removes the local job record after the active wait finishes; cancelling a
  wait leaves a running job tracked. A non-positive wait timeout selects the
  60-second default. Stopping a managed job stops its process group.
  Terminate requests a graceful stop and force-kills after five seconds; a
  process that does not stop remains tracked. A background or timed-out job
  that writes more than `max_output` bytes (default 5 MB) is killed; a
  blocking call is never stopped for its output size. A job that ends
  before the agent reads its terminal status or stops it sends that agent a
  task notification with the exit status and the output tail
- *Monitors*: `monitor` starts a line-oriented watcher whose output wakes
  the agent; `monitor_stop` terminates a watcher
- *Sub-agents*: `agent` to spawn a child run, `agent_status` to poll a
  backgrounded run, `agent_resume` to continue a prior run from its stored
  history, `send_message` for peer messages and `followup_task` for further
  work; see
  @agent-subagents
- *Documentation*: `query_docs` by topic (about, commands, config, bus, clock,
  fsm, logic, netlist, power, reset, template, validation, overview, ...)
- *Memory*: `memory_read`, `memory_write` for persistent notes across sessions
- *Todo*: `todo_list`, `todo_add`, `todo_update`, `todo_delete` for multi-step
  workflows, kept in `.qsoc/todos.md` of the bound workspace (the remote
  workspace in remote mode)
- *Skills*: `skill_find`, `skill_create` for user-defined prompt templates
  resolved across four layers (`$QSOC_HOME/skills`, `<project>/.qsoc/skills`,
  `~/.config/qsoc/skills`, and a platform-native system skills dir), plus
  any directory listed in `QSOC_SKILLS_PATH`; see @agent-skills for the
  SKILL.md format and @config-files for the full layout
- *LSP*: `lsp` for diagnostics, definitions, hover, references, and symbols
  (@agent-lsp)
- *Constraints*: `z3_solve` checks SMT-LIB formulas and optimizes ordered objectives on Linux, Windows, and macOS. See @agent-smt
- *Web*: `web_fetch` for URL content, `web_search` via SearXNG (when configured)
- *Schedules*: `schedule_create`, `schedule_list`, `schedule_delete` share the
  scheduler behind `/loop` (@agent-loop)

LLM, MCP HTTP, and web requests are pinned to HTTP/1.1. HTTP/2-only
endpoints are unsupported.

=== File Tools
<agent-file-tools>

The file tools take the same arguments and return the same texts locally and
on a remote workspace.

#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto, left),
    table.header([Tool], [Arguments]),
    table.hline(),
    [`read_file`],
    [`file_path`; `offset` (first line, default 0); `max_lines` (default 500).
     Text past the window ends with a `[truncated: ...]` line naming the next
     offset. Images (PNG, JPG, GIF, WebP) return as image content. One read
     keeps at most 16 MiB: a longer single line or image is refused],
    [`list_files`],
    [`directory` (default: the working directory); `pattern` (glob on the
     entry name); `recursive`; `include_hidden`; `limit` (default 1000).
     Sorted, directories end in `/`. A listing cut at `limit` ends with a
     `[truncated: ...]` line. A recursive listing does not follow symbolic
     links],
    [`write_file`],
    [`file_path`, `content`. Creates missing parent directories],
    [`edit_file`],
    [`file_path`, `old_string`, `new_string`; `replace_all` (default false).
     `old_string` must be unique unless `replace_all` is set, and must differ
     from `new_string`],
  )],
  caption: [FILE TOOLS],
  kind: table,
)

File tool paths follow one rule on every machine:

- A relative path resolves against the working directory that `bash` runs in
  (`/cwd`, or `path_context` with `set_working`).
- `~` and `~/x` name the home directory of that machine, the one `bash`
  uses: locally `$HOME` when it is set, else the user's home directory.
  Remotely it is the login directory the SSH server reports.
- On a Windows machine, `C:\x`, `C:/x`, the Git Bash form `/c/x` and the SFTP
  form `/C:/x` name the same file, as does the workspace root in the spelling
  Git Bash reports for it. Paths compare without case.

== Constraint Solving
<agent-smt>

`z3_solve` accepts literal SMT-LIB declarations and assertions on Linux, Windows, and macOS. Each task runs in a separate local `qsoc-smt-worker` process, which exits after its result. Calls from remote workspaces also solve locally. The tool does not read project or remote files.

`mode="check"` returns satisfiability, an optional model, and an optional unsat core for named assertions. `mode="optimize"` accepts 1 to 16 `minimize` or `maximize` objectives. Objectives use lexicographic order: earlier objectives take priority. Optimize rejects quantified formulas. Pareto, box, soft constraints, and recursive definitions are unsupported.

```json
{
  "mode": "optimize",
  "priority": "lex",
  "smtlib": "(declare-const x Int)(declare-const y Int)(assert (and (>= x 0) (>= y 0) (>= (+ x y) 7)))(minimize x)(minimize y)"
}
```

The response separates execution, satisfiability, feasibility, and optimality. A feasible model does not establish an optimum. Nonlinear or unsupported certification theories report `not_proven`. Objective bounds preserve exact rationals, infinity, and infinitesimal epsilon terms. A strict bound can describe a limit that no feasible model attains. Check `optimality`, each objective's `classification`, and `bounds_proven` before treating a model as optimal.

`timeout_ms` defaults to 10000 and accepts 1 to 120000. The deadline includes worker startup, parsing, solving, verification, and output. Cancellation and resource limits have separate execution states. An unknown result does not establish unsatisfiability.

Requests accept at most 256 KiB of SMT-LIB. Output is limited to 1 MiB. Each daemon runs at most two SMT workers concurrently across its sessions and direct clients. Waiting requests share a queue of at most 64 tasks and 16 MiB of serialized input. Each session or direct client connection can own at most 16 running and waiting tasks combined. Requests from different owners take turns as worker slots become available. These budgets apply to one daemon, not the entire machine.

A full queue returns `busy`. A request that remains queued for 120 seconds returns `timeout`. Queue waiting is separate from `timeout_ms`. Cancelling a running task stops its worker. The daemon reuses the slot only after that process exits. A failed worker does not terminate other workers or the agent.

Before launching an SMT worker, the daemon checks a fresh memory sample against its active worker reservations and configured host reserve. Reservations remain until the worker exits. Linux uses visible cgroup headroom when available. Other known host samples provide partial coverage. Unknown measurements retain the existing worker limits unless strict sampling is enabled. Waiting for memory does not extend the queue deadline.

`qsoc-agentd --smt-memory-reserve-mib <n>` sets the extra host reserve, defaulting to 512 MiB. `--smt-memory-strict` requires a fresh effective-memory sample before admission. Platforms without that sample wait until the queue deadline. These settings belong to the daemon owner and cannot be changed through a tool request.

Admission is a conservative estimate. Other applications and separate daemons can allocate memory after sampling. Disk snapshots do not prevent a filesystem from filling during a write. Worker memory limits depend on the platform:

#figure(
  align(center)[#table(
    columns: (0.25fr, 1fr),
    align: (auto, left),
    table.header([Platform], [Worker limit]),
    table.hline(),
    [Linux], [512 MiB of total virtual address space],
    [Windows], [512 MiB of committed process memory],
    [macOS], [512 MiB above the worker's initial virtual address space],
  )],
  caption: [SMT WORKER MEMORY LIMITS],
  kind: table,
)

These limits measure different resources and are not a shared resident-memory limit. Z3 also has an internal 448 MiB allocation limit. The worker refuses to solve if the operating system cannot enforce its process limit. Older macOS versions that do not enforce the address-space limit cannot run SMT tasks.

== Code Intelligence
<agent-lsp>
The agent reads HDL through a Language Server Protocol layer. A built-in slang
backend handles `.v`, `.sv`, `.svh`, and `.vh` in process, so nothing has to be
installed for it to work. After the agent writes or edits a file, that file is
re-sent to the backend, so the next `lsp diagnostics` call sees the current
text.

The `lsp` tool takes an operation and a file, plus a 1-based position for
everything except diagnostics:

#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto, left),
    table.header([Operation], [Returns]),
    table.hline(),
    [`diagnostics`], [Errors and warnings for the file; needs only `file_path`],
    [`definition`], [Declaration site of the symbol under the position],
    [`hover`], [Type and documentation for the symbol under the position],
    [`references`], [Every use of the symbol under the position],
    [`symbols`], [Outline of the file: modules, ports, parameters, nets],
  )],
  caption: [LSP OPERATIONS],
  kind: table,
)

A file type with no registered backend answers with an explicit error rather
than an empty result.

External language servers are added under `lsp.servers`. Each entry needs a
`command` and an `extensions` list; `args` is optional. A server registered
this way takes over the extensions it claims, including from the built-in
backend:

```yaml
lsp:
  servers:
    verible:
      command: verible-verilog-ls
      extensions: [".v", ".sv"]
```

Linting is an agent-side capability. There is no `qsoc lint` command, the GUI
does not surface diagnostics, and `generate verilog` does not lint its own
output; it runs the netlist checks described in @validation-format instead.
Modules brought in with `module import` are parsed by slang, so import is the
one place where a plain CLI user sees HDL diagnostics.

== Skills
<agent-skills>
A skill is a `SKILL.md` markdown file with a YAML frontmatter block. Skills
extend the agent without code changes. They are discovered across the four
configuration layers (see @config-files) plus any directory listed in the
`QSOC_SKILLS_PATH` environment variable. Same-name skills in higher layers
shadow lower ones.

In remote mode the remote workspace adds its `.qsoc/skills`, read over SFTP,
and the local project's `.qsoc/skills` stays available. The first layer that
holds a name wins, in this order:

#figure(
  align(center)[#table(
    columns: (0.25fr, 1fr),
    align: (auto, left),
    table.header([Layer], [Directory]),
    table.hline(),
    [`env`], [`$QSOC_HOME/skills`],
    [`remote`], [`.qsoc/skills` in the remote workspace (remote mode only)],
    [`local`], [`<project>/.qsoc/skills` in the local project],
    [`user`], [`~/.config/qsoc/skills`],
    [`system`], [The platform system skills directory],
    [`extra`], [Each entry of `QSOC_SKILLS_PATH`],
  )],
  caption: [SKILL LAYERS],
  kind: table,
)

Listings from `skill_find` and the system prompt show the layer of each
skill. `skill_find` takes a layer as `scope`, or `project` for the bound
workspace: `remote` in remote mode, `local` otherwise. A remote `SKILL.md`
loads only when it is at most 256 KiB and its resolved path is inside the
workspace.

`skill_create` takes the same three scopes in both modes:

#figure(
  align(center)[#table(
    columns: (0.25fr, 1fr),
    align: (auto, left),
    table.header([Scope], [Writes to]),
    table.hline(),
    [`project`], [`.qsoc/skills` of the bound workspace: the remote workspace in
     remote mode, the local project otherwise],
    [`local`], [`<project>/.qsoc/skills` in the local project],
    [`user`], [`~/.config/qsoc/skills`],
  )],
  caption: [SKILL_CREATE SCOPES],
  kind: table,
)

=== File Layout
<agent-skill-layout>
Each skill lives in its own directory:

```text
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

In remote mode both name the remote workspace: `${CWD}` is the remote working
directory and `${PROJECT}` is the remote workspace root.

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

== Tool discovery

`agent.tool_presentation` and `--tool-presentation` accept `direct`, `catalog`, or `auto`. The default is `direct`, which sends all authorized tool definitions. Resumed sessions retain their saved setting unless an explicit configuration or command-line option overrides it.

`catalog` keeps common file, shell, user, and completion tools visible. `tool_catalog` searches authorized names and descriptions or returns an exact tool schema. `tool_invoke` uses that schema version and a JSON object encoded in `arguments_json`. Discovery does not grant permission. Permissions and workspace continuity are checked again before execution.

`auto` uses the catalog when specialized tool definitions exceed 8192 estimated tokens. Smaller catalogs use direct definitions. Tool results, described schemas, and wrapper arguments still contribute to context usage.

Input replay waits for confirmation when pre-tool hooks are configured, because their side effects are uncertain.


== Reasoning Effort
<agent-effort>
The `--effort` option and the `/effort` command set the reasoning effort
sent to the current model as `reasoning_effort`, or as
`output_config.effort` with adaptive thinking for an `anthropic-messages`
entry. `off` sends nothing.
Switching models with `/model` resets the level to that entry's `effort`
default. To pair a fast model with a reasoning model, declare both under
`llm.models` and switch between them.

The receiving side always reads reasoning from the SSE stream, regardless of
the `--effort` setting. The first non-empty field of `reasoning_content`,
`reasoning`, and `reasoning_text` is used, then the text of
`reasoning_details`. Reasoning output is displayed in dim text. The history
keeps the reasoning under the field name the server used, so the next
request of a tool loop sends it back unchanged. A model entry with
`reasoning: false` never receives an effort.

== Saved Tool Results

Large tool returns enter history as their start and end, with a note at the
cut that names the `artifact_id`.
`tool_output_read` reads the captured text with UTF-8 byte offsets. Continue
at `next_offset` until `eof` is true. Each page contains complete characters.
Repeated reads do not create new artifacts.

The capture contains the text that reached the agent. A source tool can
truncate its output before returning it. `bash` and a local `!` line keep the
first and last 2 MiB of output longer than 4 MiB. A remote `!` line, and remote
`bash` in a workspace where it cannot create job directories, keep the first
and last 1 MiB of each stream longer than 2 MiB. The cut reads
`[... N bytes omitted ...]`. A return larger than `agent.tool_artifact_bytes`
is saved the same way. `source_completeness` is `truncated` when the saved
text has such a cut or the source reports truncation, and `unknown`
otherwise. `captured_bytes` describes the saved return, not the original
command output. Images remain separate attachments.

After per-image processing, a tool batch admits new images within the
remaining request context and a combined 16 MiB of encoded data URLs. This
is a local payload bound, not a provider capacity guarantee. The context
check reserves up to 4096 tokens for each outstanding tool result, including
its existing placeholder once. Unknown image token costs are rejected.
A rejected image leaves a reason in the tool result and does not create an
image artifact. Its tool-reported source may be read again if needed and
still accessible. Earlier conversation content stays unchanged.

The defaults are 16 MiB per artifact, 256 MiB per session, and 32 KiB per
read. Configure `agent.tool_artifact_bytes`, `agent.tool_artifact_session_bytes`,
and `agent.tool_artifact_page_bytes` in bytes. The reader also limits pages
to the remaining context budget. Quota or write failures leave existing
artifacts readable and report that the new return was not saved.

The C++ agent API starts with temporary result storage. Applications that
resume history across processes bind a durable directory with
`bindToolResultStore`. The command-line agent binds storage when a new session
first writes history, or when it resumes an existing session.

Artifacts belong to the local session, including when tools run remotely.
A fork or `/branch` receives independent copies of only the artifacts
referenced by its inherited history. It cannot read later parent artifacts
or sibling artifacts. Deleting the parent's records leaves those copies
readable. These permissions govern application tools, not arbitrary shell
access by the same operating-system user.

Each tool batch reserves a result for every call ID before executing tools.
Result bodies use at most 4096 estimated tokens and must fit the remaining
input window. The agent stops when another result cannot fit. Unexecuted
calls receive an explicit result. Completed side effects are never replayed
automatically after a capture failure.

== Context Compaction
<agent-context-compaction>
The context budget is the smaller of `agent.max_tokens` and the model's context window, minus the model's output limit. When the conversation passes a share of that budget, qsoc shrinks it in the two steps below.

qsoc checks the thresholds before each model request in a turn and again at the prompt after a turn. A turn continues its task after a compaction.

The status bar shows the context use as `[ctx N%]`. In the last 15 points before the summary threshold it reads `N% to compact`. It reads `over threshold` while the context stays above the threshold, for example when no compaction could make the history smaller. `≈` before the percentage marks a value that includes tokens counted locally with the model's `tokenizer` setting (@llm-token-counting). Compaction can start before the chip reaches a threshold.

#figure(
  align(center)[#table(
    columns: (auto, 1fr),
    align: (auto, left),
    table.header([Step], [Effect]),
    table.hline(),
    [Tool output pruning],
    [Above `agent.prune_threshold` (default 0.4), older tool outputs become `[output pruned]`. A saved result becomes `[output pruned; read artifact <id> with tool_output_read]` and keeps its `artifact_id`.],
    [Summary],
    [Above `agent.compact_threshold` (default 0.6), the model replaces older messages with one summary. Recent messages stay unchanged.],
  )],
  caption: [Compaction steps],
  kind: table,
)

#figure(
  align(center)[#table(
    columns: (auto, 1fr),
    align: (auto, left),
    table.header([Action], [Effect]),
    table.hline(),
    [`/compact`], [Compact now, also below the thresholds. It also retries after a failure.],
    [*Esc* or *Ctrl+C*],
    [Cancel a running compaction, manual or automatic. The history is unchanged. Input typed meanwhile runs after it ends.],
    [`/context`], [Show the token use per category.],
    [`/clear`], [Start a fresh session. `/resume` returns to the old one.],
    [`agent.compaction_model`],
    [Use another `llm.models` key for summaries. Empty uses the current model and effort. An unknown key makes compaction fail.],
  )],
  caption: [Compaction controls],
  kind: table,
)

=== What the Summary Keeps

Each summary builds on the previous summary. The model is asked to keep each todo item with its ID and latest status, concrete values such as numbers, addresses, names, paths, and commands with their source, each user requirement in its latest form, each added or removed constraint, and each decision with its reason. The summary does not keep every message word for word and can omit details.

qsoc saves the removed messages as read-only artifacts and lists their IDs with the summary. The agent can read them with `tool_output_read` when that tool is allowed. Background memory extraction still reads messages that compaction removed before extraction ran.

qsoc uses the new history only when it is smaller and fits within 90% of the context budget. Otherwise the history stays unchanged. The CLI saves the session before it switches, so `--resume` after an exit finds either the old or the new history. This does not cover power loss.

=== Context-Length Recovery

When the provider rejects a request as too long, qsoc compacts and sends the request again. A generic HTTP 413 or an image-size error does not start this. If the summary request fails here, qsoc uses a mechanical summary: the previous summary, the latest user request, and one shortened line per older message. It prints the reason in one dim line and the turn continues. A cancelled compaction does not fall back. At the thresholds and with `/compact`, a failed summary leaves the history unchanged. A conversation too large for one summary request always gets the mechanical summary.

#figure(
  align(center)[#table(
    columns: (1fr, 0.6fr),
    align: (left, left),
    table.header([Message], [Meaning]),
    table.hline(),
    [`Compacted: saved N estimated input tokens.`], [The new history is in use.],
    [`Compaction kept the history: no smaller candidate fits the context budget.`],
    [Nothing changed. Automatic compaction waits until the history changes.],
    [`Compaction cancelled. The history is unchanged.`], [*Esc* or *Ctrl+C* stopped it.],
    [`Compaction failed. The history is unchanged.`], [A summary request or a save failed.],
    [`Summary request failed (<reason>): used a mechanical summary.`],
    [See the context-length recovery above.],
  )],
  caption: [Compaction messages],
  kind: table,
)

During a turn the status bar reads `Compacting L1` after pruning or `Compacting L2` after a summary, with the token counts before and after.

=== Restored Context

After a summary, qsoc restores bounded context and prints these entries:

+ `Read <path> (N lines)` for the most recently read files small enough to
  re-inline their current content.
+ `Referenced file <path>` for a recently read file too large to re-inline. Re-read it with `read_file` when needed.
+ `Skills restored (...)` for the skills invoked this session, whose bodies
  are put back.
+ a running-background-agent line for each sub-agent still executing.

At most `agent.context_restore_max_files` files (most recent first) and the recently invoked skills are restored, each capped per item and by an overall token budget. Files still present in the kept messages are not restored. Memory is supplied separately on each request. Disable with `agent.context_restore: false`.

== Memory System
<agent-memory-system>
Persistent memory is stored as topic files with YAML frontmatter in two
scopes: user-global (`<user root>/memory/`) and project-local
(`<project>/.qsoc/memory/`), each with an auto-maintained `MEMORY.md` index.
The three mechanisms below are on by default; their knobs are in the agent
configuration table.

In remote mode the project scope is a store on this machine, one per remote
workspace: `<user root>/remote-memory/<key>/`, where `<key>` is derived from
the SSH endpoint (user and host key) and the workspace path. Only the user
can read the directory and its files. Nothing is written to the remote host.
Recall, extraction and consolidation use the same store, and `/local` returns
to `<project>/.qsoc/memory/`. A sub-agent uses the store of the workspace it
runs on: a child dispatched with `host` uses that workspace's store, and a
child sent to `local` from a remote session uses `<project>/.qsoc/memory/`. Project memory that earlier versions saved
during remote sessions stays in the local project's `.qsoc/memory/` and is
not shown in remote mode.

=== Selective Recall
<agent-memory-recall>
Each turn the agent ranks the topic-file headers (name, type, age,
description) against the current query and injects the relevant files as a
reminder for that turn. A topic already recalled in the current context is
not injected again. When there are no more topics than
`agent.memory_recall_max_files`, the selector call is skipped and they are
all injected. Each file is capped (`agent.memory_recall_per_file_cap`) and
the per-turn total is bounded (`agent.memory_recall_turn_budget`); recalled
files carry a freshness note. With `agent.memory_recall: false`, the full
`MEMORY.md` index is injected instead.

=== Background Extraction
<agent-memory-extraction>
After each turn a child agent restricted to `memory_read` / `memory_write`
distills new durable facts into memory files. Trivial turns (fewer than
`agent.memory_extract_min_messages` new messages) and turns where the agent
already saved memory are skipped. It does not save anything derivable from
code or git, debugging recipes, ephemeral state, or secrets.

=== Consolidation (Dream)
<agent-memory-dream>
Periodically a child (also allowed `memory_delete`) merges near-duplicate
topics, normalizes dates, and drops contradicted facts. It runs at most once
per `agent.memory_dream_min_hours` and only after
`agent.memory_dream_min_sessions` new sessions.

=== Model for Memory Work
<agent-memory-models>
Recall, extraction, and consolidation run on the selected model and
effort. `agent.memory_recall_model`, `agent.memory_extract_model`, and
`agent.memory_dream_model` each name an `llm.models` key to use instead;
empty keeps the selected model.

```yaml
agent:
  memory_recall_model: other-model
  memory_extract_model: other-model
  memory_dream_model: other-model
```

== Sub-agents
<agent-subagents>
A sub-agent is a child run with its own message history, its own tool
allowlist, and its own system prompt. The parent dispatches through the
`agent` tool; the child's final output is returned as the tool result.
Sub-agents cannot prompt the user, control plan mode, or complete the parent
goal; they return questions, findings, and proposed plans to the parent.

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
    [`true` returns a `task_id` immediately and the child runs detached.
     `false` (default) waits for the child, but a foreground run that
     exceeds the auto-background timeout is detached automatically and
     its result is delivered later as a notification.],
    [`isolation`],
    [`worktree` runs the child in its own `git worktree --detach` under
     `<runtime>/qsoc-worktrees/<task_id>`. Default is none. Refused with an
     error when the parent or the `host` is a remote workspace.],
    [`host`],
    [Where the child runs. Omitted: where the main agent works now.
     `local`: this machine. Any other value is a host alias
     (@agent-dispatch).],
    [`workspace`],
    [Absolute directory on a named `host`. Required when the host has no
     catalog workspace. It must exist, must not be `/` or the login
     directory, and must stay inside the granted workspace when the host
     grant names one.],
    [`model`],
    [An `agent.dispatch.models` key. Omitted: the main agent's model, or the
     model the host or the definition names.],
  )],
  caption: [`agent` TOOL FIELDS],
  kind: table,
)

`description` and `prompt` are required. Sub-agent concurrency is
unbounded by default (`agent.max_concurrent_subagents` /
`QSOC_MAX_CONCURRENT_SUBAGENTS` = `0`): every spawn runs at once and
flow control is left to the provider's HTTP 429 backpressure, which the
agent loop retries with exponential backoff and jitter. Set a positive
value to re-bound for a strict single-key provider; spawns past the cap
are then queued (status `queued`) and admitted FIFO as each slot frees,
forming a sliding window. The foreground auto-background timeout in
milliseconds (`agent.auto_background_ms`, env `QSOC_AUTO_BACKGROUND_MS`,
default `120000`, `0` disables) is independently configurable.

When a detached child reaches a terminal state, the parent receives a
`task-notification` carrying the status, a capped result body, and the
transcript path; it is injected at the next turn boundary, never
interrupting an in-progress turn. An idle main agent starts a turn for it
(@agent-task-wake).

=== Dispatch Resources
<agent-dispatch>

`agent.dispatch` names the hosts and models the main agent may give a
sub-agent. Unset fields of a call inherit the main agent.

```yaml
agent:
  dispatch:
    hosts:
      sim1:                     # ~/.ssh/config alias or host catalog alias
        workspace: /work/proj   # optional; a named workspace stays inside it
        model: pro              # optional; every child on sim1 runs on pro
      local:
        model: omni             # optional; children on this machine
    models: [pro, omni]         # llm.models keys a call may name
```

#figure(
  align(center)[#table(
    columns: (0.25fr, 1fr),
    align: (auto, left),
    table.header([Field], [Resolution]),
    table.hline(),
    [`host`],
    [Omitted: the main agent's binding. `local`: this machine. Any other alias
     must be in `hosts`; without `hosts`, it must be a host catalog alias.],
    [`workspace`],
    [The call's `workspace`, else the grant's `workspace`, else the catalog
     workspace.],
    [`model`],
    [The host's `model` when set (a different `model` in the call is
     refused), else the call's `model` (must be in `models`), else the
     definition's `model`, else the main agent's current model.],
    [effort],
    [The main agent's current effort, or the `effort` of the child's model
     entry when the child runs on another model and the entry sets one.],
    [context],
    [The child's model entry. The same model keeps the main agent's window.],
  )],
  caption: [DISPATCH RESOLUTION],
  kind: table,
)

`hosts` and `models` apply independently. An entry with an unknown field, a
model key that is not in `llm.models`, or an alias that is neither in
`~/.ssh/config` nor a catalog entry with a `target` is refused with a warning
at startup, and a call that needs it is refused with the same text. An
unknown key under `agent.dispatch` makes `hosts` count as declared, so only
`local` and the main agent's binding stay usable. A fork keeps the main
agent's model: a fork whose `model` or host model differs is refused.

The agent tool lists the granted hosts and models, and the system prompt has
a `Dispatch resources` section with each host's catalog capability and bound
model and each model's name and context window. Neither names a URL, key,
`HostName` or `User`.

The policy is read when the session starts. `/dispatch` shows it, and
`/dispatch reload` reads the config again; a config file changed during the
session has no effect until then.

Every model is called from this machine, whatever host the child runs on.
`agent.dispatch` limits where the `agent` tool sends children. It is not a
sandbox: the shell can still reach any host the user can.

=== Fork Mode
<agent-subagents-fork>

When `subagent_type` is empty or set to `fork`, the child inherits the
parent's full message history up to the spawn point. The fork point is
marked with the `<!-- qsoc-fork-tag -->` HTML comment so further `agent`
calls inside the child cannot recurse into another fork from the same
anchor.

The child reuses the parent's identity and tool restrictions. It builds environment and project rules once for its own workspace. A legacy full prompt override is not split into sections. Forks rebuild the default identity in that case. Critical reminders, plan mode, and the approved plan remain system instructions.

Unchanged prompt sections stay stable across turns. Workspace, permissions, model, or runtime reminders can change the request prefix. Provider caching depends on the endpoint and request contents. A fork does not guarantee a cache hit.

`/cache` reports the current LLM service's text, chat, and streaming calls.
A service call is one API invocation, not a user turn. Caller retries are
separate calls. Network attempts count actual sends. Token-count requests
and independent service clones are outside this report.

The report distinguishes missing, invalid, and reported token counts.
A reported zero means zero. A missing total is `null`, not zero. Cache ratios
use only calls that report both valid input and cache-read counts. Usage
from an interrupted response can be incomplete and is counted separately.
First-byte latency measures network response data, not the first model token.

Prefix changes compare consecutive requests within each request type. They
describe local changes, not the provider's cache decision. Comparisons retain
only private in-memory fingerprints, with a fresh random key per collector.
The report contains no prompt text, tool results, credentials, fingerprints,
or endpoint names. Comparisons stop at 4096 messages or 8 MiB of serialized
parts, 65536 values, or 64 nested levels and report `unobserved` beyond those
bounds. The size check runs before serialization and can conservatively
reject content near the limit. Statistics stay in memory
until the service exits. Querying them makes no model request and does not
add anything to the conversation.


Remote bindings load `AGENTS.md` and `AGENTS.local.md` through SFTP when bound and again on reconnect, under the same rules as local project instructions (see @agent-system-prompt). Each file prints one line, for example `Loaded AGENTS.md from <target>:<workspace> (812 bytes)`, or the reason it did not load. The local project's instructions stay in the prompt, and its skills stay available next to the remote workspace's (see @agent-skills). Remote `.qsoc/agents/*.md` definitions follow the same rules, with `.qsoc/agents` as the root; a refused definition is not registered.

=== Definitions
<agent-subagents-defs>

Four scopes exist, in shadowing order from highest to lowest:

- *Remote*: `.qsoc/agents/*.md` under the remote workspace, read over SFTP.
  Present only in remote mode. `/local` removes them.
- *Project*: `<project>/.qsoc/agents/*.md` in the local project, in both
  modes.
- *User*: `~/.config/qsoc/agents/*.md`
- *Builtin*: compiled in. The shipped names are `general-purpose`
  (full tool set), `explore` (read-only), and `verification` (adds
  `bash` and `lsp`).

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
    [Allowlist restricted by the parent's allowed tools. Empty inherits
     the parent set.],
    [`disallowed_tools`],
    [Additional denied tools. Parent denials still apply.
     Pipe-separated alternates.],
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
    [`true` adds the skill listing. Default `false`. With `host`, this
     listing and a fork's hold the user skills and the project skills of that
     workspace.],
    [`inject_project_md`],
    [`true` (default) injects `AGENTS.md`. `false` skips it.],
    [`model`],
    [An `llm.models` key the child runs on, with that entry's `context`
     and `effort` (the parent's effort when the entry sets none). Empty
     inherits the parent; an unknown key fails the spawn.],
  )],
  caption: [SUB-AGENT DEFINITION FRONTMATTER],
  kind: table,
)

The body of the file is the child's base system prompt.

=== Runtime State
<agent-subagents-state>

Runs belong to the session that started them and are stored next to it in
`.qsoc/sessions/<id>.jsonl.agents/`. Task ids are unique within a session: a
resumed session continues after the highest id it already holds. Each run
produces:

- `<task_id>.jsonl`: structured event stream (one JSON event per line:
  prompt, tool calls, tool results, content chunks, final output).
- `<task_id>.meta.json`: sidecar with label, `subagent_type`, status,
  isolation mode, worktree path, `host` (`local` or the alias the run was
  sent to), `endpoint` (the SSH target behind that alias), `workspace` and
  `model` (the model the call named, absent when omitted). It is replaced
  atomically.
- `<task_id>.history.jsonl`: the child's message history in the session file
  format, written when the run ends. A history larger than 16 MiB is not
  kept. The meta names it in `history_file`.

`/branch` copies the directory to the new session. Without a session, runs
stay in memory and nothing is written.

qsoc 2.7.0 and earlier kept every session's runs in
`<runtime>/qsoc/agents/`. Those runs stay readable by `/agents-history`
(marked `legacy`), `agent_status` and `agent_resume`, and are never written.
A run of the current session wins over a legacy run with the same id.

At startup, runs whose meta says `running` but whose owning process is
gone are rewritten to `aborted`, and worktrees older than one hour
without a live owner are swept.

=== Polling and Resuming
<agent-subagents-poll>

While a backgrounded run is alive:

- `agent_status` returns the current status, transcript tail, and the
  final output once the run completes. A run that was cut off before it
  reported anything, by `x` in the task panel or by a parent ESC, ends
  as `aborted` rather than `failed`: it ran for an unknown distance, so
  its side effects are unknown. `failed` means the run itself reported
  an error. The status word is the same in the meta sidecar, the task
  notification and the task panel.
- The legacy `send_message(task_id, message)` form accepts messages only
  while that run is running.
- `/agents-history` lists prior runs with their final results.
- `agent_resume(task_id, new_instructions?)` continues a run. A live child
  receives the instructions as a follow-up (`resume: live`, or `queued` while
  it is busy). A finished child whose history is stored is rebuilt from that
  history, with the same definition, host and workspace, and runs in the
  background (`resume: history`); its result arrives as a task notification.
  A model the earlier call named, or the host's bound model, is resolved
  again through `agent.dispatch` and the resume is refused when it is no
  longer granted; otherwise the rebuilt child uses the current model and
  reasoning effort. It returns to the stored alias and workspace, even when
  the catalog workspace changed since; when that alias now reaches another
  SSH target, the resume is refused. Without instructions the child is asked to
  continue, or to report the result if it is done.
- When neither applies (no stored history, a `legacy` run, a worktree run),
  `agent_resume` reads the meta sidecar plus the transcript tail and returns
  a synthesized `resume_prompt` (`resume: prompt_only`) that can be passed to
  a fresh `agent` call, with the run's `host`, `workspace` and `model`.

=== Peer Communication
<agent-subagents-messages>

`agent_list` lists the main agent and its session peers. Each has a stable
`agent_id`, a display name, a current `task_id`, and a state: `pending`,
`running`, `idle`, `cancelled`, or `closed`. Address peers by `agent_id`
or a task alias such as `a1`; names are display labels. The `main` alias
addresses the main agent, whose `agent_id` is the session ID and stays the
same when the session is resumed. A follow-up keeps the agent identity and creates
a new task ID when it wakes an idle child.

- `send_message(target, message_id, message, reply_to?)` queues information.
  An idle recipient stays idle; a running recipient reads it at an input
  boundary. The sender comes from the calling agent, and peer content
  does not grant user approval or change permissions.
- `followup_task` accepts the same fields and wakes an idle child using its
  existing history, model, reasoning effort, and workspace. A running or
  pending child receives the request in its current run. Final output is
  returned as a reply correlated to the request unless the child already
  replied explicitly. The task transcript holds the full result.
- `agent_inbox(from?, reply_to?, peek?)` reads pending messages. `peek=true`
  leaves them queued. `wait_agent(from?, reply_to?, timeout_ms?)` waits for
  matching messages, up to 60 seconds (30 seconds by default). Unmatched
  messages remain queued; a timeout neither retries nor cancels a request.
- `interrupt_agent(target)` lets the main agent cancel a child and discard
  its pending messages. Later messages cannot revive that child. Task-panel
  cancellation has the same effect. Start a fresh child for further work.
- Esc on the main agent does not cancel its mailbox. Replies and task
  notifications that arrive while it is idle stay queued and reach it on
  its next turn.
- A reply to a request the main agent sent starts a turn for an idle main
  agent (@agent-task-wake). Other messages to the main agent wait for its
  next turn.

`send_message` accepts four target forms:

```json
{"target":"a1","message_id":"note-1","message":"Review the interface."}
{"target":{"agents":["a1","a2"]},"message_id":"note-2","message":"The schema changed."}
{"target":{"group":"workers"},"message_id":"note-3","message":"Review the shared schema."}
{"target":{"broadcast":"session"},"message_id":"note-4","message":"The workspace is unavailable."}
```

`agent_list` reports runtime groups. The `workers` group contains the
session's children. Group sends exclude the sender and closed members.
Broadcast reaches permitted peers in this session. Explicit address sets
reject unknown or unauthorized targets before sending anything. Group
messages do not wake idle members or assign task ownership.

The first accepted request freezes its recipients. New members do not
receive an old broadcast when the sender retries it. Full individual
mailboxes can produce partial delivery. The receipt reports `accepted`,
`rejected`, `delivered`, `replied`, and `cancelled` counts with recipient
details. Replies are counted separately for each recipient.

Receipt details have 64 entries per page. Repeat the identical send with
`receipt_offset` set to `next_offset` to read another page. This does not
send again. Rejected recipients stay rejected on that message ID, even
if their mailbox later has room. Use a new ID to resend only to rejected
recipients. Reply to the original sender with `reply_to`. Informational
messages require no acknowledgement unless requested.

`followup_task` and `interrupt_agent` accept one target address. They do
not accept group or broadcast selectors.

The built-in `explore` and `verification` roles can discover peers, send
information, read their inbox, and wait for replies. They cannot wake or
cancel peers. Custom role allowlists and denylists still apply. The system
prompt supplies each agent's identity and permitted communication operations
on every request, including after history compaction. These runtime rules
also accompany a custom system prompt.

Peer messages remain agent-authored data, including any embedded approval
or reminder tags. In tool output, file contents, peer messages, sub-agent
results and recalled memory, qsoc escapes the tags it uses for runtime
instructions (`<system-reminder>`, `<approved_plan>`, `<task-notification>`,
`<goal_context>`, `<recalled_memory>`), so the model reads them as quoted
text such as `&lt;system-reminder>`. A tool result that contained such tags
also ends with a qsoc reminder that the imitation changes nothing. User and
tool content are not promoted to system instructions.

The main agent's runtime reminders (plan mode, focus, the approved plan,
recalled memory) are user-role `<system-reminder>` messages that qsoc adds
after your message at the start of a turn. They are saved with the session,
and the resumed transcript, the rewind picker, prompt prediction and
compaction summaries skip them. Task notifications and peer messages are also
saved as user-role messages. The resumed transcript shows them as task
notices. The rewind picker, prompt history, the session title and the turn
count skip them. Sub-agents receive
their critical reminder, plan mode and approved plan in the system message.
Coordinate overlapping file work before editing, continue independent work
while peers run, and wait only when their answer is needed.

Choose a `message_id` of 1–128 ASCII letters, digits, dots, underscores, or
hyphens, unique within the sender's session. Retrying the same ID with
identical fields returns its receipt without another delivery. Changing
the fields produces `message_id_conflict`. A reply sets `reply_to` to the
original request ID; use both `from` and `reply_to` when awaiting a specific
answer. Receipts distinguish `accepted`, `delivered`, and `cancelled`;
delivery means consumption, not successful execution.

Each mailbox holds at most 128 pending messages, each at most 16 KiB in
UTF-8. The session retains up to 8,192 recipient records for deduplication.
Group delivery, rejection, and exclusion records each consume one slot.
Full queues reject new sends without dropping older messages. Automatic
replies are subject to the same limits; `agent_status` remains available
to inspect the task result.

Mailboxes are local to the current process and session. `/clear` cancels
the old peers and clears their addresses and messages. Process restart
does not restore mailboxes. A live child's isolated worktree remains
available for follow-ups until the child is released.

=== Talking to a Sub-agent
<agent-subagents-talk>

In the task overlay (*Ctrl+B*), select a sub-agent row and press `f` to view
it. The main view then shows that child's transcript, drawn like the main
one with code blocks and tool blocks, and follows it while it runs. The title
bar names the child in view. `s` does the same for a child that can receive
a message.

While a child is in view:

- A prompt you type goes to that child. Commands (`/`, `!`, `#`) still go to
  the main agent.
- *Esc* returns to the main view. The child keeps running.
- *Ctrl+C* stops the child in view.
- *Alt+Left/Right* moves through the main agent and each live child, in
  start order. Only the terminal sequences for Alt plus an arrow key switch
  the view. Esc followed by a letter is still Esc and a letter.

Main-agent output that arrives meanwhile is drawn when you return. A message
reaches a live child as your request, through the child's own
`user_prompt_submit` hooks. An idle child starts a follow-up run with its
history, model, reasoning effort and workspace; a running child reads the
message at its next step. A finished child whose history is stored, for
example one stopped with `x`, is rebuilt from that history as `agent_resume`
does, and the view follows the new run. This waits until the main agent's
turn ends. The main agent is not notified of messages you send or of the
result of a child you resumed. A message is limited to 16 KiB, like peer messages.

These features need a daemon that advertises the `agents` capability in
its greeting. Against an older daemon the `f` and `s` keys and the
Alt+arrow switch are not offered. The protocol methods are:

- `tasks` rows for sub-agents carry `agent_id`, `host`, `workspace`, `live`
  (the child accepts a message now) and `resumable` (a message reaches it,
  directly or by resuming from the stored history).
- `task_tail` with `offset` returns `text`, `offset`, `next_offset` and
  `eof`. The offset counts UTF-8 bytes of the rendered transcript, and a page
  never splits a character. With `"format":"history"`, `offset` counts
  messages and the reply carries `messages` and `found` instead of `text`.
- `task_send` with `id` and `message` delivers the message. The reply has
  `ok`, `delivery` (`woken` or `queued`) and the `task_id` of the run that
  reads it (`delivery` is `resumed` for a rebuilt child). It never waits for
  the child and is refused while an SSH connection is being built. Resuming a
  finished child is refused while a turn runs.

== Status Line
<agent-status-line>
A user-supplied shell command can render an extra status row above the
built-in bar. Configure it in the user-level configuration:

```yaml
agent:
  status_line: "jq -r '.model.id + \" | \" + (.context.used_percentage | floor | tostring) + \"%\"'"
```

or, with an explicit timeout:

```yaml
agent:
  status_line:
    command: "~/.config/qsoc/statusline.sh"
    timeout_ms: 5000
```

The command runs through the platform shell (see @agent-shell-discovery)
with a JSON snapshot of the session on stdin: `model.id`, `effort`,
`workspace.cwd`, `workspace.project_dir`, `context.used_tokens`,
`context.max_tokens`, `context.used_percentage`, `tokens.input`,
`tokens.output`, `session.id`, and `version`. On exit code 0 the first
non-empty stdout line is displayed; ANSI SGR colors are honored. A
non-zero exit, empty output, or timeout clears the row. Refreshes fire
at startup, after each turn, and on `/model` or `/effort`, debounced so
rapid changes run the command once.
`provider_usage` and `summary_usage` carry the token counts that the provider
reported for normal and summary requests. A count the provider did not report
is `null`.

The key is read from the user and system configuration layers only. A
project `.qsoc.yml` cannot supply it: checking out a repository must
never execute code from it.

== Shell Selection
<agent-shell-discovery>
The local machine and every SSH host follow one rule. QSoC classifies the
machine as POSIX, Windows or unknown, then picks the executor that runs
`bash`, `bash_manage` and `monitor`, and the shell that runs `!`:

#figure(
  align(center)[#table(
    columns: (0.6fr, 1fr, 1fr),
    align: (auto, left, left),
    table.header([Machine], [`bash`, `bash_manage`, `monitor`], [`!<command>`]),
    table.hline(),
    [POSIX with bash], [`bash`], [The same executor],
    [POSIX without bash], [`sh`; tool descriptions say only POSIX sh is
      available], [The same executor],
    [Windows with Git Bash], [Git Bash (`bash.exe` from Git for Windows)],
      [`cmd /d /s /c "<command>"`, which receives the line unchanged],
    [Windows without Git Bash], [Not offered], [`cmd`, as above],
    [Unknown (remote only)], [Not offered], [The login shell, as typed, with
      no directory change and a notice],
  )],
  caption: [SHELL SELECTION],
  kind: table,
)

Local POSIX: `/bin/bash`, else `bash` on `PATH`, else `/bin/sh`. A remote
POSIX host runs `bash -l` or `sh -l` found on its own `PATH`.

A host is Windows when its login shell is cmd or PowerShell, or when its
`uname -s` starts with `MINGW`, `MSYS` or `CYGWIN`. Git Bash is looked
for in this order, locally and on a host:

+ The pinned path: `QSOC_GIT_BASH_PATH` locally, a `shell:` path in
  host.yml remotely. When it is set but missing, no shell is used.
+ Locally only, the Git for Windows layout around the `git` executable on
  `PATH`.
+ `bin\bash.exe` under `%GIT_INSTALL_ROOT%`, `%ProgramFiles%\Git`,
  `%ProgramFiles(x86)%\Git`, `%LOCALAPPDATA%\Programs\Git`, and scoop's
  `apps\git\current` (under `%SCOOP%` or `%USERPROFILE%\scoop`), then
  `usr\bin\bash.exe` under each.

`bash.exe` found on `PATH` or in `System32` is never used: it launches
WSL instead of a host shell. On a remote host the variables come from
the host and each candidate is checked over SFTP. Local executables
inside the current working directory are rejected, so a checked-out
repository cannot substitute its own shell. `QSOC_GIT_BASH_PATH` also
pins the local interpreter on Linux and macOS.

Git Bash takes POSIX paths: `C:\Users\me` is `/c/Users/me`. On a remote
host QSoC asks the host's `cygpath -u` for the workspace root once per
connection; when its answer differs from that form, the system prompt
names it. A remote Windows host starts Git Bash only when its login
shell is cmd or PowerShell; with any other login shell the tools are not
offered and `!` runs as on an unknown host. Background jobs on Git Bash have no boot identity, so
`bash_manage` may refuse to signal them.

The system prompt reports, in the same format locally and remotely, the
OS, architecture, shell and executor. With Git Bash it adds rules for
Windows: POSIX paths, Windows programs by name, CRLF line endings, no
`sudo`, and `cmd //c` for cmd builtins. When no shell is used, it says
why and the shell tools are not offered; everything else keeps working.

Output of `!` is read as UTF-8 when it is valid UTF-8. Otherwise local
Windows output is read in the OEM code page; output that is not
UTF-8 from a remote Windows host can show replacement characters.

Hooks and the status line run through the local executor.

== Background Tasks
<agent-tasks>
A unified task panel lists every long-running activity attached to the
current agent: backgrounded `bash` jobs (local and remote), scheduled
`/loop` prompts, and detached sub-agent runs. Monitors created by `monitor` also appear here
while they are alive.

=== Status Pill
<agent-tasks-pill>

While any task is in flight, a status pill is rendered just above the
prompt with the count and the most recent activity. *Down* on the
prompt's first row parks focus on the pill; *Enter* opens the overlay;
*Up* returns to the prompt. *Ctrl+B* opens the overlay from anywhere.

=== Overlay
<agent-tasks-overlay>

A compact activity grid appears above the prompt while tasks are active.
Its summary covers all tasks, including members outside the visible range.
Running tasks share an ASCII spinner. Queued tasks, peers waiting for messages, and terminal tasks stay still.
The summary separates running, waiting, queued, failed, stopped, and completed work.

The overlay selects a grid for a large task set on a wide terminal.
Press `v` to switch between the grid and table. Enter opens the selected
task's output. Task positions remain stable as statuses change.
Selection follows the task identity when rows reorder. If that task
disappears, select another row before requesting a stop.

#figure(
  align(center)[#table(
    columns: (0.25fr, 1fr),
    align: (auto, left),
    table.header([Key], [Action]),
    table.hline(),
    [`Up`/`Down`, `j`/`k`], [Move selection],
    [`Left`/`Right`], [Move between grid columns],
    [`v`], [Switch grid and table],
    [`m`], [Toggle task animation],
    [`e`], [Show or hide estimates],
    [`r`], [Refresh estimates],
    [`p` in details], [Show provisional numeric ranges],
    [`Enter`], [Open the task's detail tail],
    [`f`], [View the selected sub-agent (@agent-subagents-talk)],
    [`s`], [View the selected sub-agent and message it],
    [`x`],
    [Request stop or removal for the highlighted task],
    [`q`, `ESC`], [Return from details or close the overview],
  )],
  caption: [TASK OVERLAY KEYS],
  kind: table,
)

`x` sends `SIGTERM` to bash jobs, requests cancellation for sub-agents,
stops monitors, and removes a `/loop` job from the schedule. Cancelling a
sub-agent discards its queued input and leaves other sub-agents running.
Completed rows linger for a short window so their tail can still be
inspected before being evicted.

=== Task Estimates

In the interactive interface, active sub-agents receive asynchronous estimates after state changes and tool results.
The evaluator inherits the selected model and reasoning effort. It has no tools.
Requests share one stream and combine up to eight task snapshots.
Each task keeps only its latest pending snapshot.

The overview labels estimated phases with `est`. Details show remaining work, unknowns, evidence references, and update age.
Runtime states and completed counts remain independent of these estimates.
Cancelled tasks and results from previous model settings cannot receive late estimates.
Unavailable estimates leave the actual task state visible.

Numeric ranges are hidden by default. Press `p` in details to inspect provisional progress and remaining time.
These ranges have no measured accuracy guarantee. Unknown values remain unknown.
The progress range estimates work completed, not confidence or acceptance.
Set `agent.task_estimates: false` to disable evaluation. The `e` key only changes visibility.

Tool output stays attached to its invocation when messages arrive between output chunks.
Local foreground bash output appears while the command runs. Its exit status controls the result marker, independently of printed text.
Background launches show a dispatched marker. Check their task rows for execution results.

=== Notifications and Wake
<agent-task-wake>

Background work reports to the agent through task notifications: a
sub-agent that ends, monitor output and exit, a background `bash` job that
ends, and a reply to a request the main agent sent. While a turn runs, a
notification reaches the model at the next step boundary. While the agent
is idle, it starts a turn by itself about half a second after the first
notification arrives, so a burst becomes one turn. The dim line
`(background: N waiting, continuing)` marks such a turn. It adds no user
input and does not count as a user turn. Input typed while it runs is
queued like input during any other turn.

The agent does not start a turn by itself:

- in plan mode, or in a `-q` query;
- after a turn ended by Esc, an error, or a stop notice, until the next
  prompt you send;
- while a question, plan approval, or password prompt is open;
- after `agent.background_wake_limit` turns in a row that you did not start
  (default `50`, `0` for no limit). Goal continuations count toward it, and
  the limit ends a goal continuation inside such a turn. Your next prompt
  resets the count;
- when `agent.background_wake` is `false`.

Notifications that do not start a turn wait for the next one. Plain peer
messages and group messages never start a turn. A notification goes to the
agent that started the work, so a sub-agent's jobs and monitors report to that
sub-agent. Background jobs no agent started never notify the model.

Waiting notifications are bounded. Later events of a task merge into its
waiting notification, which keeps an event count and the newest 8 KiB of
output. Beyond 32 waiting tasks, new tasks are counted in one
`N more background events dropped` notification. Waiting notifications are
not saved: they are lost when the process exits.

=== Output Monitors
<agent-task-monitors>

`monitor` runs a shell command in the background and watches stdout/stderr
as a line stream. Output lines become `<task-notification>` user messages
for the agent; partial trailing lines are flushed on exit, and high-volume
bursts are coalesced before injection. An idle agent starts a turn for
them (@agent-task-wake).
The full stream is also written to an `output.log` path returned by the
tool and shown in the task overlay.

In a remote workspace, `monitor` runs the command on the remote host as a
background job over the session `/ssh` opened, from the current remote working
directory, and never starts a local `ssh` program. Its output arrives about
once a second, with stderr lines marked as for a local monitor. Leaving the workspace with
`/local` or `/ssh` to another host stops its monitors.

Users normally do not need to configure monitors manually. Ask the agent
to watch a log, poll a status endpoint, wait for CI, or react to a file
change; the agent should write the watcher command itself and call
`monitor` when event output needs to wake the session. Use `monitor_stop`
with the returned `task_id` to stop it.

== Scheduled Prompts
<agent-loop>
`/loop` runs a prompt on a cron schedule. Jobs are persisted to
`<project>/.qsoc/loops.json` and gated by a per-project file lock so only
one qsoc session fires them at a time. A project without saved jobs remains
untouched until its first durable job is added.

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
between turns of any in-flight conversation.

A job belongs to the binding it was added on (this machine, or the `/ssh`
alias and remote workspace) and stores it in `loops.json`. A job that comes
due while another binding is live does not fire. One warning names it,
`Loop <id> waits: it was scheduled on <binding> ...`, and the job fires once
when its binding is live again. `/loop list` adds `(on <binding>)`. A job
saved without a binding fires on every binding. If another qsoc session
already holds the loop lock, `/loop add/stop/clear` reports the conflict
and exits without modifying state.

== System Prompt Sources
<agent-system-prompt>
The system prompt is composed from:

- *Modular sections*: built-in role, decision flow, and tool usage guidance
- *Project instructions*: `AGENTS.md` and `AGENTS.local.md` in the project
  directory, injected verbatim. A file loads only when it is a regular file
  of at most 256 KiB and, if it is a symbolic link, its target is inside the
  project. Any other file adds a one-line notice with the reason instead of
  its content. Missing files add nothing. In remote mode the local project's
  files come first and the remote workspace's files follow, and the prompt
  says that the remote workspace's instructions win where the two conflict.
- *Environment*: model, operating system and shell, working directory, and
  `Git repository: yes` when the project is in a git work tree. In remote
  mode the remote workspace is checked through the remote shell each time the
  link is made.
- *Memory*: entries from the auto-memory store (see `memory_read` /
  `memory_write`), capped by `agent.memory_max_chars`
- *Skill listing*: names and descriptions of installed skills so the agent
  can route to them via `skill_find`, each with its layer. In remote mode
  it also holds the skills of the remote workspace (@agent-skills). The
  listing is rebuilt on `/ssh`, `/local` and `/project`.

Set `agent.system_prompt` in the config to replace the modular base with a
literal string (useful for testing or custom deployments).

The main agent builds the system prompt once and keeps it until a rebuild
point: a model switch (`/model`), a workspace or working-directory change
(`/ssh`, `/cwd`, `/project`), a new session (`/clear`, `/resume`) or a
context compaction. If a section changes in between, for example after an
edit to `AGENTS.md`, a host catalog update or a new MCP server, the next turn
starts with a reminder that carries the new section text. The next rebuild
puts the change into the system prompt.

== Remote Workspace
<agent-remote>
QSoC can drive a workspace on a remote host over SSH without installing
anything on that host. The transport is `libssh2` linked statically against
`mbedTLS 3.6 LTS` (shipped as git submodules).

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
back to 22. Bare `/ssh` opens a menu of catalog and `~/.ssh/config` hosts
(@agent-host-catalog).

The workspace is never part of the command line. It is the first of:

+ The catalog entry's `workspace`.
+ The workspace remembered for this project with the same target.
+ A two-column directory browser that opens at the remote home directory.

The chosen path becomes both the initial cwd and the first writable root
for remote file tools; `path_context` can add more
(@agent-remote-writable). Cancelling the browser connects nothing. A local
`--workspace` is never used as the remote workspace.

After a successful `/ssh`, the target and workspace are remembered for
this project in the user's local data directory
(`~/.local/share/QSoC/host-bindings/` on Linux), never in the project
tree. `/local` keeps the binding. The next interactive `qsoc agent` in
the project prints `Auto-connecting <target>` and reconnects without
showing the browser or asking about an unknown host key. If that fails,
a warning is printed and the session stays local. A run with `-q`,
`--ssh`, or `--workspace` never auto-connects.

=== Resuming a Remote Session
<agent-remote-resume>

A session records the binding of each run. When `/resume`, `--resume` or
`--continue` opens a session whose last run was on a remote workspace that is
not the live binding, an interactive frontend asks:

#figure(
  align(center)[#table(
    columns: (0.4fr, 1fr),
    align: (auto, left),
    table.header([Choice], [Effect]),
    table.hline(),
    [Rebind to `<alias>:<workspace>`],
    [Connects that workspace as `/ssh` does, then opens the session.],
    [Continue here],
    [Opens the session on the live binding.],
    [Cancel the resume],
    [Keeps the current session.],
  )],
  caption: [RESUME ON ANOTHER BINDING],
  kind: table,
)

Without a menu, for example with `-q`, the session opens on the live binding
and a warning names both bindings. Every tool call then ends the turn with
the same text until `/ssh` or `/local` chooses a binding. Remote paths are
never sent to local tools. `--ssh <alias> --workspace <dir> --resume <id>`
opens the session on its binding directly. A choice made when resuming
replaces the startup auto-connect.

=== Writable Root and Symlinks
<agent-remote-writable>

`write_file` and `edit_file` refuse a path outside the writable roots: the
workspace root and the directories added with `path_context`. The check runs on the path the *host* resolves, not on the path as typed, so a
symlink cannot be used to spell an outside directory as an inside one:

- A symlink that leads out of the workspace is refused, whether it is a
  directory in the middle of the path or the last component, and whether or
  not its target exists yet.
- A symlink that leads inside the workspace is followed. The file it points
  at is what gets written, and the link itself stays a link.
- A name the host cannot resolve at all, such as a symlink loop or a link
  into a directory that does not exist, is refused rather than treated as a
  free name.
- Every tool reports the resolved path, so the name in the transcript is the
  name on the host.

A workspace root that is itself reached through a symlink works normally:
both sides of the comparison are resolved on the host. Its resolved directory
and persistent `.qsoc-agent/tree-id` are bound when selected; changing the root
link or replacing the directory at the same path makes writes, shell cwd, and
rewinds refuse the workspace instead of following the replacement. A copied
marker deliberately identifies the copy as the same logical tree.

Remote `path_context` takes the same actions as the local tool: `list`,
`set_working`, `add`, `remove`, and `clear`. `add` accepts an existing remote
directory and binds it to the path the host resolves it to at that moment. If
the name later resolves elsewhere, writes under it are refused until it is
added again. At most ten added directories are kept; adding another drops the
oldest. The workspace root cannot be removed. `list` names the root, the
working directory, and every writable root with its resolved path, marked
`[changed]` when the host now resolves it elsewhere.

Added directories survive a reconnect to the same workspace. `/local` and
binding a different workspace drop them. They are not checkpointed, and rewind
never changes them.

=== When The Link Drops
<agent-remote-disconnect>

A remote host that reboots, loses power, or falls behind a firewall stops
answering without closing the connection. QSoC treats that as a bounded
failure rather than a wait:

- Each SSH or SFTP operation after connection has a deadline measured from
  when it started,
  covering channel setup, transfer, and teardown. Waiting for the remote
  process to report its exit status is part of that budget rather than
  extra, so a command that closes its output early still gets the rest of
  its timeout to finish. Only once the budget is spent does closing a handle
  fall back to its own separate two-second window, so cleanup is bounded
  rather than skipped. The interactive `!` shell escape is bounded too, at
  fifteen minutes, and Esc stops it sooner.
- The socket carries TCP keepalive as a second line of detection. How long
  the kernel takes to declare a silent peer dead is platform dependent and
  partly outside QSoC's control: the requested schedule is a 15-second idle
  time and probes five seconds apart, three of them where the platform takes
  a probe count at all (Windows sets the whole schedule in one call that has
  no such field), individual knobs may be unsupported or overridden by
  system policy, and the deadlines above are what actually bound an
  operation.
- A session is refused when the socket is gone, and also when a request was
  abandoned at its deadline and the protocol is stranded mid-exchange. Both
  make later tool calls fail immediately instead of retrying, and the turn
  ends rather than feeding the same failure back to the model. A command
  that simply outruns its own `timeout_ms` is *not* one of these: the read
  was abandoned, not a request, so the workspace stays usable and the next
  command runs normally.
- An abandoned SFTP initialization makes the session unusable before a usable subsystem handle exists.
- An abandoned SFTP transfer is narrower than a lost session. It leaves the
  file-transfer subsystem unusable, not the connection, so QSoC releases and
  reopens the subsystem and the workspace survives; it condemns the session
  only when that release cannot complete, which is itself the evidence that
  the connection is no longer in step. What a transfer reports about bytes
  is a lower bound: the count it acknowledges is what the host confirmed
  writing, and the host may hold more.
- The session stays in remote mode. QSoC never falls back to the local
  workspace on its own, because a command written for the remote tree must
  not silently run against local files. Reconnect with `/ssh`, or leave
  remote mode deliberately with `/local`.

A dropped link is re-established automatically, so an unattended run survives
a network hiccup. Its attempts pass one 30-second deadline through every
ProxyJump hop, SFTP startup, workspace identity check, and working-directory
verification; a later attempt receives only the time left. System resolution
and the Windows agent exchange have the exceptions described below. The budget
is one reconnect sequence per user request, not per tool call: a second failure
in the same turn is told the turn has already reconnected instead of paying for
another sequence. On Linux and macOS, Ctrl-C is answered while an attempt is
still in flight; on Windows, where Ctrl-C is a console key event read by the
same loop the attempt is holding, it takes effect when the attempt returns.
What is *not* automatic is carrying on: after a drop, remote state is unknown
rather than known-gone, because a reboot discards the working directory,
backgrounded jobs and temporary files, while a network partition leaves all of them
running. So the turn always ends, and the next one begins with a brief
telling the agent to verify remote state before acting: re-read any file it
means to edit, check named background jobs, and never re-run a command whose
effect it has not confirmed. Believed file contents are discarded on
reconnect, which makes the read-before-overwrite guard refuse an edit until
the file has actually been read again. `AGENTS.md` and `AGENTS.local.md` are
read again on reconnect; when they changed, the turn-ending notice says so
and the next turn uses the new text.

When the reconnect itself does not come up, the workspace stays unusable and
you reconnect with `/ssh` yourself.

=== Interrupted Operations
<agent-remote-uncertain>

A request that went out and got no answer has an unknown outcome: the
remote side may have carried it out anyway. QSoC reports these as
uncertain rather than as failures, and never retries them on its own,
because retrying a change that already landed applies it twice.

- `bash` reports `status: uncertain` with `exit_code: unknown`. An exit
  status is only reported when the command actually finished, so a cut-off
  command never reports `exit_code: 0`, and the field never carries a number
  a reader would take for a status the command returned. A command whose
  channel the host force-closed without ever sending a status counts as
  cut off, however cleanly the channel went away. A command killed by a
  signal is a definite failure,
  not an uncertain one: it reports `status: failed` with
  `exit_signal: SIGKILL`, because a signalled process sends no exit status
  at all.
- `bash_manage` reports that the job state is unknown rather than showing
  empty output, which would read as "no output yet" or as a kill that
  happened.
- `write_file` and `edit_file` never remove a file to make room for its
  replacement. The existing content is renamed aside, the new content is
  renamed in, and the saved copy is dropped only after that succeeds. If
  the link dies mid-publish, the content is still on the remote host under
  the original name or beside it as `<name>.qsoc-bak-<id>`; the
  tool says so instead of guessing which copy to keep.
- In the TUI these calls are marked uncertain rather than green or red.

=== What Runs Where
<agent-remote-where>

Workspace tools operate on the remote host (SFTP + SSH exec):

- `read_file`, `write_file`, `list_files`, `edit_file`
- `bash` (with optional `background=true` for detached jobs). A command
  that outlives its `timeout` keeps running as a background job and the
  result gives its `job_id`, as a local command gives a `process_id`
- `bash_manage` (status/wait/output/terminate/kill for jobs)
- `monitor`, `monitor_stop` (remote background job over the session, local
  notification stream)
- `path_context` (remote root, cwd, writable dirs, added writable dirs)
- `todo_*` (`.qsoc/todos.md` in the remote workspace)
- `skill_find`, `skill_create` (skills of the remote `.qsoc/skills` and of
  this machine, see @agent-skills)

The file tools take the local arguments and limits (@agent-file-tools).
Remote `read_file` pages text of any file size.

Every other tool runs on the local machine, in both modes:

- `agent`, `agent_status`, `agent_resume`, `send_message`, `agent_list`,
  `agent_inbox`, `wait_agent`, `followup_task`, `interrupt_agent`
- `schedule_create`, `schedule_list`, `schedule_delete`
- `host_register`, `host_update`, `host_remove`
- `memory_read`, `memory_write`, `memory_delete` (project memory in a store
  on this machine for each remote workspace, see @agent-memory-system)
- `query_docs`, `z3_solve`, `system_resources`, `tool_output_read`
- `web_fetch`, `web_search`
- `ask_user`, `enter_plan_mode`, `exit_plan_mode`, `goal_complete`
- MCP tools (`mcp__<server>__<tool>`), with this machine's credentials

On every connect and reconnect QSoC probes the host once and picks the
interpreter that runs `bash`, `bash_manage`, `monitor` and `!` by the
rule in @agent-shell-discovery.

A probe that times out or gets no usable answer leaves the host's shell
unknown: the connection stays up, file tools keep working, the shell tools
refuse with the reason, and the next reconnect probes again.

The catalog `shell:` field (@agent-host-catalog) forces `bash` or `sh`, or
a Git Bash path on Windows; when the forced shell is missing, no shell is
used. Each command reaches the interpreter on standard input, so the
remote login shell (csh, fish, cmd, PowerShell or any other) never parses
it, and a command that reads standard input sees end of file. The command
starts in the working directory and does not run if that directory
cannot be entered.

The system prompt lists, from the tools registered at that moment, the
tools that act on the remote host, the tools that run on this machine, and
the workspace tools that are not available.

The following tools act on the workspace tree and have no remote form, so
they are unavailable in remote mode:

- `project_*`, `module_*`, `bus_*`, `generate_*`, `lsp`

A sub-agent spawned in remote mode gets the same tools on the same remote
workspace, unless its `host` parameter names another catalog host.

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

The agent is tried for `ProxyJump` hops too: QSoC speaks the agent protocol on
its own socket, so a hop's keys come from the local agent as usual.

One deadline starts before name resolution and is shared by the TCP connect,
handshake and every authentication stage; those later stages consume only the
time left rather than each receiving a fresh timeout. On Linux and macOS,
Ctrl-C is answered inside any of them; on Windows it is answered once the
attempt returns, since the console delivers it as a key event on the loop the
attempt is holding. The agent is the only route that depends on a third
process, and one that accepts the connection and then stops answering is the
ordinary way to meet a forwarded agent whose upstream hop has died; it may
therefore spend at most half of what is left of the budget, leaving the
identity-file routes behind it their share. Two limits are worth knowing:
name resolution has no interruptible form, so a resolver that stops answering
may overrun the deadline until the system resolver's own retry schedule ends,
and on Windows the agent is a Pageant window or a named pipe driven by libssh2,
which the bound above does not reach.

Host key verification follows `StrictHostKeyChecking` for the target and
every `ProxyJump` hop. Keys are looked up in the `UserKnownHostsFile` list
(default `~/.ssh/known_hosts`) and in `/etc/ssh/ssh_known_hosts`. A new key
is appended to the first `UserKnownHostsFile`; existing lines are never
rewritten.

#figure(
  align(center)[#table(
    columns: (0.32fr, 1fr),
    align: (auto, left),
    table.header([Value], [Unknown host key]),
    table.hline(),
    [`yes`],
    [Refused.],
    [`ask` (default)],
    [`/ssh` and `--ssh` in an interactive session show the SHA256
     fingerprint and ask. Yes saves the key. Refused when nobody can
     answer: `-q`, reconnects, startup auto-connect, and sub-agents sent to
     a host.],
    [`accept-new`],
    [Accepted and saved.],
    [`no`, `off`],
    [Accepted with a warning. Not saved.],
  )],
  caption: [STRICTHOSTKEYCHECKING],
  kind: table,
)

A changed host key is refused under every value. Remove the old line from
known_hosts when the change is expected. A key listed on any `@revoked` line
is refused under every value. `@cert-authority` lines are ignored: host
certificates are not supported.

=== Private Key Safety
<agent-remote-keys>

QSoC code never opens, reads, copies, logs, or displays SSH private key
contents. IdentityFile paths are handed to libssh2 and only libssh2 (or
ssh-agent) reads the key material internally during authentication. Logs
refer to "configured IdentityFile" rather than literal paths where
practical, and passphrases are kept in memory only for the duration needed
to authenticate.

When automatic auth (ssh-agent + identity files with empty passphrase)
fails, `/ssh` in the main TUI prompts interactively for a secret. The
prompt runs in hidden-input mode: `termios` is flipped to disable
`ECHO` and `ICANON`, the typed bytes never appear on screen or in
terminal scrollback, and the previous terminal state is restored even
when the user cancels with `Esc` or `Ctrl+C`. The same callback handles
both encrypted private-key passphrases and the `password` userauth
method. Sub-agent dispatch deliberately leaves the callback unset so
a mid-LLM-turn child cannot block waiting on user input; the user must
first run `/ssh <alias>` in the main session to seed an authenticated
session that subsequent sub-agent spawns reuse from cache.

=== Background Jobs
<agent-remote-bg>

`bash` with `background=true` launches the command detached under
`<workspace>/.qsoc-agent/jobs/<id>/` and returns `job_id` immediately. The
wrapper writes `pid`, `output.log`, and `exit_code`, and alongside them the
identity of the host and of the process itself; `bash_manage` reads the log
and the exit code back over SSH exec. What it never reads from them is whom
to signal: the pid, and the two identities that pid is checked against, come
from the record QSoC kept at launch, because the job directory sits in a
writable remote directory and anything in it is what the host chose to put
there. A job id this session never launched therefore has no pid on record,
and `bash_manage` refuses it rather than trusting the directory.
Jobs survive SSH channel closes, and so does the shell
that waits for the exit code, so a dropped link does not cost you the
outcome.

A pid on its own is not a job. Before signalling, QSoC checks that the host
is the same incarnation it was at launch and that the pid is the same
process, because a pid is reused after a restart and signalling a number is
then signalling a stranger. When either check cannot be answered, or answers
no, nothing is signalled and the result says so as uncertain rather than
reporting a kill that did not happen. A reconnect alone does not block a
signal: the link is not the job.

`bash` without `background` runs the same way and waits up to `timeout` for
the job to end. A job that ends in time returns its output and its directory
is removed. When the workspace has no room for job directories, the command
runs over the SSH channel alone and cannot outlive the call. `terminate`
sends SIGTERM and, when the job still runs five seconds later, SIGKILL.
`wait` returns the output once the job ends, or its last lines when `timeout`
runs out first. A background or timed-out job whose output grows past
`max_output` bytes (default 5 MB) is killed at the next check, and
`bash_manage status` says why. Stopping a job from the task panel returns at
once: SIGTERM goes out with the next check, and SIGKILL follows when the job
still runs five seconds later.

While a job runs, the session checks it about every five seconds, or every
second while a monitor runs. A job that ends on its own shows its result in the
task panel (@agent-tasks) and sends the agent that launched it a task
notification with the exit status and the last 40 lines of output
(@agent-task-wake). A job its agent stopped, or whose end `bash_manage status`
already reported, sends none. A job whose directory is gone or whose host
restarted is reported as failed. Checks wait while another SSH operation runs
on the session, slow to every 30 seconds after two failed checks, and pause
while the link is down until the next reconnect. Jobs started by a sub-agent on
another host are not checked.

== Security
<agent-security>
The agent uses a read-unrestricted, write-restricted permission model:

- *Read*: Any path on the system
- *Write*: Project directory, working directory, user-added directories,
  and the OS temp directory (`/tmp` on Linux, `/var/folders/...` on macOS,
  `%TEMP%` on Windows; resolved via Qt `QDir::tempPath()`)
- *Shell*: Configurable timeout, no upper limit

A writable root reached through a symbolic link is bound to the resolved
directory when it is selected. If the link later points elsewhere, file writes
are refused until that root is selected again.

== Sessions
<agent-persistence>
Each session is persisted as `.qsoc/sessions/<id>.jsonl` under the project
directory, one JSON event per line (messages plus metadata). New-session
metadata stays in memory until the first durable record; starting, inspecting,
and exiting an unused agent does not create `.qsoc/`.

On Unix, session files, saved tool results and sub-agent run records are
readable only by their owner: files get mode `0600` and their directories
`0700`, whatever the umask. An older file with wider modes is tightened when
it is next written. The first session write also creates `.qsoc/.gitignore`
listing `sessions/` and `file-history/`, unless that file already exists.

Saved sessions enable:

- `qsoc agent --continue`: resume the most recent session
- `qsoc agent --resume [id]`: pick a session from a list, or load one by id /
  unique prefix. The TUI scrollback is rebuilt from the saved model context
  the way it looked live: code blocks, tables, folded reasoning, tool blocks
  with their outcome, `write_file` and `edit_file` diffs, todo lists, images
  kept in the context and task notices. A tool result cut to fit the context
  shows its kept part and one line naming the saved full output. The todo
  pane shows the latest todo list still in the context. `!` results in the
  conversation are restored as they were shown; slash command and `!!`
  output, run errors and interrupt notices are not restored. Sessions from
  older versions show tool outcomes inferred from their results. Terminal
  control sequences in model, tool and file text are removed, live and on
  resume
- `/resume [id]`: the same selection inside a running agent. The picker
  leaves out the current session, which is saved before the switch. It is
  refused while a turn or a sub-agent runs, or when another agent holds the
  chosen session
- `/branch [name]`: fork the current session into a new id, preserving the
  original
- `/rename <title>`: set a human-readable title shown by the resume picker

When an interactive agent exits with a saved session, it prints the command
that resumes it. `-d` is added when the project is not the launch directory:

```text
Resume this session with:
qsoc agent -d /path/to/project --resume dce6ba26-2673-4c56-a361-2634e37b9dd1
```

A session bound to an SSH workspace also gets `--ssh <target> --workspace
<path>` for the current binding, for example
`qsoc agent --ssh build@example.invalid:22 --workspace /srv/work --resume <id>`.
`/branch` prints the same command for the new branch.

An explicit resume continues an interrupted run only when QSoC can verify its
saved model, workspace, goal, and local recovery record. Finished or
inconsistent runs return to the prompt. A tool interrupted in flight is
reported as uncertain and is never replayed; continuing after it takes a new
turn, which the queued re-observation brief supplies when a dropped link was
what interrupted it.

=== File Checkpoints

File rewind covers the active local project root or the selected remote
workspace. Other locally allowed write roots, such as the working directory,
user-added directories, and the OS temp directory, remain writable but are not
checkpointed and are never changed by rewind. The same holds for directories
added to a remote workspace with `path_context`.

Each checkpoint names the tree that produced it. A local tree is bound to the
project's `.qsoc/tree-id`, created when the first local checkpoint needs to be
written; a remote tree is bound to the authenticated host, the host-resolved
workspace, and its `.qsoc-agent/tree-id`. Replacing either
directory without its bound marker, reaching a different endpoint, or loading
an old checkpoint that has no tree identity leaves its files untouched. A
copied marker denotes the same logical tree. A damaged or incomplete checkpoint
index disables file restoration instead of guessing;
conversation-only rewind remains available.

== Usage Examples
<agent-examples>

```text
qsoc> Create a new project named "soc_design" in the current directory
qsoc> Import all Verilog files from ./rtl directory
qsoc> Add AXI4 slave interface to the cpu module
qsoc> Generate Verilog from netlist.yaml with output name "top"
```

== MCP Servers
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
      framing: newline
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
      framing: content-length
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
    [`framing`], [stdio: `newline` uses standard MCP framing.
       `content-length` is the compatibility default when omitted.],
    [`url`], [http: endpoint URL. Both immediate JSON and Server-Sent
       Events responses are accepted on the same endpoint.],
    [`headers`], [http: extra request headers, e.g. `Authorization`; `Accept`,
     `Content-Type`, and `Mcp-Session-Id` are transport-owned.],
    [`proxy`], [http: per-server proxy override. Accepts the same flat
       string form as the LLM endpoint `proxy:` field (`none` /
       `system` / `http://host:port` / `socks5://host:port`). Empty or
       `system` falls back to the qsoc-wide `proxy:` block; see
       @proxy-config.],
    [`connect_timeout_ms`], [Connection timeout in milliseconds.
       Default 30000.],
    [`request_timeout_ms`], [Per-request timeout in milliseconds.
       Default 60000; non-positive values disable request deadlines.],
    [`enabled`], [Set to `false` to keep the entry in the config but skip
       it at startup.],
  )],
  caption: [MCP SERVER FIELDS],
  kind: table,
)

Use `framing: newline` with standard MCP servers. Omitted or
`content-length` preserves the framing used by QSoC 1.8.2 and earlier.
Newline framing exchanges one UTF-8 JSON message per line and rejects
messages larger than 64 MiB.

=== Tool naming
<agent-mcp-naming>
Each tool exposed by an MCP server is registered as
`mcp__<server>__<tool>`. Characters other than ASCII alphanumerics, `_`, and
`-` in either segment become underscores, which are collapsed and trimmed.
Two servers can therefore expose tools with the same short name without
colliding. Examples:

- server `fs`, tool `read_file` becomes `mcp__fs__read_file`
- server `my server`, tool `Create Issue` becomes
  `mcp__my_server__Create_Issue`

Server names that normalize to an empty namespace are ignored. If two valid,
enabled server names normalize to the same namespace, the first entry is used.
Exact duplicate tool names keep the first entry; distinct names that normalize
to the same public name are omitted.

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
    [`/mcp reconnect <name>`], [Immediately rebuild the named server,
       including one previously marked failed.],
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
Overlapping tool-list changes are coalesced, and the current catalog remains
active until the latest refresh succeeds.
Readiness is published only after `notifications/initialized` is accepted.
When positive, `request_timeout_ms` bounds each handshake send and request.
Timed-out and user-aborted tool calls discard local request state before a
best-effort cancellation, so late responses are ignored; cancellation does
not confirm remote termination. Connection loss during a call reports the
same uncertainty.
Once ready, JSON-RPC batches claim all pending response IDs before delivering
callbacks, so one callback cannot replace another batch result.

If startup, initialization, or the connection fails, the manager schedules
a rebuild on exponential backoff (1 s, 2 s, 4 s, capped at 30 s). A
successful tools/list response resets the retry budget. After three reconnect
attempts fail the server is marked failed and dropped until the next
`/mcp reconnect <name>` or agent restart. Its tools are removed while the
server is unavailable and restored after a successful tools/list response.
An initial tools/list failure follows the same bounded reconnect path. An
isolated refresh failure keeps the last valid tool catalog and waits for the
next change notification or an explicit reconnect.
Invalid descriptor fields are sanitized or ignored without discarding valid
siblings. A non-empty refresh with no valid, unambiguous descriptors leaves the
current catalog unchanged; an empty tools array clears it. Catalog replacement
is atomic, and QSoC emits one aggregate warning per connection when it adjusts
a catalog.
An isolated HTTP POST failure affects only requests carried by that POST.
A successful HTTP reply retains unanswered JSON-RPC IDs until EOF. An empty
body completes a POST containing no JSON-RPC requests; JSON or SSE EOF fails
only IDs without a valid response.
After valid responses arrive for every request in an SSE POST, QSoC closes
that response stream without waiting for server EOF.
Timeout and local cancellation release abandoned HTTP responses without
interrupting requests that remain active.
A 404 for a request carrying an MCP session ID expires the transport; the
manager rebuilds it with a fresh session.
A transport-wide error fails all outstanding requests; if the connection
remains running, later requests are still allowed.
A tool-list refresh affects new calls only; calls already in flight finish
against the server version that accepted them. Disconnects and reconnects
never replay an in-flight call. Canceling stops the local wait and sends a
best-effort notification; it cannot undo server-side effects.
QSoC validates each tools/call result before returning it to the model. Text
blocks and embedded text resources retain their order with newline boundaries;
unsupported binary or future blocks become fixed omission markers instead of
exposing encoded payloads. Unsupported structured content gets a marker when
no standard content is visible. Malformed results report that the tool may
have completed and must not be retried automatically, while `isError: true`
returns a distinct `[mcp tool error]` result and leaves the connection
available.

=== Security notes
<agent-mcp-security>
- MCP tools call out to processes (stdio) or remote endpoints (http) you
  configured yourself. Treat them with the same care as any other piece of
  third-party code.
- MCP tools stay available in an SSH remote workspace and still run on
  this machine, with its credentials. Content read from the remote host can
  steer the agent into calling them.
- Tools inherit the agent's permission rails (read unrestricted, write
  restricted to allowlisted directories). The MCP server can still touch
  resources outside the agent (a stdio server may write anywhere it has
  permission to). Configure stdio servers with the narrowest filesystem
  scope your task needs.
- `headers` may contain bearer tokens or other secrets. Keep `.qsoc.yml`
  out of version control if it carries credentials, or substitute
  environment variables and reference them from your shell.
- Tool annotations are untrusted server hints and never bypass plan-mode or
  other execution gates.

== Hooks
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
    [Once per agent lifetime before its first normal turn. Automatic recovery
     of an interrupted run does not repeat it.],
    [no],

    [`stop`],
    [Just before the agent emits `runComplete` with the final assistant
     content. Fire-and-forget.],
    [no],
  ),
  caption: [HOOK EVENTS],
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
    [`project_dir`],      [Local project directory; the launch directory
                           when there is no project.],
    [`remote`],           [Present only when the agent is in remote mode;
                           carries `target` (the alias the binding was made
                           by), `display`, `workspace`, `cwd`.],
  ),
  caption: [HOOK PAYLOAD FIELDS],
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
  caption: [HOOK STDOUT JSON FIELDS],
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
