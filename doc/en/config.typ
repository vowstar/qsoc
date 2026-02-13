= CONFIGURATION OVERVIEW
<config-overview>
QSoC provides a flexible configuration system that supports multiple configuration levels and sources.
This document describes the available configuration options and how they are managed.

== CONFIGURATION FILES
<config-files>
QSoC uses YAML-based configuration files at three different levels of priority:

#figure(
  align(center)[#table(
    columns: (0.2fr, 0.7fr, 1fr),
    align: (auto, left, left),
    table.header([Level], [Path], [Description]),
    table.hline(),
    [System], [`/etc/qsoc/qsoc.yml`], [System-wide configuration],
    [User], [`~/.config/qsoc/qsoc.yml`], [User-specific configuration],
    [Project], [`.qsoc.yml`], [Project-specific configuration],
  )],
  caption: [CONFIGURATION FILE LOCATIONS],
  kind: table,
)

== CONFIGURATION PRIORITY
<config-priority>
QSoC applies configuration settings in the following order of precedence (highest to lowest):

#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto, left),
    table.header([Priority], [Source]),
    table.hline(),
    [1 (Highest)],
    [Project-level configuration (`.qsoc.yml` in project directory)],
    [2], [User-level configuration (`~/.config/qsoc/qsoc.yml`)],
    [3 (Lowest)], [System-level configuration (`/etc/qsoc/qsoc.yml`)],
  )],
  caption: [CONFIGURATION PRIORITY ORDER],
  kind: table,
)

== LLM CONFIGURATION
<llm-config>
QSoC uses a unified LLM configuration format. All providers support the OpenAI Chat Completions API format,
so you only need to configure the endpoint URL, API key, and model name.

=== Configuration Options
<llm-options>
#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [llm.url], [API endpoint URL (OpenAI Chat Completions format)],
    [llm.key], [API key for authentication (optional for local services)],
    [llm.model], [Model name to use],
    [llm.model_reasoning], [Model for reasoning/thinking mode (optional)],
    [llm.timeout], [Request timeout in milliseconds (default: 30000)],
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
    [DeepSeek], [`https://api.deepseek.com/v1/chat/completions`],
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
  url: https://api.deepseek.com/v1/chat/completions
  key: sk-xxx
  model: deepseek-chat

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
QSoC supports various proxy settings for network connections:

#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [proxy.type], [Proxy type (system, none, http, socks5)],
    [proxy.host], [Proxy server hostname or IP address],
    [proxy.port], [Proxy server port number],
    [proxy.user], [Username for proxy authentication (optional)],
    [proxy.password], [Password for proxy authentication (optional)],
  )],
  caption: [PROXY CONFIGURATION OPTIONS],
  kind: table,
)

Example:
```yaml
proxy:
  type: http
  host: 127.0.0.1
  port: 7890
```

== AGENT CONFIGURATION
<agent-config>
Agent behavior can be configured in the YAML config file under the `agent` key.
These settings can also be overridden by command-line options (see @agent-command).

#figure(
  align(center)[#table(
    columns: (0.4fr, 1fr),
    align: (auto, left),
    table.header([Option], [Description]),
    table.hline(),
    [agent.temperature], [LLM temperature 0.0--1.0 (default: 0.2)],
    [agent.max_tokens], [Maximum context tokens (default: 128000)],
    [agent.max_iterations], [Maximum agent iterations (default: 100)],
    [agent.thinking], [Thinking level: off, low, medium, high],
    [agent.stream], [Enable streaming output: true/false (default: true)],
    [agent.prune_threshold],
    [Token ratio to trigger tool output pruning (default: 0.6)],
    [agent.compact_threshold],
    [Token ratio to trigger LLM compaction (default: 0.8)],
    [agent.compaction_model],
    [Model for compaction (empty = use primary model)],
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
    [web.search_api_url], [SearXNG instance URL (e.g., `http://localhost:8080`). Required for `web_search`.],
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
  url: https://api.deepseek.com/v1/chat/completions
  key: sk-xxx
  model: deepseek-chat
  model_reasoning: deepseek-reasoner
  timeout: 30000

# Agent Configuration
agent:
  thinking: high
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
