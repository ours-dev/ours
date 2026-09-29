#!/usr/bin/env python3
"""Check that a fresh Claude Code session picks the right skill for a request.

Each case in skill_triggers.json is run through `claude -p` in a new,
unsaved session. The event stream is watched until the session loads a
project skill (a Skill call, or a read of any file inside a skill folder),
answers, or is about to act. Read-only tools -- Read, Grep, Glob and plainly
read-only Bash pipelines -- are allowed to run so the model can look around
first. Anything else is stopped before the CLI executes it: other tools at the
start of their tool_use block, Bash once its full command is known.

    python3 .agents/evals/skill_trigger_eval.py                  # every case once
    python3 .agents/evals/skill_trigger_eval.py --case design-   # ids starting with design-
    python3 .agents/evals/skill_trigger_eval.py --runs 3 --json results.json

Each run is a full session on your own credentials; the 18 cases cost
roughly $5 together. Set CLAUDE_BIN if `claude` is not on PATH (the desktop
app keeps its copy under ~/Library/Application Support/Claude/claude-code/).
"""

from __future__ import annotations

import argparse
import concurrent.futures as cf
import json
import os
import shlex
import shutil
import subprocess
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent.parent
# Tie a child to its host session when run from inside Claude Code.
DROP_ENV = {"CLAUDECODE", "CLAUDE_CODE_SESSION_ID", "CLAUDE_CODE_HOST_SESSION_ID", "CLAUDE_CODE_MESSAGING_SOCKET",
            "CLAUDE_CODE_MESSAGING_TOKEN", "CLAUDE_PID", "CLAUDE_CODE_CHILD_SESSION", "CLAUDE_CODE_SESSION_ATTENDED"}
SAFE_TOOLS = {"Read", "Grep", "Glob", "LS", "ToolSearch", "TodoWrite"}
SAFE_BUDGET = 12  # read-only steps allowed before "no skill" is the verdict
TIMEOUT = 180

READONLY_CMDS = {"grep", "rg", "ugrep", "find", "ls", "cat", "head", "tail", "wc", "tree", "file", "stat",
                 "sed", "awk", "sort", "uniq", "cut", "tr", "basename", "dirname", "realpath", "readlink",
                 "echo", "printf", "pwd", "cd", "git", "true", "diff", "du", "nl"}
GIT_READONLY = {"log", "show", "diff", "status", "grep", "ls-files", "blame", "rev-parse"}
OPERATORS = {"|", "||", "&&", ";", "&"}


def claude_bin() -> str:
    found = os.environ.get("CLAUDE_BIN") or shutil.which("claude")
    if found:
        return found
    apps = sorted((Path.home() / "Library/Application Support/Claude/claude-code").glob(
        "*/claude.app/Contents/MacOS/claude"), key=lambda p: [int(x) for x in p.parts[-5].split(".")])
    if not apps:
        raise SystemExit("no claude binary: set CLAUDE_BIN")
    return str(apps[-1])


def project_skills() -> set[str]:
    return {p.name for p in (ROOT / ".claude/skills").iterdir() if (p / "SKILL.md").is_file()}


def readonly_bash(cmd: str) -> bool:
    """True only for pipelines that plainly cannot change anything."""
    c = cmd.replace("2>/dev/null", "").replace("2>&1", "")
    if any(bad in c for bad in ("$(", "`", " -delete", "-exec", "-ok ", "sed -i", "system(")):
        return False
    # Tokenise like a shell, so a quoted "a|b" regex stays one word.
    lex = shlex.shlex(c.replace("\n", " ; "), posix=True, punctuation_chars=True)
    lex.whitespace_split = True
    try:
        tokens = list(lex)
    except ValueError:
        return False
    segments: list[list[str]] = [[]]
    for t in tokens:
        if t in OPERATORS:
            segments.append([])
        elif set(t) <= set("<>&|;"):  # redirection or a stray operator
            return False
        else:
            segments[-1].append(t)
    for words in segments:
        if not words:
            continue
        head = words[1] if words[0] in ("xargs", "command") and len(words) > 1 else words[0]
        if head not in READONLY_CMDS:
            return False
        if head == "git" and (len(words) < 2 or words[1] not in GIT_READONLY):
            return False
    return True


def skill_touched(text: str, skills: set[str]) -> str | None:
    if "skills/" not in text:
        return None
    for name in skills:
        if f"/{name}/" in text or text.rstrip("/").endswith(f"/{name}"):
            return name
    return None


def run_once(query: str, binary: str, skills: set[str]) -> dict:
    env = {k: v for k, v in os.environ.items() if k not in DROP_ENV}
    p = subprocess.Popen([binary, "-p", query, "--no-session-persistence", "--output-format", "stream-json",
                          "--verbose", "--include-partial-messages"],
                         cwd=ROOT, env=env, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                         stderr=subprocess.DEVNULL, text=True)
    watchdog = threading.Timer(TIMEOUT, p.kill)
    watchdog.start()
    start = time.time()
    blocks: dict[int, dict] = {}
    trail: list[str] = []
    outcome, detail = "timeout", None
    try:
        for line in p.stdout:
            if not line.startswith("{"):
                continue
            ev = json.loads(line)
            if ev.get("type") == "result":
                outcome = "answered"
                break
            if ev.get("type") != "stream_event":
                continue
            e = ev["event"]
            kind = e.get("type")
            if kind == "content_block_start" and e["content_block"].get("type") == "tool_use":
                name = e["content_block"]["name"]
                if name not in SAFE_TOOLS and name not in ("Skill", "Bash"):
                    outcome, detail = "acted", name  # stopped before the CLI runs it
                    break
                blocks[e["index"]] = {"name": name, "json": ""}
            elif kind == "content_block_delta" and e["delta"].get("type") == "input_json_delta":
                if e["index"] in blocks:
                    blocks[e["index"]]["json"] += e["delta"].get("partial_json", "")
            elif kind == "content_block_stop" and e["index"] in blocks:
                b = blocks.pop(e["index"])
                try:
                    args = json.loads(b["json"] or "{}")
                except ValueError:
                    args = {}
                if b["name"] == "Skill":
                    outcome, detail = "skill", args.get("skill") or args.get("command") or args.get("name")
                    break
                text = str(args.get("command") or args.get("file_path") or args.get("path") or "")
                if b["name"] == "Bash" and not readonly_bash(text):
                    outcome, detail = "acted", "Bash: " + text[:300]
                    break
                if skill_touched(text, skills):
                    outcome, detail = "skill", skill_touched(text, skills)
                    break
                trail.append("memory" if "/memory/" in text else f"{b['name']}({text.replace(str(ROOT), '.')[:50]})")
                if len(trail) >= SAFE_BUDGET:
                    outcome = "no-skill"
                    break
    finally:
        watchdog.cancel()
        p.kill()
        p.wait()
    return {"outcome": outcome, "detail": detail, "trail": trail, "secs": round(time.time() - start, 1)}


def passed(case: dict, r: dict, skills: set[str]) -> bool:
    """expect lists the acceptable skills; "none" accepts no project skill.
    allow_defer: the skill is required before acting, so stopping to present a
    plan and ask first (answering without having acted) also passes."""
    picked = r["detail"] if r["outcome"] == "skill" and r["detail"] in skills else None
    if picked is None and r["outcome"] == "answered" and case.get("allow_defer"):
        return True
    return picked in case["expect"] or (picked is None and "none" in case["expect"])


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--cases", default=str(HERE / "skill_triggers.json"))
    ap.add_argument("--case", default="", help="only run case ids starting with this")
    ap.add_argument("--runs", type=int, default=1)
    ap.add_argument("-j", type=int, default=4, help="parallel sessions")
    ap.add_argument("--json", help="write every run's details here")
    a = ap.parse_args()

    binary, skills = claude_bin(), project_skills()
    cases = [c for c in json.loads(Path(a.cases).read_text()) if c["id"].startswith(a.case)]
    results = []
    with cf.ThreadPoolExecutor(a.j) as pool:
        futs = {pool.submit(run_once, c["query"], binary, skills): c for c in cases for _ in range(a.runs)}
        for f in cf.as_completed(futs):
            c, r = futs[f], f.result()
            r.update(id=c["id"], expect=c["expect"], ok=passed(c, r, skills))
            results.append(r)
            got = r["detail"] if r["outcome"] in ("skill", "acted") else ""
            # A stopped command is shown in full: whether the classifier was too strict or the
            # session really started acting is only visible from what it tried to run.
            shown = str(got) if r["outcome"] == "acted" else str(got)[:60]
            print(f"{'PASS' if r['ok'] else 'FAIL'} {c['id']:<22} {r['outcome']:<9} {shown:<30} "
                  f"{r['secs']:>5}s  {r['trail']}", flush=True)
    if a.json:
        Path(a.json).write_text(json.dumps(sorted(results, key=lambda r: r["id"]), ensure_ascii=False, indent=1))
    ok = sum(r["ok"] for r in results)
    print(f"\n{ok}/{len(results)} passed")
    return 0 if ok == len(results) else 1


if __name__ == "__main__":
    raise SystemExit(main())
