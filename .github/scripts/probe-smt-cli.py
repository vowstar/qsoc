#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

"""Cancel an active packaged SMT CLI request with a native console signal."""

import json
import os
import pathlib
import runpy
import signal
import subprocess
import sys
import tempfile
import time


def main(program):
    scripts = pathlib.Path(__file__).parent
    protocol = runpy.run_path(str(scripts / "probe-local-smt.py"))
    helpers = runpy.run_path(str(scripts / "probe-agent-smt.py"))
    console = None
    if os.name == "nt":
        console = runpy.run_path(str(scripts / "probe-agent-interrupt-windows.py"))
        console["detach"]()
        console["require"](console["allocate"](), "Allocate SMT probe console")
        console["require"](console["control"](None, False), "Enable CLI console interrupts")
    process = None
    try:
        temporary_root = None if os.name == "nt" else "/tmp"
        with tempfile.TemporaryDirectory(prefix="test_qsoc_smt_cli_", dir=temporary_root) as name:
            working = pathlib.Path(name).resolve()
            environment = helpers["isolated_environment"](working)
            symbols = [f"x{index}" for index in range(100)]
            source = "(set-logic QF_LIA)" + "".join(
                f"(declare-const {symbol} Int)(assert (and (<= 0 {symbol}) (< {symbol} 99)))"
                for symbol in symbols)
            source += "(assert (distinct " + " ".join(symbols) + "))"
            process = subprocess.Popen([program, "smt", "--timeout-ms", "20000", "-"],
                                       cwd=working, env=environment, stdin=subprocess.PIPE,
                                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if console:
                console["require"](console["control"](None, True), "Protect the probe controller")
            process.stdin.write(source.encode())
            process.stdin.close()
            process.stdin = None
            deadline = time.monotonic() + 15
            logs = []
            while time.monotonic() < deadline:
                logs = list(working.rglob("daemon.log"))
                if process.poll() is not None:
                    raise RuntimeError(f"SMT CLI exited before cancellation: {process.communicate()!r}")
                if logs:
                    break
                time.sleep(0.02)
            if len(logs) != 1:
                raise RuntimeError("Expected one owned SMT daemon")
            endpoint = (logs[0].parent / "agent.sock").as_posix()
            with protocol["connect"](endpoint) as stream:
                protocol["receive"](stream)
                active = False
                request_id = 1
                while time.monotonic() < deadline:
                    protocol["send"](stream, {"id": request_id, "method": "resources"})
                    result = protocol["replies"](stream, {request_id})[request_id]
                    request_id += 1
                    if result.get("smt", {}).get("active_count") == 1:
                        active = True
                        break
                    time.sleep(0.02)
                if not active:
                    raise RuntimeError("The SMT CLI did not start its worker")
                if console:
                    console["require"](console["broadcast"](0, 0), "Interrupt the SMT CLI")
                else:
                    process.send_signal(signal.SIGINT)
                output, error = process.communicate(timeout=10)
                result = json.loads(output)
                if process.returncode != 130 or result.get("execution") != "cancelled":
                    raise RuntimeError(f"SMT CLI cancellation failed: {process.returncode}, {result}")
                if error:
                    print(error.decode(errors="replace"), file=sys.stderr)
            if logs[0].parent.exists():
                raise RuntimeError("The owned daemon directory survived CLI cleanup")
    finally:
        if process is not None and process.poll() is None:
            process.kill()
            process.communicate(timeout=5)
        if console:
            console["detach"]()
    print("Packaged SMT CLI native cancellation and owned cleanup passed", flush=True)


if __name__ == "__main__":
    main(str(pathlib.Path(sys.argv[1]).resolve()))
