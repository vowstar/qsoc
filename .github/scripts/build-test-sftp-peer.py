#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

"""Build the test-only SFTP peer with a checksum-pinned Go toolchain."""

import argparse
import hashlib
import os
from pathlib import Path
import platform
import shutil
import subprocess
import sys
import tarfile
import urllib.request
import zipfile

VERSION = "1.26.11"
ARCHIVES = {
    ("linux", "amd64"): ("tar.gz", "9849f4e04ee35303714e34fbc3ac8ab44ea3e448a95aa182f91e3b9b79c710ec"),
    ("darwin", "amd64"): ("tar.gz", "394baae971332f2bc783d6e2004ae57101479bd3b06ed8209a3fe3570d4ce498"),
    ("darwin", "arm64"): ("tar.gz", "e3a42e67a7a8acf450ebada2b074c7ca863000ca514907b7185b45e8f5cdff57"),
    ("windows", "amd64"): ("zip", "71ae086f0137e665f855a811cc119152b6a783e0bbc1b906e709f46d80141eb0"),
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    system = "windows" if sys.platform == "win32" else sys.platform
    machine = platform.machine().lower()
    architecture = {"x86_64": "amd64", "amd64": "amd64", "arm64": "arm64", "aarch64": "arm64"}.get(machine)
    suffix, expected = ARCHIVES[(system, architecture)]
    cache = args.cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    name = f"go{VERSION}.{system}-{architecture}.{suffix}"
    archive = cache / name
    if not archive.exists():
        temporary = archive.with_suffix(".download")
        with urllib.request.urlopen(f"https://go.dev/dl/{name}", timeout=30) as source, temporary.open("wb") as output:
            shutil.copyfileobj(source, output, 1024 * 1024)
        temporary.replace(archive)
    with archive.open("rb") as source:
        actual = hashlib.file_digest(source, "sha256").hexdigest()
    if actual != expected:
        raise RuntimeError("Go archive checksum mismatch")
    sdk = cache / f"go-{VERSION}-{system}-{architecture}"
    executable = sdk / "go" / "bin" / ("go.exe" if system == "windows" else "go")
    marker = sdk / "archive.sha256"
    if not executable.exists() or not marker.exists() or marker.read_text().strip() != expected:
        if sdk.exists():
            shutil.rmtree(sdk)
        sdk.mkdir()
        if suffix == "zip":
            with zipfile.ZipFile(archive) as bundle:
                bundle.extractall(sdk)
        else:
            with tarfile.open(archive) as bundle:
                bundle.extractall(sdk, filter="data")
        marker.write_text(expected + "\n")
    environment = os.environ.copy()
    environment.update({"GOTOOLCHAIN": "local", "CGO_ENABLED": "0", "GOMAXPROCS": "2", "GOMEMLIMIT": "512MiB", "GOCACHE": str(cache / "build"), "GOMODCACHE": str(cache / "modules")})
    version = subprocess.check_output([executable, "version"], env=environment, text=True)
    if f"go{VERSION} " not in version:
        raise RuntimeError("Unexpected Go toolchain version")
    destination = args.output.resolve()
    destination.parent.mkdir(parents=True, exist_ok=True)
    source = Path(__file__).resolve().parents[2] / "test" / "sftp_peer"
    subprocess.run([executable, "build", "-mod=readonly", "-p", "1", "-trimpath", "-buildvcs=false", "-o", destination, "."], cwd=source, env=environment, check=True)
    subprocess.run([executable, "mod", "verify"], cwd=source, env=environment, check=True)
    print(f"Test SFTP peer built with Go {VERSION}")


if __name__ == "__main__":
    main()
