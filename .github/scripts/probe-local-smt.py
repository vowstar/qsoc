#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

"""Exercise the deployed daemon's SMT requests without an LLM session."""

import json
import os
import re
import socket
import sys
import subprocess
import time


def connect(endpoint):
    deadline = time.monotonic() + 10
    while True:
        local = None
        try:
            if os.name == "nt":
                return open("\\\\.\\pipe\\" + endpoint, "r+b", buffering=0)
            local = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            local.settimeout(10)
            local.connect(endpoint)
            stream = local.makefile("rwb", buffering=0)
            local.close()
            return stream
        except OSError:
            if local is not None:
                local.close()
            if time.monotonic() >= deadline:
                raise
            time.sleep(0.02)


def read_exact(stream, count):
    result = bytearray()
    while len(result) < count:
        data = stream.read(count - len(result))
        if not data:
            raise RuntimeError("The daemon closed an incomplete frame")
        result.extend(data)
    return result


def receive(stream):
    header = read_exact(stream, 8)
    if not re.fullmatch(b"[0-9a-fA-F]{8}", header):
        raise RuntimeError("Invalid daemon frame length")
    count = int(header, 16)
    if not 0 < count <= 16 * 1024 * 1024:
        raise RuntimeError("Daemon frame exceeds the protocol limit")
    value = json.loads(read_exact(stream, count))
    if not isinstance(value, dict):
        raise RuntimeError("Expected a JSON object")
    return value


def frame(value):
    payload = json.dumps(value, separators=(",", ":")).encode("utf-8")
    return f"{len(payload):08x}".encode("ascii") + payload


def send(stream, *requests):
    pending = memoryview(b"".join(frame(request) for request in requests))
    while pending:
        count = stream.write(pending)
        if not count:
            raise RuntimeError("The daemon stopped accepting requests")
        pending = pending[count:]


def replies(stream, ids):
    results = {}
    for _ in range(200):
        message = receive(stream)
        if message.get("id") in ids:
            if "error" in message:
                raise RuntimeError(f"Daemon request failed: {message['error']}")
            results[message["id"]] = message.get("result", {})
            if results.keys() >= ids:
                return results
    raise RuntimeError("The daemon did not complete the requests")


def main(endpoint, program=None):
    if program:
        for arguments, source, status in (
                ([], "(assert true)", "sat"),
                (["--connect", endpoint], "(assert false)", "unsat"),
                (["--connect", endpoint, "--mode", "optimize"],
                 "(declare-const x Int)(assert (>= x 1))(minimize x)", "sat")):
            completed = subprocess.run([program, "smt", *arguments, "-"],
                                       input=source.encode(), capture_output=True, timeout=20)
            result = json.loads(completed.stdout)
            if (completed.returncode != 0 or result.get("execution") != "completed"
                    or result.get("solver_status") != status):
                raise RuntimeError(f"Packaged SMT CLI failed: {result}, {completed.stderr!r}")
        print("Packaged owned and connected SMT CLI passed", flush=True)
    request = {"smtlib": "(declare-const x Int)(assert (= x 1))",
               "mode": "check", "timeout_ms": 10000}
    with connect(endpoint) as stream:
        greeting = receive(stream)
        if greeting.get("daemon") != "qsoc-agentd" or greeting.get("protocol") != 1:
            raise RuntimeError("Unexpected daemon greeting")
        send(stream, {"id": 1, "method": "smt.solve", "params": request})
        result = replies(stream, {1})[1]
        if result.get("execution") != "completed" or result.get("solver_status") != "sat":
            raise RuntimeError(f"The packaged solver failed: {result}")
        count = 100
        symbols = [f"x{index}" for index in range(count)]
        problem = "(set-logic QF_LIA)" + "".join(
            f"(declare-const {symbol} Int)(assert (and (<= 0 {symbol}) (< {symbol} {count - 1})))"
            for symbol in symbols)
        problem += "(assert (distinct " + " ".join(symbols) + "))"
        send(stream,
             {"id": 2, "method": "smt.solve", "params": dict(request, smtlib=problem)},
             {"id": 3, "method": "smt.cancel", "params": {"request_id": 2}})
        results = replies(stream, {2, 3})
        if results[3].get("canceled") is not True:
            raise RuntimeError(f"The packaged daemon rejected cancellation: {results[3]}")
        result = results[2]
        if result.get("execution") != "cancelled":
            raise RuntimeError(f"The packaged solver did not cancel: {result}")
        send(stream, *[{"id": index, "method": "smt.solve", "params": request}
                       for index in (4, 5)])
        for result in replies(stream, {4, 5}).values():
            if result.get("execution") != "completed" or result.get("solver_status") != "sat":
                raise RuntimeError(f"The packaged solver did not recover: {result}")
    print("Packaged SMT solve, cancellation and recovery passed", flush=True)


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
