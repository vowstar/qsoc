#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

"""Run the local-process contract tests and reject skipped coverage."""

import argparse
import os
import pathlib
import subprocess
import tempfile
import xml.etree.ElementTree as ET


CORE_TESTS = (
    "test_qsocagentdaemon",
    "test_qsocprocessowner",
    "test_qsoclocalpeer",
    "test_qsocguihandoff",
    "test_qsocsmtservice",
    "test_qsoctoolsmt",
    "test_qsocresourceusage",
    "test_qsocdaemonresources",
    "test_qsoctoolresources",
    "test_qsocsmtbroker",
    "test_qsocagentprefixstability",
)


def check_report(report):
    root = ET.parse(report).getroot()
    functions = [node for node in root.findall("TestFunction")
                 if node.get("name") not in ("initTestCase", "cleanupTestCase")]
    if not functions or not root.findall(".//Incident"):
        raise RuntimeError(f"No executed test cases in {report}")
    for node in root.iter():
        if node.get("type") in ("skip", "fail", "xfail", "xpass"):
            raise RuntimeError(f"Incomplete core coverage in {report}: {ET.tostring(node, encoding='unicode')}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("build", type=pathlib.Path)
    parser.add_argument("tests", nargs="*")
    args = parser.parse_args()
    build = args.build.resolve()
    reports = build / "core-test-reports"
    reports.mkdir(exist_ok=True)
    suffix = ".exe" if os.name == "nt" else ""
    environment = os.environ.copy()
    environment["QSOC_TEST_DEPS_REQUIRED"] = "1"
    environment["QT_QPA_PLATFORM"] = "offscreen"
    for name in args.tests or CORE_TESTS:
        matches = [path for path in build.rglob(name + suffix) if path.is_file()]
        if len(matches) != 1:
            raise RuntimeError(f"Expected one built {name}, found {len(matches)}")
        report = reports / (name + ".xml")
        report.unlink(missing_ok=True)
        with tempfile.TemporaryDirectory(prefix="test_qsoc_core_") as working:
            try:
                subprocess.run([str(matches[0]), "-o", str(report) + ",xml"],
                               cwd=working, env=environment, check=True, timeout=180)
                check_report(report)
            except (subprocess.SubprocessError, RuntimeError, ET.ParseError):
                if report.exists():
                    print(report.read_text(errors="replace"), flush=True)
                raise
        print(f"{name}: passed without skips", flush=True)


if __name__ == "__main__":
    main()
