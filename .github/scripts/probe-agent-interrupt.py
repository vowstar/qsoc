#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

"""Verify terminal-generated interrupts isolate an owned daemon on Unix."""

import fcntl
import json
import os
import pathlib
import pty
import runpy
import select
import signal
import socket
import struct
import sys
import tempfile
import termios
import time


def read_frame(connection):
    def read_exact(size):
        data = b""
        while len(data) < size:
            chunk = connection.recv(size - len(data))
            if not chunk:
                raise RuntimeError("Daemon disconnected during the terminal interrupt probe")
            data += chunk
        return data
    length = int(read_exact(8), 16)
    if not 0 < length <= 2 * 1024 * 1024:
        raise RuntimeError("Invalid daemon frame length")
    return json.loads(read_exact(length))


def send_frame(connection, message):
    payload = json.dumps(message).encode()
    connection.sendall(f"{len(payload):08x}".encode() + payload)


def pump(master, output, duration=0.1):
    if select.select([master], [], [], duration)[0]:
        try:
            output.extend(os.read(master, 65536))
        except OSError:
            pass


def wait_exit(pid, master, output, timeout=10):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        child, status = os.waitpid(pid, os.WNOHANG)
        if child:
            return os.waitstatus_to_exitcode(status)
        pump(master, output)
    raise RuntimeError("Terminal process did not exit before its deadline")


def launch(arguments, working, environment):
    pid, master = pty.fork()
    if pid == 0:
        os.chdir(working)
        fcntl.ioctl(0, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))
        os.execve(arguments[0], arguments, environment)
    return pid, master


def main(program):
    executable = pathlib.Path(program).resolve()
    helpers = runpy.run_path(str(pathlib.Path(__file__).with_name("probe-agent-smt.py")))
    with tempfile.TemporaryDirectory(prefix="test_qsoc_interrupt_", dir="/tmp") as directory:
        working = pathlib.Path(directory)
        environment = helpers["isolated_environment"](working)
        environment["TERM"] = "xterm-256color"
        with helpers["MockServer"]() as mock:
            configuration = {"llm": {"model": mock.model, "models": {mock.model: {
                "name": "Interrupt probe", "key": mock.key,
                "url": f"http://127.0.0.1:{mock.server_port}/v1/chat/completions"}}},
                "proxy": {"type": "none"}}
            (working / ".qsoc.yml").write_text(json.dumps(configuration), encoding="utf-8")
        for mode in ("owned", "parent_exit", "standalone"):
            owned = mode != "standalone"
            endpoint = working / "standalone.sock"
            arguments = ([str(executable), "agent"] if owned else
                         [str(executable.with_name("qsoc-agentd")), "--socket", str(endpoint)])
            pid, master = launch(arguments, working, environment)
            output = bytearray()
            reaped = False
            try:
                deadline = time.monotonic() + 15
                while time.monotonic() < deadline:
                    pump(master, output)
                    if owned:
                        endpoints = list(working.glob("*/agent.sock"))
                        if endpoints and b"Ready" in output:
                            endpoint = endpoints[0]
                            break
                    elif endpoint.exists():
                        break
                else:
                    raise RuntimeError("The terminal process did not become ready")
                with socket.socket(socket.AF_UNIX) as connection:
                    connection.settimeout(10)
                    connection.connect(str(endpoint))
                    greeting = read_frame(connection)
                    daemon_pid = greeting["pid"]
                    foreground = os.tcgetpgrp(master)
                    daemon_group = os.getpgid(daemon_pid)
                    if foreground != pid or not termios.tcgetattr(master)[3] & termios.ISIG:
                        raise RuntimeError("The probe requires foreground terminal signal delivery")
                    if owned:
                        os.write(master, b"pending draft")
                        deadline = time.monotonic() + 0.3
                        while time.monotonic() < deadline:
                            pump(master, output)
                    os.write(master, b"\x03")
                    deadline = time.monotonic() + 0.3
                    while time.monotonic() < deadline:
                        pump(master, output)
                    if owned:
                        send_frame(connection, {"id": 1, "method": "smt.solve", "params": {
                            "smtlib": "(declare-const value Int)(assert (= value 7))",
                            "timeout_ms": 5000}})
                        result = read_frame(connection).get("result", {})
                        if result.get("execution") != "completed" or result.get("solver_status") != "sat":
                            raise RuntimeError(f"The daemon did not survive terminal Ctrl-C: {result}")
                        if daemon_group == foreground:
                            raise RuntimeError("The owned daemon remained in the terminal process group")
                        if mode == "parent_exit":
                            os.kill(pid, signal.SIGKILL)
                            if connection.recv(1):
                                raise RuntimeError("The daemon remained after its owning CLI exited")
                        else:
                            os.write(master, b"/exit\r")
                status = wait_exit(pid, master, output)
                reaped = True
                expected = -signal.SIGKILL if mode == "parent_exit" else 0
                if status != expected:
                    raise RuntimeError(f"Terminal process exited with status {status}")
            except Exception:
                print(output.decode(errors="replace"), file=sys.stderr)
                raise
            finally:
                if not reaped:
                    try:
                        os.kill(pid, signal.SIGKILL)
                        os.waitpid(pid, 0)
                    except ProcessLookupError:
                        pass
                os.close(master)
    print("Terminal Ctrl-C isolation, standalone interrupts and owned daemon cleanup passed",
          flush=True)


if __name__ == "__main__":
    main(sys.argv[1])
