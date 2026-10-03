#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

"""Check packaged programs, SMT execution and GUI startup."""

import argparse
import json
import os
import pathlib
import plistlib
import struct
import subprocess
import sys
import tempfile
import uuid


def read_pe_subsystem(program):
    with program.open("rb") as source:
        dos = source.read(64)
        if len(dos) != 64 or dos[:2] != b"MZ":
            raise RuntimeError(f"Invalid DOS header: {program}")
        offset = struct.unpack_from("<I", dos, 60)[0]
        if offset < len(dos):
            raise RuntimeError(f"Invalid PE header offset: {program}")
        source.seek(offset)
        header = source.read(24)
        if len(header) != 24 or header[:4] != b"PE\0\0":
            raise RuntimeError(f"Invalid PE header: {program}")
        size = struct.unpack_from("<H", header, 20)[0]
        optional = source.read(size)
        if size < 2 or len(optional) != size:
            raise RuntimeError(f"Truncated PE optional header: {program}")
        minimum = {0x10B: 96, 0x20B: 112}.get(struct.unpack_from("<H", optional)[0])
        if minimum is None:
            raise RuntimeError(f"Unsupported PE optional header: {program}")
        if size < minimum:
            raise RuntimeError(f"Truncated PE optional header: {program}")
        return struct.unpack_from("<H", optional, 68)[0]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=pathlib.Path)
    parser.add_argument("--bundle", type=pathlib.Path)
    parser.add_argument("--desktop", type=pathlib.Path)
    args = parser.parse_args()
    directory = args.directory.resolve()
    suffix = ".exe" if sys.platform == "win32" else ""
    programs = ["qsoc", "qsoc-gui", "qsoc-agentd", "qsoc-smt-worker"]
    for name in programs:
        program = directory / (name + suffix)
        if not program.is_file() or not os.access(program, os.X_OK):
            raise RuntimeError(f"Missing executable: {program}")
        if sys.platform == "win32":
            expected = 2 if name == "qsoc-gui" else 3
            actual = read_pe_subsystem(program)
            if actual != expected:
                raise RuntimeError(f"Wrong PE subsystem for {name}: {actual}, expected {expected}")
    if args.bundle:
        with (args.bundle / "Contents" / "Info.plist").open("rb") as source:
            if plistlib.load(source).get("CFBundleExecutable") != "qsoc-gui":
                raise RuntimeError("The app bundle must start qsoc-gui")
    if args.desktop:
        entries = args.desktop.read_text().splitlines()
        if "Exec=qsoc gui" not in entries:
            raise RuntimeError("The desktop entry must start qsoc gui")
        if "Terminal=false" not in entries:
            raise RuntimeError("The desktop entry must not open a terminal")
    environment = os.environ.copy()
    for variable in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "QT_PLUGIN_PATH",
                     "QT_QPA_PLATFORM_PLUGIN_PATH", "QSOC_BIN_DIR"):
        environment.pop(variable, None)
    qt_root = environment.get("QT_ROOT_DIR")
    if qt_root:
        environment["PATH"] = os.pathsep.join(
            item for item in environment.get("PATH", "").split(os.pathsep)
            if not pathlib.Path(item).resolve().is_relative_to(pathlib.Path(qt_root).resolve())
        )
    package = args.bundle.resolve() if args.bundle else (
        directory if sys.platform == "win32" else directory.parent)
    platforms = [path.parent for path in package.rglob("*offscreen*")
                 if path.parent.name == "platforms"]
    if not platforms and sys.platform.startswith("linux"):
        raise RuntimeError("The package is missing the offscreen platform plugin")
    environment.pop("QT_QPA_PLATFORM", None)
    if platforms:
        environment["QT_QPA_PLATFORM_PLUGIN_PATH"] = str(platforms[0])
        environment["QT_QPA_PLATFORM"] = "offscreen"
    entry = directory.parent.parent / "AppRun" if sys.platform.startswith("linux") else None
    if entry and not entry.is_file():
        raise RuntimeError("The package is missing its AppRun entry point")
    temporary_root = None if os.name == "nt" else "/tmp"
    with tempfile.TemporaryDirectory(prefix="test_qsoc_deployment_", dir=temporary_root) as working:
        for name in ("config", "cache", "data", "runtime"):
            (pathlib.Path(working) / name).mkdir(mode=0o700)
        environment.update({
            "HOME": working, "USERPROFILE": working,
            "TMPDIR": working, "TMP": working, "TEMP": working,
            "QSOC_HOME": str(pathlib.Path(working) / "config" / "qsoc"),
            "XDG_CONFIG_HOME": str(pathlib.Path(working) / "config"),
            "XDG_CACHE_HOME": str(pathlib.Path(working) / "cache"),
            "XDG_DATA_HOME": str(pathlib.Path(working) / "data"),
            "XDG_RUNTIME_DIR": str(pathlib.Path(working) / "runtime"),
            "APPDATA": str(pathlib.Path(working) / "config"),
            "LOCALAPPDATA": str(pathlib.Path(working) / "cache"),
        })
        for name in ("qsoc", "qsoc-agentd"):
            program = entry if entry and name == "qsoc" else directory / (name + suffix)
            try:
                version = subprocess.run([str(program), "--version"],
                                         cwd=working, env=environment, check=True, timeout=15,
                                         stdin=subprocess.DEVNULL, capture_output=True)
            except (subprocess.CalledProcessError, subprocess.TimeoutExpired) as error:
                print(((error.stdout or b"") + (error.stderr or b"")).decode(errors="replace"),
                      flush=True)
                raise
            print((version.stdout + version.stderr).decode(errors="replace"), flush=True)
        resource_probe = subprocess.run(
            [str(entry or directory / ("qsoc" + suffix)), "agent", "--resources"],
            cwd=working, env=environment, timeout=15,
            stdin=subprocess.DEVNULL, capture_output=True)
        try:
            resource_probe.check_returncode()
            snapshot = json.loads(resource_probe.stdout)
            if snapshot.get("scope") != "local_daemon" or snapshot.get("status") not in ("ok", "partial"):
                raise RuntimeError("The packaged resource probe did not produce a snapshot")
            system = snapshot["system"]
            for metric in ("memory_total_bytes", "memory_available_bytes", "cpu_total_ns"):
                if not isinstance(system.get(metric), (int, float)) or system[metric] < 0:
                    raise RuntimeError(f"Missing native resource metric: {metric}")
            if len(snapshot["processes"]) != 1 or not snapshot["storage"]:
                raise RuntimeError("Resource queries must sample storage without starting a session")
            if not snapshot["storage"][0].get("valid"):
                raise RuntimeError("The packaged storage probe failed")
        except (subprocess.CalledProcessError, ValueError, KeyError, RuntimeError):
            print((resource_probe.stdout + resource_probe.stderr).decode(errors="replace"), flush=True)
            raise
        print("Packaged native resource snapshot passed", flush=True)
        subprocess.run(
            [sys.executable, str(pathlib.Path(__file__).with_name("probe-agent-smt.py")),
             str(directory / ("qsoc" + suffix))],
            cwd=working, env=environment, check=True, timeout=100)
        interrupt_probe = ("probe-agent-interrupt-windows.py" if os.name == "nt"
                           else "probe-agent-interrupt.py")
        subprocess.run(
            [sys.executable, str(pathlib.Path(__file__).with_name(interrupt_probe)),
             str(directory / ("qsoc" + suffix))],
            cwd=working, env=environment, check=True, timeout=90 if os.name == "nt" else 60)
        subprocess.run(
            [sys.executable, str(pathlib.Path(__file__).with_name("probe-agent-smt.py")),
             str(directory / ("qsoc" + suffix)), "--resources"],
            cwd=working, env=environment, check=True, timeout=100)
        endpoint = ("qsoc-deployment-" + uuid.uuid4().hex if os.name == "nt"
                    else str(pathlib.Path(working) / "daemon.sock"))
        daemon = subprocess.Popen(
            [str(directory / ("qsoc-agentd" + suffix)), "--socket", endpoint,
             "--parent-pid", str(os.getpid())],
            cwd=working, env=environment, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            subprocess.run([sys.executable, str(pathlib.Path(__file__).with_name("probe-local-smt.py")),
                            endpoint], cwd=working, env=environment, check=True, timeout=30)
        finally:
            if daemon.poll() is None:
                daemon.terminate()
            try:
                output, error = daemon.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                daemon.kill()
                output, error = daemon.communicate()
            if output or error:
                print((output + error).decode(errors="replace"), flush=True)
        gui_entry = entry or directory / ("qsoc-gui" + suffix)
        process = subprocess.Popen([str(gui_entry)],
                                   cwd=working, env=environment,
                                   stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        try:
            try:
                output, error = process.communicate(timeout=3)
            except subprocess.TimeoutExpired:
                pass
            else:
                raise RuntimeError(f"GUI exited during startup ({process.returncode}): "
                                   f"{(output + error).decode(errors='replace')}")
        finally:
            if process.poll() is None:
                process.terminate()
                try:
                    process.communicate(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.communicate()


if __name__ == "__main__":
    main()
