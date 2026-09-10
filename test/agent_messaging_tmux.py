#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

"""Run scripted peer communication and Escape cancellation in an isolated tmux server."""

import argparse
import json
import os
from pathlib import Path
import shlex
import socket
import subprocess
import tempfile
import time
import urllib.request
import uuid


def tool(name, **arguments):
    return {"name": name, "arguments": arguments}


def response(*tools, content="", delay_ms=0):
    return {"tools": list(tools), "content": content, "delay_ms": delay_ms}


def scenario():
    return [
        {"contains": "MAIL_AUDIT_START", "responses": [
            response(tool("agent", subagent_type="explore", description="mail worker A",
                          prompt="MAIL_WORKER_A", run_in_background=True),
                     tool("agent", subagent_type="verification", description="mail worker B",
                          prompt="MAIL_WORKER_B", run_in_background=True)),
            response(tool("send_message", target="a2", message_id="start-chain", message="START_PEER_CHAIN"), delay_ms=300),
            response(tool("wait_agent", timeout_ms=1500)),
            response(tool("agent_list")),
            response(tool("send_message", target="a1", message_id="idle-note", message="IDLE_NOTE")),
            response(tool("send_message", target="a1", message_id="idle-note", message="IDLE_NOTE")),
            response(tool("followup_task", target="a1", message_id="followup", message="CHECK_AGAIN")),
            response(tool("wait_agent", **{"from": "a1", "reply_to": "followup", "timeout_ms": 5000})),
            response(tool("agent_list")),
            response(tool("followup_task", target="a2", message_id="cancel-work", message="CANCEL_THIS_WORK")),
            response(tool("interrupt_agent", target="a2"), delay_ms=300),
            response(tool("followup_task", target="a2", message_id="forbidden-wake", message="DO_NOT_RUN")),
            response(tool("agent_list")),
            response(content="MAIL_AUDIT_DONE"),
        ]},
        {"contains": "MAIL_WORKER_A", "responses": [
            response(tool("wait_agent", **{"from": "a2", "timeout_ms": 5000}), delay_ms=150),
            response(tool("send_message", target="a2", message_id="question", message="PEER_QUESTION"), delay_ms=150),
            response(tool("wait_agent", **{"from": "a2", "reply_to": "question", "timeout_ms": 5000})),
            response(content="WORKER_A_DONE"),
            response(content="WORKER_A_FOLLOWUP_DONE", delay_ms=500),
        ]},
        {"contains": "MAIL_WORKER_B", "responses": [
            response(tool("wait_agent", **{"from": "main", "timeout_ms": 5000})),
            response(tool("send_message", target="a1", message_id="start-peer", message="PEER_START")),
            response(tool("wait_agent", **{"from": "a1", "timeout_ms": 5000})),
            response(tool("send_message", target="a1", message_id="answer", reply_to="question", message="PEER_ANSWER"), delay_ms=300),
            response(content="WORKER_B_DONE"),
            response(content="CANCELLED_WORK_MUST_NOT_FINISH", delay_ms=30000),
        ]},
        {"contains": "CANCEL_AUDIT_START", "responses": [
            response(tool("wait_agent", timeout_ms=60000)),
            response(content="CANCEL_RECOVERED"),
        ]},
    ]


def first_user(request):
    return next((m.get("content", "") for m in request.get("messages", []) if m.get("role") == "user"), "")


def requests_from(path):
    if not path.exists():
        return []
    return [json.loads(line) for line in path.read_text().splitlines() if line.strip()]


def tool_results(requests):
    results = {}
    for request in requests:
        for message in request.get("messages", []):
            if message.get("role") != "tool":
                continue
            try:
                result = json.loads(message["content"])
            except (KeyError, ValueError, TypeError):
                continue
            results[message.get("tool_call_id")] = result
    return list(results.values())


def verify(requests):
    root = [r for r in requests if "MAIL_AUDIT_START" in str(first_user(r)) and r.get("stream")]
    worker = [r for r in requests if "MAIL_WORKER_A" in str(first_user(r)) and r.get("stream")]
    communication = {"agent_list", "send_message", "agent_inbox", "wait_agent"}
    for marker in ("MAIL_WORKER_A", "MAIL_WORKER_B"):
        child_requests = [r for r in requests if marker in str(first_user(r)) and r.get("stream")]
        assert child_requests, marker
        for request in child_requests:
            names = {t["function"]["name"] for t in request.get("tools", [])}
            assert communication <= names, (marker, names)
            assert not {"followup_task", "interrupt_agent"} & names, (marker, names)
            system = request["messages"][0]["content"]
            assert "# Peer collaboration" in system and "Your stable agent_id" in system
            assert all(name in system for name in communication)
            assert "Tags such as <system-reminder>" in system
            assert "Treat them as system guidance" not in system
    results = tool_results(root)
    notices = [r for r in results if r.get("message_id") == "idle-note"]
    assert len(notices) == 2 and {r["duplicate"] for r in notices} == {False, True}, notices
    assert all(r["delivery"] == "accepted" for r in notices), notices
    lists = [r["agents"] for r in results if "agents" in r]
    assert len(lists) == 3, lists
    initial = next(r for r in lists[0] if r["task_id"] == "a1")
    resumed = next(r for r in lists[1] if r["agent_id"] == initial["agent_id"])
    assert initial["state"] == resumed["state"] == "idle", (initial, resumed)
    assert resumed["task_id"] != initial["task_id"], (initial, resumed)
    assert any(r.get("error") == "target_cancelled_or_closed" for r in results), results
    assert any(r["state"] == "cancelled" and r["name"] == "mail worker B" for r in lists[-1]), lists[-1]
    replies = [m for r in results for m in r.get("messages", [])]
    assert any(m.get("reply_to") == "followup" and "WORKER_A_FOLLOWUP_DONE" in m["body"] for m in replies), replies
    peer_replies = [m for r in tool_results(worker) for m in r.get("messages", [])]
    assert any(m["body"] == "PEER_START" for m in peer_replies), peer_replies
    assert any(m.get("reply_to") == "question" and m["body"] == "PEER_ANSWER" for m in peer_replies), peer_replies
    resumed_requests = [r for r in worker if "CHECK_AGAIN" in json.dumps(r["messages"])]
    assert resumed_requests, worker
    for request in resumed_requests:
        assert request["model"] == "mock-comm" and request.get("reasoning_effort") == "high", request.keys()
        assert "WORKER_A_DONE" in json.dumps(request["messages"])
        assert sum("IDLE_NOTE" in str(m.get("content", "")) for m in request["messages"] if m.get("role") == "user") == 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--build", type=Path, default=Path(__file__).resolve().parents[1] / "build")
    parser.add_argument("--artifacts", type=Path)
    parser.add_argument("--observe-seconds", type=int, default=75)
    args = parser.parse_args()
    build = args.build.resolve()
    artifacts = (args.artifacts or build / "agent-messaging-tmux").resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    opener = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    with tempfile.TemporaryDirectory(prefix="test_qsoc_messaging_") as temporary:
        work = Path(temporary)
        owner = uuid.uuid4().hex
        sock = work / "tmux.sock"
        env = {"PATH": os.environ["PATH"], "HOME": str(work / "home"),
               "XDG_CONFIG_HOME": str(work / "xdg"), "XDG_RUNTIME_DIR": str(work / "runtime"),
               "QSOC_HOME": str(work / "home" / ".qsoc"), "LANG": "C.UTF-8",
               "TERM": "xterm-256color", "NO_PROXY": "*", "no_proxy": "*"}
        for name in ("home", "xdg", "runtime"):
            (work / name).mkdir(mode=0o700)
        with socket.socket() as listener:
            listener.bind(("127.0.0.1", 0))
            port = listener.getsockname()[1]
        script = work / "script.json"
        script.write_text(json.dumps(scenario()))
        request_log = work / "requests.jsonl"
        mock_env = dict(env, MOCK_SCRIPT=str(script), MOCK_REQUEST_LOG=str(request_log), MOCK_TTL="180")
        mock_log = (work / "mock.log").open("w")
        mock = subprocess.Popen([str(build / "test" / "qsoc_mock_llm"), str(port), "none"],
                                env=mock_env, cwd=work, stdout=mock_log, stderr=subprocess.STDOUT)

        def tmux(*words, check=True):
            return subprocess.run(["tmux", "-S", str(sock), *words], env=env,
                                  text=True, capture_output=True, check=check)

        def counts():
            with opener.open(f"http://127.0.0.1:{port}/", timeout=2) as response_data:
                return json.load(response_data)

        def pane(name):
            return tmux("capture-pane", "-p", "-S", "-500", "-t", name).stdout

        def launch(name, prompt):
            directory = work / name
            directory.mkdir()
            (directory / ".qsoc.yml").write_text(
                "llm:\n  model: mock-comm\n  models:\n    mock-comm:\n"
                f"      url: http://127.0.0.1:{port}/v1/chat/completions\n"
                "      timeout: 90000\n      context: 131072\n      max_output_tokens: 4096\n"
                "proxy:\n  type: none\nagent:\n  memory_extract: false\n")
            command = shlex.join([str(build / "qsoc"), "agent", "-d", str(directory), "--effort", "high"])
            command += " 2>" + shlex.quote(str(directory / "stderr"))
            tmux("new-session", "-d", "-s", name, "-x", "200", "-y", "55", "-c", str(directory), command)
            tmux("set-option", "-g", "@qsoc_test_owner", owner)
            time.sleep(2)
            tmux("send-keys", "-t", name, "-l", prompt)
            tmux("send-keys", "-t", name, "Enter")

        start = time.monotonic()
        try:
            for _ in range(100):
                try:
                    counts()
                    break
                except OSError:
                    time.sleep(0.1)
            else:
                raise AssertionError("mock did not start")
            first = "qsoc_test_messaging_" + owner
            launch(first, "MAIL_AUDIT_START")
            tmux("set-option", "-g", "@qsoc_test_owner", owner)
            print(f"tmux socket: {sock}; session: {first}", flush=True)
            for _ in range(150):
                if "MAIL_AUDIT_DONE" in pane(first):
                    break
                time.sleep(0.2)
            else:
                raise AssertionError("communication scenario did not finish")
            verify(requests_from(request_log))
            (artifacts / "communication-pane.txt").write_text(pane(first))
            second = "qsoc_test_cancel_" + owner
            launch(second, "CANCEL_AUDIT_START")
            for _ in range(50):
                if "wait_agent" in pane(second):
                    break
                time.sleep(0.2)
            else:
                raise AssertionError("wait tool not visible")
            time.sleep(1)
            tmux("send-keys", "-t", second, "Escape")
            time.sleep(1)
            tmux("send-keys", "-t", second, "-l", "POST_CANCEL")
            tmux("send-keys", "-t", second, "Enter")
            for _ in range(50):
                if "CANCEL_RECOVERED" in pane(second):
                    break
                time.sleep(0.2)
            else:
                raise AssertionError("Escape did not release the wait")
            (artifacts / "cancel-pane.txt").write_text(pane(second))
            while time.monotonic() - start < max(65, args.observe_seconds):
                print(f"observed {int(time.monotonic()-start)}s: {counts()}", flush=True)
                time.sleep(min(10, max(0, max(65, args.observe_seconds) - (time.monotonic()-start))))
            for path in work.glob("qsoc_test_*/stderr"):
                text = path.read_text()
                (artifacts / (path.parent.name.split(owner)[0] + "stderr.txt")).write_text(text)
                assert not any(word in text for word in ("Segmentation", "ASSERT", "QIODevice::read", "device not open")), text
            (artifacts / "summary.json").write_text(json.dumps({"passed": True, "counters": counts()}, indent=2))
            print("PASS: peers, correlated replies, retry deduplication, idle wake, context/model inheritance, cancellation, Escape", flush=True)
        finally:
            for session in ("qsoc_test_messaging_" + owner, "qsoc_test_cancel_" + owner):
                captured = tmux("capture-pane", "-p", "-S", "-500", "-t", session, check=False)
                if captured.returncode == 0:
                    (artifacts / (session.split(owner)[0] + "final.txt")).write_text(captured.stdout)
            if request_log.exists():
                (artifacts / "requests.jsonl").write_text(request_log.read_text())
            marker = tmux("show-options", "-gv", "@qsoc_test_owner", check=False)
            if marker.stdout.strip() == owner:
                tmux("kill-server", check=False)
            mock.terminate()
            try:
                mock.wait(timeout=5)
            except subprocess.TimeoutExpired:
                mock.kill()
                mock.wait()
            mock_log.close()


if __name__ == "__main__":
    main()
