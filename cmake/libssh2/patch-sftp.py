# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

"""Apply the pinned SFTP ownership fix without changing the source tree."""

import hashlib
import pathlib
import re
import sys

source, patch, destination = map(pathlib.Path, sys.argv[1:])
original = source.read_bytes().replace(b"\r\n", b"\n")
if hashlib.sha256(original).hexdigest() != "409c60734784b5ead795a813ce43c24a2b2440ebb5a7496554bd958bca441ba6":
    sys.exit("libssh2 SFTP source changed: review its ownership fix")
lines = original.decode().splitlines(keepends=True)
output = []
position = 0
for line in patch.read_text().splitlines(keepends=True):
    if line.startswith(("--- ", "+++ ")):
        continue
    if line.startswith("@@ "):
        match = re.fullmatch(r"@@ -(\d+)(?:,\d+)? \+\d+(?:,\d+)? @@[^\n]*\n", line)
        if not match:
            sys.exit("Invalid SFTP patch hunk")
        start = int(match[1]) - 1
        if start < position or start > len(lines):
            sys.exit("Invalid SFTP patch range")
        output.extend(lines[position:start])
        position = start
    elif line.startswith((" ", "-", "+")):
        if line[0] != "+":
            if position >= len(lines) or lines[position] != line[1:]:
                sys.exit("SFTP patch context changed")
            position += 1
        if line[0] != "-":
            output.append(line[1:])
    else:
        sys.exit("Invalid SFTP patch line")
output.extend(lines[position:])
result = "".join(output).encode()
if hashlib.sha256(result).hexdigest() != "f2ce73969332c27cb1da696ac7e9c9f89be63875a8a7ad4344a592a7aec7ff82":
    sys.exit("SFTP ownership fix digest mismatch")
destination.parent.mkdir(parents=True, exist_ok=True)
if not destination.exists() or destination.read_bytes() != result:
    destination.write_bytes(result)
