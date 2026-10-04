#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

"""Export committed sources and pinned recursive submodules reproducibly."""

import argparse
import contextlib
import hashlib
import io
import json
import lzma
import os
from pathlib import Path, PurePosixPath
import re
import subprocess
import tarfile
import tempfile


MANIFEST = "source-revisions.json"


def git(repository, *arguments):
    environment = dict(os.environ, GIT_NO_REPLACE_OBJECTS="1")
    result = subprocess.run(
        ["git", "-C", str(repository), *arguments], stdout=subprocess.PIPE,
        stderr=subprocess.PIPE, env=environment, check=False)
    if result.returncode:
        raise ValueError("Git command failed: " + result.stderr.decode(errors="replace").strip())
    return result.stdout


def safe_path(value):
    if (not value or value.startswith("/") or "\\" in value or ":" in value
            or any(part in ("", ".", "..") or part.casefold() == ".git"
                   for part in value.split("/"))):
        raise ValueError(f"Unsafe archive path: {value!r}")
    return value


def initialized_repository(repository):
    if not (repository / ".git").exists():
        raise ValueError("Repository or submodule is not initialized")
    top = Path(os.fsdecode(git(repository, "rev-parse", "--show-toplevel")).strip())
    if top.resolve() != repository.resolve():
        raise ValueError("Submodule does not have its own Git worktree")


def collect(repository, commit, prefix="", ancestors=()):
    identity = (repository.resolve(), commit)
    if identity in ancestors:
        raise ValueError("Recursive submodule cycle")
    entries, revisions = [], []
    for record in git(repository, "ls-tree", "-rz", "--full-tree", commit).split(b"\0"):
        if not record:
            continue
        metadata, raw_path = record.split(b"\t", 1)
        mode, kind, oid = metadata.decode("ascii").split()
        local = safe_path(raw_path.decode("utf-8"))
        path = safe_path(prefix + local)
        if path.split("/", 1)[0].casefold() == MANIFEST.casefold():
            raise ValueError("Committed path conflicts with source revision manifest")
        if mode == "160000" and kind == "commit":
            child = repository / local
            if child.resolve() != repository.resolve().joinpath(*PurePosixPath(local).parts):
                raise ValueError(f"Submodule path crosses a symbolic link: {path}")
            initialized_repository(child)
            if git(child, "rev-parse", "HEAD").strip().decode("ascii") != oid:
                raise ValueError(f"Submodule HEAD differs from gitlink: {path}")
            revisions.append({"path": path, "commit": oid})
            nested, pins = collect(child, oid, path + "/", ancestors + (identity,))
            entries.append((path, "040000", child, oid))
            entries.extend(nested)
            revisions.extend(pins)
        elif kind == "blob" and mode in ("100644", "100755", "120000"):
            entries.append((path, mode, repository, oid))
        else:
            raise ValueError(f"Unsupported Git entry: {path}")
    return entries, revisions


class Blobs:
    def __init__(self, repository):
        self.process = subprocess.Popen(
            ["git", "-C", str(repository), "cat-file", "--batch"],
            stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
            env=dict(os.environ, GIT_NO_REPLACE_OBJECTS="1"))

    def read(self, oid):
        self.process.stdin.write(oid.encode("ascii") + b"\n")
        self.process.stdin.flush()
        header = self.process.stdout.readline().split()
        if len(header) != 3 or header[0].decode("ascii") != oid or header[1] != b"blob":
            raise ValueError("Cannot read pinned Git blob")
        size = int(header[2])
        data = self.process.stdout.read(size)
        if len(data) != size or self.process.stdout.read(1) != b"\n":
            raise ValueError("Incomplete Git blob")
        return data

    def close(self):
        self.process.stdin.close()
        self.process.stdout.close()
        self.process.wait()


def check_links(links):
    for path, target in links.items():
        if (not target or "\0" in target or "\\" in target or ":" in target
                or target.startswith("/")):
            raise ValueError(f"Unsafe symbolic link: {path}")
    for path in links:
        pending = path.split("/")
        resolved, followed = [], 0
        while pending:
            part, *pending = pending
            if part in ("", "."):
                continue
            if part == "..":
                if not resolved:
                    raise ValueError(f"Symbolic link escapes archive: {path}")
                resolved.pop()
                continue
            current = "/".join(resolved + [part])
            if current in links:
                followed += 1
                if followed > 40:
                    raise ValueError(f"Symbolic link cycle or excessive indirection: {path}")
                pending = links[current].split("/") + pending
            else:
                resolved.append(part)


def archive(repository, reference, output, version=None):
    repository = Path(repository).resolve()
    initialized_repository(repository)
    commit = git(repository, "rev-parse", "--verify", "--end-of-options",
                 reference + "^{commit}").strip().decode("ascii")
    timestamp = int(git(repository, "show", "-s", "--format=%ct", commit))
    if version is None:
        config = git(repository, "show", commit + ":src/common/config.h")
        match = re.search(rb'^\s*#define\s+QSOC_VERSION\s+"([^"]+)"', config, re.MULTILINE)
        if not match:
            raise ValueError("Cannot find QSOC_VERSION at the requested ref")
        version = match[1].decode("ascii")
    if not re.fullmatch(r"[0-9]+\.[0-9]+\.[0-9]+", version):
        raise ValueError("Archive version must have the form X.Y.Z")
    entries, revisions = collect(repository, commit)
    manifest = json.dumps({"schema_version": 1, "root": {"commit": commit},
                           "submodules": sorted(revisions, key=lambda item: item["path"])},
                          sort_keys=True, indent=2).encode("utf-8") + b"\n"
    entries.append((MANIFEST, "manifest", None, None))
    paths = {entry[0] for entry in entries}
    if len(paths) != len(entries):
        raise ValueError("Duplicate archive paths")
    directories = {str(parent) for path in paths for parent in PurePosixPath(path).parents
                   if str(parent) != "."}
    for path in directories - paths:
        entries.append((path, "040000", None, None))
    top = "qsoc-" + version
    entries.append(("", "040000", None, None))
    output = Path(output).resolve()
    output.mkdir(parents=True, exist_ok=True)
    target = output / (top + ".tar.xz")
    checksum = output / (target.name + ".sha256")
    if os.path.lexists(target) or os.path.lexists(checksum):
        raise ValueError("Archive or checksum already exists")
    with tempfile.TemporaryDirectory(prefix=".qsoc-source-", dir=output) as temporary:
        packed = Path(temporary) / target.name
        digest_file = Path(temporary) / checksum.name
        with contextlib.ExitStack() as stack:
            readers = {}
            def read_blob(repo, oid):
                if repo not in readers:
                    readers[repo] = Blobs(repo)
                    stack.callback(readers[repo].close)
                return readers[repo].read(oid)
            links = {path: read_blob(repo, oid).decode("utf-8")
                     for path, mode, repo, oid in entries if mode == "120000"}
            check_links(links)
            compressed = stack.enter_context(lzma.open(packed, "wb", preset=6))
            tar = stack.enter_context(tarfile.open(fileobj=compressed, mode="w|",
                                                  format=tarfile.PAX_FORMAT))
            for path, mode, repo, oid in sorted(entries):
                info = tarfile.TarInfo(top + ("/" + path if path else ""))
                info.uid = info.gid = 0
                info.uname = info.gname = ""
                info.mtime = timestamp
                info.pax_headers = {}
                data = b""
                if mode == "040000":
                    info.type, info.mode = tarfile.DIRTYPE, 0o755
                else:
                    if mode == "manifest":
                        data = manifest
                    elif mode != "120000":
                        data = read_blob(repo, oid)
                    if mode == "120000":
                        info.type, info.mode, info.linkname = tarfile.SYMTYPE, 0o777, links[path]
                    else:
                        info.mode = 0o755 if mode == "100755" else 0o644
                        info.size = len(data)
                tar.addfile(info, io.BytesIO(data))
        with packed.open("rb") as stream:
            hasher = hashlib.sha256()
            for chunk in iter(lambda: stream.read(1024 * 1024), b""):
                hasher.update(chunk)
            digest = hasher.hexdigest()
        digest_file.write_text(digest + "  " + target.name + "\n", encoding="ascii")
        created = []
        try:
            for source, destination in ((packed, target), (digest_file, checksum)):
                os.link(source, destination)
                created.append((source, destination))
        except OSError:
            for source, destination in created:
                if destination.exists() and os.path.samestat(source.stat(), destination.lstat()):
                    destination.unlink()
            raise
    return target, checksum


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repository", type=Path, required=True)
    parser.add_argument("--ref", required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--version")
    args = parser.parse_args()
    try:
        for path in archive(args.repository, args.ref, args.output_dir, args.version):
            print(path)
    except (ValueError, OSError, UnicodeError) as error:
        parser.exit(1, f"source archive: {error}\n")


if __name__ == "__main__":
    main()
