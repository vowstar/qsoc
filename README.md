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
qsoc agent                                              # TUI with a private agent daemon
qsoc agent --workspace /tmp/scratch                     # tools run in a different cwd
qsoc agent --ssh user@host --workspace /home/u/proj     # remote workspace via SSH

# A shared daemon that several clients can attach to
qsoc-agentd                                             # listen on $XDG_RUNTIME_DIR/qsoc/agentd.sock
qsoc-agentd -s /tmp/agent.sock                          # ... or an explicit socket
qsoc agent --connect /tmp/agent.sock                    # TUI attached to that daemon
```

## Programs

| Program | Role |
|---|---|
| `qsoc` | Command line, generators and the agent TUI; `qsoc gui` starts `qsoc-gui` |
| `qsoc-gui` | Main window, editors and schematic |
| `qsoc-agentd` | Agent daemon: one process per session behind a versioned socket protocol, for the TUI or any other client |
| `qsoc-smt-worker` | Isolated SMT solver worker |

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

Releases include `qsoc-X.Y.Z.tar.xz` with all recursive Git submodule sources and a `.sha256` checksum file. The archive records the main commit and dependency commits in `source-revisions.json`. GitHub's automatic source downloads omit submodule contents. Qt and other system build dependencies still need installation.

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
