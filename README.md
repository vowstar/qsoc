# QSoC - Quick System on Chip Studio

![QSoC Logo](./doc/en/image/logo.svg)

QSoC is a Qt-based studio for SoC design. It bundles a conversational
agent, schematic editor, RTL generation, and bus interface management
in one application.

![QSoC GUI and agent mode](https://github.com/user-attachments/assets/18ce6680-d869-485f-9dff-a93f8afb51d6)

## Quick start

With Nix, enter a shell that provides QSoC:

```bash
nix shell github:vowstar/qsoc#qsoc
```

```bash
qsoc agent -q "list the modules in this project"        # one-shot query
qsoc agent                                              # TUI + owned daemon child
qsoc agent --workspace /tmp/scratch                     # tools run in a different cwd
qsoc agent --ssh user@host --workspace /home/u/proj     # remote workspace via SSH

# Daemon mode: all agent infrastructure in one process, any frontend
qsoc-agentd                                             # listen on $XDG_RUNTIME_DIR/qsoc/agentd.sock
qsoc-agentd -s /tmp/agent.sock                          # ... or an explicit socket
qsoc agent --connect /tmp/agent.sock                    # TUI frontend over the socket
```

## Architecture

The agent is split along a clean boundary:

- **`qsoc_agent` library** (`src/agent/runtime/`): the agent runtime.
  `QSocAgentRuntime` assembles every piece of agent infrastructure
  (managers, tools, MCP, LSP, scheduler, session persistence, recovery,
  background memory) and exposes a structured event stream plus a small
  API for turns, commands, sessions and remote workspaces. Frontends
  provide presentation and user interaction only.
- **`qsoc` TUI**: the interactive frontend. It drives the runtime
  over a Unix socket in both modes. `qsoc agent` launches a private daemon
  child and opens the input prompt immediately; closing the TUI stops that
  child. `qsoc agent --connect /path/to/socket` attaches to a separately
  started daemon; closing this TUI leaves the daemon running. Each connection
  owns its agent session, and disconnecting aborts that connection's work;
  session history remains available through `--resume` / `--continue`.
- **`qsoc-agentd` daemon** (`src/agent/daemon/`): hosts agent sessions
  behind a unix socket with a length-prefixed JSON protocol, so any GUI,
  web, or TUI frontend can drive the same agent infrastructure.

## Features

- Conversational agent with tool calling for file, shell, path, project,
  module, bus, generate, schematic, LSP, skills, memory, docs, and web
- Lifecycle hooks at five events (pre/post tool use, user prompt submit,
  session start, stop) for policy, audit, and context injection
- Remote workspace over SSH and SFTP; nothing is installed on the host
- MCP servers as additional tool sources, namespaced under `mcp__`
- Session persistence with resume, branch, clear, and rename
- Schematic editor GUI alongside the CLI agent
- Verilog generation, bus interface management, slang-based linting
- PRCM generation with MMIO control, domain service handshakes, and formal checks

## Documentation

The full manual lives under `doc/en/`. Build the PDF with Nix:

```bash
cd doc && nix build
# result/qsoc_manual_<version>.pdf
```

See [doc/README.md](doc/README.md) for build instructions.

## Development

QSoC uses Nix to provide a reproducible development environment with
all dependencies pinned:

```bash
nix develop
cmake -B build -G Ninja
cmake --build build -j16
cmake --build build --target test
cmake --build build --target clang-format
```

## Third-party licenses

QSoC ships third-party components under their own licenses.
`qsoc --licenses` lists them, `qsoc --licenses <name>` prints a full
text, and the GUI shows them under Help, About QSoC.

The embedded o200k_base table comes from OpenAI
[tiktoken](https://github.com/openai/tiktoken) (MIT). The same table ships
under Apache-2.0 in the [gpt-oss](https://huggingface.co/openai/gpt-oss-20b)
tokenizer.
