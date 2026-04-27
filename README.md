# QSoC - Quick System on Chip Studio

![QSoC Logo](./doc/en/image/logo.svg)

QSoC is a Qt-based studio for SoC design. It bundles a conversational
agent, schematic editor, RTL generation, and bus interface management
behind one binary; the agent calls into the same tools you would invoke
by hand, so a prompt is always equivalent to a sequence of explicit
operations.

## Quick start

```bash
qsoc agent -q "list the modules in this project"        # one-shot query
qsoc agent                                              # interactive REPL
qsoc agent --workspace /tmp/scratch                     # tools run in a different cwd
qsoc agent --ssh user@host --workspace /home/u/proj     # remote workspace via SSH
```

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

## Documentation

The full manual lives under `doc/en/`. Build the PDF with Nix:

```bash
cd doc && nix build
# result/qsoc_manual_<version>.pdf
```

See [doc/README.md](doc/README.md) for the multi-language build targets.

## Development

QSoC uses Nix to provide a reproducible development environment with
all dependencies pinned:

```bash
nix develop
cmake -B build -G Ninja
cmake --build build -j
cmake --build build --target test
cmake --build build --target clang-format
```
