= Configuration Overview
<config-overview>
Configuration comes from layered `qsoc.yml` files and a short list of
environment variables. This chapter lists the layers, the keys, and what
each key does.

== Configuration Files
<config-files>
QSoC resolves configuration, skills, and memory across four layered roots.
User-level and project-level roots are identical on all platforms; only the
system-level root follows platform conventions.

#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto, left),
    table.header([Layer], [Root (all platforms)]),
    table.hline(),
    [Env], [`$QSOC_HOME` (when set)],
    [Project], [`<projectPath>/.qsoc`],
    [User], [`~/.config/qsoc`],
    [System], [platform-specific, see @config-files],
  )],
  caption: [RESOURCE ROOTS PER LAYER],
  kind: table,
)

#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto, left),
    table.header([Platform], [System root]),
    table.hline(),
    [Linux], [`/etc/qsoc`],
    [macOS], [`/Library/Application Support/qsoc`],
    [Windows], [`%PROGRAMDATA%\qsoc`],
  )],
  caption: [SYSTEM ROOT BY PLATFORM],
  kind: table,
)

Config files live at `<root>/qsoc.yml` in every layer, except the project
layer which uses the legacy `<projectPath>/.qsoc.yml` filename. Skills and
memory live at `<root>/skills/` and `<root>/memory/` respectively.

`$XDG_CONFIG_HOME` is honored on every platform: if set, the user root
becomes `$XDG_CONFIG_HOME/qsoc` instead of `~/.config/qsoc`.

Same-name skills in higher layers shadow lower ones. Listings and
`skill_find` with the default `scope: "all"` show the effective (unshadowed)
set; pass `scope: "system"` / `"user"` / `"project"` to inspect a specific
layer for debugging.

== Configuration Priority
<config-priority>
QSoC applies configuration settings in the following order of precedence (highest to lowest):

#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto, left),
    table.header([Priority], [Source]),
    table.hline(),
    [1 (Highest)], [Environment variables, see @config-env],
    [2], [Project-level configuration (`.qsoc.yml` in project directory)],
    [3], [Environment root (`$QSOC_HOME/qsoc.yml` when set)],
    [4], [User-level configuration (`~/.config/qsoc/qsoc.yml`)],
    [5 (Lowest)], [System-level configuration (platform-specific)],
  )],
  caption: [CONFIGURATION PRIORITY ORDER],
  kind: table,
)

=== Environment Variables
<config-env>
Only the names below are read. `QSOC_*` is not a general mapping onto
configuration keys, so an invented name such as `QSOC_AGENT_EFFORT` is ignored.

#figure(
  align(center)[#table(
    columns: (0.75fr, 1fr),
    align: (auto, left),
    table.header([Variable], [Sets],),
    table.hline(),
    [`QSOC_HOME`], [Environment root searched for `qsoc.yml`, skills, and memory],
    [`QSOC_LLM_MODEL`], [`llm.model`],
    [`QSOC_AGENT_TEMPERATURE`], [`agent.temperature`],
    [`QSOC_AGENT_MAX_TOKENS`], [`agent.max_tokens`],
    [`QSOC_AGENT_MAX_ITERATIONS`], [`agent.max_iterations`],
    [`QSOC_AGENT_SYSTEM_PROMPT`], [`agent.system_prompt`],
    [`QSOC_AGENT_AUTO_LOAD_MEMORY`], [`agent.auto_load_memory`],
    [`QSOC_AGENT_MEMORY_MAX_CHARS`], [`agent.memory_max_chars`],
    [`QSOC_WEB_SEARCH_API_URL`], [`web.search_api_url`],
    [`QSOC_WEB_SEARCH_API_KEY`], [`web.search_api_key`],
    [`QSOC_SKILLS_PATH`], [Extra skill search roots],
    [`QSOC_MAX_CONCURRENT_SUBAGENTS`], [Sub-agent concurrency ceiling],
    [`QSOC_AUTO_BACKGROUND_MS`], [Delay before a shell call moves to the background],
    [`QSOC_NO_IMAGE_GRAPHICS`], [Disables inline image rendering when set to any value],
    [`XDG_CONFIG_HOME`], [Moves the user configuration root off `~/.config`],
  )],
  caption: [ENVIRONMENT VARIABLES],
  kind: table,
)

== LLM Configuration
<llm-config>
Every provider speaks the OpenAI Chat Completions format. Each model is
an entry under `llm.models`; `llm.model` names the entry in use, and
`/model` switches between entries and writes the choice back.

An entry has three names. The key is the handle you type in `/model`
and see in the status bar. `name` is the label in pickers. `model` is
the string sent to the server, and defaults to the key. One served model
behind two URLs is two entries with the same `model`.

=== Configuration Options
<llm-options>
#figure(
  align(center)[#table(
    columns: (0.55fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [llm.model], [Key of the `llm.models` entry to use],
    [llm.models], [Per-model entries, see @llm-models-registry],
    [llm.cost_input_per_mtok],
    [Input price per million tokens (used by `/cost`)],
    [llm.cost_output_per_mtok],
    [Output price per million tokens (used by `/cost`)],
    [llm.cost_currency], [Currency label for cost display (default: USD)],
  )],
  caption: [LLM CONFIGURATION OPTIONS],
  kind: table,
)

=== Per-Model Fields
<llm-models-registry>
Every key under an entry is optional except `url`.

#figure(
  align(center)[#table(
    columns: (0.65fr, 1fr),
    align: (auto, left),
    table.header([Field], [Description]),
    table.hline(),
    [`name`], [Label shown in pickers; defaults to the key],
    [`model`], [Name sent in the request body; defaults to the key],
    [`url`],
    [Chat Completions URL (required). Cloud providers publish theirs;
     Ollama serves `http://localhost:11434/v1/chat/completions`],
    [`key`], [API key; empty for keyless local services],
    [`auth_header`],
    [Auth header name. Empty or `Authorization` sends
    `Bearer <key>` (default). Any other value sends the bare key
    under that header],
    [`timeout`], [Request timeout in milliseconds],
    [`context`], [Context window in tokens],
    [`max_output_tokens`], [Reply cap; `0` defers to the backend],
    [`effort`],
    [Effort applied when this entry is selected: `low`, `medium`, `high`;
     empty means off],
    [`modalities.image`], [`true` opts the model into image input],
    [`modalities.image_max_tokens`],
    [Reject the image when the client-side estimate exceeds this],
    [`modalities.image_max_dimension`],
    [Resize short edge to this many pixels before encoding],
    [`modalities.image_max_bytes`],
    [On-wire byte cap; `0` means no byte limit],
    [`modalities.image_provider_hint`],
    [Token-cost formula hint; the wire payload still uses the
    OpenAI `image_url` shape],
  )],
  caption: [PER-MODEL CONFIGURATION FIELDS],
  kind: table,
)

```yaml
llm:
  model: pro
  models:
    pro:
      name: Pro (thinking)
      model: vendor-pro-2026
      url: https://api.example.com/v1/chat/completions
      key: sk-xxx
      timeout: 180000
      context: 131072
      max_output_tokens: 32768
      effort: high
    omni:
      name: Omni
      model: vendor-omni-2026
      url: https://api.example.com/v1/chat/completions
      key: sk-xxx
      auth_header: api-key
      context: 1048576
      modalities:
        image: true
        image_max_tokens: 4000
    omni-lab:
      name: Omni (lab server)
      model: vendor-omni-2026
      url: http://gpu-box.lab:8000/v1/chat/completions
      context: 1048576
      modalities:
        image: true
```

`omni` and `omni-lab` are the same served model on two servers; `/model`
picks the server, the request body carries `vendor-omni-2026` either way.

== LSP Configuration
<lsp-config>
`lsp.servers` registers external language servers for the agent's `lsp` tool
(@agent-lsp). Each entry needs `command` and `extensions`; `args` is optional.
An entry replaces the built-in slang backend for the extensions it lists.

```yaml
lsp:
  servers:
    verible:
      command: verible-verilog-ls
      args: []
      extensions: [".v", ".sv"]
```

== Network Proxy Configuration
<proxy-config>
QSoC resolves the proxy used for every HTTP-based subsystem (LLM
endpoints, MCP HTTP transports, web tools) in three tiers, highest
priority first:

+ *Per-target spec* attached to a single endpoint or server, e.g. an
  MCP server's `proxy:` field.
+ *qsoc-wide* `proxy:` block at the top of the loaded config (the
  block below).
+ *System / environment* fallback, picked up via `libproxy` and the
  `http_proxy` / `https_proxy` env vars.

The qsoc-wide block accepts the legacy nested form:

#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [proxy.type], [Proxy type: `system`, `none`, `http`, `socks5`],
    [proxy.host], [Proxy server hostname or IP address],
    [proxy.port], [Proxy server port number],
    [proxy.user], [Username for proxy authentication (optional)],
    [proxy.password], [Password for proxy authentication (optional)],
  )],
  caption: [QSOC-WIDE PROXY OPTIONS],
  kind: table,
)

```yaml
proxy:
  type: http
  host: 127.0.0.1
  port: 7890
```

Per-target specs use a flat string instead, accepted at the LLM
endpoint level and at every MCP server entry. The same vocabulary
applies in both places:

#figure(
  align(center)[#table(
    columns: (0.4fr, 1fr),
    align: (auto, left),
    table.header([Value], [Effect]),
    table.hline(),
    [empty / `system` / `default`], [Inherit the qsoc-wide tier (then
       system if that is also empty).],
    [`none` / `off` / `direct`], [Bypass every proxy, connect directly.],
    [`http://[user:pass@]host:port`], [Explicit HTTP proxy.],
    [`socks5://[user:pass@]host:port`], [Explicit SOCKS5 proxy.],
  )],
  caption: [PER-TARGET PROXY VALUES],
  kind: table,
)

Example combining the tiers:

```yaml
# qsoc-wide default: route everything through a corporate HTTP proxy.
proxy:
  type: http
  host: proxy.internal
  port: 8080

mcp:
  servers:
    # Inherit the wide default.
    - name: cloud_search
      type: http
      url: https://cloud.example/mcp

    # Bypass the proxy for an internal service.
    - name: lan_docs
      type: http
      url: http://10.0.0.50/mcp
      proxy: none
```

== Agent Configuration
<agent-config>
Agent behavior can be configured in the YAML config file under the `agent` key.
These settings can also be overridden by command-line options (see @agent-command).

#figure(
  align(center)[#table(
    columns: (1.2fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [agent.temperature], [LLM temperature 0.0--1.0 (default: 0.2)],
    [agent.max_tokens], [Maximum context tokens (default: 128000)],
    [agent.max_iterations], [Maximum agent iterations (default: 100)],
    [agent.effort], [Reasoning effort: off, low, medium, high],
    [agent.stream], [Enable streaming output: true/false (default: true)],
    [agent.prune_threshold],
    [Token ratio to trigger tool output pruning (default: 0.4)],
    [agent.compact_threshold],
    [Token ratio to trigger LLM compaction (default: 0.6)],
    [agent.compaction_model],
    [`llm.models` key for compaction (empty = the selected model)],
    [agent.auto_load_memory],
    [Auto-inject memory into the system prompt (default: true)],
    [agent.memory_max_chars],
    [Max characters of memory to inject (default: 24000)],
    [agent.memory_recall],
    [Rank and inject only relevant memories per turn (default: true)],
    [agent.memory_recall_model],
    [`llm.models` key for the recall selector (empty = the selected model)],
    [agent.memory_recall_max_files],
    [Max memory files selected per turn (default: 5)],
    [agent.memory_recall_per_file_cap],
    [Max bytes injected from one memory file (default: 4096)],
    [agent.memory_recall_turn_budget],
    [Max cumulative recall bytes per turn (default: 61440)],
    [agent.memory_extract],
    [Background-extract memory after each turn (default: true)],
    [agent.memory_extract_model],
    [`llm.models` key for the extraction child (empty = the selected model)],
    [agent.memory_extract_cadence],
    [Run extraction every N turns (default: 1)],
    [agent.memory_extract_min_messages],
    [Skip extraction below this many new messages (default: 2)],
    [agent.memory_dream],
    [Periodic memory consolidation pass (default: true)],
    [agent.memory_dream_model],
    [`llm.models` key for the consolidation child (empty = the selected model)],
    [agent.memory_dream_min_hours],
    [Minimum hours between consolidations (default: 24)],
    [agent.memory_dream_min_sessions],
    [Minimum sessions since last consolidation (default: 5)],
    [agent.session_title],
    [Auto-generate a session title after the first turn (default: true)],
    [agent.session_title_model],
    [`llm.models` key for the title call (empty = the selected model)],
    [agent.away_summary],
    [Show a "while you were away" recap after the terminal loses focus
     (default: true)],
    [agent.away_summary_model],
    [`llm.models` key for the recap call (empty = the selected model)],
    [agent.away_summary_delay_seconds],
    [Idle seconds of lost focus before the recap is generated (default: 300)],
    [agent.context_restore],
    [Re-inject recent files, skills, and running agents after a compaction
     (default: true)],
    [agent.context_restore_max_files],
    [Max files restored after a compaction (default: 5)],
    [agent.context_restore_file_budget],
    [Max cumulative tokens for restored files (default: 50000)],
    [agent.context_restore_max_tokens_per_file],
    [Files above this token count restore as a path-only pointer
     (default: 5000)],
    [agent.context_restore_max_tokens_per_skill],
    [Max tokens of each restored skill body (default: 5000)],
    [agent.context_restore_skill_budget],
    [Max cumulative tokens for restored skills (default: 25000)],
    [agent.system_prompt], [Custom system prompt override],
    [agent.predict_input],
    [Predict next input as ghost text: true/false (default: true)],
    [agent.status_line],
    [Shell command (or map with `command` and `timeout_ms`) whose stdout
     becomes an extra status row; see @agent-status-line. Read from the
     user/system config layers only, never from a project `.qsoc.yml`],
  )],
  caption: [AGENT CONFIGURATION OPTIONS],
  kind: table,
)

== Web Configuration
<web-config>
The agent can search the web via SearXNG and fetch URL content. Web search
requires a SearXNG instance URL to be configured; web fetch works without
configuration.

#figure(
  align(center)[#table(
    columns: (0.4fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [web.search_api_url],
    [SearXNG instance URL (e.g., `http://localhost:8080`). Required for `web_search`.],
    [web.search_api_key], [SearXNG API key (optional)],
  )],
  caption: [WEB CONFIGURATION OPTIONS],
  kind: table,
)

Environment variables: `QSOC_WEB_SEARCH_API_URL`, `QSOC_WEB_SEARCH_API_KEY`.

Example:
```yaml
web:
  search_api_url: http://localhost:8080
  search_api_key: my-secret-key
```

== Complete Configuration Example
<config-example>
Below is an example of a complete QSoC configuration file:

```yaml
# LLM Configuration
llm:
  model: pro
  models:
    pro:
      model: deepseek-v4-pro
      url: https://api.deepseek.com/chat/completions
      key: sk-xxx
      context: 131072

# Agent Configuration
agent:
  effort: high
  max_tokens: 128000

# Network Proxy (if needed)
proxy:
  type: http
  host: 127.0.0.1
  port: 7890

# Web Search (optional, requires SearXNG)
web:
  search_api_url: http://localhost:8080
```

== Troubleshooting
<troubleshooting>
1. QSoC writes a commented template to `~/.config/qsoc/qsoc.yml` on first
   start. Delete the file and restart to get a fresh one.
2. A YAML syntax error stops the whole file from loading; validate it
   before looking elsewhere.
3. `/status` shows the selected entry. If requests fail, `curl` the
   entry's `url` with its `key` and the `model` string from the same
   entry; the server must accept that exact name.
