# QSoC

IDE for building SoC.

## Development

### Environment Setup

```bash
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
