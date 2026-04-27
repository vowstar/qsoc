# Documentation

The manual is written in Typst and built via Nix. The flake pins the
toolchain, fonts (Sarasa Gothic + Noto Sans), and stamps the version
read from `src/common/config.h`.

## Build

From the `doc/` directory:

```bash
nix build                  # default target (same as below)
nix build .#qsoc-manual    # named target
```

The PDF lands in `result/qsoc_manual_<version>.pdf`.

## Develop

```bash
nix develop                # shell with typst + fonts wired up
typst watch en/main.typ    # live-rebuild while editing
```

## Layout

- `flake.nix`: toolchain, font stack, version source
- `en/main.typ`: entry point; imports section files and the `datasheet.typ` template
- `en/<section>.typ`: chapter sources (about, agent, command, config, format_*, ...)
- `en/image/`: shared artwork
  - `logo.svg`: wordmark used in the manual cover and the project README
  - `icon.svg`: square app icon for Linux / macOS / Windows
  - `cover.svg`: manual cover artwork
