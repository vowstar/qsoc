= CONFIGURATION OVERVIEW
<config-overview>
QSoC provides a flexible configuration system that supports multiple configuration levels and sources.
This document describes the available configuration options and how they are managed.

== CONFIGURATION FILES
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
    [System], [platform-specific, see below],
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

== CONFIGURATION PRIORITY
<config-priority>
QSoC applies configuration settings in the following order of precedence (highest to lowest):

#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto, left),
    table.header([Priority], [Source]),
    table.hline(),
    [1 (Highest)], [Environment variables (`QSOC_*`)],
    [2], [Project-level configuration (`.qsoc.yml` in project directory)],
    [3], [Environment root (`$QSOC_HOME/qsoc.yml` when set)],
    [4], [User-level configuration (`~/.config/qsoc/qsoc.yml`)],
    [5 (Lowest)], [System-level configuration (platform-specific)],
  )],
  caption: [CONFIGURATION PRIORITY ORDER],
  kind: table,
)

Same-name skills in higher layers shadow lower ones. Listings and
`skill_find` with the default `scope: "all"` show the effective (unshadowed)
set; pass `scope: "system"` / `"user"` / `"project"` to inspect a specific
layer for debugging.

== LLM CONFIGURATION
<llm-config>
QSoC uses a unified LLM configuration format. All providers support the OpenAI Chat Completions API format,
so you only need to configure the endpoint URL, API key, and model name.

=== Configuration Options
<llm-options>
#figure(
  align(center)[#table(
    columns: (0.55fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [llm.url], [API endpoint URL (OpenAI Chat Completions format)],
    [llm.key], [API key for authentication (optional for local services)],
    [llm.model], [Model name to use],
    [llm.model_reasoning], [Model for reasoning/effort mode (optional)],
    [llm.timeout], [Request timeout in milliseconds (default: 30000)],
    [llm.cost_input_per_mtok],
    [Input price per million tokens (used by `/cost`)],
    [llm.cost_output_per_mtok],
    [Output price per million tokens (used by `/cost`)],
    [llm.cost_currency], [Currency label for cost display (default: USD)],
  )],
  caption: [LLM CONFIGURATION OPTIONS],
  kind: table,
)

=== Supported Endpoints
<llm-endpoints>
All major LLM providers support the OpenAI Chat Completions format:

#figure(
  align(center)[#table(
    columns: (0.3fr, 0.7fr),
    align: (auto, left),
    table.header([Provider], [Endpoint URL]),
    table.hline(),
    [DeepSeek], [`https://api.deepseek.com/chat/completions`],
    [OpenAI], [`https://api.openai.com/v1/chat/completions`],
    [Groq], [`https://api.groq.com/openai/v1/chat/completions`],
    [Ollama], [`http://localhost:11434/v1/chat/completions`],
  )],
  caption: [SUPPORTED LLM ENDPOINTS],
  kind: table,
)

=== Configuration Examples
<llm-examples>
Example configurations for different providers:

```yaml
# DeepSeek
llm:
  url: https://api.deepseek.com/chat/completions
  key: sk-xxx
  model: deepseek-v4-pro

# OpenAI
llm:
  url: https://api.openai.com/v1/chat/completions
  key: sk-xxx
  model: gpt-4o-mini

# Local Ollama (no key required)
llm:
  url: http://localhost:11434/v1/chat/completions
  model: llama3
```

== NETWORK PROXY CONFIGURATION
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

== AGENT CONFIGURATION
<agent-config>
Agent behavior can be configured in the YAML config file under the `agent` key.
These settings can also be overridden by command-line options (see @agent-command).

#figure(
  align(center)[#table(
    columns: (0.55fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [agent.temperature], [LLM temperature 0.0--1.0 (default: 0.2)],
    [agent.max_tokens], [Maximum context tokens (default: 128000)],
    [agent.max_iterations], [Maximum agent iterations (default: 100)],
    [agent.effort], [Reasoning effort: off, low, medium, high],
    [agent.stream], [Enable streaming output: true/false (default: true)],
    [agent.prune_threshold],
    [Token ratio to trigger tool output pruning (default: 0.6)],
    [agent.compact_threshold],
    [Token ratio to trigger LLM compaction (default: 0.8)],
    [agent.compaction_model],
    [Model for compaction (empty = use primary model)],
    [agent.auto_load_memory],
    [Auto-inject memory into the system prompt (default: true)],
    [agent.memory_max_chars],
    [Max characters of memory to inject (default: 24000)],
    [agent.system_prompt], [Custom system prompt override],
  )],
  caption: [AGENT CONFIGURATION OPTIONS],
  kind: table,
)

== WEB CONFIGURATION
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

== COMPLETE CONFIGURATION EXAMPLE
<config-example>
Below is an example of a complete QSoC configuration file:

```yaml
# LLM Configuration
llm:
  url: https://api.deepseek.com/chat/completions
  key: sk-xxx
  model: deepseek-v4-pro
  timeout: 30000

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

== AUTOMATIC TEMPLATE CREATION
<auto-template>
When QSoC is run for the first time and the user configuration file (`~/.config/qsoc/qsoc.yml`) does not exist,
the software will automatically create a template configuration file with recommended settings and detailed comments.

== TROUBLESHOOTING
<troubleshooting>
If you encounter issues with QSoC startup or configuration-related problems:

1. Delete the user configuration directory (`~/.config/qsoc/`) and restart the application
  - This will cause QSoC to regenerate a fresh template configuration file

2. Ensure the YAML syntax in your configuration files is valid
  - Invalid YAML syntax can cause configuration loading failures

3. Verify the LLM endpoint URL is correct
  - All providers should use OpenAI Chat Completions compatible endpoints
  - Test the endpoint with curl to ensure it is accessible
