#!/usr/bin/env python3
# SPDX-License-Identifier: Apache-2.0
# SPDX-FileCopyrightText: 2026 Huang Rui <vowstar@gmail.com>

"""Check natural-language peer coordination with the configured model in isolated tmux.

Requires tmux and PyYAML. Credentials stay in the temporary configuration.
"""

import argparse
import json
import os
from pathlib import Path
import re
import secrets
import shlex
import shutil
import subprocess
import tempfile
import time
import uuid

import yaml


def records(path):
    result = []
    if path.exists():
        for line in path.read_text().splitlines():
            try:
                result.append(json.loads(line))
            except ValueError:
                pass
    return result


def verify(messages, transcripts, expected):
    communication = {"agent_list", "send_message", "agent_inbox", "wait_agent", "followup_task", "interrupt_agent"}
    for message in messages:
        for call in message.get("tool_calls", []):
            function = call.get("function", {})
            if function.get("name") == "agent":
                prompt = json.loads(function["arguments"]).get("prompt", "")
                assert not any(name in prompt for name in communication), "parent supplied communication tool names"
                assert not re.search(r"\ba[1-9][0-9]*\b|[0-9a-f]{8}(?:-[0-9a-f]{4}){3}-[0-9a-f]{12}", prompt), "parent supplied peer addresses"
    children = {}
    for message in messages:
        if message.get("role") != "tool":
            continue
        try:
            result = json.loads(message.get("content", ""))
        except (ValueError, TypeError):
            continue
        if isinstance(result, dict) and result.get("agent_id") and result.get("task_id"):
            children[result["task_id"]] = result["agent_id"]
    assert len(children) == 2, f"expected two children, got {len(children)}"
    replies = {}
    for message in messages:
        if message.get("role") != "tool":
            continue
        try:
            result = json.loads(message.get("content", ""))
        except (ValueError, TypeError):
            continue
        if not isinstance(result, dict):
            continue
        for reply in result.get("messages", []):
            try:
                body = json.loads(reply["body"])
            except (KeyError, ValueError, TypeError):
                continue
            if isinstance(body, dict) and body.get("task_id") and reply.get("reply_to"):
                replies[reply["sender"]] = body["task_id"]
    evidence = {}
    for task_id, identity in children.items():
        events = records(transcripts / (task_id + ".jsonl"))
        text = "".join(event.get("data", "") for event in events if event.get("kind") == "chunk")
        peer_ids = (set(children) | set(children.values())) - {task_id, identity}
        sends = []
        for encoded in re.findall(r"\[result send_message\] (\{[^\n]*\})", text):
            receipt = json.loads(encoded)
            if receipt.get("status") == "ok" and receipt.get("agent_id") in peer_ids:
                sends.append(receipt)
        assert sends, f"{task_id} did not send directly to its sibling"
        assert '[tool] agent_list' in text, f"{task_id} did not discover peers"
        meta = json.loads((transcripts / (task_id + ".meta.json")).read_text())
        initial = next((e["data"] for e in reversed(events) if e.get("kind") == "final"), "")
        final_task = replies.get(identity, task_id)
        finals = records(transcripts / (final_task + ".jsonl"))
        final = next((e["data"] for e in reversed(finals) if e.get("kind") == "final"), "")
        assert str(expected) in final.replace(",", ""), f"{task_id} returned the wrong sum"
        evidence[task_id] = {"role": meta["subagent_type"], "peer_sends": len(sends),
                             "initial_correct": str(expected) in initial.replace(",", ""),
                             "final_task": final_task, "final": final}
    assert {item["role"] for item in evidence.values()} == {"explore", "verification"}
    return evidence


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", type=Path, default=Path.home() / ".config/qsoc/qsoc.yml")
    parser.add_argument("--build", type=Path, default=Path(__file__).resolve().parents[1] / "build")
    parser.add_argument("--timeout", type=int, default=720)
    parser.add_argument("--artifacts", type=Path)
    parser.add_argument("--replay", type=Path, help="verify an existing live run without model calls")
    args = parser.parse_args()
    if args.replay:
        artifacts = args.replay
        summary = json.loads((artifacts / "summary.json").read_text())
        messages = [r for p in (artifacts / "sessions").glob("*.jsonl") for r in records(p)]
        summary["children"] = verify(messages, artifacts / "children", summary["expected"])
        summary["passed"] = True
        (artifacts / "summary.json").write_text(json.dumps(summary, indent=2, ensure_ascii=False))
        print("PASS: recorded live peer discovery, delivery receipts and complete final results")
        return
    original = yaml.safe_load(args.config.read_text())
    selected = os.environ.get("QSOC_LLM_MODEL", original["llm"]["model"])
    model = original["llm"]["models"][selected]
    config = {"llm": {"model": selected, "models": {selected: model}},
              "agent": {"auto_load_memory": False, "memory_extract": False,
                        "memory_recall": False, "max_iterations": 60}}
    if "proxy" in original:
        config["proxy"] = original["proxy"]
    artifacts = (args.artifacts or args.build / "peer-prompt-live").resolve()
    artifacts.mkdir(parents=True, exist_ok=True)
    if any(artifacts.iterdir()):
        raise ValueError("Artifacts directory must be empty; choose a new --artifacts path")
    left, right = (secrets.randbelow(90000) + 10000 for _ in range(2))
    expected = left + right
    (artifacts / "scenario.json").write_text(json.dumps({"alpha": left, "beta": right, "expected": expected}))
    with tempfile.TemporaryDirectory(prefix="test_qsoc_live_peers_") as temporary:
        work = Path(temporary)
        for name in ("home", "xdg", "runtime", "project"):
            (work / name).mkdir(mode=0o700)
        project = work / "project"
        config_path = project / ".qsoc.yml"
        config_path.touch(mode=0o600)
        config_path.write_text(yaml.safe_dump(config))
        (project / "alpha.txt").write_text(f"alpha = {left}\n")
        (project / "beta.txt").write_text(f"beta = {right}\n")
        env = dict(os.environ, HOME=str(work / "home"), XDG_CONFIG_HOME=str(work / "xdg"),
                   XDG_RUNTIME_DIR=str(work / "runtime"), QSOC_HOME=str(work / "home" / ".qsoc"),
                   TERM="xterm-256color")
        for key in list(env):
            if key.startswith("QSOC_AGENT_"):
                del env[key]
        owner = uuid.uuid4().hex
        sock = work / "tmux.sock"

        def tmux(*words, check=True):
            return subprocess.run(["tmux", "-S", str(sock), *words], env=env,
                                  text=True, capture_output=True, check=check)

        prompt = (
            "请并行安排两个子任务：explore 角色命名 Alpha，只读 alpha.txt；verification 角色命名 Beta，"
            "只读 beta.txt。每个子 agent 必须主动发现对方，直接交换读到的数值，收到对方数值后计算两数之和，"
            "然后各自提交最终答案。不要提前结束子任务。你只负责安排和汇总，不读这两个文件，也不要替他们"
            "转发数值。给子 agent 说明目标和分工即可，不要告诉他们具体通信工具名、调用参数或地址。"
            "不要修改文件或启动额外子 agent。拿到双方一致的答案后，最终输出 LIVE_PEER_DONE 和两数之和。"
        )
        command = shlex.join([str((args.build / "qsoc").resolve()), "agent", "-d", str(project)])
        command += " 2>" + shlex.quote(str(work / "stderr"))
        try:
            tmux("new-session", "-d", "-s", "live", "-x", "200", "-y", "55", "-c", str(project), command)
            tmux("set-option", "-g", "@qsoc_test_owner", owner)
            time.sleep(2)
            tmux("send-keys", "-t", "live", "-l", prompt)
            tmux("send-keys", "-t", "live", "Enter")
            print(f"Live test: model={selected}, effort={model.get('effort', 'off')}; tmux socket={sock}", flush=True)
            start = time.monotonic()
            messages = []
            while time.monotonic() - start < args.timeout:
                messages = [r for p in (project / ".qsoc/sessions").glob("*.jsonl") for r in records(p)]
                finals = [m.get("content", "") for m in messages if m.get("role") == "assistant"
                          and not m.get("tool_calls")]
                if any(f"{expected}" in str(text) and "LIVE_PEER_DONE" in str(text) for text in finals):
                    if time.monotonic() - start >= 75:
                        break
                time.sleep(5)
                elapsed = int(time.monotonic() - start)
                if elapsed % 30 < 5:
                    print(f"observed {elapsed}s; session records={len(messages)}", flush=True)
            else:
                raise AssertionError("live model did not complete peer coordination before timeout")
            evidence = verify(messages, work / "runtime/qsoc/agents", expected)
            (artifacts / "summary.json").write_text(json.dumps(
                {"model": selected, "effort": model.get("effort", "off"), "expected": expected,
                 "children": evidence, "passed": True}, indent=2, ensure_ascii=False))
            print("PASS: natural-language task, peer discovery, direct sibling sends and matching final sums", flush=True)
        finally:
            for name, source in (("sessions", project / ".qsoc/sessions"),
                                 ("children", work / "runtime/qsoc/agents")):
                if source.exists():
                    shutil.copytree(source, artifacts / name, dirs_exist_ok=True)
            capture = tmux("capture-pane", "-p", "-S", "-1000", "-t", "live", check=False)
            (artifacts / "pane.txt").write_text(capture.stdout)
            marker = tmux("show-option", "-gv", "@qsoc_test_owner", check=False)
            if marker.stdout.strip() == owner:
                tmux("kill-server", check=False)


if __name__ == "__main__":
    main()
