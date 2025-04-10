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
    align: (auto,left,left,),
    table.header([Level], [Path], [Description],),
    table.hline(),
    [System], [`/etc/qsoc/qsoc.yml`], [System-wide configuration],
    [User], [`~/.config/qsoc/qsoc.yml`], [User-specific configuration],
    [Project], [`.qsoc.yml`], [Project-specific configuration],
  )]
  , caption: [CONFIGURATION FILE LOCATIONS]
  , kind: table
  )

== CONFIGURATION PRIORITY
<config-priority>
QSoC applies configuration settings in the following order of precedence (highest to lowest):

#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto,left,),
    table.header([Priority], [Source],),
    table.hline(),
    [1 (Highest)], [Environment variables (QSOC_API_KEY, QSOC_AI_PROVIDER, etc.)],
    [2], [Project-level configuration (`.qsoc.yml` in project directory)],
    [3], [User-level configuration (`~/.config/qsoc/qsoc.yml`)],
    [4 (Lowest)], [System-level configuration (`/etc/qsoc/qsoc.yml`)],
  )]
  , caption: [CONFIGURATION PRIORITY ORDER]
  , kind: table
  )

== GLOBAL CONFIGURATION OPTIONS
<global-config-options>
These options can be specified at the root level of any configuration file:

#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto,left,),
    table.header([Option], [Description],),
    table.hline(),
    [ai_provider], [AI provider to use (deepseek, openai, groq, claude, ollama)],
    [api_key], [Global API key for AI service authentication],
    [ai_model], [Default AI model to use],
    [api_url], [Base URL for the API endpoint],
  )]
  , caption: [GLOBAL CONFIGURATION OPTIONS]
  , kind: table
  )

== PROVIDER-SPECIFIC CONFIGURATION
<provider-config>
Provider-specific settings can be specified in nested format for different AI providers:

=== DeepSeek Configuration
<deepseek-config>
#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto,left,),
    table.header([Option], [Description],),
    table.hline(),
    [api_key], [DeepSeek API key],
    [api_url], [DeepSeek API endpoint URL (default: https://api.deepseek.com/v1/chat/completions)],
    [ai_model], [DeepSeek model to use (default: deepseek-chat)],
  )]
  , caption: [DEEPSEEK CONFIGURATION OPTIONS]
  , kind: table
  )

=== OpenAI Configuration
<openai-config>
#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto,left,),
    table.header([Option], [Description],),
    table.hline(),
    [api_key], [OpenAI API key],
    [api_url], [OpenAI API endpoint URL (default: https://api.openai.com/v1/chat/completions)],
    [ai_model], [OpenAI model to use (default: gpt-4o-mini)],
  )]
  , caption: [OPENAI CONFIGURATION OPTIONS]
  , kind: table
  )

=== Groq Configuration
<groq-config>
#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto,left,),
    table.header([Option], [Description],),
    table.hline(),
    [api_key], [Groq API key],
    [api_url], [Groq API endpoint URL (default: https://api.groq.com/openai/v1/chat/completions)],
    [ai_model], [Groq model to use (default: mixtral-8x7b-32768)],
  )]
  , caption: [GROQ CONFIGURATION OPTIONS]
  , kind: table
  )

=== Claude Configuration
<claude-config>
#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto,left,),
    table.header([Option], [Description],),
    table.hline(),
    [api_key], [Claude API key],
    [api_url], [Claude API endpoint URL (default: https://api.anthropic.com/v1/messages)],
    [ai_model], [Claude model to use (default: claude-3-5-sonnet-20241022)],
  )]
  , caption: [CLAUDE CONFIGURATION OPTIONS]
  , kind: table
  )

=== Ollama Configuration
<ollama-config>
#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto,left,),
    table.header([Option], [Description],),
    table.hline(),
    [api_url], [Ollama API endpoint URL (default: http://localhost:11434/api/generate)],
    [ai_model], [Ollama model to use (default: llama3)],
  )]
  , caption: [OLLAMA CONFIGURATION OPTIONS]
  , kind: table
  )

== NETWORK PROXY CONFIGURATION
<proxy-config>
QSoC supports various proxy settings for network connections:

#figure(
  align(center)[#table(
    columns: (0.3fr, 1fr),
    align: (auto,left,),
    table.header([Option], [Description],),
    table.hline(),
    [proxy_type], [Proxy type (system, none, default, socks5, http)],
    [proxy_host], [Proxy server hostname or IP address],
    [proxy_port], [Proxy server port number],
    [proxy_user], [Username for proxy authentication (if required)],
    [proxy_password], [Password for proxy authentication (if required)],
  )]
  , caption: [PROXY CONFIGURATION OPTIONS]
  , kind: table
  )

== ENVIRONMENT VARIABLES
<environment-variables>
QSoC supports configuration via environment variables, which have the highest priority:

#figure(
  align(center)[#table(
    columns: (0.4fr, 1fr),
    align: (auto,left,),
    table.header([Variable], [Description],),
    table.hline(),
    [QSOC_AI_PROVIDER], [AI provider to use (overrides configuration files)],
    [QSOC_API_KEY], [API key for authentication (overrides configuration files)],
    [QSOC_AI_MODEL], [AI model to use (overrides configuration files)],
    [QSOC_API_URL], [API endpoint URL (overrides configuration files)],
  )]
  , caption: [ENVIRONMENT VARIABLES]
  , kind: table
  )

== CONFIGURATION EXAMPLE
<config-example>
Below is an example of a complete QSoC configuration file:

```yaml
# Global settings
ai_provider: openai
api_key: global_api_key_here

# Provider-specific settings
openai:
  api_key: your_openai_api_key_here
  ai_model: gpt-4o-mini

deepseek:
  api_key: your_deepseek_api_key_here
  ai_model: deepseek-chat

# Proxy settings
proxy_type: http
proxy_host: 127.0.0.1
proxy_port: 8080
```

This example configures OpenAI as the default provider with provider-specific settings for both OpenAI and DeepSeek, plus HTTP proxy configuration.

== AUTOMATIC TEMPLATE CREATION
<auto-template>
When QSoC is run for the first time and the user configuration file (`~/.config/qsoc/qsoc.yml`) does not exist, the software will automatically create a template configuration file with recommended settings and detailed comments. This template includes examples for all supported providers and configuration options, making it easy to customize according to your needs.

The template includes well-documented examples of:
- Global configuration options
- Provider-specific settings for all supported AI providers
- Network proxy configuration options

All settings in the template are commented out by default, and you can uncomment and modify the ones you need.

== TROUBLESHOOTING
<troubleshooting>
If you encounter issues with QSoC startup or configuration-related problems, you can try the following steps:

1. Delete the user configuration directory (`~/.config/qsoc/`) and restart the application
   - This will cause QSoC to regenerate a fresh template configuration file
   - Note that this will remove any custom settings you have configured

2. Check environment variables that might be overriding your configuration file settings
   - Remember that environment variables have the highest priority

3. Ensure the YAML syntax in your configuration files is valid
   - Invalid YAML syntax can cause configuration loading failures
