# QSoC - Quick System on Chip Studio

QSoC is a Quick, Quality, Quintessential development environment for modern
SoC (System on Chip) development based on the Qt framework.

QSoC empowers hardware engineers with streamlined features for designing complex
SoC systems.

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
```

### SPDX License Headers

QSoC supports automatic management of SPDX license and copyright headers for source files.

To enable SPDX headers:

```bash
cmake -B build -G Ninja -DENABLE_SPDX_HEADERS=ON
```

For detailed information on SPDX header management, please see [cmake/README.md](cmake/README.md).
