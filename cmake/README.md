# CMake Modules

This directory contains custom CMake modules for the QSoC project.

## AddSpdxHeaders

A CMake module to automatically add SPDX license and copyright headers to C++ source and header files.

### Features

- Automatically reads author information from git config (if not specified)
- Automatically detects file's first commit year from git history
- Displays single year if file was created in current year, or year range otherwise
- Falls back to default values if git is unavailable
- When SPDX header already exists, only updates the year information while preserving the original license and author
- Works cross-platform (Linux, macOS, Windows)
- Can be enabled/disabled via CMake option
- When disabled (ENABLE_SPDX_HEADERS=OFF), all SPDX header operations are skipped and no dependencies are created
- Automatically disables SPDX headers if git is not available and no copyright information is provided

### Usage in CMakeLists.txt

The module is included in the main CMakeLists.txt and can be controlled with the `ENABLE_SPDX_HEADERS` option:

```cmake
# Enable or disable SPDX headers (default: OFF)
option(ENABLE_SPDX_HEADERS "Enable adding SPDX headers to source files" OFF)

# ... later in the file ...

# Add SPDX headers to source files if enabled
if(ENABLE_SPDX_HEADERS)
    include(AddSpdxHeaders)
    add_spdx_headers(TARGET ${PROJECT_NAME})
endif()
```

### Adding SPDX Headers

Enable SPDX headers when configuring with CMake:

```bash
cmake -B build -G Ninja -DENABLE_SPDX_HEADERS=ON
```

Then, you can add SPDX headers in the following ways:

1. For a specific target (adds headers to all source files in that target):

   ```bash
   cmake --build build --target <target_name>_add_spdx_headers
   ```

   Example: `cmake --build build --target qsoc_add_spdx_headers`

2. For all test files:

   ```bash
   cmake --build build --target test_add_spdx_headers
   ```

3. During normal build (headers will be added automatically as the targets depend on the SPDX header targets):

   ```bash
   cmake --build build
   ```

### Customizing SPDX Headers

You can customize the SPDX header information:

```bash
cmake -B build -G Ninja -DENABLE_SPDX_HEADERS=ON \
  -DSPDX_LICENSE="MIT" \
  -DSPDX_COPYRIGHT_HOLDER="Your Name <your.email@example.com>" \
  -DSPDX_COPYRIGHT_YEAR_START="2020"
```

### For Individual Files

To add or update SPDX headers for individual files:

```bash
# Add SPDX header to a single file
cmake -P cmake/AddSpdxHeadersHelper.cmake <file_path> <license> <copyright_holder> <copyright_year>

# Example
cmake -P cmake/AddSpdxHeadersHelper.cmake src/main.cpp "Apache-2.0" "Huang Rui <vowstar@gmail.com>" "2023-2025"
```

### Header Format

Headers will be added in this format:

```cpp
// SPDX-License-Identifier: Apache-2.0
// SPDX-FileCopyrightText: 2023-2025 Huang Rui <vowstar@gmail.com>

// Original file content starts here...
```

Where the year range is automatically determined:

- Single year (e.g., "2025") if the file was created in the current year
- Year range (e.g., "2023-2025") if the file was first committed in a previous year

### Notes on Git Dependency

If git is not available on the system and no copyright information is provided, SPDX header generation will be automatically disabled, even if `ENABLE_SPDX_HEADERS` is set to ON. To use SPDX headers without git, you must explicitly specify:

```bash
cmake -B build -G Ninja -DENABLE_SPDX_HEADERS=ON \
  -DSPDX_COPYRIGHT_HOLDER="Your Name <your.email@example.com>"
```
