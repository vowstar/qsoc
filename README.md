# QSoC - Quick System on Chip Studio

![QSoC Logo](./doc/en/image/logo.svg)

QSoC is a Quick, Quality, Quintessential development environment for modern
SoC (System on Chip) development based on the Qt framework.

QSoC empowers hardware engineers with streamlined features for designing complex
SoC systems, including advanced netlist validation with bit-level overlap detection
and comprehensive port direction checking.

## Development

### Environment Setup

QSoC uses Nix to provide a consistent and reproducible development environment
with all dependencies automatically managed:

```bash
# Enter the development environment
nix develop

# Once inside the Nix environment, you can run development commands
cmake -B build -G Ninja
```

### Code Formatting

```bash
cmake --build build --target clang-format
```

### Building

```bash
cmake --build build -j
```

### Testing

```bash
cmake --build build --target test
# or using ctest directly
cd build && ctest
```

### Building Documentation

To build the documentation:

```bash
cd doc && nix build .#qsoc-manual
```

For detailed information on documentation building, please see [doc/README.md](doc/README.md).
