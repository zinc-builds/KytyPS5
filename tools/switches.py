#!/usr/bin/env python3
"""Inventory of the emulator's KYTY_* environment switches, generated from the code.

    python3 -I tools/switches.py          write docs/mt-rewrite/SWITCHES.md and src/common/envSwitchList.inc
    python3 -I tools/switches.py --check  exit 1 if either file is out of date (run it before a PR)

A switch is a string literal "KYTY_NAME" passed to getenv(), Common::EnvFlag() or a local helper.
For each one the table gives where it is read, how its value is parsed (and so its default), when
it is read, its kind and the comment next to it. Kinds:
  verify      re-checks a fast path against the slow one (exits or counts on a difference)
  diagnostic  statistics, traces, dumps, timing; changes nothing the game sees
  kill switch on by default; "0" turns the feature off (A/B and bisecting)
  opt-in      off by default; turns a feature or behaviour on
  tuning      a number, CPU or list that changes how something runs
  tool hook   set by a script or the emulator itself (bench overlays, screenshots, PGO training)
The heuristics are in classify(); where they guess wrong, fix the code's comment or parse, not this
table. src/common/envSwitchList.inc feeds the startup report (unknown names are reported as typos).
"""
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, "src")
DOC = os.path.join(ROOT, "docs", "mt-rewrite", "SWITCHES.md")
INC = os.path.join(SRC, "common", "envSwitchList.inc")
SKIP_DIRS = {"launcher"}  # the Qt launcher reads its own settings, not KYTY_* switches
# Hooks for tools rather than settings: the emulator sets KYTY_RELAUNCHED for a program it
# relaunches; the bench and training scripts set the others.
TOOL_HOOKS = {"KYTY_RELAUNCHED", "KYTY_APP0_OVERLAY", "KYTY_SHOT_FILE", "KYTY_SHOT_COUNT",
              "KYTY_PGO_DUMP", "KYTY_PGO_DUMP_INTERVAL"}

NAME = re.compile(r'"(KYTY_[A-Z0-9_]+)"')
CHAR_VALUE = re.compile(r"\[0\]\s*==\s*'([1-9a-z])'")
VALUE_STR = re.compile(r'(?:strcmp\([^,]+,\s*|==\s*|string_view\([^)]*\)\s*==\s*)"([a-z0-9_]+)"')


def sources():
    for dirpath, dirnames, filenames in os.walk(SRC):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for f in sorted(filenames):
            # The report's own tables name switches without reading them.
            if f.endswith((".cpp", ".h", ".inc")) and f not in ("envSwitchList.inc", "configReport.cpp"):
                yield os.path.join(dirpath, f)


def comment_above(lines, i):
    """The // comment block right above line i (or ending one line above a lambda header)."""
    out = []
    j = i - 1
    # step over a lambda/static header line or two
    for _ in range(2):
        if j >= 0 and not lines[j].strip().startswith("//") and re.search(r"(static|\[\]|\{\s*$)", lines[j]):
            j -= 1
    while j >= 0 and lines[j].strip().startswith("//"):
        out.insert(0, lines[j].strip()[2:].strip())
        j -= 1
    trailing = re.search(r"//\s*(.*)$", lines[i])
    if trailing and not out:
        out = [trailing.group(1)]
    return " ".join(out)


def comment_naming(text_lines, name):
    """A comment anywhere in the file that names the switch."""
    for line in text_lines:
        s = line.strip()
        if s.startswith("//") and name in s:
            return s[2:].strip()
    return ""


def classify(name, window, values):
    """(parse, default, kind) from the code around the read."""
    w = " ".join(window)
    n = name
    # parse and default
    m = re.search(r'EnvFlag\("%s",\s*(true|false)\)' % n, w)
    if m:
        parse, default = "flag (0/false/off/no = off)", "on" if m.group(1) == "true" else "off"
    elif re.search(r'EnvFlag\("%s"\)' % n, w):
        parse, default = "flag (0/false/off/no = off)", "off"
    elif re.search(r'EnvCpu\("%s"\)' % n, w):
        parse, default = "CPU number", "automatic"
    elif re.search(r"(atoi|strtoull|strtoll|strtoul|stoi|stoull)\(", w) and not values:
        d = re.search(r":\s*(-?[0-9][0-9.]*)(?:u|ull|ll)?\s*[;)]", w)
        parse, default = "number", d.group(1) if d else "see code"
    elif re.search(r"getline\(|ParseCpuList|ParseKeepList|ReadCpuList", w):
        parse, default = "list", "see code"
    elif values:
        parse = "mode: " + ", ".join(sorted(set(values)))
        default = "see code"
        unset = re.search(r"== nullptr[^)]*\)\s*\{?\s*return\s+([A-Za-z:]*\w)", w)
        if unset:
            r = unset.group(1)
            default = ("on" if re.fullmatch(r"[1-9]|.*::On", r) else "off" if re.fullmatch(r"0|.*::(Off|Skip)", r)
                       else r.split("::")[-1] if "::" in r else "see code")
    elif re.search(r"== nullptr \|\| [a-z_]+\[0\] != '0'|== nullptr \|\| std::strcmp\([^)]*\"0\"\) != 0|== nullptr \|\| [a-z_]+\[0\] != '1'", w):
        parse, default = "on unless 0", "on"
        if re.search(r"\[0\] != '1'", w):
            parse, default = "off unless 1 (inverted)", "off"
    elif re.search(r"!= nullptr && [a-z_]+\[0\] == '0'|!= nullptr && std::strcmp\([^)]*\"0\"\) == 0", w):
        parse, default = "on unless 0", "on"
    elif re.search(r"!= nullptr && \*?[a-z_]+(\[0\])? ?== '1'", w):
        parse, default = "1 = on", "off"
    elif re.search(r"!= nullptr && [a-z_]+\[0\] != '0'|!= nullptr && std::strcmp\([^)]*\"0\"\) != 0", w):
        parse, default = "on unless 0", "off"
    elif re.search(r'getenv\("%s"\)\s*!= nullptr' % n, w) or re.search(r'getenv\("%s"\)\s*== nullptr' % n, w):
        parse, default = "PRESENCE (any value = on, even 0)", "off"
    else:
        parse, default = "value", "unset"
    # kind
    is_mode = parse.startswith("mode: ")
    if ("VERIFY" in n or "SELFTEST" in n) or (values and set(values) <= {"verify"}):
        kind = "verify"
    elif re.search(r"(STATS|DIAG|TRACE|DUMP|_LOG|SELFTEST|COUNT|_TIME|SAMPLE|PMU|_PERF|PERF_LOG|OFFCPU|SHOT|MEM_DIAG|FAULT_DIAG)", n):
        kind = "diagnostic"
    elif parse in ("number", "CPU number", "list") or (
            not is_mode and re.search(r"(_CPU|_CPUS|NICE|_SPIN_US|_DEPTH|_MIN|_MAX|_US|_K$|_WORKERS|_INTERVAL|_NAMES|PRIORITY|_KEEP)$", n)):
        kind = "tuning"
    elif default == "on":
        kind = "kill switch"
    else:
        kind = "opt-in"
    if "verify" in values and kind != "verify":
        kind += " + verify mode"
    return parse, default, kind


def when_read(lines, i, path):
    ctx = " ".join(lines[max(0, i - 3): i + 1])
    if path.endswith("main.cpp"):
        return "startup"
    if re.search(r"\bstatic\b", ctx):
        return "first use"
    return "each call"


def scan():
    switches = {}
    for path in sources():
        rel = os.path.relpath(path, ROOT)
        with open(path, encoding="utf-8", errors="replace") as f:
            lines = f.read().split("\n")
        for i, line in enumerate(lines):
            for m in NAME.finditer(line):
                name = m.group(1)
                window = lines[i: i + 8]
                if not re.search(r"getenv|EnvFlag|EnvCpu|Env[A-Z][a-z]+\(", line + " ".join(lines[i - 1: i])):
                    # A name in another call (a table, a helper argument): still a site.
                    pass
                joined = " ".join(window)
                values = [v for v in VALUE_STR.findall(joined) + CHAR_VALUE.findall(joined) if v != "0"]
                if not VALUE_STR.findall(joined):
                    values = []  # a single char test is a flag, not a mode
                s = switches.setdefault(name, {"sites": [], "parses": [], "comment": ""})
                s["sites"].append(f"{rel}:{i + 1}")
                s["parses"].append(classify(name, window, values))
                s["whens"] = s.get("whens", []) + [when_read(lines, i, path)]
                if not s["comment"]:
                    s["comment"] = comment_above(lines, i) or comment_naming(lines, name)
    return switches


def summarize(switches):
    rows = []
    for name in sorted(switches):
        s = switches[name]
        parses = s["parses"]
        # The most informative parse: anything beats "value".
        best = next((p for p in parses if p[0] != "value" and not p[0].startswith("PRESENCE")), None) or next(
            (p for p in parses if p[0].startswith("PRESENCE")), parses[0])
        modes = sorted({v for p in parses if p[0].startswith("mode: ") for v in p[0][6:].split(", ")})
        if modes and best[0].startswith("mode: "):
            default = next((p[1] for p in parses if p[0].startswith("mode: ") and p[1] in ("on", "off")), best[1])
            kind = best[2]
            if kind.split(" + ")[0] not in ("diagnostic", "tuning") and (set(modes) - {"verify"} or default == "on"):
                kind = ("kill switch" if default == "on" else "opt-in") + (" + verify mode" if "verify" in modes else "")
            best = ("mode: " + ", ".join(modes), default, kind)
        kind = "tool hook" if name in TOOL_HOOKS else best[2]
        if any(p[2] == "verify" for p in parses) and kind not in ("verify", "tool hook") and "verify mode" not in kind:
            kind = kind + " + verify mode"
        when = "first use" if "first use" in s["whens"] else s["whens"][0]
        comment = re.sub(r"\s+", " ", s["comment"]).replace("|", "\\|")
        if len(comment) > 220:
            comment = comment[:217].rstrip() + "..."
        rows.append((name, kind, best[1], best[0], when, s["sites"], comment))
    return rows


def script_variables(emulator_names):
    """KYTY_* names the bench and build scripts use that the emulator does not read."""
    found = {}
    for d in ("tools", os.path.join("tools", "astro-bench")):
        full = os.path.join(ROOT, d)
        for f in sorted(os.listdir(full)):
            if not f.endswith((".sh", ".py")) or f == "switches.py":
                continue
            with open(os.path.join(full, f), encoding="utf-8", errors="replace") as fh:
                for name in re.findall(r"KYTY_[A-Z0-9_]+", fh.read()):
                    if name not in emulator_names:
                        found.setdefault(name, []).append(f"{d}/{f}")
    return {k: sorted(set(v)) for k, v in sorted(found.items())}


def render_md(rows, scripts):
    out = [
        "# KYTY_* switches",
        "",
        "Generated by `tools/switches.py` from the code; do not edit by hand (run it after adding or",
        "changing a switch; `--check` fails when this file is stale). Every switch is an environment",
        "variable read by the emulator. The startup report (`CONFIG` lines in the log) prints the",
        "ones set in the environment, and warns about names not in this table.",
        "",
        "On/off switches read through `Common::EnvFlag` treat `0`, `false`, `off` and `no` as off.",
        "",
        "| Switch | Kind | Default | Values | Read | Where | What |",
        "|---|---|---|---|---|---|---|",
    ]
    for name, kind, default, parse, when, sites, comment in rows:
        where = "<br>".join(f"`{s}`" for s in sites[:3]) + (f" (+{len(sites) - 3})" if len(sites) > 3 else "")
        out.append(f"| `{name}` | {kind} | {default} | {parse} | {when} | {where} | {comment} |")
    out += ["", "## Script variables", "",
            "Set or read by the bench and build scripts, never by the emulator (the startup report",
            "lists them as such, not as unknown switches).", "",
            "| Variable | Used in |", "|---|---|"]
    for name, files in scripts.items():
        out.append(f"| `{name}` | " + ", ".join(f"`{f}`" for f in files) + " |")
    counts = {}
    for r in rows:
        counts[r[1]] = counts.get(r[1], 0) + 1
    out += ["", "Totals: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())) + f"; {len(rows)} switches.", ""]
    return "\n".join(out)


def render_inc(rows, scripts):
    out = ["// Generated by tools/switches.py; do not edit. KYTY_SWITCH(name, kind, default)"]
    for name, kind, default, parse, when, sites, comment in rows:
        out.append(f'KYTY_SWITCH("{name}", "{kind}", "{default}")')
    for name in scripts:
        out.append(f'KYTY_SWITCH("{name}", "script variable, not read by the emulator", "-")')
    return "\n".join(out) + "\n"


def main():
    rows = summarize(scan())
    scripts = script_variables({r[0] for r in rows})
    md, inc = render_md(rows, scripts), render_inc(rows, scripts)
    if "--check" in sys.argv:
        stale = [p for p, t in ((DOC, md), (INC, inc)) if not os.path.exists(p) or open(p, encoding="utf-8").read() != t]
        for p in stale:
            print(f"switches.py: {os.path.relpath(p, ROOT)} is out of date; run tools/switches.py", file=sys.stderr)
        return 1 if stale else 0
    for p, t in ((DOC, md), (INC, inc)):
        with open(p, "w", encoding="utf-8") as f:
            f.write(t)
    presence = [r[0] for r in rows if r[3].startswith("PRESENCE")]
    print(f"switches.py: {len(rows)} switches; presence-only: {', '.join(presence) or 'none'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
