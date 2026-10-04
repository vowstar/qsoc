#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

"""Exercise source archives with repositories created for each test."""

import hashlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from unittest import mock


SPEC = importlib.util.spec_from_file_location(
    "source_archive", Path(__file__).with_name("source-archive.py"))
ARCHIVE = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(ARCHIVE)


class SourceArchive(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="test_qsoc_source_archive_")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.main = self.root / "main"
        self.output = self.root / "output"
        leaf = self.repository("leaf")
        (leaf / "nested.txt").write_text("nested committed content\n")
        self.leaf_commit = self.commit(leaf)
        child = self.repository("child")
        (child / "child.txt").write_text("child committed content\n")
        self.git(child, "-c", "protocol.file.allow=always", "submodule", "add",
                 str(leaf), "nested")
        self.child_commit = self.commit(child)
        main = self.repository("main")
        (main / "src/common").mkdir(parents=True)
        (main / "src/common/config.h").write_text('#define QSOC_VERSION "1.2.3"\n')
        (main / "keep.txt").write_text("committed root content\n")
        (main / ".gitattributes").write_text(
            "keep.txt export-ignore\n.git_archival.txt export-subst\n")
        (main / ".git_archival.txt").write_text("node: $Format:%H$\n")
        (main / "run.sh").write_text("#!/bin/sh\nexit 0\n")
        (main / "run.sh").chmod(0o755)
        (main / "link").symlink_to("keep.txt")
        self.git(main, "-c", "protocol.file.allow=always", "submodule", "add",
                 str(child), "external/child")
        self.git(main, "-c", "protocol.file.allow=always", "submodule", "update",
                 "--init", "--recursive")
        self.commit_id = self.commit(main)
        self.git(main, "tag", "v9.8.7")

    def git(self, repository, *arguments):
        environment = dict(os.environ, GIT_AUTHOR_DATE="2026-01-02T03:04:05Z",
                           GIT_COMMITTER_DATE="2026-01-02T03:04:05Z",
                           GIT_CONFIG_NOSYSTEM="1", GIT_CONFIG_GLOBAL=os.devnull)
        return subprocess.check_output(["git", "-C", str(repository), *arguments],
                                       stderr=subprocess.PIPE, env=environment).decode().strip()

    def repository(self, name):
        repository = self.root / name
        repository.mkdir()
        self.git(repository, "init", "-q")
        self.git(repository, "config", "user.name", "Source Fixture")
        self.git(repository, "config", "user.email", "fixture@example.invalid")
        self.git(repository, "config", "commit.gpgsign", "false")
        return repository

    def commit(self, repository):
        self.git(repository, "add", "--all")
        self.git(repository, "commit", "-qm", "fixture content")
        return self.git(repository, "rev-parse", "HEAD")

    def test_recursive_content_metadata_and_reproducibility(self):
        (self.main / "keep.txt").write_text("dirty replacement\n")
        (self.main / "untracked.txt").write_text("untracked content\n")
        child = self.main / "external/child"
        (child / "child.txt").write_text("dirty child\n")
        (child / "nested/nested.txt").write_text("dirty nested child\n")
        (child / "untracked.txt").write_text("untracked child\n")
        first, checksum = ARCHIVE.archive(self.main, "v9.8.7", self.output)
        second, _ = ARCHIVE.archive(self.main, self.commit_id, self.root / "second")
        self.assertEqual(first.read_bytes(), second.read_bytes())
        self.assertEqual(checksum.read_text(), hashlib.sha256(first.read_bytes()).hexdigest()
                         + "  qsoc-1.2.3.tar.xz\n")
        timestamp = int(self.git(self.main, "show", "-s", "--format=%ct", self.commit_id))
        with tarfile.open(first) as archive:
            names = archive.getnames()
            self.assertEqual(names, sorted(names))
            self.assertEqual(len(names), len(set(names)))
            self.assertFalse(any(".git" in Path(name).parts for name in names))
            self.assertFalse(any(name.endswith("untracked.txt") for name in names))
            prefix = "qsoc-1.2.3/"
            self.assertEqual(archive.extractfile(prefix + "keep.txt").read(),
                             b"committed root content\n")
            self.assertEqual(archive.extractfile(prefix + ".git_archival.txt").read(),
                             b"node: $Format:%H$\n")
            self.assertEqual(archive.extractfile(prefix + "external/child/child.txt").read(),
                             b"child committed content\n")
            self.assertEqual(archive.extractfile(prefix + "external/child/nested/nested.txt").read(),
                             b"nested committed content\n")
            self.assertEqual(archive.getmember(prefix + "run.sh").mode, 0o755)
            self.assertTrue(archive.getmember(prefix + "link").issym())
            self.assertEqual(archive.getmember(prefix + "link").linkname, "keep.txt")
            for member in archive:
                self.assertEqual((member.uid, member.gid, member.uname, member.gname, member.mtime),
                                 (0, 0, "", "", timestamp))
                self.assertFalse(set(member.pax_headers) - {"path", "linkpath"})
            manifest = json.load(archive.extractfile(prefix + ARCHIVE.MANIFEST))
            self.assertEqual(manifest, {"schema_version": 1, "root": {"commit": self.commit_id},
                "submodules": [{"path": "external/child", "commit": self.child_commit},
                               {"path": "external/child/nested", "commit": self.leaf_commit}]})
        shutil.rmtree(self.main)
        extracted = self.root / "extracted"
        with tarfile.open(first) as archive:
            archive.extractall(extracted, filter="data")
        self.assertEqual((extracted / "qsoc-1.2.3/link").read_text(), "committed root content\n")
        self.assertEqual((extracted / "qsoc-1.2.3/external/child/nested/nested.txt").read_text(),
                         "nested committed content\n")

    def test_explicit_historical_version_does_not_change_sources(self):
        (self.main / "src/common/config.h").write_text('#define QSOC_VERSION "2.0.0"\n')
        self.commit(self.main)
        packed, _ = ARCHIVE.archive(self.main, "v9.8.7", self.output, "9.8.7")
        self.assertEqual(packed.name, "qsoc-9.8.7.tar.xz")
        with tarfile.open(packed) as archive:
            self.assertEqual(archive.extractfile("qsoc-9.8.7/src/common/config.h").read(),
                             b'#define QSOC_VERSION "1.2.3"\n')

    def test_uninitialized_submodule_rejected(self):
        self.git(self.main, "submodule", "deinit", "-f", "--all")
        with self.assertRaisesRegex(ValueError, "not initialized"):
            ARCHIVE.archive(self.main, "HEAD", self.output)
        self.assertFalse(self.output.exists())

    def test_wrong_recursive_commit_rejected(self):
        child = self.main / "external/child/nested"
        self.git(child, "-c", "user.name=Source Fixture", "-c",
                 "user.email=fixture@example.invalid", "-c", "commit.gpgsign=false",
                 "commit", "--allow-empty", "-qm", "different committed fixture")
        with self.assertRaisesRegex(ValueError, "HEAD differs"):
            ARCHIVE.archive(self.main, "HEAD", self.output)
        self.assertFalse(self.output.exists())

    def test_unsafe_link_cleans_temporary_output(self):
        (self.main / "escape").symlink_to("../outside")
        self.commit(self.main)
        with self.assertRaisesRegex(ValueError, "escapes archive"):
            ARCHIVE.archive(self.main, "HEAD", self.output)
        self.assertEqual(list(self.output.iterdir()), [])

    def test_manifest_path_conflict(self):
        (self.main / ARCHIVE.MANIFEST).mkdir()
        (self.main / ARCHIVE.MANIFEST / "file").write_text("tracked fixture\n")
        self.commit(self.main)
        with self.assertRaisesRegex(ValueError, "conflicts"):
            ARCHIVE.archive(self.main, "HEAD", self.output)

    def test_links_cannot_escape_through_another_link(self):
        (self.main / "alias").symlink_to(".")
        (self.main / "escape").symlink_to("alias/../outside")
        self.commit(self.main)
        with self.assertRaisesRegex(ValueError, "escapes archive"):
            ARCHIVE.archive(self.main, "HEAD", self.output)
        self.assertEqual(list(self.output.iterdir()), [])

    def test_internal_parent_links_remain_valid(self):
        (self.main / "directory").mkdir()
        (self.main / "directory/link").symlink_to("../keep.txt")
        self.commit(self.main)
        packed, _ = ARCHIVE.archive(self.main, "HEAD", self.output)
        with tarfile.open(packed) as archive:
            self.assertEqual(archive.getmember("qsoc-1.2.3/directory/link").linkname, "../keep.txt")

    def test_invalid_versions_and_paths(self):
        for version in ("../1.2.3", "v1.2.3", "1.2", "1.2.3/extra", "1.2.3\n"):
            with self.subTest(version=version), self.assertRaises(ValueError):
                ARCHIVE.archive(self.main, "HEAD", self.output, version)
        for path in ("../file", "/file", ".git/config", "dir/.GIT/config", "a/../b",
                     "a//b", "C:/file", "a\\b"):
            with self.subTest(path=path), self.assertRaises(ValueError):
                ARCHIVE.safe_path(path)

    def test_failure_publishing_checksum_removes_new_archive(self):
        link = os.link
        def fail_checksum(source, destination):
            if str(destination).endswith(".sha256"):
                raise OSError("injected checksum publication failure")
            return link(source, destination)
        with mock.patch.object(ARCHIVE.os, "link", side_effect=fail_checksum):
            with self.assertRaises(OSError):
                ARCHIVE.archive(self.main, "HEAD", self.output)
        self.assertEqual(list(self.output.iterdir()), [])

    def test_existing_outputs_are_not_replaced(self):
        packed, digest = ARCHIVE.archive(self.main, "HEAD", self.output)
        expected = (packed.read_bytes(), digest.read_bytes())
        with self.assertRaisesRegex(ValueError, "already exists"):
            ARCHIVE.archive(self.main, "HEAD", self.output)
        self.assertEqual((packed.read_bytes(), digest.read_bytes()), expected)


if __name__ == "__main__":
    unittest.main()
