#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

"""Check AppDir icon staging with a real SVG renderer."""

import pathlib
import runpy
import subprocess
import tempfile
import unittest

SCRIPTS = pathlib.Path(__file__).parent
stage_icon = runpy.run_path(str(SCRIPTS / "stage-deployment.py"))["stage_appdir_icon"]
verify_icon = runpy.run_path(str(SCRIPTS / "verify-deployment.py"))[
    "verify_appdir_icon"
]


class AppDirIconTest(unittest.TestCase):
    def test_icon_staging(self):
        with tempfile.TemporaryDirectory(
            prefix="test_qsoc_stage_", dir="/tmp"
        ) as working:
            appdir = pathlib.Path(working)
            svg = appdir / "usr/share/icons/hicolor/scalable/apps/qsoc.svg"
            svg.parent.mkdir(parents=True)
            svg.write_text(
                '<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 32 32">'
                '<rect x="2" y="2" width="28" height="28" fill="#145c91"/>'
                '<circle cx="16" cy="16" r="8" fill="#ffffff"/></svg>',
                encoding="utf-8",
            )
            original = svg.read_bytes()
            icon = appdir / ".DirIcon"
            with self.subTest("missing"):
                with self.assertRaisesRegex(RuntimeError, "DirIcon"):
                    verify_icon(appdir)
            icon.symlink_to(svg.relative_to(appdir))
            with self.subTest("svg"):
                with self.assertRaisesRegex(RuntimeError, "256x256 PNG"):
                    verify_icon(appdir)
            stage_icon(appdir)
            verify_icon(appdir)
            self.assertFalse(icon.is_symlink())
            self.assertEqual(svg.read_bytes(), original)
            png = icon.read_bytes()
            with self.subTest("truncated"):
                icon.write_bytes(png[:20])
                with self.assertRaisesRegex(RuntimeError, "256x256 PNG"):
                    verify_icon(appdir)
            with self.subTest("wrong size"):
                subprocess.run(
                    ["rsvg-convert", "-w", "32", "-h", "32", "-o", str(icon), str(svg)],
                    check=True,
                )
                with self.assertRaisesRegex(RuntimeError, "256x256 PNG"):
                    verify_icon(appdir)


if __name__ == "__main__":
    unittest.main()
