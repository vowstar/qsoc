#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

"""Exercise packaged CLI discovery, daemon startup and SMT tool execution."""

import http.server
import json
import os
import pathlib
import secrets
import subprocess
import sys
import tempfile
import threading
import uuid


class MockServer(http.server.ThreadingHTTPServer):
    daemon_threads = True

    def __init__(self, resource_probe=False):
        super().__init__(("127.0.0.1", 0), MockHandler)
        self.resource_probe = resource_probe
        self.tool_name = "system_resources" if resource_probe else "z3_solve"
        self.call_id = uuid.uuid4().hex
        self.marker = "SMT_DEPLOYMENT_" + uuid.uuid4().hex
        self.key = secrets.token_hex(24)
        self.model = "deployment-probe"
        self.tool_calls = 0
        self.tool_results = 0
        self.errors = []
        self.lock = threading.Lock()

    def reply(self, request):
        if request.get("model") != self.model:
            raise RuntimeError("The agent did not inherit the configured model")
        tools = request.get("tools", [])
        if not any(tool.get("function", {}).get("name") == self.tool_name for tool in tools):
            return {"role": "assistant", "content": "Deployment probe"}, "stop"
        results = [message for message in request.get("messages", [])
                   if message.get("role") == "tool"
                   and message.get("tool_call_id") == self.call_id]
        with self.lock:
            if results:
                if self.tool_calls != 1 or len(results) != 1:
                    raise RuntimeError("Expected exactly one SMT tool call and result")
                result = json.loads(results[0]["content"])
                if self.resource_probe:
                    if (result.get("scope") != "local_daemon"
                            or result.get("status") not in ("ok", "partial")
                            or not isinstance(result.get("system", {}).get("memory_total_bytes"), (int, float))
                            or len(result.get("processes", [])) < 2
                            or not result.get("storage")):
                        raise RuntimeError(f"The packaged resource tool failed: {result}")
                else:
                    if result.get("execution") != "completed" or result.get("solver_status") != "sat":
                        raise RuntimeError(f"The packaged SMT tool failed: {result}")
                    if "deployment_value" not in result.get("model_smtlib", ""):
                        raise RuntimeError("The solver did not return the requested model")
                self.tool_results += 1
                return {"role": "assistant", "content": self.marker}, "stop"
            if self.tool_calls:
                raise RuntimeError("The agent continued without returning the SMT tool result")
            self.tool_calls += 1
        arguments = {"smtlib": "(declare-const deployment_value Int)"
                     "(assert (= deployment_value 7))", "timeout_ms": 10000}
        return {"role": "assistant", "tool_calls": [{
            "index": 0, "id": self.call_id, "type": "function",
            "function": {"name": self.tool_name, "arguments": json.dumps({} if self.resource_probe else arguments)},
        }]}, "tool_calls"


class MockHandler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *_args):
        pass

    def do_POST(self):
        try:
            if self.path != "/v1/chat/completions":
                raise RuntimeError(f"Unexpected mock endpoint: {self.path}")
            if self.headers.get("Authorization") != "Bearer " + self.server.key:
                raise RuntimeError("The isolated test configuration was not used")
            length = int(self.headers.get("Content-Length", "0"))
            if not 0 < length <= 4 * 1024 * 1024:
                raise RuntimeError("Invalid mock request length")
            request = json.loads(self.rfile.read(length))
            message, finish = self.server.reply(request)
            if request.get("stream"):
                chunks = [{"choices": [{"index": 0, "delta": message}]},
                          {"choices": [{"index": 0, "delta": {}, "finish_reason": finish}]}]
                body = ("".join("data: " + json.dumps(chunk) + "\n\n" for chunk in chunks)
                        + "data: [DONE]\n\n").encode()
                content_type = "text/event-stream"
            else:
                body = json.dumps({"choices": [{"index": 0, "message": message,
                                                 "finish_reason": finish}]}).encode()
                content_type = "application/json"
            self.send_response(200)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
        except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
            pass
        except Exception as error:
            self.server.errors.append(str(error))
            try:
                self.send_error(400, "Deployment probe rejected the request")
            except (BrokenPipeError, ConnectionResetError, ConnectionAbortedError):
                pass


def isolated_environment(working):
    environment = os.environ.copy()
    for variable in list(environment):
        if variable.startswith("QSOC_") or variable.lower() in (
                "http_proxy", "https_proxy", "all_proxy"):
            environment.pop(variable)
    for variable in ("LD_LIBRARY_PATH", "DYLD_LIBRARY_PATH", "QT_PLUGIN_PATH",
                     "QT_QPA_PLATFORM_PLUGIN_PATH"):
        environment.pop(variable, None)
    qt_root = environment.get("QT_ROOT_DIR")
    if qt_root:
        environment["PATH"] = os.pathsep.join(
            item for item in environment.get("PATH", "").split(os.pathsep)
            if not pathlib.Path(item).resolve().is_relative_to(pathlib.Path(qt_root).resolve()))
    for name in ("config", "cache", "data", "runtime"):
        (working / name).mkdir(mode=0o700)
    environment.update({
        "HOME": str(working), "USERPROFILE": str(working),
        "TMPDIR": str(working), "TMP": str(working), "TEMP": str(working),
        "QSOC_HOME": str(working / "config" / "qsoc"),
        "XDG_CONFIG_HOME": str(working / "config"),
        "XDG_CACHE_HOME": str(working / "cache"),
        "XDG_DATA_HOME": str(working / "data"),
        "XDG_RUNTIME_DIR": str(working / "runtime"),
        "APPDATA": str(working / "config"), "LOCALAPPDATA": str(working / "cache"),
        "NO_PROXY": "*", "no_proxy": "*",
    })
    return environment


def main(program, resource_probe=False):
    executable = str(pathlib.Path(program).resolve())
    temporary_root = None if os.name == "nt" else "/tmp"
    with tempfile.TemporaryDirectory(prefix="test_qsoc_agent_smt_", dir=temporary_root) as directory:
        working = pathlib.Path(directory)
        environment = isolated_environment(working)
        with MockServer(resource_probe) as server:
            configuration = {"llm": {"model": server.model, "models": {
                server.model: {"name": "Deployment probe", "key": server.key,
                               "url": f"http://127.0.0.1:{server.server_port}/v1/chat/completions",
                               "timeout": 20000, "context": 131072, "max_output_tokens": 4096}}},
                "proxy": {"type": "none"}}
            (working / ".qsoc.yml").write_text(json.dumps(configuration), encoding="utf-8")
            thread = threading.Thread(target=server.serve_forever, daemon=True)
            thread.start()
            try:
                result = subprocess.run(
                    [executable, "agent", "-q", "Read the local resource snapshot." if resource_probe else "Check the deployment SMT constraint."],
                    cwd=working, env=environment, stdin=subprocess.DEVNULL,
                    capture_output=True, timeout=90)
            finally:
                server.shutdown()
                thread.join()
            output = result.stdout.decode(errors="replace")
            error = result.stderr.decode(errors="replace")
            if (result.returncode != 0 or server.tool_calls != 1 or server.tool_results != 1
                    or server.errors or server.marker not in output):
                raise RuntimeError(
                    f"Packaged agent SMT probe failed: exit={result.returncode}, "
                    f"calls={server.tool_calls}, results={server.tool_results}, "
                    f"errors={server.errors}\nstdout:\n{output}\nstderr:\n{error}")
    print("Packaged agent queried system_resources through its parent daemon" if resource_probe
          else "Packaged CLI discovered its daemon and completed z3_solve through the SMT worker",
          flush=True)


if __name__ == "__main__":
    main(sys.argv[1], "--resources" in sys.argv[2:])
