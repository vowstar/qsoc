#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

"""Stage the same program and Qt dependency layout for CI and releases."""

import argparse
import os
import pathlib
import shutil
import subprocess
import sys


PROGRAMS = ("qsoc", "qsoc-gui", "qsoc-agentd", "qsoc-smt-worker")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=pathlib.Path)
    parser.add_argument("destination", type=pathlib.Path)
    parser.add_argument("--linuxdeploy", type=pathlib.Path)
    args = parser.parse_args()
    source = pathlib.Path(__file__).resolve().parents[2]
    build = args.build.resolve()
    destination = args.destination.resolve()
    destination.mkdir(parents=True, exist_ok=True)
    if sys.platform == "win32":
        for name in PROGRAMS:
            shutil.copy2(build / (name + ".exe"), destination)
        subprocess.run(["windeployqt", *[str(destination / (name + ".exe")) for name in PROGRAMS],
                        "--release", "--no-translations", "--no-system-d3d-compiler",
                        "--no-opengl-sw"], check=True)
        compatibility = pathlib.Path(os.environ["QT_ROOT_DIR"]) / "bin" / "Qt6Core5Compat.dll"
        shutil.copy2(compatibility, destination)
        licenses = destination / "license"
    elif sys.platform == "darwin":
        bundle = destination / "QSoC.app"
        shutil.copytree(build / "qsoc-gui.app", bundle, dirs_exist_ok=True)
        companions = [name for name in PROGRAMS if name != "qsoc-gui"]
        for name in companions:
            shutil.copy2(build / name, bundle / "Contents" / "MacOS")
        subprocess.run(["macdeployqt", str(bundle), "-verbose=1",
                        *[f"-executable={bundle / 'Contents' / 'MacOS' / name}" for name in companions]],
                       check=True)
        licenses = bundle / "Contents" / "Resources" / "license"
    else:
        if args.linuxdeploy is None:
            parser.error("--linuxdeploy is required for Linux staging")
        environment = os.environ.copy()
        environment["APPIMAGE_EXTRACT_AND_RUN"] = "1"
        environment["EXTRA_QT_MODULES"] = "svg"
        environment["EXTRA_PLATFORM_PLUGINS"] = "libqoffscreen.so"
        subprocess.run([str(args.linuxdeploy.resolve()), "--appdir", str(destination),
                        "--desktop-file", str(destination / "usr/share/applications/qsoc.desktop"),
                        "--icon-file", str(destination / "usr/share/icons/hicolor/scalable/apps/qsoc.svg"),
                        *[value for name in PROGRAMS for value in
                          ("--executable", str(destination / "usr/bin" / name))],
                        "--plugin", "qt"], env=environment, check=True)
        entry = destination / "AppRun"
        entry.unlink(missing_ok=True)
        shutil.copy2(source / ".github/scripts/AppRun", entry)
        entry.chmod(0o755)
        licenses = destination / "usr" / "share" / "licenses" / "qsoc"
    licenses.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source / "external/z3/LICENSE.txt", licenses / "z3.txt")
    shutil.copy2(source / "external/tiktoken/LICENSE", licenses / "tiktoken.txt")


if __name__ == "__main__":
    main()
