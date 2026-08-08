#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>
"""Mock OpenAI-compatible chat-completions endpoint for qsoc agent tests.

Speaks the streaming SSE and non-streaming JSON wire formats qsoc's
QLLMService expects and can inject HTTP 429 / 503 failures to exercise
the agent's retry + backoff path, and tool_calls to drive concurrent
sub-agents through the rate limiter. No real provider, no secrets: the
test config points the default model here with a placeholder key the
mock ignores.

Usage:
  python3 mock_llm.py <port> [failmode]
    failmode:
      none           always 200            (default)
      window:<sec>   429 for the first <sec> after the FIRST request,
                     then 200 (deterministic single-agent recovery)
      prob:<p>       429 with probability p (0..1) per request
                     (concurrent fan-out: each agent retries independently)
      always         429 forever (retry-exhaustion -> graceful runError)
    status:<code> may replace 429, e.g. failmode "prob:0.4" defaults to
    429; set env MOCK_FAIL_CODE=503 to push 503 instead.

  Env:
    MOCK_SPAWN_N=<n>   on the parent's first 200, emit <n>
                       `agent` tool_calls (drives n concurrent sub-agents).
                       0 (default) = always reply with text.
    MOCK_SPAWN_BG=0    spawn the children in the FOREGROUND (default
                       background); use to exercise the foreground path.
    MOCK_REPLY=<text>  text for non-spawn turns (default "DONE").
    MOCK_FAIL_CODE=<n> HTTP code for the injected failure (default 429).
    MOCK_TTL=<sec>     self-exit after this many seconds (default 120).
    MOCK_DELAY=<sec>   sleep this long before a 200 response, so a slow
                       turn is observable (e.g. to watch a `queued`
                       sub-agent under a finite cap). Default 0.
    MOCK_CHUNK_DELAY=<sec>
                       pause between SSE chunks (default 0).
    MOCK_TOOL_NAME=<name>
                       emit this tool call on a successful turn. By default
                       once, on the first one; see MOCK_TOOL_MAX and
                       MOCK_TOOL_GATE.
    MOCK_TOOL_ARGS=<json>
                       arguments object for MOCK_TOOL_NAME (default {}).
    MOCK_TOOL_MAX=<n>  emit the tool on the first <n> successful turns
                       instead of just the first (default 1). Use 0 for no
                       limit, e.g. to drive a retry loop until a cap fires.
    MOCK_TOOL_GATE=<path>
                       emit the tool only while <path> exists, and remove it
                       on emit. Lets a test decide exactly when each tool
                       call happens instead of racing a sleep. Overrides
                       MOCK_TOOL_MAX.
    MOCK_REQUEST_LOG=<path>
                       append each decoded request as one JSON line.
    MOCK_TLS_CERT=<path>
    MOCK_TLS_KEY=<path> serve TLS and advertise h2 plus http/1.1 via ALPN.

  GET / returns a JSON hit counter so a test can assert how many 429s
  were served and whether the spawn turn and child turns recovered.

Parent vs child detection: the parent's request carries the `agent`
tool definition, whose description contains "Spawn a child sub-agent";
child requests do not (the sub-agent recursion guard filters that tool
out), so children always get the text reply and never recurse.
"""
import http.server
import json
import os
import random
import ssl
import sys
import threading
import time

PORT = int(sys.argv[1]) if len(sys.argv) > 1 else 18429
FAILMODE = sys.argv[2] if len(sys.argv) > 2 else "none"
SPAWN_N = int(os.environ.get("MOCK_SPAWN_N", "0"))
SPAWN_BG = os.environ.get("MOCK_SPAWN_BG", "1") not in ("0", "false", "no")
REPLY = os.environ.get("MOCK_REPLY", "DONE")
FAIL_CODE = int(os.environ.get("MOCK_FAIL_CODE", "429"))
TTL = float(os.environ.get("MOCK_TTL", "120"))
DELAY = float(os.environ.get("MOCK_DELAY", "0"))
CHUNK_DELAY = float(os.environ.get("MOCK_CHUNK_DELAY", "0"))
TOOL_NAME = os.environ.get("MOCK_TOOL_NAME", "")
TOOL_MAX = int(os.environ.get("MOCK_TOOL_MAX", "1"))
TOOL_GATE = os.environ.get("MOCK_TOOL_GATE", "")
TOOL_ARGS = json.loads(os.environ.get("MOCK_TOOL_ARGS", "{}"))
REQUEST_LOG = os.environ.get("MOCK_REQUEST_LOG", "")
TLS_CERT = os.environ.get("MOCK_TLS_CERT", "")
TLS_KEY = os.environ.get("MOCK_TLS_KEY", "")
if not isinstance(TOOL_ARGS, dict):
    raise ValueError("MOCK_TOOL_ARGS must decode to an object")

_start = [None]
_emitted = [0]
_hits = {
    "fail": 0,
    "200_toolcalls": 0,
    "200_text": 0,
    "200_sync": 0,
    "alpn_h2": 0,
    "alpn_http1": 0,
    "alpn_none": 0,
}
_lock = threading.Lock()


def _should_fail():
    """Decide, under lock, whether this request gets the failure code."""
    with _lock:
        if _start[0] is None:
            _start[0] = time.time()
        if FAILMODE == "always":
            return True
        if FAILMODE.startswith("window:"):
            return (time.time() - _start[0]) < float(FAILMODE.split(":", 1)[1])
        if FAILMODE.startswith("prob:"):
            return random.random() < float(FAILMODE.split(":", 1)[1])
        return False


def _sse(wfile, chunks):
    for index, chunk in enumerate(chunks):
        wfile.write(b"data: " + json.dumps(chunk).encode() + b"\n\n")
        wfile.flush()
        if CHUNK_DELAY > 0 and index + 1 < len(chunks):
            time.sleep(CHUNK_DELAY)
    wfile.write(b"data: [DONE]\n\n")
    wfile.flush()


class Handler(http.server.BaseHTTPRequestHandler):
    def log_message(self, *args):
        pass

    def setup(self):
        super().setup()
        if TLS_CERT:
            protocol = self.connection.selected_alpn_protocol()
            with _lock:
                if protocol == "h2":
                    _hits["alpn_h2"] += 1
                elif protocol == "http/1.1":
                    _hits["alpn_http1"] += 1
                else:
                    _hits["alpn_none"] += 1

    def do_POST(self):
        length = int(self.headers.get("Content-Length", 0) or 0)
        body = self.rfile.read(length).decode("utf-8", "replace") if length else ""
        try:
            request = json.loads(body) if body else {}
        except json.JSONDecodeError:
            request = {}
        streaming = request.get("stream") is True
        if REQUEST_LOG:
            with _lock:
                with open(REQUEST_LOG, "a", encoding="utf-8") as log:
                    log.write(json.dumps(request, separators=(",", ":")) + "\n")

        if _should_fail():
            with _lock:
                _hits["fail"] += 1
            payload = json.dumps({"error": {"message": "rate limited", "type": "rate_limit_error"}}).encode()
            self.send_response(FAIL_CODE)
            self.send_header("Content-Type", "application/json")
            self.send_header("Retry-After", "1")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return

        is_parent = "Spawn a child sub-agent" in body
        with _lock:
            wants_tools = bool(TOOL_NAME) or (SPAWN_N > 0 and is_parent)
            if TOOL_GATE:
                # The gate is the caller's clock: emit only while the file is
                # there, and consume it so each creation buys one tool call.
                allowed = os.path.exists(TOOL_GATE)
            else:
                allowed = TOOL_MAX <= 0 or _emitted[0] < TOOL_MAX
            emit_tools = wants_tools and allowed
            if emit_tools:
                _emitted[0] += 1
                if TOOL_GATE:
                    try:
                        os.remove(TOOL_GATE)
                    except OSError:
                        pass
            _hits["200_toolcalls" if emit_tools else "200_text"] += 1
            if not streaming:
                _hits["200_sync"] += 1

        if DELAY > 0:
            time.sleep(DELAY)
        tool_calls = []
        if emit_tools and TOOL_NAME:
            tool_calls = [
                {
                    "index": 0,
                    "id": "call_0",
                    "type": "function",
                    "function": {
                        "name": TOOL_NAME,
                        "arguments": json.dumps(TOOL_ARGS),
                    },
                }
            ]
        elif emit_tools:
            tool_calls = [
                {
                    "index": i,
                    "id": "call_%d" % i,
                    "type": "function",
                    "function": {
                        "name": "agent",
                        "arguments": json.dumps({
                            "subagent_type": "general-purpose",
                            "description": "child%d" % i,
                            "prompt": "reply with the single word %s" % REPLY,
                            "run_in_background": SPAWN_BG,
                        }),
                    },
                }
                for i in range(SPAWN_N)
            ]

        if not streaming:
            message = {"role": "assistant", "content": REPLY}
            finish_reason = "stop"
            if emit_tools:
                message = {"role": "assistant", "content": None, "tool_calls": tool_calls}
                finish_reason = "tool_calls"
            payload = json.dumps({
                "choices": [{"index": 0, "message": message, "finish_reason": finish_reason}],
                "usage": {"prompt_tokens": 1, "completion_tokens": 1, "total_tokens": 2},
            }).encode()
            self.send_response(200)
            self.send_header("Content-Type", "application/json")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
            return

        self.send_response(200)
        self.send_header("Content-Type", "text/event-stream")
        self.send_header("Cache-Control", "no-cache")
        self.end_headers()
        if emit_tools:
            _sse(self.wfile, [
                {"choices": [{"delta": {"role": "assistant", "tool_calls": tool_calls}}]},
                {"choices": [{"delta": {}, "finish_reason": "tool_calls"}]},
            ])
        else:
            _sse(self.wfile, [
                {"choices": [{"delta": {"role": "assistant", "content": REPLY}}]},
                {"choices": [{"delta": {}, "finish_reason": "stop"}]},
            ])

    def do_GET(self):
        with _lock:
            payload = json.dumps(_hits).encode()
        self.send_response(200)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(payload)))
        self.end_headers()
        self.wfile.write(payload)


def main():
    threading.Thread(target=lambda: (time.sleep(TTL), os._exit(0)), daemon=True).start()
    server = http.server.ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    if bool(TLS_CERT) != bool(TLS_KEY):
        raise ValueError("MOCK_TLS_CERT and MOCK_TLS_KEY must be set together")
    if TLS_CERT:
        context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        context.load_cert_chain(TLS_CERT, TLS_KEY)
        context.set_alpn_protocols(["h2", "http/1.1"])
        server.socket = context.wrap_socket(server.socket, server_side=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
