#!/usr/bin/env python3
"""Flame graphs for a performance report: record, fold, render.

A flame graph shows where a run spent its time: every stack the profiler
sampled, merged into a tree, each frame as wide as the share of samples it
was on. The stacks are kept as folded text, one line per distinct stack:
`root;caller;...;leaf COUNT`. That is the raw data a report keeps beside its
CSV (docs/perf/TEMPLATE.md). The HTML page is made from it, and so is any
other view. prof-fold (kernel/lib/sampler/cli) writes the same format from
the kernel's sampler.

Usage:

    python3 scripts/flamegraph.py record --seconds 5 --out RUN.folded -- COMMAND...
        Starts COMMAND, waits for it to print a line `ready` (so that the
        profiler does not attach while the loader still holds the process),
        samples it with macOS's /usr/bin/sample every millisecond for
        SECONDS, and writes its stacks, folded. COMMAND must keep running
        that long.

    python3 scripts/flamegraph.py fold SAMPLE.txt
        Folds a report /usr/bin/sample already wrote, to stdout.

    python3 scripts/flamegraph.py render RUN.folded --out RUN.html [--title TITLE]
        A self-contained page: the flame graph, a frame's full name and share
        on hover, click to zoom into a frame, a search that highlights every
        frame whose name matches and says what share they cover together.

Frame names stay whole in the folded file; the page shortens them for its
labels (namespaces, template and parameter lists) and shows the whole name
on hover.
"""

# macOS still ships Python 3.9 as python3; this keeps the annotations lazy.
from __future__ import annotations

import argparse
import html
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path

# ------------------------------------------------------------------- folding

# A node of /usr/bin/sample's call graph: its indent (spaces and the tree's
# + ! : | marks), its sample count, and the rest of the line.
NODE = re.compile(r"^(?P<indent>[ +!:|]*?)(?P<count>\d+) (?P<rest>\S.*)$")


def fold_sample(text: str) -> dict[str, int]:
    """Folded stacks of a /usr/bin/sample report: each node's own samples
    (its count less its children's) under the path from its thread's root."""
    lines = text.splitlines()
    try:
        start = next(i for i, line in enumerate(lines) if line.startswith("Call graph:")) + 1
    except StopIteration:
        sys.exit("not a /usr/bin/sample report: no 'Call graph:' section")

    folded: dict[str, int] = {}
    # The open path: (depth, name, count, samples of its children so far).
    path: list[list] = []

    def close_to(depth: int) -> None:
        # Every open node at |depth| or deeper is complete: its own samples
        # go under the path of the nodes still open above it.
        while path and path[-1][0] >= depth:
            node = path.pop()
            own = node[2] - node[3]
            if own > 0:
                stack = ";".join([n[1] for n in path] + [node[1]])
                folded[stack] = folded.get(stack, 0) + own

    for line in lines[start:]:
        if not line.strip():
            break
        m = NODE.match(line)
        if not m:
            continue
        depth = len(m.group("indent"))
        count = int(m.group("count"))
        rest = m.group("rest")
        name = rest.split("  (in ", 1)[0].strip()
        close_to(depth)
        if path:
            path[-1][3] += count
        path.append([depth, name, count, 0])
    close_to(0)

    # The first frame of every stack is the thread line; it says nothing the
    # report needs and differs per run, so it goes.
    out: dict[str, int] = {}
    for stack, count in folded.items():
        frames = stack.split(";")
        if frames and frames[0].startswith("Thread_"):
            frames = frames[1:]
        if frames:
            key = ";".join(frames)
            out[key] = out.get(key, 0) + count
    return out


def write_folded(folded: dict[str, int], path: Path) -> None:
    lines = [f"{stack} {count}" for stack, count in sorted(folded.items())]
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def read_folded(path: Path) -> dict[str, int]:
    folded: dict[str, int] = {}
    for n, line in enumerate(path.read_text(encoding="utf-8").splitlines(), start=1):
        if not line.strip() or line.startswith("#"):
            continue
        stack, _, count = line.rpartition(" ")
        if not stack or not count.isdigit():
            sys.exit(f"{path}:{n}: expected 'frame;frame;... COUNT'")
        folded[stack] = folded.get(stack, 0) + int(count)
    return folded


def record(seconds: int, out: Path, command: list[str]) -> None:
    if not Path("/usr/bin/sample").exists():
        sys.exit("record needs macOS's /usr/bin/sample")
    proc = subprocess.Popen(command, stdout=subprocess.PIPE, text=True)
    first = proc.stdout.readline().strip() if proc.stdout else ""
    if first != "ready":
        proc.kill()
        sys.exit(f"{command[0]} printed {first!r}, not 'ready'")
    with tempfile.NamedTemporaryFile("r", suffix=".txt") as report:
        subprocess.run(["/usr/bin/sample", str(proc.pid), str(seconds), "1", "-mayDie", "-file",
                        report.name], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
        text = Path(report.name).read_text(encoding="utf-8", errors="replace")
    proc.wait()
    folded = fold_sample(text)
    if not folded:
        sys.exit("the profiler recorded no stacks")
    write_folded(folded, out)
    print(f"{out}: {sum(folded.values())} samples, {len(folded)} distinct stacks")


# ----------------------------------------------------------------- rendering

def short_name(name: str) -> str:
    """A label: no anonymous namespace, no parameter list, and templates cut
    down to what tells one instance from another (a paging level)."""
    s = name.replace("(anonymous namespace)::", "")
    if s.endswith(" const"):
        s = s[: -len(" const")]
    # The parameter list: the parenthesised group the name ends with.
    if s.endswith(")"):
        depth = 0
        for i in range(len(s) - 1, -1, -1):
            if s[i] == ")":
                depth += 1
            elif s[i] == "(":
                depth -= 1
                if depth == 0:
                    if i > 0:
                        s = s[:i]
                    break
    levels = re.findall(r"\(arch::paging::X86PagingLevel\)(\d)", s)
    # Every template argument list goes; a paging level is kept as L<n>.
    out, depth = [], 0
    for c in s:
        if c == "<":
            depth += 1
            continue
        if c == ">":
            depth -= 1
            continue
        if depth == 0:
            out.append(c)
    s = "".join(out)
    s = s.replace("arch::paging::Paging::", "Paging::")
    if levels and ("split_at" in s or "map_one_from" in s or "walk_levels" in s):
        s += f"<L{levels[-1]}>"
    return s


def tree_of(folded: dict[str, int]) -> dict:
    root = {"n": "all", "v": 0, "c": {}}
    for stack, count in folded.items():
        root["v"] += count
        node = root
        for frame in stack.split(";"):
            child = node["c"].get(frame)
            if child is None:
                child = node["c"][frame] = {"n": frame, "v": 0, "c": {}}
            child["v"] += count
            node = child

    def pack(node: dict) -> list:
        children = sorted(node["c"].values(), key=lambda c: c["n"])
        return [node["n"], short_name(node["n"]) if node["n"] != "all" else "all", node["v"],
                [pack(c) for c in children]]

    return pack(root)


PAGE = """<!doctype html>
<html lang="en">
<head>
<meta charset="utf-8">
<meta name="viewport" content="width=device-width, initial-scale=1">
<title>__TITLE__</title>
<style>
  :root { --bg: #fbfaf7; --fg: #1f1f1d; --muted: #6b6a66; --line: #d9d6cf; --card: #ffffff; --match: #7b3fd4; }
  @media (prefers-color-scheme: dark) {
    :root { --bg: #1b1b1a; --fg: #ecebe7; --muted: #a3a19b; --line: #3a3936; --card: #242422; --match: #b48cf2; }
  }
  * { box-sizing: border-box; }
  body { margin: 0; background: var(--bg); color: var(--fg); font: 14px/1.5 -apple-system, "Helvetica Neue", sans-serif; }
  main { padding: 16px; }
  h1 { font-size: 18px; margin: 0 0 4px; }
  .sub { color: var(--muted); margin: 0 0 12px; }
  .bar { display: flex; flex-wrap: wrap; gap: 8px; align-items: center; margin-bottom: 10px; }
  input, button { font: inherit; padding: 4px 10px; border: 1px solid var(--line); border-radius: 6px; background: var(--card); color: var(--fg); }
  #info { min-height: 3em; font-family: ui-monospace, Menlo, monospace; font-size: 12px; white-space: pre-wrap; word-break: break-all;
          border: 1px solid var(--line); border-radius: 6px; background: var(--card); padding: 6px 8px; margin-bottom: 10px; }
  svg { width: 100%; display: block; }
  svg text { font: 11px ui-monospace, Menlo, monospace; fill: #1f1f1d; pointer-events: none; }
  svg rect { stroke: var(--bg); stroke-width: 0.5; cursor: pointer; }
  svg rect.match { fill: var(--match) !important; }
  svg rect.faded { opacity: 0.35; }
</style>
</head>
<body>
<main>
  <h1>__TITLE__</h1>
  <p class="sub">__SUB__ &middot; hover a frame for its full name and share, click to zoom, search to highlight.</p>
  <div class="bar">
    <input id="q" placeholder="search, e.g. split_at" size="28">
    <button id="reset">reset zoom</button>
    <span id="found" class="sub" style="margin:0"></span>
  </div>
  <div id="info">&nbsp;</div>
  <svg id="g" xmlns="http://www.w3.org/2000/svg"></svg>
</main>
<script>
const DATA = __DATA__;
const ROW = 18, svg = document.getElementById("g"), info = document.getElementById("info");
const total = DATA[2];
let zoom = DATA;
function hue(name) {
  let h = 0; for (const c of name) h = (h * 31 + c.charCodeAt(0)) >>> 0;
  if (/Paging::/.test(name)) return `hsl(${20 + h % 25}, 85%, ${60 + h % 12}%)`;
  if (/^(main|run|run_bulk|yardsticks)/.test(name)) return `hsl(${200 + h % 30}, 55%, ${68 + h % 10}%)`;
  return `hsl(${40 + h % 20}, 70%, ${62 + h % 14}%)`;
}
function depthOf(node) { return 1 + Math.max(0, ...node[3].map(depthOf)); }
function draw() {
  const depth = depthOf(zoom), W = svg.clientWidth || 1000, H = depth * ROW;
  svg.setAttribute("viewBox", `0 0 ${W} ${H}`); svg.setAttribute("height", H);
  svg.innerHTML = "";
  const query = document.getElementById("q").value.trim();
  const re = query ? new RegExp(query.replace(/[.*+?^${}()|[\\]\\\\]/g, "\\\\$&"), "i") : null;
  let matched = 0;
  function place(node, x, level, counted) {
    const w = node[2] / zoom[2] * W;
    if (w < 0.3) return;
    const y = H - (level + 1) * ROW;
    const r = document.createElementNS("http://www.w3.org/2000/svg", "rect");
    r.setAttribute("x", x); r.setAttribute("y", y); r.setAttribute("width", w); r.setAttribute("height", ROW - 1);
    r.setAttribute("fill", hue(node[1]));
    const isMatch = re && re.test(node[0]);
    if (isMatch) { r.classList.add("match"); if (!counted) matched += node[2]; }
    r.addEventListener("mousemove", () => {
      info.textContent = `${node[0]}\\n${node[2]} samples, ${(node[2] / total * 100).toFixed(2)}% of all, ${(node[2] / zoom[2] * 100).toFixed(2)}% of the view`;
    });
    r.addEventListener("click", () => { zoom = node; draw(); });
    svg.appendChild(r);
    if (w > 40) {
      const t = document.createElementNS("http://www.w3.org/2000/svg", "text");
      t.setAttribute("x", x + 3); t.setAttribute("y", y + ROW - 5);
      const fit = Math.floor((w - 6) / 6.6);
      t.textContent = node[1].length > fit ? node[1].slice(0, Math.max(0, fit - 1)) + "\\u2026" : node[1];
      svg.appendChild(t);
    }
    let cx = x;
    for (const c of node[3]) { place(c, cx, level + 1, counted || isMatch); cx += c[2] / zoom[2] * W; }
  }
  place(zoom, 0, 0, false);
  document.getElementById("found").textContent = re ? `matching frames cover ${(matched / zoom[2] * 100).toFixed(2)}% of the view` : "";
}
document.getElementById("q").addEventListener("input", draw);
document.getElementById("reset").addEventListener("click", () => { zoom = DATA; draw(); });
window.addEventListener("resize", draw);
draw();
</script>
</body>
</html>
"""


def render(folded_path: Path, out: Path, title: str) -> None:
    folded = read_folded(folded_path)
    data = tree_of(folded)
    sub = f"{sum(folded.values())} samples from {html.escape(folded_path.name)}"
    page = (PAGE.replace("__TITLE__", html.escape(title)).replace("__SUB__", sub)
            .replace("__DATA__", json.dumps(data, separators=(",", ":"))))
    out.write_text(page, encoding="utf-8")
    print(f"{out}: {sum(folded.values())} samples")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest="what", required=True)
    rec = sub.add_parser("record", help="sample a command and write its folded stacks")
    rec.add_argument("--seconds", type=int, default=5)
    rec.add_argument("--out", type=Path, required=True)
    rec.add_argument("command", nargs=argparse.REMAINDER)
    fold = sub.add_parser("fold", help="fold a /usr/bin/sample report")
    fold.add_argument("report", type=Path)
    ren = sub.add_parser("render", help="a flame graph page from folded stacks")
    ren.add_argument("folded", type=Path)
    ren.add_argument("--out", type=Path, required=True)
    ren.add_argument("--title", default="flame graph")
    args = parser.parse_args()

    if args.what == "record":
        command = args.command[1:] if args.command[:1] == ["--"] else args.command
        if not command:
            parser.error("record needs a command after --")
        record(args.seconds, args.out, command)
    elif args.what == "fold":
        folded = fold_sample(args.report.read_text(encoding="utf-8", errors="replace"))
        for stack, count in sorted(folded.items()):
            print(f"{stack} {count}")
    else:
        render(args.folded, args.out, args.title)
    return 0


if __name__ == "__main__":
    sys.exit(main())
