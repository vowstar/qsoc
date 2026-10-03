#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

"""Exercise console broadcasts and keyboard cancellation in the installed CLI."""

import ctypes
from ctypes import wintypes
import io
import json
import msvcrt
import os
import pathlib
import runpy
import subprocess
import sys
import tempfile
import threading
import time


class Coord(ctypes.Structure):
    _fields_ = [("x", wintypes.SHORT), ("y", wintypes.SHORT)]


class Rectangle(ctypes.Structure):
    _fields_ = [(name, wintypes.SHORT) for name in ("left", "top", "right", "bottom")]


class ScreenInfo(ctypes.Structure):
    _fields_ = [("size", Coord), ("cursor", Coord), ("attributes", wintypes.WORD),
                ("window", Rectangle), ("maximum", Coord)]


class KeyEvent(ctypes.Structure):
    _fields_ = [("down", wintypes.BOOL), ("repeat", wintypes.WORD),
                ("key", wintypes.WORD), ("scan", wintypes.WORD),
                ("character", wintypes.WCHAR), ("state", wintypes.DWORD)]


class InputEvent(ctypes.Union):
    _fields_ = [("key", KeyEvent), ("padding", ctypes.c_byte * 16)]


class InputRecord(ctypes.Structure):
    _fields_ = [("kind", wintypes.WORD), ("event", InputEvent)]


kernel = ctypes.WinDLL("kernel32", use_last_error=True)


def api(name, arguments, result=wintypes.BOOL):
    function = getattr(kernel, name)
    function.argtypes = arguments
    function.restype = result
    return function


allocate = api("AllocConsole", [])
detach = api("FreeConsole", [])
control = api("SetConsoleCtrlHandler", [ctypes.c_void_p, wintypes.BOOL])
broadcast = api("GenerateConsoleCtrlEvent", [wintypes.DWORD, wintypes.DWORD])
open_console = api("CreateFileW", [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD,
                                  ctypes.c_void_p, wintypes.DWORD, wintypes.DWORD,
                                  wintypes.HANDLE], wintypes.HANDLE)
close_handle = api("CloseHandle", [wintypes.HANDLE])
write_input = api("WriteConsoleInputW", [wintypes.HANDLE, ctypes.POINTER(InputRecord),
                                       wintypes.DWORD, ctypes.POINTER(wintypes.DWORD)])
screen_info = api("GetConsoleScreenBufferInfo", [wintypes.HANDLE, ctypes.POINTER(ScreenInfo)])
read_screen = api("ReadConsoleOutputCharacterW", [wintypes.HANDLE, wintypes.LPWSTR,
                                                wintypes.DWORD, Coord,
                                                ctypes.POINTER(wintypes.DWORD)])


def require(result, operation):
    if not result:
        raise ctypes.WinError(ctypes.get_last_error(), operation)


def console_file(name):
    handle = open_console(name, 0xC0000000, 3, None, 3, 0, None)
    if handle == ctypes.c_void_p(-1).value:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        descriptor = msvcrt.open_osfhandle(handle, os.O_RDWR | os.O_BINARY)
    except OSError:
        close_handle(handle)
        raise
    return io.FileIO(descriptor, mode="r+b", closefd=True)


def screen():
    with console_file("CONOUT$") as output:
        handle = msvcrt.get_osfhandle(output.fileno())
        info = ScreenInfo()
        require(screen_info(handle, ctypes.byref(info)), "Read console dimensions")
        count = min(65536, info.size.x * (info.window.bottom - info.window.top + 1))
        text = ctypes.create_unicode_buffer(count + 1)
        read = wintypes.DWORD()
        require(read_screen(handle, text, count, Coord(0, info.window.top), ctypes.byref(read)),
                "Read console text")
        return text[:read.value]


def type_text(input_file, text):
    records = (InputRecord * len(text))()
    for record, character in zip(records, text):
        record.kind = 1
        record.event.key = KeyEvent(True, 1, ord(character.upper()), 0, character, 0)
        if character == "\x03":
            record.event.key.key = ord("C")
            record.event.key.state = 8
    written = wintypes.DWORD()
    require(write_input(msvcrt.get_osfhandle(input_file.fileno()), records, len(records),
                        ctypes.byref(written)), "Write console input")
    if written.value != len(records):
        raise RuntimeError("The console did not accept every input record")


def wait_for(condition, process, label):
    deadline = time.monotonic() + 15
    while time.monotonic() < deadline:
        if process.poll() is not None:
            raise RuntimeError(f"The CLI exited while waiting for {label}: {process.returncode}")
        if condition():
            return
        time.sleep(0.05)
    raise RuntimeError("Timed out waiting for " + label)


def main(program):
    executable = pathlib.Path(program).resolve()
    scripts = pathlib.Path(__file__).parent
    helpers = runpy.run_path(str(scripts / "probe-agent-smt.py"))
    protocol = runpy.run_path(str(scripts / "probe-local-smt.py"))
    detach()
    require(allocate(), "Allocate a private console")
    require(control(None, False), "Enable Ctrl-C before starting the CLI")
    try:
        with tempfile.TemporaryDirectory(prefix="test_qsoc_console_interrupt_") as directory:
            working = pathlib.Path(directory)
            environment = helpers["isolated_environment"](working)
            environment["TERM"] = "xterm-256color"
            started = threading.Event()
            release = threading.Event()
            with helpers["MockServer"]() as mock:
                def reply(request):
                    if request.get("model") != mock.model:
                        raise RuntimeError("The CLI did not inherit the configured model")
                    if any(tool.get("function", {}).get("name") == "z3_solve"
                           for tool in request.get("tools", [])):
                        started.set()
                        if not release.wait(60):
                            raise RuntimeError("The console probe did not release its LLM request")
                    return {"role": "assistant", "content": "Console probe complete"}, "stop"
                mock.reply = reply
                configuration = {"llm": {"model": mock.model, "models": {mock.model: {
                    "name": "Console probe", "key": mock.key,
                    "url": f"http://127.0.0.1:{mock.server_port}/v1/chat/completions",
                    "timeout": 90000}}}, "proxy": {"type": "none"}}
                (working / ".qsoc.yml").write_text(json.dumps(configuration), encoding="utf-8")
                server_thread = threading.Thread(target=mock.serve_forever, daemon=True)
                server_thread.start()
                process = None
                try:
                    with console_file("CONIN$") as input_file, \
                            console_file("CONOUT$") as output_file, \
                            (working / "cli.stderr").open("wb") as error_file:
                        for mode in ("broadcast", "keyboard", "query"):
                            started = threading.Event()
                            release = threading.Event()
                            require(control(None, False), "Enable Ctrl-C before starting the CLI")
                            arguments = [str(executable), "agent"]
                            if mode == "query":
                                arguments += ["-q", "Wait for the interrupt probe."]
                            process = subprocess.Popen(arguments, cwd=working, env=environment,
                                stdin=subprocess.DEVNULL if mode == "query" else input_file,
                                stdout=subprocess.PIPE if mode == "query" else output_file,
                                stderr=error_file)
                            # The CLI must inherit normal signal delivery before the controller ignores it.
                            require(control(None, True), "Protect the controller from its broadcast")
                            if mode != "query":
                                wait_for(lambda: "Ready" in screen(), process, "the interactive prompt")
                                draft = "draft-" + mode + "-" + mock.call_id[:12]
                                type_text(input_file, draft)
                                wait_for(lambda: draft in screen(), process, "the rendered idle draft")
                                type_text(input_file, "\x03")
                                def draft_cleared():
                                    text = screen()
                                    return draft not in text and "Ready" in text
                                wait_for(draft_cleared, process, "the cleared idle draft")
                                time.sleep(2.1)
                                wait_for(draft_cleared, process, "the live session after clearing its draft")
                                if started.is_set():
                                    raise RuntimeError("Clearing the idle draft unexpectedly started a turn")
                                type_text(input_file, "Wait for the interrupt probe.\r")
                            wait_for(started.is_set, process, "the active LLM request")
                            if mode != "query":
                                def busy():
                                    text = screen()
                                    return "Reasoning" in text and "Ready" not in text
                                wait_for(busy, process, "the busy prompt without stale Ready text")
                                if "(interrupted)" in screen():
                                    raise RuntimeError("A stale cancellation notice remains before the test")
                            logs = list(working.glob("*/daemon.log"))
                            if len(logs) != 1:
                                raise RuntimeError("Expected exactly one owned daemon directory")
                            endpoint = (logs[0].parent / "agent.sock").as_posix()
                            with protocol["connect"](endpoint) as stream:
                                if protocol["receive"](stream).get("daemon") != "qsoc-agentd":
                                    raise RuntimeError("The owned endpoint returned an invalid greeting")
                                if mode == "keyboard":
                                    type_text(input_file, "\x03")
                                else:
                                    require(broadcast(0, 0), "Broadcast Ctrl-C to the console")
                                if mode == "query":
                                    output, _ = process.communicate(timeout=10)
                                    if process.returncode != 0 or b"(interrupted)" not in output:
                                        raise RuntimeError(f"The redirected query did not cancel: {output!r}")
                                else:
                                    def cancelled():
                                        text = screen()
                                        return "Ready" in text and "(interrupted)" in text
                                    wait_for(cancelled, process, "a fresh cancellation notice and Ready prompt")
                                    protocol["send"](stream, {"id": 1, "method": "smt.solve", "params": {
                                        "smtlib": "(declare-const value Int)(assert (= value 7))",
                                        "timeout_ms": 5000}})
                                    result = protocol["replies"](stream, {1})[1]
                                    if result.get("execution") != "completed" or result.get("solver_status") != "sat":
                                        raise RuntimeError(f"The daemon failed after {mode} cancellation: {result}")
                            release.set()
                            if mode != "query":
                                type_text(input_file, "/exit\r")
                                if process.wait(timeout=10) != 0:
                                    raise RuntimeError("The CLI did not exit cleanly after cancellation")
                            if mock.errors:
                                raise RuntimeError(f"The mock LLM failed: {mock.errors}")
                            print(f"Console interrupt case passed: {mode}", flush=True)
                        require(control(None, False), "Enable Ctrl-C before the standalone daemon")
                        endpoint = "qsoc-console-" + mock.call_id
                        process = subprocess.Popen(
                            [str(executable.with_name("qsoc-agentd.exe")), "--socket", endpoint],
                            cwd=working, env=environment, stdin=input_file,
                            stdout=output_file, stderr=error_file)
                        require(control(None, True), "Protect the controller from its broadcast")
                        with protocol["connect"](endpoint) as stream:
                            if protocol["receive"](stream).get("daemon") != "qsoc-agentd":
                                raise RuntimeError("The standalone daemon did not send its greeting")
                            require(broadcast(0, 0), "Broadcast Ctrl-C to the standalone daemon")
                            if process.wait(timeout=10) != 0:
                                raise RuntimeError("The standalone daemon did not stop on Ctrl-C")
                except Exception:
                    print(screen(), file=sys.stderr)
                    print((working / "cli.stderr").read_text(errors="replace"), file=sys.stderr)
                    raise
                finally:
                    release.set()
                    if process is not None and process.poll() is None:
                        process.kill()
                        process.wait(timeout=10)
                    if process is not None and process.stdout is not None:
                        process.stdout.close()
                    mock.shutdown()
                    server_thread.join()
    finally:
        detach()
    print("Console broadcasts, keyboard cancellation and redirected query interruption passed",
          flush=True)


if __name__ == "__main__":
    main(sys.argv[1])
